use std::fs::{File, OpenOptions};
use std::io::{Read, Write, Seek, SeekFrom, BufReader, BufWriter};
use std::path::Path;
use crc32fast::Hasher;
use std::collections::HashMap;

pub const BIN_MAGIC: u32 = 0x4E49422E; // ".BIN"
pub const IDX_MAGIC: u32 = 0x5844492E; // ".IDX"
pub const VERSION: u16 = 1;

// --- Legacy Compatibility Structures ---

pub const MAGIC: &[u8; 4] = b"FIDX";
pub const OLD_VERSION: u32 = 1;
pub const HEADER_SIZE: usize = 48;
pub const RECORD_SIZE: usize = 40;

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
    pub frn_to_idx: HashMap<u64, u32>,
    pub sorted_idx: Vec<u32>,
}

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

    pub fn build_sorted_idx(&self) -> Vec<u32> {
        let mut idx: Vec<u32> = (0..self.frns.len() as u32).collect();
        idx.sort_unstable_by(|&a, &b| {
            let name_a = pool_get_name_lower(&self.string_pool, self.name_offsets[a as usize] as usize);
            let name_b = pool_get_name_lower(&self.string_pool, self.name_offsets[b as usize] as usize);
            name_a.cmp(&name_b)
        });
        idx
    }

    pub fn build_frn_map(&self) -> HashMap<u64, u32> {
        let mut map = HashMap::with_capacity(self.frns.len());
        for (i, &frn) in self.frns.iter().enumerate() {
            map.insert(frn, i as u32);
        }
        map
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
    if let Some(cached) = lru.get(&frn) { return cached.clone(); }

    let mut parts: Vec<String> = Vec::new();
    let mut current = idx;
    loop {
        let current_frn = frns[current as usize];
        let parent_frn = parent_frns[current as usize];
        parts.push(pool_get_name(pool, name_offsets[current as usize] as usize));
        if current_frn == 5 || parent_frn == 0 || current_frn == parent_frn { break; }
        match frn_map.get(&parent_frn) {
            Some(&parent_idx) => current = parent_idx,
            None => break,
        }
    }
    parts.reverse();
    let path = format!("{}:\\{}", drive, parts.join("\\"));
    lru.put(frn, path.clone());
    path
}

// --- Plan-137 New Dual-File Storage Implementation ---

#[repr(C, packed)]
#[derive(Debug, Copy, Clone)]
pub struct BinHeader {
    pub magic: u32,
    pub version: u16,
    pub volume_serial: u64,
    pub record_count: u64,
}

#[repr(C, packed)]
#[derive(Debug, Copy, Clone)]
pub struct RecordHeader {
    pub frn: u64,
    pub parent_frn: u64,
    pub name_len: u16,
    // name bytes follow
}

#[repr(C, packed)]
#[derive(Debug, Copy, Clone)]
pub struct RecordFooter {
    pub attributes: u32,
    pub timestamp: u64,
    pub crc32: u32,
}

#[repr(C, packed)]
#[derive(Debug, Copy, Clone)]
pub struct IdxHeader {
    pub magic: u32,
    pub version: u16,
    pub volume_serial: u64,
    pub main_count: u64,
    pub delta_count: u64,
}

#[repr(C, packed)]
#[derive(Debug, Copy, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct IndexEntry {
    pub frn: u64,
    pub offset: u64,
}

pub struct Storage {
    pub bin_path: String,
    pub idx_path: String,
    pub volume_serial: u64,
    pub main_index: Vec<IndexEntry>,
    pub delta_layer: Vec<IndexEntry>,
}

impl Storage {
    pub fn open(bin_path: &str, idx_path: &str, volume_serial: u64) -> std::io::Result<Self> {
        let mut s = Self {
            bin_path: bin_path.to_string(),
            idx_path: idx_path.to_string(),
            volume_serial,
            main_index: Vec::new(),
            delta_layer: Vec::new(),
        };

        if Path::new(bin_path).exists() {
            if !Path::new(idx_path).exists() || s.load_index().is_err() {
                s.rebuild_index()?;
            }
        } else {
            s.init_new_bin()?;
        }
        Ok(s)
    }

    fn init_new_bin(&self) -> std::io::Result<()> {
        let mut file = File::create(&self.bin_path)?;
        let header = BinHeader { magic: BIN_MAGIC, version: VERSION, volume_serial: self.volume_serial, record_count: 0 };
        file.write_all(unsafe { std::slice::from_raw_parts(&header as *const _ as *const u8, 22) })?;
        file.sync_all()?;
        Ok(())
    }

    fn load_index(&mut self) -> std::io::Result<()> {
        let mut file = File::open(&self.idx_path)?;
        let mut h = [0u8; 30];
        file.read_exact(&mut h)?;
        let header: IdxHeader = unsafe { std::ptr::read(h.as_ptr() as *const _) };
        if header.magic != IDX_MAGIC || header.volume_serial != self.volume_serial {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "IDX mismatch"));
        }
        self.main_index.clear();
        for _ in 0..header.main_count {
            let mut e = [0u8; 16];
            file.read_exact(&mut e)?;
            self.main_index.push(unsafe { std::ptr::read(e.as_ptr() as *const _) });
        }
        self.delta_layer.clear();
        for _ in 0..header.delta_count {
            let mut e = [0u8; 16];
            file.read_exact(&mut e)?;
            self.delta_layer.push(unsafe { std::ptr::read(e.as_ptr() as *const _) });
        }
        for i in 1..self.main_index.len() {
            if self.main_index[i].frn < self.main_index[i-1].frn { return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "IDX unsorted")); }
        }
        Ok(())
    }

    pub fn rebuild_index(&mut self) -> std::io::Result<()> {
        let mut file = File::open(&self.bin_path)?;
        let mut h = [0u8; 22];
        file.read_exact(&mut h)?;
        let header: BinHeader = unsafe { std::ptr::read(h.as_ptr() as *const _) };
        if header.magic != BIN_MAGIC || header.volume_serial != self.volume_serial {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "BIN mismatch"));
        }
        self.main_index.clear();
        self.delta_layer.clear();
        let mut pos = 22u64;
        for _ in 0..header.record_count {
            file.seek(SeekFrom::Start(pos))?;
            let mut frn_bytes = [0u8; 8];
            file.read_exact(&mut frn_bytes)?;
            let frn = u64::from_le_bytes(frn_bytes);
            self.main_index.push(IndexEntry { frn, offset: pos });
            file.seek(SeekFrom::Current(8))?; // parent
            let mut len_bytes = [0u8; 2];
            file.read_exact(&mut len_bytes)?;
            let name_len = u16::from_le_bytes(len_bytes);
            pos += (18 + name_len + 16) as u64; // Header(18) + Name + Footer(16)
        }
        self.main_index.sort_unstable();
        self.save_index()
    }

    pub fn append_record(&mut self, frn: u64, parent_frn: u64, name: &str, attributes: u32, timestamp: u64) -> std::io::Result<()> {
        let mut file = OpenOptions::new().read(true).write(true).open(&self.bin_path)?;
        let offset = file.seek(SeekFrom::End(0))?;
        let name_bytes = name.as_bytes();
        let name_len = name_bytes.len() as u16;

        let mut hasher = Hasher::new();
        hasher.update(&frn.to_le_bytes());
        hasher.update(&parent_frn.to_le_bytes());
        hasher.update(&name_len.to_le_bytes());
        hasher.update(name_bytes);
        hasher.update(&attributes.to_le_bytes());
        hasher.update(&timestamp.to_le_bytes());
        let crc32 = hasher.finalize();

        file.write_all(&frn.to_le_bytes())?;
        file.write_all(&parent_frn.to_le_bytes())?;
        file.write_all(&name_len.to_le_bytes())?;
        file.write_all(name_bytes)?;
        file.write_all(&attributes.to_le_bytes())?;
        file.write_all(&timestamp.to_le_bytes())?;
        file.write_all(&crc32.to_le_bytes())?;

        file.seek(SeekFrom::Start(14))?;
        let mut cb = [0u8; 8];
        file.read_exact(&mut cb)?;
        let mut count = u64::from_le_bytes(cb);
        count += 1;
        file.seek(SeekFrom::Start(14))?;
        file.write_all(&count.to_le_bytes())?;
        file.sync_all()?;

        self.delta_layer.push(IndexEntry { frn, offset });
        if self.delta_layer.len() >= 5000 { self.merge_index() } else { self.save_index() }
    }

    fn save_index(&self) -> std::io::Result<()> {
        let temp = format!("{}.tmp", self.idx_path);
        {
            let mut f = File::create(&temp)?;
            let h = IdxHeader { magic: IDX_MAGIC, version: VERSION, volume_serial: self.volume_serial, main_count: self.main_index.len() as u64, delta_count: self.delta_layer.len() as u64 };
            f.write_all(unsafe { std::slice::from_raw_parts(&h as *const _ as *const u8, 30) })?;
            for e in &self.main_index { f.write_all(unsafe { std::slice::from_raw_parts(e as *const _ as *const u8, 16) })?; }
            for e in &self.delta_layer { f.write_all(unsafe { std::slice::from_raw_parts(e as *const _ as *const u8, 16) })?; }
            f.sync_all()?;
        }
        std::fs::rename(temp, &self.idx_path)
    }

    fn merge_index(&mut self) -> std::io::Result<()> {
        let mut map = HashMap::new();
        for e in &self.main_index { map.insert(e.frn, e.offset); }
        for e in &self.delta_layer { map.insert(e.frn, e.offset); }
        self.main_index = map.into_iter().map(|(frn, offset)| IndexEntry { frn, offset }).collect();
        self.main_index.sort_unstable();
        self.delta_layer.clear();
        self.save_index()
    }

    pub fn load_into_store(&self) -> std::io::Result<IndexStore> {
        let mut store = IndexStore::new();
        store.volume_serial = self.volume_serial;
        let mut file = File::open(&self.bin_path)?;
        
        let mut sorted_entries = self.main_index.clone();
        if !self.delta_layer.is_empty() {
            let mut map = HashMap::new();
            for e in &self.main_index { map.insert(e.frn, e.offset); }
            for e in &self.delta_layer { map.insert(e.frn, e.offset); }
            sorted_entries = map.into_iter().map(|(frn, offset)| IndexEntry { frn, offset }).collect();
        }

        for entry in sorted_entries {
            file.seek(SeekFrom::Start(entry.offset))?;
            let mut rh_bytes = [0u8; 18];
            file.read_exact(&mut rh_bytes)?;
            let rh: RecordHeader = unsafe { std::ptr::read(rh_bytes.as_ptr() as *const _) };

            let mut name_bytes = vec![0u8; rh.name_len as usize];
            file.read_exact(&mut name_bytes)?;

            let mut rf_bytes = [0u8; 16];
            file.read_exact(&mut rf_bytes)?;
            let rf: RecordFooter = unsafe { std::ptr::read(rf_bytes.as_ptr() as *const _) };

            store.frns.push(rh.frn);
            store.parent_frns.push(rh.parent_frn);
            store.sizes.push(0);
            store.timestamps.push(rf.timestamp);
            store.flags.push(rf.attributes);

            let offset = store.string_pool.len() as u32;
            store.name_offsets.push(offset);
            store.string_pool.extend_from_slice(&name_bytes);
            store.string_pool.push(0);
        }
        store.frn_to_idx = store.build_frn_map();
        store.sorted_idx = store.build_sorted_idx();
        Ok(store)
    }

    pub fn find_record_offset(&self, frn: u64) -> Option<u64> {
        for entry in self.delta_layer.iter().rev() {
            if entry.frn == frn { return Some(entry.offset); }
        }
        match self.main_index.binary_search_by_key(&frn, |e| e.frn) {
            Ok(idx) => Some(self.main_index[idx].offset),
            Err(_) => None,
        }
    }
}
