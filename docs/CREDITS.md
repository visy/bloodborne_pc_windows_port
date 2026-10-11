# Credits and licenses (full)

Back to the [README](../README.md).



This project is licensed under the **GNU General Public License v3 or later** ([LICENSE](LICENSE)). Up to v0.3.2 it was distributed under GPL-2.0-or-later; every component allows the move to version 3 (the shadPS4 parts and the port's own files are GPL-2.0-or-later), and it lets the port include code from [bbhost](https://github.com/droogie/bbhost) by droogie and team (GPL-3.0-or-later). Third-party components keep their licenses: [shadPS4](https://github.com/shadps4-emu/shadPS4) video core and shader recompiler (GPL-2.0+), [sirit](https://github.com/shadps4-emu/sirit), [half](https://half.sourceforge.net/), [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn (MIT), AMD FidelityFX SDK (MIT), [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) (MIT), [Dear ImGui](https://github.com/ocornut/imgui) (MIT), DejaVu fonts, [dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) (MIT, used to build the FSR 4.1.1 assets). The DLSS bridge (`gpu/dlss_bridge`, MIT) and loader come from [Supermedo's Windows port](https://github.com/Supermedo/bloodborne_pc), adapted from [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR). AMD's FSR 4 DLLs and model data and NVIDIA's DLSS SDK are not distributed here. Release packages include NVIDIA's `nvngx_dlss.dll` runtime under its license (`out/NVIDIA_DLSS_LICENSE.txt`).

Special thanks to the original creators and open source projects that made this port possible:

- **deadinside28**: Creator of the original Linux port (`bbport`), pioneering the flat memory image architecture, custom Vulkan two-stage pipeline, and synthetic motion vector generation.
- **Mrsuss60**: creator of the original native Windows port ([bloodborne_pc_windows_port](https://github.com/Mrsuss60/bloodborne_pc_windows_port)): the Win32 runtime and Vulkan on Windows, the Windows build scripts, character creation and keyboard menu fixes, and merging community pull requests.
- **shadPS4 Team**: Base Vulkan video core and shader recompiler.
- **FireBurn**: FSR-Vulkan runtime and temporal upscaling backend.
- **Thealexbarney**: LibAtrac9 audio decoding library.
- **ocornut**: Dear ImGui library for the in-game settings overlay.
- **Supermedo** and **IFreemz**: DLSS bridge.
- **droogie and the bbhost team** ([bbhost](https://github.com/droogie/bbhost), GPL-3.0-or-later): the long-session stability fixes in `gpu/shim/bbport_stability.cpp` and `gpu/shim/bbport_ribbons.cpp` (heap size table growth, GX resource-table reclaim wait, frame-pool borrow fix, red-zone-free effect ribbon writers, blend write-mask fix, 6 GiB direct memory) come from or follow bbhost.
- **Community Patch Authors**: Kyo, Lance McDonald, illusion, emoose, auser1337, and contributors for the 60 FPS, camera, and engine patches.
- **zackcage6**: testing and bug reporting.
