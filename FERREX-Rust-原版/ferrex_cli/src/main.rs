use clap::{Parser, Subcommand};
use indexer::{acquire_privileges, get_ntfs_volumes, MftScanner};
use storage::{save_index, MappedIndex, IndexStore};
use search::Searcher;
use std::path::Path;
use std::sync::Arc;

#[derive(Parser)]
#[command(name = "ferrex")]
#[command(about = "Ferrex - High Performance NTFS Indexer", long_about = None)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Force full MFT scan and save index (.idx)
    Index {
        /// Drive to index (e.g., C). If omitted, all NTFS drives are indexed.
        drive: Option<String>,
    },
    /// Load index via mmap and search for substring
    Search {
        /// Search query
        query: String,
    },
    /// Monitor USN journal for incremental updates
    Watch,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse();

    match &cli.command {
        Commands::Index { drive } => {
            acquire_privileges()?;
            let volumes: Vec<String> = if let Some(d) = drive {
                vec![format!("{}:", d.to_uppercase().replace(":", ""))]
            } else {
                get_ntfs_volumes().into_iter().map(|(v, _)| v).collect()
            };

            for vol in volumes {
                println!("Scanning volume {}...", vol);
                let scanner = MftScanner::new(&vol)?;
                let raw_entries = scanner.scan()?;
                println!("Found {} entries", raw_entries.len());

                let mut store = IndexStore::new();
                for entry in raw_entries {
                    let name_offset = store.string_pool.len() as u32;
                    store.string_pool.extend_from_slice(entry.name.as_bytes());
                    store.string_pool.push(0); 

                    store.frns.push(entry.frn);
                    store.parent_frns.push(entry.parent_frn);
                    store.sizes.push(entry.file_size);
                    store.timestamps.push(entry.modified);
                    store.name_offsets.push(name_offset);
                    store.flags.push(entry.flags);
                }

                let idx_path = format!("{}_drive.idx", vol.chars().next().unwrap().to_lowercase());
                save_index(&idx_path, &store)?;
                println!("Successfully saved index to {}", idx_path);
            }
        }
        Commands::Search { query } => {
            let drives = vec!['c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'];
            let mut loaded_stores = Vec::new();

            for d in drives {
                let idx_path = format!("{}_drive.idx", d);
                if Path::new(&idx_path).exists() {
                    if let Ok(mapped) = MappedIndex::load(&idx_path) {
                        println!("Loaded index: {} ({} records)", idx_path, mapped.record_count);
                        let mut store = IndexStore::from_mapped(&mapped);
                        store.rebuild_lookups();
                        loaded_stores.push((d.to_uppercase().to_string(), Arc::new(store)));
                    }
                }
            }

            if loaded_stores.is_empty() {
                println!("Error: No index files found. Please run 'ferrex index' first.");
                return Ok(());
            }

            for (drive_letter, store) in loaded_stores {
                let searcher = Searcher::new(
                    &store.frns, &store.parent_frns, &store.sizes, &store.timestamps,
                    &store.flags, &store.name_offsets, &store.string_pool
                );
                let matches = searcher.search_with_ext(query, None);

                for idx in matches {
                    let path = store.get_path(idx);
                    let name_offset = store.name_offsets[idx] as usize;
                    let name = storage::pool_get_name(&store.string_pool, name_offset);
                    println!("{}:\\{}\\{}", drive_letter, path, name);
                }
            }
        }
        Commands::Watch => {
            println!("USN Watcher mode not fully implemented in this CLI version. Use GUI or check back later.");
        }
    }

    Ok(())
}
