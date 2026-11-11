### Fixed
- **Non-ASCII user or folder names** bbport.exe now carries a manifest that makes UTF-8 its process code page (Windows 10 1903 and later), so a Windows user name or install folder with letters outside ASCII no longer breaks narrow paths at the start (by intent from bbhost 5b78718). `run.bat` reads the mods step's game path as UTF-8 too, so a folder like `tëst_äö` starts. A log line says so when Windows keeps the old code page.
- **Mod animations past 8192 clips** the game's behaviour clip capacity is raised from 0x2000 to 0x4000 (Nightwatch 8cfe003), so mods that add animations (Nocturne has 8271 clips) no longer lose the clips past the limit. Vanilla (7784 clips) is unchanged. `BB_CLIP_CAPACITY=0` leaves the game's limit.

### Changed
- **Launcher taskbar identity** the launcher has its own taskbar identity instead of grouping under Python, and uses `icon.ico` beside it as the window icon when that file exists (Mrsuss60 1cc0678c, by intent).
