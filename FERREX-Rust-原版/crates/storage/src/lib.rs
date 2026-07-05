use std::fs::File;
use std::io::Write;
use memmap2::Mmap;
use crc32fast::Hasher;
use std::collections::HashMap;

pub const MAGIC: &[u8; 4] = b"FIDX";
pub const VERSION: u32 = 1;
pub const HEADER_SIZE: usize = 48;
pub const RECORD_SIZE: usize = 40;

/// Structure of Arrays (SoA) layout for in-memory IndexStore
pub struct IndexStore {
    pub frns: Vec<u64>,
    pub parent_frns: Vec<u64>,
    pub sizes: Vec<u64>,
    pub timestamps: Vec<u64>,
    pub flags: Vec<u32>,
    pub name_offsets: Vec<u32>,
    pub string_pool: Vec<u8>,
    pub usn_watermark: u64,
    pub volume_serial: u64,
    
    // Lookup structures (Rebuilt on load, not persisted)
    pub frn_to_idx: HashMap<u64, u32>,
    pub sorted_idx: Vec<u32>,
}

pub type LoadedIndex = IndexStore;

impl IndexStore {
    pub fn new() -> Self {
        Self {
            frns: Vec::new(),
            parent_frns: Vec::new(),
            sizes: Vec::new(),
            timestamps: Vec::new(),
            flags: Vec::new(),
            name_offsets: Vec::new(),
            string_pool: Vec::new(),
            usn_watermark: 0,
            volume_serial: 0,
            frn_to_idx: HashMap::new(),
            sorted_idx: Vec::new(),
        }
    }

    pub fn from_mapped(mapped: &MappedIndex) -> Self {
        let mut store = Self::new();
        let records_ptr = unsafe {
            mapped.mmap.as_ptr().add(HEADER_SIZE) as *const FileRecord
        };
        let records = unsafe {
            std::slice::from_raw_parts(records_ptr, mapped.record_count)
        };

        store.frns.reserve(mapped.record_count);
        store.parent_frns.reserve(mapped.record_count);
        store.sizes.reserve(mapped.record_count);
        store.timestamps.reserve(mapped.record_count);
        store.name_offsets.reserve(mapped.record_count);
        store.flags.reserve(mapped.record_count);

        for r in records {
            store.frns.push(r.frn);
            store.parent_frns.push(r.parent_frn);
            store.sizes.push(r.size);
            store.timestamps.push(r.timestamp);
            store.name_offsets.push(r.name_offset);
            store.flags.push(r.flags);
        }

        let pool_ptr = unsafe { mapped.mmap.as_ptr().add(mapped.string_pool_offset) };
        let pool_len = mapped.mmap.len() - mapped.string_pool_offset;
        store.string_pool = unsafe { std::slice::from_raw_parts(pool_ptr, pool_len).to_vec() };
        store.usn_watermark = mapped.usn_watermark;
        store.volume_serial = mapped.volume_serial;

        store
    }

    /// Build a sorted index array (indices into records, sorted by lowercase filename).
    /// Call once after loading. Store result in FerrexApp.
    pub fn build_sorted_idx(&self) -> Vec<u32> {
        let mut idx: Vec<u32> = (0..self.frns.len() as u32).collect();
        idx.sort_unstable_by(|&a, &b| {
            let name_a = pool_get_name_lower(&self.string_pool, self.name_offsets[a as usize] as usize);
            let name_b = pool_get_name_lower(&self.string_pool, self.name_offsets[b as usize] as usize);
            name_a.cmp(&name_b)
        });
        idx
    }

    /// Build FRN → record index map for path resolution.
    pub fn build_frn_map(&self) -> HashMap<u64, u32> {
        let mut map = HashMap::with_capacity(self.frns.len());
        for (i, &frn) in self.frns.iter().enumerate() {
            map.insert(frn, i as u32);
        }
        map
    }

    /// Backwards compatibility for CLI
    pub fn rebuild_lookups(&mut self) {
        self.frn_to_idx = self.build_frn_map();
        self.sorted_idx = self.build_sorted_idx();
    }

    /// Backwards compatibility for CLI
    pub fn get_path(&self, index: usize) -> String {
        let mut lru = lru::LruCache::new(std::num::NonZeroUsize::new(10000).unwrap());
        resolve_path(
            "",
            index as u32,
            &self.frns,
            &self.parent_frns,
            &self.name_offsets,
            &self.string_pool,
            &self.frn_to_idx,
            &mut lru
        ).replace(":\\", "")
    }
}

pub fn pool_get_name(pool: &[u8], offset: usize) -> String {
    if offset >= pool.len() { return String::new(); }
    let slice = &pool[offset..];
    let end = slice.iter().position(|&b| b == 0).unwrap_or(slice.len());
    String::from_utf8_lossy(&slice[..end]).to_string()
}

pub fn pool_get_name_lower(pool: &[u8], offset: usize) -> String {
    pool_get_name(pool, offset).to_lowercase()
}

/// Resolve full path from FRN chain using a pre-built FRN map.
/// Uses an LRU cache to avoid redundant traversals.
pub fn resolve_path(
    drive:   &str,
    idx:     u32,
    frns:    &[u64],
    parent_frns: &[u64],
    name_offsets: &[u32],
    pool:    &[u8],
    frn_map: &HashMap<u64, u32>,
    lru:     &mut lru::LruCache<u64, String>,
) -> String {
    let frn = frns[idx as usize];

    // Check LRU for this FRN
    if let Some(cached) = lru.get(&frn) {
        return cached.clone();
    }

    let mut parts: Vec<String> = Vec::new();
    let mut current = idx;

    loop {
        let current_frn = frns[current as usize];
        let parent_frn = parent_frns[current as usize];
        let name_offset = name_offsets[current as usize] as usize;
        
        parts.push(pool_get_name(pool, name_offset));

        // Root FRN on NTFS is typically 5
        if current_frn == 5 || parent_frn == 0 || current_frn == parent_frn {
            break;
        }

        match frn_map.get(&parent_frn) {
            Some(&parent_idx) => current = parent_idx,
            None => break,
        }
    }

    parts.reverse();
    let path = format!("{}:\\{}", drive, parts.join("\\"));

    // Cache it
    lru.put(frn, path.clone());
    path
}

#[repr(C, packed)]
#[derive(Debug, Copy, Clone)]
pub struct FileRecord {
    pub frn: u64,
    pub parent_frn: u64,
    pub size: u64,
    pub timestamp: u64,
    pub name_offset: u32,
    pub flags: u32,
}

pub fn save_index(
    path: &str,
    store: &IndexStore,
) -> std::io::Result<()> {
    let record_count = store.frns.len();
    let string_pool_size = store.string_pool.len();
    
    let total_size = HEADER_SIZE + (record_count * RECORD_SIZE) + string_pool_size;
    let mut buffer = Vec::with_capacity(total_size);
    
    buffer.extend_from_slice(MAGIC);
    buffer.extend_from_slice(&VERSION.to_le_bytes());
    buffer.extend_from_slice(&(record_count as u64).to_le_bytes());
    buffer.extend_from_slice(&(string_pool_size as u64).to_le_bytes());
    buffer.extend_from_slice(&store.usn_watermark.to_le_bytes());
    buffer.extend_from_slice(&store.volume_serial.to_le_bytes());
    buffer.extend_from_slice(&0u32.to_le_bytes());
    buffer.extend_from_slice(&0u32.to_le_bytes());
    
    for i in 0..record_count {
        buffer.extend_from_slice(&store.frns[i].to_le_bytes());
        buffer.extend_from_slice(&store.parent_frns[i].to_le_bytes());
        buffer.extend_from_slice(&store.sizes[i].to_le_bytes());
        buffer.extend_from_slice(&store.timestamps[i].to_le_bytes());
        buffer.extend_from_slice(&store.name_offsets[i].to_le_bytes());
        buffer.extend_from_slice(&store.flags[i].to_le_bytes());
    }
    
    buffer.extend_from_slice(&store.string_pool);
    
    let mut hasher = Hasher::new();
    hasher.update(&buffer);
    let checksum = hasher.finalize();
    buffer[0x28..0x2C].copy_from_slice(&checksum.to_le_bytes());
    
    let mut file = File::create(path)?;
    file.write_all(&buffer)?;
    file.sync_all()?;
    Ok(())
}

pub struct MappedIndex {
    pub mmap: Mmap,
    pub record_count: usize,
    pub string_pool_offset: usize,
    pub usn_watermark: u64,
    pub volume_serial: u64,
}

impl MappedIndex {
    pub fn load(path: &str) -> std::io::Result<Self> {
        let file = File::open(path)?;
        let mmap = unsafe { Mmap::map(&file)? };
        
        if mmap.len() < HEADER_SIZE {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "File too small"));
        }
        
        if &mmap[0..4] != MAGIC {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "Invalid magic"));
        }
        
        let stored_crc = u32::from_le_bytes(mmap[0x28..0x2C].try_into().unwrap());
        let mut hasher = Hasher::new();
        hasher.update(&mmap[0..0x28]);
        hasher.update(&[0u8; 4]);
        hasher.update(&mmap[0x2C..]);
        let calculated_crc = hasher.finalize();
        
        if stored_crc != calculated_crc {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "CRC mismatch"));
        }
        
        let record_count = u64::from_le_bytes(mmap[8..16].try_into().unwrap()) as usize;
        let usn_watermark = u64::from_le_bytes(mmap[0x18..0x20].try_into().unwrap());
        let volume_serial = u64::from_le_bytes(mmap[0x20..0x28].try_into().unwrap());
        
        Ok(Self {
            mmap,
            record_count,
            string_pool_offset: HEADER_SIZE + (record_count * RECORD_SIZE),
            usn_watermark,
            volume_serial,
        })
    }
}
