#![windows_subsystem = "windows"]

use eframe::egui;
use usvg::TreeParsing;
use egui::{
    Color32, RichText, FontId, FontFamily, Pos2, Vec2, Margin, Frame, Sense, Layout, 
    Align, Align2, Stroke, Rounding, Image, TextEdit, ScrollArea, 
    ViewportCommand, Direction
};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::collections::{HashSet, HashMap};
use serde::{Serialize, Deserialize};
use std::process::Command;
use std::time::{Instant, Duration};
use std::num::NonZeroUsize;

use storage::{IndexStore, LoadedIndex, MappedIndex, pool_get_name};
use search::{Searcher, SearchOptions};
use indexer::{UsnEvent, spawn_usn_watcher, get_ntfs_volumes};
use lru::LruCache;

#[cfg(windows)]
use windows::Win32::Foundation::HWND;
#[cfg(windows)]
use windows::Win32::UI::Input::KeyboardAndMouse::{RegisterHotKey, MOD_ALT};
#[cfg(windows)]
use windows::Win32::UI::Shell::{SHGetFileInfoW, SHGFI_ICON, SHGFI_SMALLICON, SHGFI_USEFILEATTRIBUTES, SHFILEINFOW, ShellExecuteExW, SHELLEXECUTEINFOW, SEE_MASK_INVOKEIDLIST};

const BG: Color32 = Color32::from_rgb(7, 9, 11);
const PANEL: Color32 = Color32::from_rgb(13, 16, 20);
const BG2: Color32 = Color32::from_rgb(17, 21, 25);
const BG3: Color32 = Color32::from_rgb(22, 27, 32);
const BORDER2: Color32 = Color32::from_rgb(37, 46, 55);
const ACCENT: Color32 = Color32::from_rgb(255, 140, 0);
const DANGER: Color32 = Color32::from_rgb(231, 76, 60);
const TEXT: Color32 = Color32::from_rgb(200, 212, 220);
const TEXT2: Color32 = Color32::from_rgb(122, 143, 158);
const TEXT3: Color32 = Color32::from_rgb(61, 80, 96);
const SUCCESS: Color32 = Color32::from_rgb(70, 180, 120);

const PAGE_SIZE: usize = 1_000_000;

#[derive(Serialize, Deserialize, Default)]
struct AppConfig {
    active_drives: HashSet<String>,
    default_drives: HashSet<String>,
    ignored_drives: HashSet<String>,
    #[serde(default)]
    query_history: Vec<String>,
    #[serde(default)]
    ext_history: Vec<String>,
}

impl AppConfig {
    fn load() -> Self {
        std::fs::read_to_string("ferrex_config.toml")
            .ok()
            .and_then(|s| toml::from_str(&s).ok())
            .unwrap_or_default()
    }

    fn save(&self) {
        if let Ok(s) = toml::to_string_pretty(self) {
            let _ = std::fs::write("ferrex_config.toml", s);
        }
    }
}

struct IconCache {
    textures: HashMap<String, egui::TextureHandle>,
}

impl IconCache {
    fn new() -> Self {
        Self { textures: HashMap::new() }
    }

    fn get_for_path(&mut self, ctx: &egui::Context, path: &str, is_dir: bool) -> egui::ImageSource<'static> {
        let ext = if is_dir { "folder".to_string() } else {
            path.rsplit('.').next().unwrap_or("").to_lowercase()
        };

        // 修复：EXE和ICO文件各自独立图标，不能仅用后缀作为缓存Key
        let cache_key = match ext.as_str() {
            "exe" | "ico" => path.to_lowercase(),
            _ => ext.clone(),
        };

        if let Some(texture) = self.textures.get(&cache_key) {
            return texture.into();
        }

        if let Some(texture) = self.load_system_icon(ctx, path, is_dir) {
            self.textures.insert(cache_key.clone(), texture.clone());
            return (&self.textures[&cache_key]).into();
        }

        egui::include_image!("../icons/FERREX.svg") // Fallback
    }

    fn load_system_icon(&self, ctx: &egui::Context, path: &str, is_dir: bool) -> Option<egui::TextureHandle> {
        #[cfg(windows)]
        unsafe {
            use windows::core::HSTRING;
            use windows::Win32::Graphics::Gdi::{GetDIBits, BITMAPINFO, DIB_RGB_COLORS, GetDC, ReleaseDC, DeleteObject};
            use windows::Win32::UI::WindowsAndMessaging::{DestroyIcon, GetIconInfo};
            let mut shfi = SHFILEINFOW::default();
            let flags = SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES;
            let attr = if is_dir { windows::Win32::Storage::FileSystem::FILE_ATTRIBUTE_DIRECTORY.0 } else { windows::Win32::Storage::FileSystem::FILE_ATTRIBUTE_NORMAL.0 };
            
            SHGetFileInfoW(
                &HSTRING::from(path),
                windows::Win32::Storage::FileSystem::FILE_FLAGS_AND_ATTRIBUTES(attr),
                Some(&mut shfi),
                std::mem::size_of::<SHFILEINFOW>() as u32,
                flags,
            );

            if shfi.hIcon.is_invalid() { return None; }

            let mut icon_info = windows::Win32::UI::WindowsAndMessaging::ICONINFO::default();
            if GetIconInfo(shfi.hIcon, &mut icon_info).is_err() {
                let _ = DestroyIcon(shfi.hIcon);
                return None;
            }

            let h_dc = GetDC(HWND(0));
            let mut bmi = BITMAPINFO::default();
            bmi.bmiHeader.biSize = std::mem::size_of::<windows::Win32::Graphics::Gdi::BITMAPINFOHEADER>() as u32;
            bmi.bmiHeader.biWidth = 16;
            bmi.bmiHeader.biHeight = -16; // Top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = windows::Win32::Graphics::Gdi::BI_RGB.0;

            let mut pixels = vec![0u8; 16 * 16 * 4];
            let _ = GetDIBits(h_dc, icon_info.hbmColor, 0, 16, Some(pixels.as_mut_ptr() as *mut _), &mut bmi, DIB_RGB_COLORS);
            
            let _ = ReleaseDC(HWND(0), h_dc);
            let _ = DeleteObject(icon_info.hbmColor);
            let _ = DeleteObject(icon_info.hbmMask);
            let _ = DestroyIcon(shfi.hIcon);

            // BGRA to RGBA
            for i in 0..(pixels.len() / 4) {
                pixels.swap(i * 4, i * 4 + 2);
            }

            let color_image = egui::ColorImage::from_rgba_unmultiplied([16, 16], &pixels);
            Some(ctx.load_texture(format!("icon_{}", path), color_image, Default::default()))
        }
        #[cfg(not(windows))]
        { let _ = (ctx, path, is_dir); None }
    }
}

struct OverlayEntry {
    name: String,
    parent_frn: u64,
    flags: u32,
    is_deleted: bool,
}

impl OverlayEntry {
    fn matches(&self, opts: &SearchOptions) -> bool {
        if self.name.is_empty() { return false; }
        let is_dir = self.flags & 0x10 != 0;
        if is_dir && !opts.include_dirs { return false; }
        if let Some(ext) = opts.ext_filter {
            if !self.name.to_lowercase().ends_with(&format!(".{}", ext.to_lowercase())) {
                return false;
            }
        }
        if !opts.query.is_empty() {
            if !self.name.to_lowercase().contains(&opts.query.to_lowercase()) {
                return false;
            }
        }
        true
    }
}

struct VolumeStore {
    drive: String,
    name: String,
    index: Arc<LoadedIndex>,
    frn_map: HashMap<u64, u32>,
    path_cache: LruCache<u64, String>,
    usn_rx: Option<std::sync::mpsc::Receiver<UsnEvent>>,
    record_count: usize,
    overlay: HashMap<u64, OverlayEntry>,
}

impl VolumeStore {
    fn resolve_live_path(&mut self, frn: u64) -> String {
        if let Some(cached) = self.path_cache.get(&frn) {
            return cached.clone();
        }

        let mut parts = Vec::new();
        let mut current_frn = frn;

        loop {
            let (name, parent) = if let Some(entry) = self.overlay.get(&current_frn) {
                if entry.is_deleted { break; }
                (entry.name.clone(), entry.parent_frn)
            } else if let Some(&idx) = self.frn_map.get(&current_frn) {
                let n = pool_get_name(&self.index.string_pool, self.index.name_offsets[idx as usize] as usize);
                (n, self.index.parent_frns[idx as usize])
            } else {
                break;
            };

            // 修复：排除根目录名字 (FRN 为 5 时，名字是一个点 .)，避免路径出现 C:\.\xxx
            if current_frn != 5 {
                parts.push(name);
            }

            if current_frn == 5 || parent == 0 || current_frn == parent {
                break;
            }
            current_frn = parent;
        }

        parts.reverse();
        let path = format!("{}\\{}", self.drive, parts.join("\\"));
        self.path_cache.put(frn, path.clone());
        path
    }
}

#[derive(Clone)]
struct SearchResult {
    drive: String,
    name: String,
    full_path: String,
    size: u64,
    timestamp: u64,
    is_dir: bool,
}

#[derive(PartialEq, Clone, Copy)]
enum SortColumn { Name, Path, Size, Date }

enum ScanProgress {
    Progress { done: usize, total: usize },
    Done,
    Error(String),
}

struct FerrexApp {
    stores: Vec<VolumeStore>,
    config: AppConfig,
    hardware_ok: Arc<AtomicBool>,
    query: String,
    ext_filter: String,
    results: Vec<SearchResult>,
    selected_rows: HashSet<usize>,
    last_clicked_row: Option<usize>,
    last_search_ms: f64,
    sort_col: SortColumn,
    sort_asc: bool,
    icons: IconCache,
    status_text: String,
    is_scanning: bool,
    scan_progress: f32,
    scan_tx: std::sync::mpsc::Sender<ScanProgress>,
    scan_rx: std::sync::mpsc::Receiver<ScanProgress>,
    active_scan_count: usize,
    current_page: usize,
    #[cfg(windows)]
    tray: Option<tray_icon::TrayIcon>,
    should_exit: Arc<AtomicBool>,
    is_pinned: bool,
    editing_idx: Option<usize>,
    edit_buffer: String,
    edit_ext: String,
    just_started_editing: bool,
}

impl FerrexApp {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        setup_fonts(&cc.egui_ctx);
        egui_extras::install_image_loaders(&cc.egui_ctx);

        let mut visuals = egui::Visuals::dark();
        visuals.window_fill = PANEL;
        visuals.panel_fill = PANEL;
        visuals.widgets.active.bg_fill = BG3;
        visuals.widgets.hovered.bg_fill = BG3;
        visuals.widgets.noninteractive.bg_fill = PANEL;
        visuals.menu_rounding = Rounding::same(4.0);
        cc.egui_ctx.set_visuals(visuals);
        
        let _ = indexer::acquire_privileges();
        let config = AppConfig::load();
        let (scan_tx, scan_rx) = std::sync::mpsc::channel();

        let hardware_ok = Arc::new(AtomicBool::new(false));
        let hardware_ok_clone = hardware_ok.clone();

        let mut app = Self {
            stores: Vec::new(),
            config,
            hardware_ok,
            query: String::new(),
            ext_filter: String::new(),
            results: Vec::new(),
            selected_rows: HashSet::new(),
            last_clicked_row: None,
            last_search_ms: 0.0,
            sort_col: SortColumn::Name,
            sort_asc: true,
            icons: IconCache::new(),
            status_text: "正在校验硬件...".to_string(),
            is_scanning: false,
            scan_progress: 0.0,
            scan_tx,
            scan_rx,
            active_scan_count: 0,
            current_page: 0,
            #[cfg(windows)]
            tray: None,
            should_exit: Arc::new(AtomicBool::new(false)),
            is_pinned: false,
            editing_idx: None,
            edit_buffer: String::new(),
            edit_ext: String::new(),
            just_started_editing: false,
        };

        let ctx = cc.egui_ctx.clone();
        std::thread::spawn(move || {
            if verify_hardware_wmi_static() {
                hardware_ok_clone.store(true, Ordering::SeqCst);
            } else {
                // 如果需要记录校验失败的状态，可以在这里处理
            }
            ctx.request_repaint();
        });

        app.load_volumes();
        #[cfg(windows)]
        app.setup_tray(cc.egui_ctx.clone());

        app
    }


    fn load_volumes(&mut self) {
        let volumes = get_ntfs_volumes();
        
        if self.config.active_drives.is_empty() && !volumes.is_empty() {
            for (vol, _) in &volumes {
                if !self.config.ignored_drives.contains(vol) {
                    self.config.active_drives.insert(vol.clone());
                }
            }
            if self.config.default_drives.is_empty() && !volumes.is_empty() {
                self.config.default_drives.insert(volumes[0].0.clone());
            }
            self.config.save();
        }

        for (vol, name) in volumes {
            if self.config.ignored_drives.contains(&vol) { continue; }
            if let Some(store) = self.stores.iter_mut().find(|s| s.drive == vol) {
                store.name = name;
                continue;
            }
            
            let drive_letter = vol.chars().next().unwrap_or('c').to_lowercase().to_string();
            let idx_path = format!("{}_drive.idx", drive_letter);
            
            if let Ok(mapped) = MappedIndex::load(&idx_path) {
                let store = IndexStore::from_mapped(&mapped);
                let frn_map = store.build_frn_map();
                let usn_rx = spawn_usn_watcher(&vol).ok();
                
                self.stores.push(VolumeStore {
                    drive: vol,
                    name,
                    record_count: store.frns.len(),
                    index: Arc::new(store),
                    frn_map,
                    path_cache: LruCache::new(NonZeroUsize::new(10000).expect("Capacity is non-zero")),
                    usn_rx,
                    overlay: HashMap::new(),
                });
            } else {
                self.is_scanning = true;
                self.active_scan_count += 1;
                spawn_scan(vol, self.scan_tx.clone());
            }
        }
    }

    #[cfg(windows)]
    fn setup_tray(&mut self, ctx: egui::Context) {
        use tray_icon::menu::{Menu, MenuItem, MenuId, MenuEvent};
        use tray_icon::TrayIconEvent;
        let should_exit = self.should_exit.clone();
        let tray_menu = Menu::new();
        let show_id = MenuId::new("show_main");
        let quit_id = MenuId::new("quit");
        let show_i = MenuItem::with_id(show_id.clone(), "打开主界面", true, None);
        let quit_i = MenuItem::with_id(quit_id.clone(), "退出 FERREX", true, None);
        let _ = tray_menu.append_items(&[&show_i, &quit_i]);

        if let Some(icon) = load_icon() {
            let tray = tray_icon::TrayIconBuilder::new()
                .with_tooltip("FERREX")
                .with_icon(icon)
                .with_menu(Box::new(tray_menu))         
                .with_menu_on_left_click(false)         
                .build()
                .ok();
            
            self.tray = tray;
            
            std::thread::spawn(move || {
                #[cfg(windows)]
                unsafe {
                    let _ = RegisterHotKey(HWND(0), 1001, MOD_ALT, 0x20);
                }

                let menu_rx = MenuEvent::receiver();
                let tray_rx = TrayIconEvent::receiver();
                loop {
                    while let Ok(event) = menu_rx.try_recv() {
                        if event.id == show_id {
                            ctx.send_viewport_cmd(ViewportCommand::Visible(true));
                            ctx.send_viewport_cmd(ViewportCommand::Focus);
                            ctx.request_repaint();
                        } else if event.id == quit_id {
                            should_exit.store(true, Ordering::SeqCst);
                            ctx.send_viewport_cmd(ViewportCommand::Close);
                            ctx.request_repaint();
                            std::thread::spawn(|| {
                                std::thread::sleep(std::time::Duration::from_secs(1));
                                std::process::exit(0);
                            });
                        }
                    }
                    while let Ok(event) = tray_rx.try_recv() {
                        use tray_icon::{TrayIconEvent, MouseButton};
                        match event {
                            TrayIconEvent::Click { button: MouseButton::Left, .. } => {
                                ctx.send_viewport_cmd(ViewportCommand::Visible(true));
                                ctx.send_viewport_cmd(ViewportCommand::Focus);
                                ctx.request_repaint();
                            }
                            _ => {}
                        }
                    }

                    #[cfg(windows)]
                    unsafe {
                        use windows::Win32::UI::WindowsAndMessaging::{PeekMessageW, PM_REMOVE, MSG, WM_HOTKEY};
                        let mut msg = MSG::default();
                        while PeekMessageW(&mut msg, HWND(0), WM_HOTKEY, WM_HOTKEY, PM_REMOVE).as_bool() {
                            if msg.wParam.0 == 1001 {
                                ctx.send_viewport_cmd(ViewportCommand::Visible(true));
                                ctx.send_viewport_cmd(ViewportCommand::Focus);
                                ctx.request_repaint();
                            }
                        }
                    }

                    std::thread::sleep(std::time::Duration::from_millis(16));
                }
            });
        }
    }

    fn process_usn_events(&mut self) {
        for store in &mut self.stores {
            if let Some(ref rx) = store.usn_rx {
                while let Ok(event) = rx.try_recv() {
                    match event {
                        UsnEvent::Created { frn, parent_frn, name, flags } => {
                            store.overlay.insert(frn, OverlayEntry { name, parent_frn, flags, is_deleted: false });
                            store.path_cache.pop(&frn);
                        }
                        UsnEvent::Deleted { frn } => {
                            if let Some(entry) = store.overlay.get_mut(&frn) {
                                entry.is_deleted = true;
                            } else {
                                store.overlay.insert(frn, OverlayEntry { 
                                    name: String::new(), parent_frn: 0, flags: 0, is_deleted: true 
                                });
                            }
                            store.path_cache.pop(&frn);
                        }
                        UsnEvent::Renamed { old_frn, new_name, new_parent_frn } => {
                            let flags = if let Some(entry) = store.overlay.get(&old_frn) {
                                entry.flags
                            } else if let Some(&idx) = store.frn_map.get(&old_frn) {
                                store.index.flags[idx as usize]
                            } else { 0 };

                            store.overlay.insert(old_frn, OverlayEntry { 
                                name: new_name, 
                                parent_frn: new_parent_frn, 
                                flags, 
                                is_deleted: false 
                            });
                            store.path_cache.clear();
                        }
                        UsnEvent::Modified { frn } => {
                            store.path_cache.pop(&frn);
                        }
                    }
                }
            }
        }
    }

    fn run_search(&mut self, _ctx: &egui::Context) {
        if self.query.trim().is_empty() && self.ext_filter.trim().is_empty() {
            self.results.clear();
            self.current_page = 0;
            self.selected_rows.clear();
            self.last_clicked_row = None;
            self.last_search_ms = 0.0;
            return;
        }

        let t0 = Instant::now();
        let opts = SearchOptions {
            query: &self.query,
            use_regex: false,
            ext_filter: if self.ext_filter.is_empty() { None } else { Some(&self.ext_filter) },
            include_dirs: true,
            include_hidden: false,
            include_system: false,
        };

        let mut all_results = Vec::new();
        for store in self.stores.iter_mut() {
            if !self.config.active_drives.contains(&store.drive) { continue; }
            
            let searcher = Searcher::new(
                &store.index.frns, &store.index.parent_frns, &store.index.sizes, 
                &store.index.timestamps, &store.index.flags, &store.index.name_offsets, 
                &store.index.string_pool
            );
            let matches = searcher.search(&opts);
            for rec_idx in matches {
                let frn = store.index.frns[rec_idx];
                if store.overlay.contains_key(&frn) { continue; }
                let name = pool_get_name(&store.index.string_pool, store.index.name_offsets[rec_idx] as usize);
                let full_path = store.resolve_live_path(frn);
                let is_dir = store.index.flags[rec_idx] & 0x10 != 0;
                
                let mut size = store.index.sizes[rec_idx];
                #[allow(unused_mut)]
                let mut timestamp = store.index.timestamps[rec_idx];

                // 核心修复 1：针对底层 MFT 扫描遗漏大小的情况（0字节），执行即时元数据补偿
                if !is_dir && size == 0 {
                    if let Ok(meta) = std::fs::metadata(&full_path) {
                        size = meta.len();
                        #[cfg(windows)]
                        {
                            use std::os::windows::fs::MetadataExt;
                            timestamp = meta.last_write_time(); // 同步修正精确的修改时间
                        }
                    }
                }

                all_results.push(SearchResult {
                    drive: store.drive.clone(),
                    name,
                    full_path,
                    size,
                    timestamp,
                    is_dir,
                });
            }

            let candidates: Vec<(u64, String, u32)> = store.overlay.iter()
                .filter(|(_, e)| !e.is_deleted && e.matches(&opts))
                .map(|(&f, e)| (f, e.name.clone(), e.flags))
                .collect();

            for (frn, name, flags) in candidates {
                let full_path = store.resolve_live_path(frn);
                let is_dir = flags & 0x10 != 0;
                
                let mut size = 0;
                
                // 修复：建立兼容 Windows FILETIME 标准的当前时间戳兜底 (Epoch 为 1601年)
                #[allow(unused_mut)]
                let mut timestamp = match std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH) {
                    Ok(d) => (d.as_secs() + 11644473600) * 10_000_000,
                    Err(_) => 0,
                };

                // 核心修复 2：动态向系统索要 USN 覆盖层（新产生/修改过）的真实大小
                if let Ok(meta) = std::fs::metadata(&full_path) {
                    if !is_dir {
                        size = meta.len();
                    }
                    #[cfg(windows)]
                    {
                        use std::os::windows::fs::MetadataExt;
                        timestamp = meta.last_write_time();
                    }
                }

                all_results.push(SearchResult {
                    drive: store.drive.clone(),
                    name,
                    full_path,
                    size, 
                    timestamp, 
                    is_dir,
                });
            }
        }

        self.results = all_results;
        self.current_page = 0;
        self.apply_sort();
        
        // 修复：每次搜索后清理选中状态，防止幽灵越界访问
        self.selected_rows.clear();
        self.last_clicked_row = None;
        
        self.last_search_ms = t0.elapsed().as_secs_f64() * 1000.0;
    }

    fn apply_sort(&mut self) {
        let asc = self.sort_asc;
        match self.sort_col {
            SortColumn::Name => self.results.sort_by(|a, b| { let c = cmp_ignore_ascii_case(&a.name, &b.name); if asc { c } else { c.reverse() } }),
            SortColumn::Path => self.results.sort_by(|a, b| { let c = cmp_ignore_ascii_case(&a.full_path, &b.full_path); if asc { c } else { c.reverse() } }),
            SortColumn::Size => self.results.sort_by(|a, b| { let c = a.size.cmp(&b.size); if asc { c } else { c.reverse() } }),
            SortColumn::Date => self.results.sort_by(|a, b| { let c = a.timestamp.cmp(&b.timestamp); if asc { c } else { c.reverse() } }),
        }
    }

    fn export_selected_csv(&self) {
        #[cfg(target_os = "windows")]
        {
            let path = rfd::FileDialog::new()
                .set_file_name("ferrex_export.csv")
                .add_filter("CSV", &["csv"])
                .save_file();
            if let Some(p) = path {
                let mut content = String::from("名称,路径,大小(字节),修改时间,类型\n");
                for &idx in &self.selected_rows {
                    if let Some(r) = self.results.get(idx) {
                        content.push_str(&format!("\"{}\",\"{}\",{},{},{}\n",
                            r.name, r.full_path, r.size,
                            format_timestamp(r.timestamp),
                            if r.is_dir { "目录" } else { "文件" }));
                    }
                }
                let _ = std::fs::write(p, content);
            }
        }
    }
}

// 移除 Drop 逻辑，因为热键释放已经在线程内完成，强行在 Drop 中调用会导致异常

impl eframe::App for FerrexApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        // 修复：强制 200ms 刷新一次，确保 USN 积压数据能被及时抽出，防止 OOM 假死
        ctx.request_repaint_after(Duration::from_millis(200));

        if !self.hardware_ok.load(Ordering::SeqCst) {
            self.show_lock_screen(ctx);
            return;
        }

        self.process_usn_events();
        self.handle_scan_progress(ctx);

        if ctx.input(|i| i.viewport().close_requested()) {
            if self.should_exit.load(Ordering::SeqCst) {
            } else {
                ctx.send_viewport_cmd(ViewportCommand::CancelClose);
                ctx.send_viewport_cmd(ViewportCommand::Visible(false));
            }
        }

        egui::TopBottomPanel::top("titlebar")
            .exact_height(32.0)
            .frame(Frame::none().fill(PANEL).inner_margin(Margin::symmetric(8.0, 0.0)))
            .show(ctx, |ui| { self.draw_titlebar(ui); });

        egui::TopBottomPanel::top("drives")
            .exact_height(40.0)
            .frame(Frame::none().fill(BG2).inner_margin(Margin::symmetric(16.0, 0.0)))
            .show(ctx, |ui| { self.draw_drive_selector(ui, ctx); });

        egui::TopBottomPanel::top("searchbar_area")
            .exact_height(if self.is_scanning { 52.0 } else { 50.0 })
            .frame(Frame::none().fill(PANEL))
            .show(ctx, |ui| {
                Frame::none().inner_margin(Margin::symmetric(5.0, 8.0)).show(ui, |ui| {
                    self.draw_search_bar(ui, ctx);
                });
                if self.is_scanning { 
                    ui.add(egui::ProgressBar::new(self.scan_progress)
                        .text(RichText::new(&self.status_text).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(ACCENT))
                        .fill(ACCENT).desired_height(2.0).rounding(Rounding::ZERO)); 
                }
            });

        egui::TopBottomPanel::bottom("statusbar")
            .exact_height(26.0)
            .frame(Frame::none().fill(PANEL).inner_margin(Margin::symmetric(16.0, 0.0)))
            .show(ctx, |ui| { self.draw_status_bar(ui); });

        egui::CentralPanel::default().frame(Frame::none().fill(BG)).show(ctx, |ui| {
            ui.with_layout(Layout::top_down(Align::Min), |ui| {
                self.draw_column_header(ui);
                self.draw_results_list(ui);
            });
        });
    }
}

impl FerrexApp {
    fn show_lock_screen(&self, ctx: &egui::Context) {
        egui::CentralPanel::default().frame(Frame::none().fill(BG)).show(ctx, |ui| {
            ui.vertical_centered(|ui| {
                ui.add_space(ui.available_height() / 2.0 - 100.0);
                ui.add(egui::Image::new(egui::include_image!("../icons/warning.svg")).max_size(Vec2::splat(64.0)).tint(DANGER));
                ui.add_space(20.0);
                ui.label(RichText::new("未授权的硬件设备").font(FontId::new(24.0, FontFamily::Name("cond".into()))).color(DANGER).strong());
                ui.add_space(8.0);
                ui.label(RichText::new("请联系开发者以获取授权").font(FontId::new(14.0, FontFamily::Name("mono".into()))).color(TEXT3));
            });
        });
    }

    fn draw_titlebar(&mut self, ui: &mut egui::Ui) {
        ui.horizontal_centered(|ui| {
            ui.add_space(5.0);
            ui.add(egui::Image::new(egui::include_image!("../icons/FERREX.svg")).max_size(Vec2::new(18.0, 18.0)));
            ui.add_space(10.0);
            ui.label(RichText::new("FERREX").font(FontId::new(14.0, FontFamily::Name("cond".into()))).color(ACCENT).extra_letter_spacing(1.5));
            ui.add_space(12.0);
            
            let total_records: usize = self.stores.iter().map(|s| s.record_count).sum();
            ui.label(RichText::new(format!("READY - {}", format_number(total_records))).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(SUCCESS));

            let drag_rect = ui.available_rect_before_wrap();
            let drag_response = ui.interact(drag_rect, ui.id().with("drag"), Sense::click_and_drag());
            if drag_response.drag_started_by(egui::PointerButton::Primary) {
                ui.ctx().send_viewport_cmd(ViewportCommand::StartDrag);
            }

            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.spacing_mut().item_spacing.x = 0.0;
                let icon_normal = Color32::from_rgb(238, 238, 238);
                let close_response = self.draw_svg_button(
                    ui, 
                    egui::include_image!("../icons/close.svg"), 
                    Color32::TRANSPARENT, 
                    Color32::from_rgb(241, 112, 122), 
                    Color32::from_rgb(165, 0, 0),     
                    icon_normal,
                    Color32::WHITE
                );
                if close_response.clicked() {
                    ui.ctx().send_viewport_cmd(ViewportCommand::Close);
                }
                let is_max = ui.ctx().input(|i| i.viewport().maximized.unwrap_or(false));
                let max_img = if is_max { egui::include_image!("../icons/restore.svg") } else { egui::include_image!("../icons/maximize.svg") };
                let max_response = self.draw_svg_button(
                    ui, 
                    max_img, 
                    Color32::TRANSPARENT, 
                    Color32::from_rgba_unmultiplied(255, 255, 255, 25), 
                    Color32::from_rgba_unmultiplied(255, 255, 255, 51), 
                    icon_normal,
                    icon_normal
                );
                if max_response.clicked() {
                    ui.ctx().send_viewport_cmd(ViewportCommand::Maximized(!is_max));
                }
                let min_response = self.draw_svg_button(
                    ui, 
                    egui::include_image!("../icons/minimize.svg"), 
                    Color32::TRANSPARENT, 
                    Color32::from_rgba_unmultiplied(255, 255, 255, 25), 
                    Color32::from_rgba_unmultiplied(255, 255, 255, 51), 
                    icon_normal,
                    icon_normal
                );
                if min_response.clicked() {
                    ui.ctx().send_viewport_cmd(ViewportCommand::Minimized(true));
                }
                let pin_bg = if self.is_pinned { Color32::from_rgba_unmultiplied(255, 255, 255, 25) } else { Color32::TRANSPARENT };
                let pin_icon_normal = if self.is_pinned { Color32::from_rgb(255, 85, 28) } else { icon_normal };
                let pin_icon_hover = if self.is_pinned { Color32::from_rgb(255, 85, 28) } else { icon_normal };
                let pin_response = self.draw_svg_button(
                    ui, 
                    egui::include_image!("../icons/pin.svg"), 
                    pin_bg, 
                    Color32::from_rgba_unmultiplied(255, 255, 255, 25), 
                    Color32::from_rgba_unmultiplied(255, 255, 255, 51), 
                    pin_icon_normal,
                    pin_icon_hover
                );
                if pin_response.clicked() {
                    self.is_pinned = !self.is_pinned;
                    let level = if self.is_pinned { egui::WindowLevel::AlwaysOnTop } else { egui::WindowLevel::Normal };
                    ui.ctx().send_viewport_cmd(ViewportCommand::WindowLevel(level));
                }
            });
        });
    }

    fn draw_svg_button(
        &self, 
        ui: &mut egui::Ui, 
        img_source: egui::ImageSource<'static>, 
        base_bg: Color32, 
        hover_bg: Color32, 
        press_bg: Color32, 
        normal_tint: Color32,
        hover_tint: Color32,
    ) -> egui::Response {
        let (rect, response) = ui.allocate_exact_size(Vec2::splat(24.0), Sense::click());
        let fill = if response.is_pointer_button_down_on() { press_bg } 
                   else if response.hovered() { hover_bg } 
                   else { base_bg };
        if fill != Color32::TRANSPARENT {
            ui.painter().rect_filled(rect, Rounding::same(4.0), fill);
        }
        let tint = if response.hovered() || response.is_pointer_button_down_on() { hover_tint } else { normal_tint };
        let img = egui::Image::new(img_source).max_size(Vec2::splat(14.0)).tint(tint);
        let mut child_ui = ui.child_ui(rect, Layout::centered_and_justified(Direction::LeftToRight), None);
        child_ui.add(img);
        response
    }

    fn draw_drive_selector(&mut self, ui: &mut egui::Ui, ctx: &egui::Context) {
        let mut toggle_drive = None;
        let mut ignore_drive = None;
        ui.horizontal_centered(|ui| {
            ui.label(RichText::new("DRIVES").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
            ui.add_space(8.0);
            let draw_action_btn = |ui: &mut egui::Ui, text: &str| -> egui::Response {
                let (rect, response) = ui.allocate_exact_size(Vec2::new(32.0, 20.0), Sense::click());
                let color = if response.hovered() { ACCENT } else { TEXT3 };
                ui.painter().text(rect.center(), Align2::CENTER_CENTER, text, FontId::new(10.0, FontFamily::Name("cond".into())), color);
                response
            };
            let all_selected = !self.stores.is_empty() && self.stores.iter().all(|s| self.config.active_drives.contains(&s.drive));
            if draw_action_btn(ui, if all_selected { "全清" } else { "全选" }).clicked() {
                if all_selected {
                    self.config.active_drives.clear();
                    for def in &self.config.default_drives { self.config.active_drives.insert(def.clone()); }
                } else {
                    for store in &self.stores { self.config.active_drives.insert(store.drive.clone()); }
                }
                self.config.save();
                self.run_search(ctx);
            }
            ui.add_space(8.0);
            ScrollArea::horizontal().show(ui, |ui| {
                for store in &self.stores {
                    let is_active = self.config.active_drives.contains(&store.drive);
                    let is_default = self.config.default_drives.contains(&store.drive);
                    let label = if is_default { 
                        format!("★ {} ({})", store.drive, store.name) 
                    } else { 
                        format!("{} ({})", store.drive, store.name) 
                    };
                    let (bg, stroke_color, text_color) = if is_active { (Color32::from_rgba_unmultiplied(255, 140, 0, 30), ACCENT, ACCENT) } else { (BG3, BORDER2, TEXT2) };
                    let btn = egui::Button::new(RichText::new(&label).font(FontId::new(12.0, FontFamily::Name("cond".into()))).color(text_color)).fill(bg).stroke(Stroke::new(1.0, stroke_color)).rounding(Rounding::ZERO);
                    let response = ui.add(btn);
                    if response.clicked() { toggle_drive = Some((store.drive.clone(), is_active)); }
                    let drive = store.drive.clone();
                    response.context_menu(|ui| {
                        ui.set_min_width(140.0);
                        let is_default = self.config.default_drives.contains(&drive);
                        if menu_item(ui, if is_default { "取消默认选项" } else { "设为默认选项" }) {
                            if is_default { self.config.default_drives.remove(&drive); } else { self.config.default_drives.insert(drive.clone()); }
                            self.config.save(); ui.close_menu();
                        }
                        if menu_item(ui, "忽略此驱动器") { ignore_drive = Some(drive); ui.close_menu(); }
                    });
                    ui.add_space(4.0);
                }
            });

            let mut restore_drive = None;
            for ignored in &self.config.ignored_drives {
                let label = format!("{}: IGNORED", ignored);
                let btn = egui::Button::new(RichText::new(&label).font(FontId::new(12.0, FontFamily::Name("cond".into()))).color(TEXT3)).fill(Color32::TRANSPARENT).stroke(Stroke::new(1.0, TEXT3.gamma_multiply(0.2))).rounding(Rounding::ZERO);
                let response = ui.add(btn);
                let drive = ignored.clone();
                response.context_menu(|ui| {
                    ui.set_min_width(120.0);
                    if menu_item(ui, "恢复驱动器") { restore_drive = Some(drive); ui.close_menu(); }
                });
                ui.add_space(4.0);
            }
            if let Some(drive) = ignore_drive {
                self.config.ignored_drives.insert(drive.clone());
                self.config.active_drives.remove(&drive);
                self.config.default_drives.remove(&drive);
                self.config.save();
                self.stores.retain(|s| s.drive != drive);
                self.run_search(ctx);
            }
            if let Some(drive) = restore_drive {
                self.config.ignored_drives.remove(&drive);
                self.config.save();
                self.load_volumes();
                self.run_search(ctx);
            }
        });

        if let Some((drive, was_active)) = toggle_drive {
            if was_active && self.config.active_drives.len() > 1 { self.config.active_drives.remove(&drive); }
            else if !was_active { self.config.active_drives.insert(drive); }
            self.config.save();
            self.run_search(ctx);
        }
    }

    fn draw_search_bar(&mut self, ui: &mut egui::Ui, ctx: &egui::Context) {
        let query_pop_id = ui.id().with("query_pop");
        let ext_pop_id = ui.id().with("ext_pop");
        let query_hit_id = ui.id().with("query_hit");
        let ext_hit_id = ui.id().with("ext_hit");

        let frame = Frame::none().fill(BG2).stroke(Stroke::new(1.0, BORDER2)).rounding(Rounding::ZERO).inner_margin(Margin::symmetric(5.0, 0.0));
        frame.show(ui, |ui| {
            ui.spacing_mut().item_spacing.x = 0.0;
            ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
                // 1. 图标
                let (icon_rect, _) = ui.allocate_exact_size(Vec2::new(30.0, 34.0), Sense::hover());
                let mut icon_ui = ui.child_ui(icon_rect, Layout::centered_and_justified(Direction::LeftToRight), None);
                icon_ui.add(egui::Image::new(egui::include_image!("../icons/search.svg")).max_size(Vec2::splat(16.0)));
                
                let mut trigger_search = false;
                let mut ext_response_opt = None;
                let mut query_response_opt = None;

                // 2. 右侧对齐部分 (RTL)
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    ui.spacing_mut().item_spacing.x = 5.0;

                    // 最右侧：分页控件
                    let total_pages = (self.results.len() + PAGE_SIZE - 1) / PAGE_SIZE;
                    let total_pages = total_pages.max(1);
                    let next_btn = egui::Button::new(RichText::new(">").font(FontId::new(14.0, FontFamily::Name("mono".into()))).color(TEXT)).fill(BG3).rounding(Rounding::ZERO);
                    if ui.add_sized(Vec2::new(24.0, 24.0), next_btn).clicked() {
                        if self.current_page + 1 < total_pages { self.current_page += 1; ctx.request_repaint(); }
                    }
                    ui.label(RichText::new(format!("{}/{}", self.current_page + 1, total_pages)).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(TEXT2));
                    let prev_btn = egui::Button::new(RichText::new("<").font(FontId::new(14.0, FontFamily::Name("mono".into()))).color(TEXT)).fill(BG3).rounding(Rounding::ZERO);
                    if ui.add_sized(Vec2::new(24.0, 24.0), prev_btn).clicked() {
                        if self.current_page > 0 { self.current_page -= 1; ctx.request_repaint(); }
                    }

                    // 紧邻分页：搜索按钮
                    let search_btn = egui::Button::new(RichText::new("搜索").font(FontId::new(13.0, FontFamily::Name("cond".into()))).color(Color32::BLACK).strong()).fill(ACCENT).rounding(Rounding::ZERO);
                    if ui.add_sized(Vec2::new(80.0, 34.0), search_btn).clicked() {
                        trigger_search = true;
                    }

                    // 扩展名区域 (严格锁定 150 像素宽度)
                    ui.allocate_ui(Vec2::new(150.0, 34.0), |ui| {
                        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                            ui.spacing_mut().item_spacing.x = 0.0;
                            let (clear_rect, clear_resp) = ui.allocate_exact_size(Vec2::new(20.0, 34.0), Sense::click());
                            if !self.ext_filter.is_empty() {
                                let color = if clear_resp.hovered() { DANGER } else { TEXT3 };
                                ui.painter().text(clear_rect.center(), Align2::CENTER_CENTER, "×", FontId::new(14.0, FontFamily::Name("mono".into())), color);
                                if clear_resp.clicked() { self.ext_filter.clear(); self.run_search(ctx); }
                            }
                            let ext_edit = TextEdit::singleline(&mut self.ext_filter).font(FontId::new(13.0, FontFamily::Name("mono".into()))).hint_text(RichText::new("扩展名").color(TEXT3)).frame(false).margin(Margin::symmetric(4.0, 8.0)).text_color(TEXT);
                            let resp = ui.add_sized(Vec2::new(130.0, 34.0), ext_edit);
                            if ui.interact(resp.rect, ext_hit_id, Sense::click()).double_clicked() { ui.memory_mut(|m| m.open_popup(ext_pop_id)); }
                            ext_response_opt = Some(resp);
                        });
                    });

                    // 橙色分隔符 (紧贴扩展名区域)
                    ui.spacing_mut().item_spacing.x = 0.0;
                    let (dot_rect, _) = ui.allocate_exact_size(Vec2::new(24.0, 34.0), Sense::hover());
                    ui.painter().rect_filled(dot_rect, Rounding::ZERO, BG3);
                    ui.painter().text(dot_rect.center(), Align2::CENTER_CENTER, "|", FontId::new(14.0, FontFamily::Name("mono".into())), ACCENT);

                    // 3. 中间填充部分：主搜索输入框 (自适应宽度，消除空白)
                    ui.spacing_mut().item_spacing.x = 5.0;
                    let remaining_w = ui.available_width();
                    ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
                        ui.spacing_mut().item_spacing.x = 0.0;
                        let search_edit = TextEdit::singleline(&mut self.query).font(FontId::new(13.0, FontFamily::Name("mono".into()))).hint_text(RichText::new("文件名 / 关键词...").color(TEXT3)).frame(false).margin(Margin::symmetric(4.0, 8.0)).text_color(TEXT);
                        let search_response = ui.add_sized(Vec2::new(remaining_w - 20.0, 34.0), search_edit);
                        
                        let (rect, resp) = ui.allocate_exact_size(Vec2::new(20.0, 34.0), Sense::click());
                        if !self.query.is_empty() {
                            let color = if resp.hovered() { DANGER } else { TEXT3 };
                            ui.painter().text(rect.center(), Align2::CENTER_CENTER, "×", FontId::new(14.0, FontFamily::Name("mono".into())), color);
                            if resp.clicked() { self.query.clear(); self.run_search(ctx); }
                        }
                        
                        if ui.interact(search_response.rect, query_hit_id, Sense::click()).double_clicked() { ui.memory_mut(|m| m.open_popup(query_pop_id)); }
                        query_response_opt = Some(search_response);
                    });
                });

                let ext_response = ext_response_opt.expect("Extension response should be initialized");
                let search_response = query_response_opt.expect("Query response should be initialized");
                
                egui::popup_below_widget(ui, query_pop_id, &search_response, egui::PopupCloseBehavior::CloseOnClickOutside, |ui| { self.draw_history_popup_content(ui, ctx, true, search_response.rect.width()); });
                egui::popup_below_widget(ui, ext_pop_id, &ext_response, egui::PopupCloseBehavior::CloseOnClickOutside, |ui| { self.draw_history_popup_content(ui, ctx, false, ext_response.rect.width()); });

                if !trigger_search {
                    trigger_search = (search_response.has_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter))) || 
                                     (ext_response.has_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter)));
                }

                if trigger_search {
                    if !self.query.is_empty() { self.config.query_history.retain(|h| h != &self.query); self.config.query_history.insert(0, self.query.clone()); if self.config.query_history.len() > 10 { self.config.query_history.truncate(10); } }
                    if !self.ext_filter.is_empty() { self.config.ext_history.retain(|h| h != &self.ext_filter); self.config.ext_history.insert(0, self.ext_filter.clone()); if self.config.ext_history.len() > 10 { self.config.ext_history.truncate(10); } }
                    self.config.save(); self.run_search(ctx); 
                }
            });
        });
    }

    fn draw_history_popup_content(&mut self, ui: &mut egui::Ui, ctx: &egui::Context, is_query: bool, width: f32) {
        let mut history_to_clear = false;
        let mut to_remove = None;
        let mut selected_item = None;
        Frame::none().fill(PANEL).stroke(Stroke::new(1.0, BORDER2)).inner_margin(Margin::same(8.0)).show(ui, |ui| {
            ui.set_width(width);
            ui.horizontal(|ui| {
                ui.spacing_mut().item_spacing.x = 4.0;
                ui.add(egui::Image::new(egui::include_image!("../icons/restore.svg")).max_size(Vec2::splat(12.0)).tint(TEXT3));
                ui.label(RichText::new("搜索历史").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| { if ui.link(RichText::new("清空").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3)).clicked() { history_to_clear = true; } });
            });
            ui.add_space(6.0);
            let history = if is_query { &self.config.query_history } else { &self.config.ext_history };
            for (idx, item) in history.iter().enumerate() {
                let (rect, resp) = ui.allocate_exact_size(Vec2::new(ui.available_width(), 26.0), Sense::click());
                if resp.hovered() { ui.painter().rect_filled(rect, Rounding::same(2.0), BG3); }
                ui.painter().text(Pos2::new(rect.left() + 8.0, rect.center().y), Align2::LEFT_CENTER, item, FontId::new(12.0, FontFamily::Name("mono".into())), Color32::WHITE);
                let del_rect = egui::Rect::from_center_size(Pos2::new(rect.right() - 14.0, rect.center().y), Vec2::splat(18.0));
                let del_resp = ui.interact(del_rect, ui.id().with(idx), Sense::click());
                let del_color = if del_resp.hovered() { DANGER } else { TEXT3 };
                if del_resp.hovered() { ui.painter().rect_filled(del_rect, Rounding::same(2.0), Color32::from_rgba_unmultiplied(255, 255, 255, 10)); }
                ui.painter().text(del_rect.center(), Align2::CENTER_CENTER, "×", FontId::new(14.0, FontFamily::Name("mono".into())), del_color);
                if del_resp.clicked() { to_remove = Some(idx); } else if resp.clicked() { selected_item = Some(item.clone()); ui.memory_mut(|m| m.close_popup()); }
            }
        });
        let mut save_needed = false;
        {
            let history = if is_query { &mut self.config.query_history } else { &mut self.config.ext_history };
            if history_to_clear { history.clear(); save_needed = true; }
            if let Some(i) = to_remove { history.remove(i); save_needed = true; }
        }
        if save_needed { self.config.save(); }
        if let Some(item) = selected_item { if is_query { self.query = item; } else { self.ext_filter = item; } self.run_search(ctx); }
    }

    fn draw_column_header(&mut self, ui: &mut egui::Ui) {
        const ICON_W:  f32 = 14.0;
        const TAG_W:   f32 = 22.0;
        const NAME_W:  f32 = 260.0;
        const SIZE_W:  f32 = 80.0;
        const DATE_W:  f32 = 130.0;
        const PADDING: f32 = 8.0 + 8.0 + 6.0 + 16.0;
        let total_w = ui.available_width();
        let path_w  = (total_w - ICON_W - TAG_W - NAME_W - SIZE_W - DATE_W - PADDING).max(40.0);
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 0.0;
            ui.add_space(ICON_W + 8.0 + TAG_W + 6.0 + 8.0);
            if col_header_btn(ui, "名称", NAME_W, false) { self.sort_col = SortColumn::Name; self.sort_asc = !self.sort_asc; self.apply_sort(); }
            if col_header_btn(ui, "路径", path_w, false) { self.sort_col = SortColumn::Path; self.sort_asc = !self.sort_asc; self.apply_sort(); }
            if col_header_btn(ui, "大小", SIZE_W, true)  { self.sort_col = SortColumn::Size; self.sort_asc = !self.sort_asc; self.apply_sort(); }
            if col_header_btn(ui, "修改日期", DATE_W, true)  { self.sort_col = SortColumn::Date; self.sort_asc = !self.sort_asc; self.apply_sort(); }
        });
    }

    fn draw_results_list(&mut self, ui: &mut egui::Ui) {
        if self.results.is_empty() { self.draw_empty_state(ui); return; }
        let mut new_selected:    Option<usize> = None;
        let available_width  = ui.available_width();
        
        let start = self.current_page * PAGE_SIZE;
        let end = (start + PAGE_SIZE).min(self.results.len());
        if start >= self.results.len() { return; }
        let page_results = &self.results[start..end];

        const ICON_W:   f32 = 14.0;
        const TAG_W:    f32 = 22.0;
        const NAME_W:   f32 = 260.0;
        const SIZE_W:   f32 = 80.0;
        const DATE_W:   f32 = 130.0;
        const PADDING:  f32 = 8.0 + 8.0 + 6.0 + 16.0;
        let path_w = (available_width - ICON_W - TAG_W - NAME_W - SIZE_W - DATE_W - PADDING).max(40.0);

        ScrollArea::vertical().auto_shrink([false, false]).show_rows(ui, 30.0, page_results.len(), |ui, row_range| {
            ui.spacing_mut().item_spacing.y = 0.0;
            for row_idx in row_range {
                let idx = start + row_idx;
                let result = self.results[idx].clone();
                let (rect, response) = ui.allocate_exact_size(Vec2::new(available_width, 30.0), Sense::click());
                let is_hovered = response.hovered();
                let is_selected = self.selected_rows.contains(&idx);
                let bg = if is_hovered || is_selected { BG3 } else if idx % 2 == 0 { BG } else { BG2 };
                ui.painter().rect_filled(rect, Rounding::ZERO, bg);
                if response.double_clicked() { open_file(&result.full_path); } 
                else if response.clicked() { new_selected = Some(idx); }
                response.context_menu(|ui| {
                    ui.set_min_width(200.0);
                    let mut current_selection: Vec<usize> = self.selected_rows.iter().cloned().collect();
                    if !current_selection.contains(&idx) {
                        current_selection = vec![idx];
                    }

                    let paths: Vec<String> = current_selection.iter().filter_map(|&i| self.results.get(i)).map(|r| r.full_path.clone()).collect();
                    let names: Vec<String> = current_selection.iter().filter_map(|&i| self.results.get(i)).map(|r| r.name.clone()).collect();

                    if menu_item(ui, if paths.len() > 1 { "批量打开文件" } else { "打开文件" }) { 
                        batch_open_files(paths.clone()); 
                        ui.close_menu(); 
                    }
                    if menu_item(ui, "在“资源管理器”中显示") { 
                        reveal_in_explorer(&result.full_path); 
                        ui.close_menu(); 
                    }
                    if menu_item(ui, if paths.len() > 1 { "批量复制路径" } else { "复制路径" }) { 
                        ui.ctx().output_mut(|o| o.copied_text = paths.join("\n")); 
                        ui.close_menu(); 
                    }
                    if menu_item(ui, if names.len() > 1 { "批量复制文件名" } else { "复制文件名" }) { 
                        ui.ctx().output_mut(|o| o.copied_text = names.join("\n")); 
                        ui.close_menu(); 
                    }
                    
                    if paths.len() == 1 {
                        if menu_item(ui, "重命名") {
                            let path = std::path::Path::new(&result.full_path);
                            let stem = path.file_stem().and_then(|s| s.to_str()).unwrap_or(&result.name);
                            let ext = path.extension().and_then(|s| s.to_str()).map(|s| format!(".{}", s)).unwrap_or_default();
                            
                            self.editing_idx = Some(idx);
                            self.edit_buffer = stem.to_string();
                            self.edit_ext = ext;
                            self.just_started_editing = true;
                            ui.close_menu();
                        }
                    }

                    ui.separator();

                    if menu_item(ui, if paths.len() > 1 { "批量删除" } else { "删除" }) {
                        if delete_files(paths.clone()) {
                            // 简单的刷新：清空当前结果，提示用户重新搜索
                            // 实际上应该从 self.results 中移除，但由于是带索引的，比较复杂
                            self.run_search(ui.ctx()); 
                        }
                        ui.close_menu();
                    }

                    ui.separator();
                    if menu_item(ui, "属性") { open_properties(&result.full_path); ui.close_menu(); }
                });
                
                let y_center = rect.center().y;
                let mut x = rect.left() + 8.0;
                let icon_rect = egui::Rect::from_min_size(Pos2::new(x, y_center - 7.0), Vec2::new(ICON_W, ICON_W));
                let icon_src = self.icons.get_for_path(ui.ctx(), &result.full_path, result.is_dir);
                ui.child_ui(icon_rect, Layout::left_to_right(Align::Min), None).add(Image::new(icon_src).fit_to_exact_size(Vec2::new(ICON_W, ICON_W)));
                x += ICON_W + 8.0;
                let tag_rect = egui::Rect::from_min_size(Pos2::new(x, y_center - 7.0), Vec2::new(TAG_W, 14.0));
                ui.painter().rect_stroke(tag_rect, 1.0, Stroke::new(1.0, BORDER2));
                ui.painter().text(tag_rect.center(), Align2::CENTER_CENTER, &result.drive[..2], FontId::new(9.0, FontFamily::Name("cond".into())), Color32::WHITE);
                x += TAG_W + 6.0;
                let draw_text = |painter: &egui::Painter, text: &str, col_x: f32, col_w: f32, font: FontId, color: Color32, right_align: bool| {
                    let text_pos = if right_align { Pos2::new(col_x + col_w, y_center) } else { Pos2::new(col_x, y_center) };
                    let align = if right_align { Align2::RIGHT_CENTER } else { Align2::LEFT_CENTER };
                    let clip_rect = egui::Rect::from_min_size(Pos2::new(col_x, rect.top()), Vec2::new(col_w, rect.height()));
                    painter.with_clip_rect(clip_rect).text(text_pos, align, text, font, color);
                };
                if self.editing_idx == Some(idx) {
                    let edit_rect = egui::Rect::from_min_size(Pos2::new(x, rect.top() + 4.0), Vec2::new(NAME_W, 22.0));
                    let mut edit_ui = ui.child_ui(edit_rect, Layout::left_to_right(Align::Center), None);
                    let response = edit_ui.add(TextEdit::singleline(&mut self.edit_buffer).font(FontId::new(12.5, FontFamily::Name("mono".into()))).desired_width(NAME_W - 40.0));
                    if self.just_started_editing {
                        response.request_focus();
                        self.just_started_editing = false;
                    }
                    edit_ui.label(RichText::new(&self.edit_ext).font(FontId::new(12.5, FontFamily::Name("mono".into()))).color(TEXT2));

                    let should_cancel = ui.input(|i| i.key_pressed(egui::Key::Escape));
                    let should_save = (response.has_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter))) || 
                                      (response.lost_focus() && !should_cancel);

                    if should_save {
                        let old_p = std::path::Path::new(&result.full_path);
                        let stem = old_p.file_stem().and_then(|s| s.to_str()).unwrap_or("");
                        if self.edit_buffer != stem && !self.edit_buffer.trim().is_empty() {
                            if let Some(parent) = old_p.parent() {
                                let new_name = format!("{}{}", self.edit_buffer, self.edit_ext);
                                let new_p = parent.join(new_name);
                                let _ = std::fs::rename(old_p, new_p);
                                self.run_search(ui.ctx());
                            }
                        }
                        self.editing_idx = None;
                    } else if should_cancel {
                        self.editing_idx = None;
                    }
                } else {
                    draw_text(ui.painter(), &result.name, x, NAME_W, FontId::new(12.5, FontFamily::Name("mono".into())), Color32::WHITE, false);
                }
                x += NAME_W;
                draw_text(ui.painter(), &result.full_path, x, path_w, FontId::new(11.0, FontFamily::Name("mono".into())), Color32::WHITE, false);
                x += path_w;
                let size_str = if result.is_dir { "-".to_string() } else { format_size(result.size) };
                draw_text(ui.painter(), &size_str, x, SIZE_W, FontId::new(11.0, FontFamily::Name("mono".into())), Color32::WHITE, true);
                x += SIZE_W;
                draw_text(ui.painter(), &format_timestamp(result.timestamp), x, DATE_W, FontId::new(11.0, FontFamily::Name("mono".into())), Color32::WHITE, true);
            }
        });

        if let Some(idx) = new_selected {
            let modifiers = ui.input(|i| i.modifiers);
            if modifiers.command {
                if !self.selected_rows.insert(idx) { self.selected_rows.remove(&idx); }
                self.last_clicked_row = Some(idx);
            } else if modifiers.shift {
                if let Some(start) = self.last_clicked_row {
                    let (min, max) = if start < idx { (start, idx) } else { (idx, start) };
                    for i in min..=max { self.selected_rows.insert(i); }
                } else { self.selected_rows.insert(idx); }
            } else {
                self.selected_rows.clear();
                self.selected_rows.insert(idx);
                self.last_clicked_row = Some(idx);
            }
        }
    }
    
    fn draw_empty_state(&self, ui: &mut egui::Ui) {
        ui.vertical_centered(|ui| { ui.add_space(100.0); ui.label(RichText::new("无匹配结果").font(FontId::new(14.0, FontFamily::Name("cond".into()))).color(TEXT3)); });
    }

    fn draw_status_bar(&self, ui: &mut egui::Ui) {
        let total = self.results.len();
        let total_pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
        let total_pages = total_pages.max(1);
        let start = self.current_page * PAGE_SIZE;
        let end = (start + PAGE_SIZE).min(total);
        let page_count = if total == 0 { 0 } else { end - start };
        let memory_mb = (total as f64 * 184.0) / 1024.0 / 1024.0;

        ui.horizontal_centered(|ui| {
            if self.selected_rows.len() > 1 {
                let total_size: u64 = self.selected_rows.iter().filter_map(|&idx| self.results.get(idx)).map(|r| r.size).sum();
                stat_item(ui, "已选", &format!("{} 项", self.selected_rows.len())); ui.add_space(8.0);
                stat_item(ui, "合计大小", &format_size(total_size)); ui.add_space(16.0);
                if ui.link(RichText::new("导出所选为 CSV").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(ACCENT)).clicked() { self.export_selected_csv(); }
            } else {
                stat_item(ui, "共", &format!("{} 条", format_number(total))); ui.add_space(12.0);
                stat_item(ui, "本页", &format!("{} 条", format_number(page_count))); ui.add_space(12.0);
                stat_item(ui, "第", &format!("{} / {} 页", self.current_page + 1, total_pages)); ui.add_space(16.0);
                stat_item(ui, "耗时", &format!("{:.1} ms", self.last_search_ms));
            }
            
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                stat_item(ui, "数据占用", &format!("{:.1} MB", memory_mb));
            });
        });
    }

    fn handle_scan_progress(&mut self, ctx: &egui::Context) {
        let mut reload_needed = false;
        while let Ok(msg) = self.scan_rx.try_recv() {
            match msg {
                ScanProgress::Progress { done, total } => {
                    self.scan_progress = if total == 0 { 0.0 } else { done as f32 / total as f32 };
                    self.status_text = format!("正在扫描 {}/{}", done, total);
                }
                ScanProgress::Done => {
                    if self.active_scan_count > 0 { self.active_scan_count -= 1; }
                    if self.active_scan_count == 0 { self.is_scanning = false; self.status_text = "扫描完成".to_string(); }
                    reload_needed = true;
                }
                ScanProgress::Error(e) => {
                    self.status_text = format!("错误: {}", e);
                    if self.active_scan_count > 0 { self.active_scan_count -= 1; }
                    if self.active_scan_count == 0 { self.is_scanning = false; }
                }
            }
        }
        if reload_needed { self.load_volumes(); self.run_search(ctx); }
    }
}

fn col_header_btn(ui: &mut egui::Ui, text: &str, width: f32, right_align: bool) -> bool {
    let (rect, response) = ui.allocate_exact_size(Vec2::new(width, 22.0), Sense::click());
    let pos = if right_align { Pos2::new(rect.right(), rect.center().y) } else { Pos2::new(rect.left(), rect.center().y) };
    let align = if right_align { Align2::RIGHT_CENTER } else { Align2::LEFT_CENTER };
    ui.painter().text(pos, align, text, FontId::new(10.0, FontFamily::Name("cond".into())), Color32::WHITE);
    response.clicked()
}

fn stat_item(ui: &mut egui::Ui, label: &str, value: &str) { 
    ui.label(RichText::new(label).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3)); 
    ui.add_space(4.0); 
    ui.label(RichText::new(value).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT2).strong()); 
}

fn menu_item(ui: &mut egui::Ui, label: &str) -> bool {
    let button = egui::Button::new(RichText::new(label).font(FontId::new(11.0, FontFamily::Name("cond".into()))).color(Color32::TRANSPARENT))
        .fill(Color32::TRANSPARENT)
        .min_size(Vec2::new(ui.available_width(), 26.0))
        .rounding(Rounding::same(4.0));
    
    let res = ui.add(button);
    let is_hovered = res.hovered();
    
    if is_hovered {
        ui.painter().rect_filled(res.rect, Rounding::same(4.0), Color32::from_rgb(35, 45, 55));
    }
    
    let text_color = if is_hovered { Color32::WHITE } else { TEXT2 };
    ui.painter().text(res.rect.left_center() + Vec2::new(8.0, 0.0), Align2::LEFT_CENTER, label, FontId::new(11.0, FontFamily::Name("cond".into())), text_color);
    
    res.clicked()
}

fn open_file(path: &str) {
    let mut cmd = std::process::Command::new("cmd");
    cmd.args(["/c", "start", "", path]);
    #[cfg(windows)]
    { use std::os::windows::process::CommandExt; cmd.creation_flags(0x08000000); }
    let _ = cmd.spawn();
}

fn batch_open_files(paths: Vec<String>) {
    for path in paths {
        open_file(&path);
    }
}

fn reveal_in_explorer(path: &str) {
    let mut cmd = std::process::Command::new("explorer");
    #[cfg(windows)]
    { 
        use std::os::windows::process::CommandExt; 
        cmd.creation_flags(0x08000000); 
        // 修复：必须使用 raw_arg，强制按 Windows 规范加引号，绕过 Rust 的自动加引号导致错误
        cmd.raw_arg(format!("/select,\"{}\"", path)); 
    }
    #[cfg(not(windows))]
    { 
        cmd.arg("/select,");
        cmd.arg(path);
    }
    let _ = cmd.spawn();
}

fn delete_files(paths: Vec<String>) -> bool {
    #[cfg(windows)]
    {
        let count = paths.len();
        let msg = if count == 1 {
            format!("确定要永久删除此文件吗？\n{}", paths[0])
        } else {
            format!("确定要永久删除这 {} 个项目吗？", count)
        };

        let confirm = rfd::MessageDialog::new()
            .set_title("确认删除")
            .set_description(&msg)
            .set_buttons(rfd::MessageButtons::OkCancel)
            .show();

        if confirm == rfd::MessageDialogResult::Ok {
            for path in paths {
                let p = std::path::Path::new(&path);
                if p.is_dir() {
                    let _ = std::fs::remove_dir_all(p);
                } else {
                    let _ = std::fs::remove_file(p);
                }
            }
            return true;
        }
    }
    let _ = paths;
    false
}

#[allow(unused_variables)]
fn open_properties(path: &str) {
    #[cfg(windows)]
    unsafe {
        use windows::core::HSTRING;
        // 修复：必须用局部变量接住 HSTRING，保证它的生命周期不会中途被销毁（消除野指针 Bug）
        let verb = HSTRING::from("properties");
        let file = HSTRING::from(path);

        let mut info = SHELLEXECUTEINFOW {
            cbSize: std::mem::size_of::<SHELLEXECUTEINFOW>() as u32,
            lpVerb: windows::core::PCWSTR(verb.as_ptr()),
            lpFile: windows::core::PCWSTR(file.as_ptr()),
            nShow: 1, // SW_SHOWNORMAL
            fMask: SEE_MASK_INVOKEIDLIST,
            ..Default::default()
        };
        let _ = ShellExecuteExW(&mut info);
    }
}

fn render_svg_to_rgba(svg_bytes: &[u8], size: u32) -> Option<(Vec<u8>, u32, u32)> {
    let opt = usvg::Options::default();
    let tree = usvg::Tree::from_data(svg_bytes, &opt).ok()?;
    
    let rtree = resvg::Tree::from_usvg(&tree);
    let mut pixmap = resvg::tiny_skia::Pixmap::new(size, size)?;
    let ts = resvg::tiny_skia::Transform::from_scale(
        size as f32 / rtree.size.width(),
        size as f32 / rtree.size.height(),
    );
    rtree.render(ts, &mut pixmap.as_mut());
    
    let mut rgba = pixmap.data().to_vec();
    // Tiny-skia outputs premultiplied alpha, convert to straight RGBA
    for pixel in rgba.chunks_exact_mut(4) {
        let a = pixel[3] as f32 / 255.0;
        if a > 0.0 {
            pixel[0] = (pixel[0] as f32 / a).min(255.0) as u8;
            pixel[1] = (pixel[1] as f32 / a).min(255.0) as u8;
            pixel[2] = (pixel[2] as f32 / a).min(255.0) as u8;
        }
    }
    Some((rgba, size, size))
}

#[cfg(windows)]
fn load_icon() -> Option<tray_icon::Icon> {
    let icon_bytes = include_bytes!("../icons/FERREX.svg");
    // 托盘图标使用 32x32 渲染以确保在小尺寸下的清晰度和一致性
    let (rgba, width, height) = render_svg_to_rgba(icon_bytes, 32)?;
    tray_icon::Icon::from_rgba(rgba, width, height).ok()
}

fn setup_fonts(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    let mut _msyh_loaded = false;
    #[cfg(windows)]
    if let Ok(font_bytes) = std::fs::read("C:\\Windows\\Fonts\\msyh.ttc") {
        fonts.font_data.insert("msyh".to_owned(), egui::FontData::from_owned(font_bytes));
        _msyh_loaded = true;
    }
    if _msyh_loaded {
        if let Some(v) = fonts.families.get_mut(&FontFamily::Proportional) { v.insert(0, "msyh".to_owned()); }
        if let Some(v) = fonts.families.get_mut(&FontFamily::Monospace) { v.insert(0, "msyh".to_owned()); }
        fonts.families.insert(FontFamily::Name("cond".into()), vec!["msyh".to_owned()]);
        fonts.families.insert(FontFamily::Name("mono".into()), vec!["msyh".to_owned()]);
    } else {
        let default_sans = fonts.families.get(&FontFamily::Proportional).cloned().unwrap_or_default();
        let default_mono = fonts.families.get(&FontFamily::Monospace).cloned().unwrap_or_default();
        fonts.families.insert(FontFamily::Name("cond".into()), default_sans);
        fonts.families.insert(FontFamily::Name("mono".into()), default_mono);
    }
    ctx.set_fonts(fonts);
}

fn format_number(n: usize) -> String { 
    let s = n.to_string(); let mut result = String::new(); 
    for (i, c) in s.chars().rev().enumerate() { if i > 0 && i % 3 == 0 { result.push(','); } result.push(c); } 
    result.chars().rev().collect() 
}


fn format_size(bytes: u64) -> String { 
    if bytes == 0 { return "0 B".to_string(); } 
    const UNITS: &[&str] = &["B", "KB", "MB", "GB"]; 
    let mut size = bytes as f64; let mut unit = 0; 
    while size >= 1024.0 && unit < UNITS.len() - 1 { size /= 1024.0; unit += 1; } 
    format!("{:.2} {}", size, UNITS[unit]) 
}

fn format_timestamp(ts: u64) -> String { 
    if ts == 0 { return "-".to_string(); } 
    let secs = (ts / 10_000_000) as i64 - 11644473600; 
    use chrono::{TimeZone, Local};
    if let Some(dt) = Local.timestamp_opt(secs, 0).single() { return dt.format("%Y-%m-%d %H:%M").to_string(); }
    "-".to_string() 
}

fn cmp_ignore_ascii_case(a: &str, b: &str) -> std::cmp::Ordering {
    a.as_bytes().iter().map(|b| b.to_ascii_lowercase()).cmp(b.as_bytes().iter().map(|b| b.to_ascii_lowercase()))
}

fn spawn_scan(vol: String, tx: std::sync::mpsc::Sender<ScanProgress>) {
    std::thread::spawn(move || {
        let scanner = match indexer::MftScanner::new(&vol) { 
            Ok(s) => s, 
            Err(e) => { let _ = tx.send(ScanProgress::Error(e.to_string())); return; } 
        };
        match scanner.scan() {
            Ok(entries) => {
                let mut store = IndexStore::new();
                let total = entries.len();
                for (i, entry) in entries.into_iter().enumerate() {
                    if i % 10000 == 0 { let _ = tx.send(ScanProgress::Progress { done: i, total }); }
                    let offset = store.string_pool.len() as u32;
                    store.string_pool.extend_from_slice(entry.name.as_bytes()); store.string_pool.push(0);
                    store.frns.push(entry.frn); store.parent_frns.push(entry.parent_frn); 
                    store.sizes.push(entry.file_size); store.timestamps.push(entry.modified); 
                    store.name_offsets.push(offset); store.flags.push(entry.flags);
                }
                let drive_letter = vol.chars().next().unwrap_or('c').to_lowercase().to_string();
                let idx_path = format!("{}_drive.idx", drive_letter);
                let _ = storage::save_index(&idx_path, &store);
                let _ = tx.send(ScanProgress::Done);
            }
            Err(e) => { let _ = tx.send(ScanProgress::Error(e.to_string())); }
        }
    });
}

fn verify_hardware_wmi_static() -> bool {
    let whitelist =[
        "BFEBFBFF000306C3", "SGH412RF00", "494000PA0D9L",
        "PHYS825203NX480BGN", "NA5360WJ", "NA7G89GQ", "03000210052122072519"
    ];
    let cmds =["cpu get processorid", "baseboard get serialnumber", "bios get serialnumber"];
    for cmd_args in cmds {
        let mut cmd = Command::new("wmic");
        cmd.args(cmd_args.split_whitespace());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x08000000); // CREATE_NO_WINDOW
        }
        if let Ok(output) = cmd.output() {
            let raw_text = String::from_utf8_lossy(&output.stdout);
            let text = raw_text.replace('\0', "").replace('\u{FFFD}', ""); 
            for line in text.lines().skip(1) {
                let id = line.trim();
                if !id.is_empty() && whitelist.iter().any(|&w| w.eq_ignore_ascii_case(id)) {
                    return true;
                }
            }
        }
    }
    false 
}

fn main() -> eframe::Result<()> {
    let icon = (|| {
        let icon_bytes = include_bytes!("../icons/FERREX.svg");
        // 任务栏图标使用 256x256 高清渲染
        let (rgba, width, height) = render_svg_to_rgba(icon_bytes, 256)?;
        Some(egui::IconData { rgba, width, height })
    })();
    let native_options = eframe::NativeOptions { 
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([1000.0, 700.0])
            .with_min_inner_size([800.0, 500.0])
            .with_title("FERREX")
            .with_icon(icon.unwrap_or_default())
            .with_decorations(false), 
        ..Default::default() 
    };
    eframe::run_native("ferrex", native_options, Box::new(|cc| Ok(Box::new(FerrexApp::new(cc)) as Box<dyn eframe::App>)))
}