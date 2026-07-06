use memchr::memmem;
use rayon::prelude::*;

pub struct SearchOptions<'a> {
    pub query:        &'a str,           // substring or regex pattern
    pub use_regex:    bool,
    pub ext_filter:   Option<&'a str>,   // e.g. "rs" — no dot
    pub include_dirs: bool,              // false = files only
    pub include_hidden:  bool,
    pub include_system:  bool,
}

impl Default for SearchOptions<'_> {
    fn default() -> Self {
        Self {
            query: "",
            use_regex: false,
            ext_filter: None,
            include_dirs: true,
            include_hidden: false,
            include_system: false,
        }
    }
}

pub struct Searcher<'a> {
    pub frns: &'a [u64],
    pub parent_frns: &'a [u64],
    pub sizes: &'a [u64],
    pub timestamps: &'a [u64],
    pub flags: &'a [u32],
    pub name_offsets: &'a [u32],
    pub string_pool: &'a [u8],
}

impl<'a> Searcher<'a> {
    pub fn new(
        frns: &'a [u64],
        parent_frns: &'a [u64],
        sizes: &'a [u64],
        timestamps: &'a [u64],
        flags: &'a [u32],
        name_offsets: &'a [u32],
        string_pool: &'a [u8],
    ) -> Self {
        Self {
            frns,
            parent_frns,
            sizes,
            timestamps,
            flags,
            name_offsets,
            string_pool,
        }
    }

    pub fn search(&self, opts: &SearchOptions) -> Vec<usize> {
        if opts.query.is_empty() && opts.ext_filter.is_none() {
            return Vec::new();
        }

        let regex_pattern: Option<regex::Regex> = if opts.use_regex && !opts.query.is_empty() {
            regex::RegexBuilder::new(opts.query)
                .case_insensitive(true)
                .build()
                .ok()
        } else {
            None
        };

        let query_lower = opts.query.to_lowercase();
        let finder = if !opts.use_regex && !query_lower.is_empty() {
            Some(memmem::Finder::new(query_lower.as_bytes()))
        } else {
            None
        };

        let ext = opts.ext_filter.map(|e| e.to_lowercase());

        (0..self.frns.len())
            .into_par_iter()
            .filter(|&idx| {
                let flags = self.flags[idx];

                // Attribute filters
                let is_dir    = flags & 0x0010 != 0;
                let is_hidden = flags & 0x0002 != 0;
                let is_system = flags & 0x0004 != 0;

                if is_dir && !opts.include_dirs { return false; }
                if is_hidden && !opts.include_hidden { return false; }
                if is_system && !opts.include_system { return false; }

                // Get name bytes from pool
                let offset = self.name_offsets[idx] as usize;
                if offset >= self.string_pool.len() { return false; }
                let name_bytes = &self.string_pool[offset..];
                let end = name_bytes.iter().position(|&b| b == 0).unwrap_or(name_bytes.len());
                let name_bytes = &name_bytes[..end];

                // Extension filter
                if let Some(ref ext_filter) = ext {
                    let dot_pos = name_bytes.iter().rposition(|&b| b == b'.');
                    let name_ext = if let Some(p) = dot_pos {
                        &name_bytes[p+1..]
                    } else {
                        &[]
                    };
                    
                    if name_ext.len() != ext_filter.len() { return false; }
                    if !name_ext.iter().zip(ext_filter.as_bytes().iter())
                        .all(|(&a, &b)| a.to_ascii_lowercase() == b) {
                        return false;
                    }
                }

                // Name query
                if !opts.query.is_empty() {
                    if let Some(ref re) = regex_pattern {
                        if !re.is_match(std::str::from_utf8(name_bytes).unwrap_or("")) { 
                            return false; 
                        }
                    } else if let Some(ref f) = finder {
                        if !contains_ignore_case(name_bytes, f.needle()) {
                            return false;
                        }
                    }
                }

                true
            })
            .collect()
    }

    pub fn search_prefix<'b>(
        &self,
        prefix: &str,
        sorted_idx: &'b [u32],
    ) -> &'b [u32] {
        if prefix.is_empty() { return sorted_idx; }
        let prefix_lower = prefix.to_lowercase();
        let prefix_bytes = prefix_lower.as_bytes();

        let lo = sorted_idx.partition_point(|&idx| {
            let offset = self.name_offsets[idx as usize] as usize;
            let pool = &self.string_pool[offset..];
            let end = pool.iter().position(|&b| b == 0).unwrap_or(pool.len());
            let name_bytes = &pool[..end];
            compare_bytes_ignore_case(name_bytes, prefix_bytes) < 0
        });

        let hi = sorted_idx.partition_point(|&idx| {
            let offset = self.name_offsets[idx as usize] as usize;
            let pool = &self.string_pool[offset..];
            let end = pool.iter().position(|&b| b == 0).unwrap_or(pool.len());
            let name_bytes = &pool[..end];
            let cmp = compare_bytes_ignore_case(name_bytes, prefix_bytes);
            cmp < 0 || starts_with_ignore_case(name_bytes, prefix_bytes)
        });

        &sorted_idx[lo..hi]
    }

    pub fn search_with_ext(&self, query: &str, ext: Option<&str>) -> Vec<usize> {
        let opts = SearchOptions {
            query,
            ext_filter: ext,
            ..Default::default()
        };
        self.search(&opts)
    }
}

#[inline]
fn contains_ignore_case(haystack: &[u8], needle_lower: &[u8]) -> bool {
    if needle_lower.is_empty() { return true; }
    if haystack.len() < needle_lower.len() { return false; }

    for i in 0..=(haystack.len() - needle_lower.len()) {
        let mut matched = true;
        for j in 0..needle_lower.len() {
            if haystack[i + j].to_ascii_lowercase() != needle_lower[j] {
                matched = false;
                break;
            }
        }
        if matched { return true; }
    }
    false
}

fn compare_bytes_ignore_case(a: &[u8], b: &[u8]) -> i32 {
    let len = a.len().min(b.len());
    for i in 0..len {
        let ca = a[i].to_ascii_lowercase();
        let cb = b[i].to_ascii_lowercase();
        if ca < cb { return -1; }
        if ca > cb { return 1; }
    }
    if a.len() < b.len() { return -1; }
    if a.len() > b.len() { return 1; }
    0
}

fn starts_with_ignore_case(haystack: &[u8], prefix: &[u8]) -> bool {
    if haystack.len() < prefix.len() { return false; }
    haystack[..prefix.len()].iter().zip(prefix.iter())
        .all(|(&a, &b)| a.to_ascii_lowercase() == b.to_ascii_lowercase())
}
