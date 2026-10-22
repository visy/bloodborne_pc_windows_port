# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
# The Windows port's version: VERSION at the repository root (launcher.py and the tests read it too).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" BBPORT_VERSION)
# Reconfigure when VERSION changes (otherwise a version bump keeps the old string in the exe).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
string(STRIP "${BBPORT_VERSION}" BBPORT_VERSION)
project(bbgpu VERSION ${BBPORT_VERSION} LANGUAGES C CXX)
add_compile_definitions(BBPORT_VERSION="${BBPORT_VERSION}")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# bbport: link-time optimization (BB_LTO, default on) and profile-guided optimization
# (BB_PGO: off, generate = instrumented build writing into BB_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(BB_LTO "Link-time optimization of bbgpu" ON)
# BB_MARCH: optional -march for a build that only runs on this CPU family (e.g. skylake, native).
# Empty (default) keeps the portable x86-64 build.
set(BB_MARCH "" CACHE STRING "Target CPU for -march (empty: portable build)")
if (BB_MARCH)
    # The x86-64-vN levels are -march only: tune those for current CPUs in general.
    if (BB_MARCH MATCHES "^x86-64")
        add_compile_options(-march=${BB_MARCH} -mtune=generic)
    else()
        add_compile_options(-march=${BB_MARCH} -mtune=${BB_MARCH})
    endif()
endif()
set(BB_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(BB_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
# Where bbport.exe and bb-gpu-capabilities.exe go (a PGO training build uses its own folder).
set(BB_OUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../out" CACHE PATH "Executable output directory")
if (BB_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${bb_ipo_error}")
    endif()
endif()
if (BB_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE)
elseif (BB_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

if (WIN32)
    if (DEFINED ENV{MINGW_PREFIX})
        list(APPEND CMAKE_PREFIX_PATH "$ENV{MINGW_PREFIX}")
    elseif (EXISTS "C:/msys64/mingw64")
        list(APPEND CMAKE_PREFIX_PATH "C:/msys64/mingw64")
    endif()
    if (DEFINED ENV{SDL3_DIR})
        list(APPEND CMAKE_PREFIX_PATH "$ENV{SDL3_DIR}")
    endif()
    if (DEFINED ENV{VULKAN_SDK})
        list(APPEND CMAKE_PREFIX_PATH "$ENV{VULKAN_SDK}")
    endif()
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
    if (EXISTS "C:/msys64/mingw64/lib")
        link_directories("C:/msys64/mingw64/lib")
    endif()
endif()

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
if (WIN32)
    if (NOT TARGET magic_enum::magic_enum)
        add_library(magic_enum::magic_enum INTERFACE IMPORTED)
        find_path(MAGIC_ENUM_INC NAMES magic_enum/magic_enum.hpp PATHS "$ENV{MINGW_PREFIX}/include" "C:/msys64/mingw64/include")
        if (MAGIC_ENUM_INC)
            target_include_directories(magic_enum::magic_enum INTERFACE "${MAGIC_ENUM_INC}")
        endif()
    endif()
else()
    find_package(magic_enum REQUIRED)
endif()
find_package(tsl-robin-map REQUIRED)
if (NOT TARGET GPUOpen::VulkanMemoryAllocator)
    add_library(GPUOpen::VulkanMemoryAllocator INTERFACE IMPORTED)
    find_path(VMA_INC NAMES vk_mem_alloc.h PATHS "$ENV{MINGW_PREFIX}/include" "C:/msys64/mingw64/include")
    if (VMA_INC)
        target_include_directories(GPUOpen::VulkanMemoryAllocator INTERFACE "${VMA_INC}")
    endif()
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
if (WIN32)
    find_package(SDL3 QUIET CONFIG)
    if (TARGET SDL3::SDL3)
        add_library(PkgConfig::SDL3 INTERFACE IMPORTED)
        target_link_libraries(PkgConfig::SDL3 INTERFACE SDL3::SDL3)
    else()
        find_path(SDL3_INC NAMES SDL3/SDL.h PATHS "$ENV{SDL3_DIR}/include" "C:/msys64/mingw64/include")
        find_library(SDL3_LIB NAMES SDL3 libSDL3.dll.a PATHS "$ENV{SDL3_DIR}/lib" "C:/msys64/mingw64/lib")
        if (SDL3_INC AND SDL3_LIB)
            add_library(PkgConfig::SDL3 INTERFACE IMPORTED)
            target_include_directories(PkgConfig::SDL3 INTERFACE "${SDL3_INC}")
            target_link_libraries(PkgConfig::SDL3 INTERFACE "${SDL3_LIB}")
        else()
            pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
        endif()
    endif()
else()
    pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()
if (WIN32)
    if (NOT TARGET miniz::miniz)
        add_library(miniz::miniz INTERFACE IMPORTED)
        find_path(MINIZ_INC NAMES miniz.h PATHS "$ENV{MINGW_PREFIX}/include" "C:/msys64/mingw64/include")
        if (MINIZ_INC)
            target_include_directories(miniz::miniz INTERFACE "${MINIZ_INC}")
        endif()
        find_library(MINIZ_LIB NAMES miniz libminiz.a PATHS "$ENV{MINGW_PREFIX}/lib" "C:/msys64/mingw64/lib")
        if (MINIZ_LIB)
            target_link_libraries(miniz::miniz INTERFACE "${MINIZ_LIB}")
        endif()
    endif()
else()
    find_package(miniz REQUIRED)
endif()
find_package(Zydis REQUIRED)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator PATHS "C:/msys64/mingw64/bin" REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
# FSR 3.1.6 frame generation (from Supermedo/bloodborne_pc PR #69 by FIocker).
set(FFX_VK_PORTABLE_BUILD_FSR3_3_1_5_SCAFFOLD ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(hdr_compose.frag hdr_compose_frag)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(fg_depth.comp fg_depth_comp)
add_host_shader(fg_hud.comp fg_hud_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(fsr4_color.comp fsr4_decode_comp)
add_host_shader(fsr4_color.comp fsr4_encode_comp ENCODE=1)
add_host_shader(fsr4_reactive.comp fsr4_reactive_comp)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(occlusion_resolve.comp occlusion_resolve_comp)
add_host_shader(gpu_timestamp.comp gpu_timestamp_comp)
add_host_shader(indirect_guard.comp indirect_guard_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
# The layer's own modules (independent of the shadPS4 code): layer/.
file(GLOB LAYER_SOURCES CONFIGURE_DEPENDS layer/*.cpp)
# Game profiles (what the translator knows about one game, out of its core): games/.
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS games/*.cpp)
# The translator's own command processor (docs/TRANSLATOR_ARCHITECTURE.ru.md, step 2): cp/.
file(GLOB CP_SOURCES CONFIGURE_DEPENDS cp/*.cpp)
if (WIN32)
    add_library(bbgpu STATIC ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES} ${LAYER_SOURCES} ${GAME_SOURCES} ${CP_SOURCES}
        shadps4/core/libraries/gnmdriver/gnmdriver.cpp
        shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
        shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
        shadps4/core/libraries/videodec/video_utils.cpp)
else()
    add_library(bbgpu SHARED ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES} ${LAYER_SOURCES} ${GAME_SOURCES} ${CP_SOURCES}
        shadps4/core/libraries/gnmdriver/gnmdriver.cpp
        shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
        shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
        shadps4/core/libraries/videodec/video_utils.cpp)
endif()
add_dependencies(bbgpu host_shaders)
# The build's identity for the party version check (A9): "<VERSION> (<git describe>)", regenerated
# on every build, rewritten only when it changes (one small file recompiles after a commit).
set(BBPORT_BUILD_ID_C "${CMAKE_CURRENT_BINARY_DIR}/generated/bbport_build_id.c")
add_custom_target(bbport_build_id
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/.. -DOUT=${BBPORT_BUILD_ID_C}
            -DVERSION=${BBPORT_VERSION} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bbport_build_id.cmake
    BYPRODUCTS ${BBPORT_BUILD_ID_C}
    COMMENT "bbport build id" VERBATIM)
target_sources(bbgpu PRIVATE ${BBPORT_BUILD_ID_C})
add_dependencies(bbgpu bbport_build_id)
# The shader translator's identity for the shader cache (vk_cache_header.h): a hash of
# shadps4/shader_recompiler, regenerated on every build, rewritten only when it changes.
set(BBPORT_TRANSLATOR_ID_C "${CMAKE_CURRENT_BINARY_DIR}/generated/shader_translator_id.c")
add_custom_target(shader_translator_id
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shader_recompiler
            -DOUT=${BBPORT_TRANSLATOR_ID_C} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/shader_translator_id.cmake
    BYPRODUCTS ${BBPORT_TRANSLATOR_ID_C}
    COMMENT "shader translator id" VERBATIM)
target_sources(bbgpu PRIVATE ${BBPORT_TRANSLATOR_ID_C})
add_dependencies(bbgpu shader_translator_id)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
# Dear ImGui stays private to libbbgpu.so. The library is linked into bb-probe, so its exports are
# in the global symbol scope; a Vulkan layer carrying its own Dear ImGui without -Bsymbolic
# (vkShade 0.1) had its ImGui_ImplVulkan_Init bound to this copy, which called the Vulkan loader
# with the layer's down-chain physical device handle ("Invalid physicalDevice", abort).
set_source_files_properties(third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf"
    BB_SERIF_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSerif.ttf"
    BB_GAME_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/LibreBaskerville.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf;${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSerif.ttf;${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/LibreBaskerville.ttf")
# Code guest threads run in place of the game's (System V functions): no red zone (Windows
# exception frames from the write watch would land on it), and the ribbon writers compute the
# game's bits exactly (no fused multiply-adds).
set_source_files_properties(shim/bbport_stability.cpp PROPERTIES COMPILE_OPTIONS "-mno-red-zone")
# party_fourp.cpp: a top-level asm stub (H4) refers to C symbols, which LTO would discard.
set_source_files_properties(shim/party/party_fourp.cpp PROPERTIES COMPILE_OPTIONS "-fno-lto")
set_source_files_properties(shim/bbport_ribbons.cpp PROPERTIES COMPILE_OPTIONS
    "-mno-red-zone;-ffp-contract=off;-fno-fast-math")
target_include_directories(bbgpu PRIVATE shim layer games cp shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include)
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
if (WIN32)
    target_compile_definitions(bbgpu PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
if (NOT WIN32)
    # Loaded with bb-probe at startup (DT_NEEDED, never dlopen): thread_local access without a
    # TLS descriptor call (_dl_tlsdesc_return was ~2% of the draw recording thread).
    # (Windows: bbgpu is a static library linked into bbport.exe; ELF TLS models don't apply.)
    target_compile_options(bbgpu PRIVATE -ftls-model=initial-exec)
    # Runtime symbols (runtime_memory_*, runtime_process_*) resolve against bb-probe.
    target_link_options(bbgpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan
                                    ffx-vulkan::fsr3-vk-framegeneration-3.1.6)
if (WIN32)
    target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
        tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit ws2_32 iphlpapi synchronization)
else()
    target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
        tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::X11 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
endif()

# Explicit test targets; neither launches the game nor needs a Vulkan device.
add_executable(shader-user-data-test EXCLUDE_FROM_ALL ../tests/test_shader_user_data.cpp)
target_compile_options(shader-user-data-test PRIVATE -UNDEBUG)
target_include_directories(shader-user-data-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(shader-user-data-test PRIVATE fmt::fmt Boost::headers)

add_executable(motion-history-test EXCLUDE_FROM_ALL ../tests/test_motion_history.cpp)
target_compile_options(motion-history-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(motion-history-test PRIVATE ..)
add_executable(motion-shader-test EXCLUDE_FROM_ALL ../tests/test_motion_shaders.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(motion-shader-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(motion-shader-test PRIVATE bbgpu)
# Out of VRAM for a new image: the reclaim rounds (from Supermedo/bloodborne_pc 5f56ed6a).
add_executable(vram-reclaim-test EXCLUDE_FROM_ALL ../tests/test_vram_reclaim.cpp)
target_compile_options(vram-reclaim-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(vram-reclaim-test PRIVATE ..)
# The texture collector's marks, levels and per-frame quota (texture_cache/gc_policy.h).
add_executable(gc-policy-test EXCLUDE_FROM_ALL ../tests/test_gc_policy.cpp)
target_compile_options(gc-policy-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(gc-policy-test PRIVATE ..)
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ..)
add_executable(cp-decoder-test EXCLUDE_FROM_ALL ../tests/test_cp_decoder.cpp cp/cp_decoder.cpp)
target_compile_options(cp-decoder-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(cp-decoder-test PRIVATE cp)
# Frame generation (Supermedo/bloodborne_pc PR #69 by FIocker): presentation cadence and settings.
add_executable(fg-timing-test EXCLUDE_FROM_ALL ../tests/test_fg_timing.cpp)
target_include_directories(fg-timing-test PRIVATE .. shadps4)
target_compile_options(fg-timing-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_executable(fg-settings-test EXCLUDE_FROM_ALL ../tests/test_fg_settings.cpp shim/bbport_settings.cpp)
target_include_directories(fg-settings-test PRIVATE .. shim)
target_compile_options(fg-settings-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
# NVIDIA Reflex's CPU side (Supermedo/bloodborne_pc PR #69 by FIocker): simulation cycles, locks.
add_executable(reflex-cpu-test EXCLUDE_FROM_ALL ../tests/test_reflex_cpu.cpp
    shadps4/video_core/renderer_vulkan/vk_reflex.cpp shim/bbport_settings.cpp)
target_include_directories(reflex-cpu-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} shim shadps4 third_party/gcn/include)
target_compile_options(reflex-cpu-test PRIVATE -UNDEBUG)
target_link_libraries(reflex-cpu-test PRIVATE Vulkan::Headers fmt::fmt)
if (WIN32)
    target_compile_definitions(reflex-cpu-test PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()
# Native HDR output (shim/bbport_hdr.h, I4): PQ, Rec.2020, the inverse tone curve, settings.
add_executable(hdr-test EXCLUDE_FROM_ALL ../tests/test_hdr.cpp shim/bbport_settings.cpp)
target_compile_options(hdr-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(hdr-test PRIVATE .. shim)
add_executable(settings-test EXCLUDE_FROM_ALL ../tests/test_settings.cpp shim/bbport_settings.cpp)
target_compile_options(settings-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(settings-test PRIVATE .. shim)
# The port's pages in the game's System menu (shim/bbport_game_menu_pages.cpp): limits, settings, party buttons.
add_executable(game-menu-pages-test EXCLUDE_FROM_ALL ../tests/test_game_menu_pages.cpp shim/bbport_game_menu_pages.cpp shim/party/party_npctalk.cpp
    shim/bbport_settings.cpp shim/party/party_status.cpp shim/party/party_balance_rules.cpp shim/net/json.cpp)
target_compile_definitions(game-menu-pages-test PRIVATE BB_PARTY_NPCTALK_NO_GAME)
target_compile_options(game-menu-pages-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(game-menu-pages-test PRIVATE .. shim)
set_target_properties(game-menu-pages-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# The same pages with the party flow's rows: no page shows more rows than its slots in any party state.
add_executable(game-menu-rows-test EXCLUDE_FROM_ALL ../tests/test_game_menu_rows.cpp shim/bbport_game_menu_pages.cpp shim/party/party_npctalk.cpp
    shim/party/party_flow_rows.cpp shim/bbport_settings.cpp shim/party/party_status.cpp shim/party/party_balance_rules.cpp
    shim/net/json.cpp)
target_compile_definitions(game-menu-rows-test PRIVATE BB_PARTY_NPCTALK_NO_GAME)
target_compile_options(game-menu-rows-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(game-menu-rows-test PRIVATE .. shim)
set_target_properties(game-menu-rows-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../tests/test_scene_resolution.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_options(scene-resolution-test PRIVATE -fPIC)
set_source_files_properties(../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE bbgpu)
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../tests/test_taa_shader.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(taa-shader-test PRIVATE -fPIC)
set_source_files_properties(../tests/test_taa_shader.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(taa-shader-test PRIVATE bbgpu)
add_dependencies(taa-shader-test host_shaders)

add_executable(camera-motion-test EXCLUDE_FROM_ALL ../tests/test_camera_motion.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(camera-motion-test PRIVATE -fPIC)
target_link_libraries(camera-motion-test PRIVATE bbgpu)
add_dependencies(camera-motion-test host_shaders)

add_executable(translation-memory-test EXCLUDE_FROM_ALL ../tests/test_translation_memory.cpp
    ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(translation-memory-test PRIVATE shim shadps4 third_party/gcn/include)
target_compile_options(translation-memory-test PRIVATE -fPIC)
set_source_files_properties(../tests/test_translation_memory.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(translation-memory-test PRIVATE bbgpu)
# The Vulkan tests share bbgpu's headers, so they see them with bbgpu's configuration: NDEBUG
# (Vulkan-Hpp's DispatchLoaderBase has an extra member without it) and, on Windows,
# VK_USE_PLATFORM_WIN32_KHR. PE/COFF merges the tests' inline Vulkan-Hpp methods (COMDAT) with
# bbgpu's whatever their visibility; with another DispatchLoaderDynamic layout bbgpu's dispatcher
# was filled at the wrong offsets ("Vulkan 1.3 is required, but only 1.0 is supported by
# instance"). The tests turn assert() back on after those headers (#undef NDEBUG).
foreach(vulkan_test scene-resolution-test taa-shader-test camera-motion-test translation-memory-test)
    target_compile_definitions(${vulkan_test} PRIVATE NDEBUG)
    if (WIN32)
        target_compile_definitions(${vulkan_test} PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    endif()
endforeach()
if (WIN32)
    target_compile_definitions(motion-shader-test PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    # bbgpu's Windows dependencies (as bbport links them): NtSetInformationFile (io_file.cpp) etc.
    foreach(vulkan_test motion-shader-test scene-resolution-test taa-shader-test camera-motion-test
            translation-memory-test)
        target_link_libraries(${vulkan_test} PRIVATE ws2_32 iphlpapi synchronization dbghelp ntdll winmm)
    endforeach()
endif()

# Party network library (shim/net): STUN, vport framing, the network simulator and a loopback
# UDP exchange through the guest-facing socket functions. ninja -C out/gpu party-net-test
file(GLOB PARTY_NET_SOURCES CONFIGURE_DEPENDS shim/net/*.cpp)
add_executable(party-net-test EXCLUDE_FROM_ALL ../tests/test_party_net.cpp ${PARTY_NET_SOURCES})
target_compile_options(party-net-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-net-test PRIVATE shim/net ..)
set_target_properties(party-net-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
if (WIN32)
    target_link_libraries(party-net-test PRIVATE ws2_32 iphlpapi synchronization)
endif()

# Party host service (shim/net, A3): rooms, events, heartbeats, signaling resolve, FromApi through
# sceHttp, Matching2/Signaling over LocalHost. ninja -C out/gpu party-host-test
add_executable(party-host-test EXCLUDE_FROM_ALL ../tests/test_party_host.cpp ${PARTY_NET_SOURCES})
target_compile_options(party-host-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-host-test PRIVATE shim/net ..)
set_target_properties(party-host-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
if (WIN32)
    target_link_libraries(party-host-test PRIVATE ws2_32 iphlpapi synchronization)
endif()

# bbport: FSR 4 benchmark outside the game (tools/fsr4_bench.cpp): ninja -C out/gpu fsr4-bench.
add_executable(upscaler-support-test EXCLUDE_FROM_ALL ../tests/test_upscaler_support.cpp
    shim/bbport_settings.cpp)
target_include_directories(upscaler-support-test PRIVATE shim)
target_compile_options(upscaler-support-test PRIVATE -UNDEBUG)

add_executable(fsr4-bench EXCLUDE_FROM_ALL tools/fsr4_bench.cpp
    shadps4/video_core/renderer_vulkan/fsr411/fsr411.cpp)
target_include_directories(fsr4-bench PRIVATE shadps4/video_core/renderer_vulkan/fsr411)
target_link_libraries(fsr4-bench PRIVATE ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)

file(GLOB RUNTIME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../src/runtime*.c")
if (WIN32)
    list(APPEND RUNTIME_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/win32_compat.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/win32_memory.c"
    )
endif()
add_executable(bbport
    ../src/probe.c
    ${RUNTIME_SOURCES}
    ../src/vulkan_smoke.c
)
if (WIN32)
    # The exe's version resource (Properties -> Details): file and product version.
    enable_language(RC)
    # Exactly four numbers (0.5 -> 0,5,0,0; 0.5.1 -> 0,5,1,0) for FILEVERSION / PRODUCTVERSION.
    string(REPLACE "." ";" _bb_ver_parts "${BBPORT_VERSION}")
    list(APPEND _bb_ver_parts 0 0 0)
    list(SUBLIST _bb_ver_parts 0 4 _bb_ver_parts)
    string(REPLACE ";" "," BBPORT_VERSION_COMMAS "${_bb_ver_parts}")
    configure_file(../src/bbport.rc.in "${CMAKE_CURRENT_BINARY_DIR}/bbport.rc" @ONLY)
    target_sources(bbport PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bbport.rc")
endif()
target_include_directories(bbport PRIVATE .. ../src)
target_link_libraries(bbport PRIVATE bbgpu "${CMAKE_CURRENT_SOURCE_DIR}/../out/libatrac9.a" Vulkan::Vulkan)
if (WIN32)
    target_link_libraries(bbport PRIVATE ws2_32 iphlpapi synchronization dbghelp ntdll winmm)
    target_link_options(bbport PRIVATE "-Wl,--disable-dynamicbase" "-Wl,--disable-high-entropy-va")
endif()
set_target_properties(bbport PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BB_OUT_DIR}")
# bbport: strip the shipped exe (Mrsuss60/bloodborne_pc_windows_port a5de3761: ~100 MB -> ~12 MB).
# The debug info moves to bbport.exe.debug next to it with a debuglink, so addr2line/gdb on a crash
# log's "bbport.exe+0x..." offsets still work with the matching build; keep that file with the
# release build, do not ship it. Runtime needs no symbols (crash logs print module offsets; PGO
# profiles are keyed by object path). Default: with LTO, i.e. release builds; agent builds
# (BB_LTO=OFF) keep their symbols in the exe.
option(BB_STRIP "Strip bbport.exe after linking (debug info kept in bbport.exe.debug)" ${BB_LTO})
if (BB_STRIP AND CMAKE_OBJCOPY AND CMAKE_STRIP)
    add_custom_command(TARGET bbport POST_BUILD
        COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug "$<TARGET_FILE:bbport>" "$<TARGET_FILE:bbport>.debug"
        COMMAND "${CMAKE_STRIP}" -s "$<TARGET_FILE:bbport>"
        COMMAND "${CMAKE_OBJCOPY}" "--add-gnu-debuglink=$<TARGET_FILE:bbport>.debug" "$<TARGET_FILE:bbport>"
        COMMENT "Stripping bbport (debug info: bbport.exe.debug)" VERBATIM)
endif()

# Runtime unit tests (tests/test_sema.c, tests/test_content.c) for tests/test_sema.py and
# tests/test_content.py, which run out/sema-test and out/content-test (build.sh --test builds them
# on Linux). cmake --build out/gpu --target sema-test content-test
add_executable(sema-test EXCLUDE_FROM_ALL ../tests/test_sema.c ${RUNTIME_SOURCES})
target_include_directories(sema-test PRIVATE .. ../src)
target_compile_options(sema-test PRIVATE -UNDEBUG)
target_link_libraries(sema-test PRIVATE bbgpu "${CMAKE_CURRENT_SOURCE_DIR}/../out/libatrac9.a")
add_executable(content-test EXCLUDE_FROM_ALL ../tests/test_content.c ../src/runtime_content.c)
target_include_directories(content-test PRIVATE .. ../src)
target_compile_options(content-test PRIVATE -UNDEBUG -Wall -Wextra)
# -Werror for the test only: win32_compat.c redefines WIN32_LEAN_AND_MEAN (also set globally).
set_source_files_properties(../tests/test_content.c PROPERTIES COMPILE_OPTIONS -Werror)
target_compile_definitions(content-test PRIVATE _GNU_SOURCE)
if (WIN32)
    target_link_libraries(sema-test PRIVATE ws2_32 iphlpapi synchronization dbghelp ntdll winmm)
    target_sources(content-test PRIVATE ../src/win32_compat.c)
    target_link_libraries(content-test PRIVATE ws2_32)
endif()
set_target_properties(sema-test content-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BB_OUT_DIR}")

# src/runtime_pad.c with the button prompts' sets and labels (src/pad_prompts.h); build.bat also
# builds it as out/pad-test.exe. cmake --build out/gpu --target pad-test
add_executable(pad-test EXCLUDE_FROM_ALL ../tests/test_pad.c)
target_include_directories(pad-test PRIVATE .. ../src)
target_compile_options(pad-test PRIVATE -UNDEBUG -Wall -Wextra)
target_link_libraries(pad-test PRIVATE PkgConfig::SDL3)

add_executable(bb-gpu-capabilities ../tools/gpu_capabilities.c)
target_include_directories(bb-gpu-capabilities PRIVATE .. ../src)
target_link_libraries(bb-gpu-capabilities PRIVATE Vulkan::Vulkan PkgConfig::SDL3)
set_target_properties(bb-gpu-capabilities PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BB_OUT_DIR}")

# Party link (shim/party: PartyLink control channel, party codes, UPnP, public address) with
# monocypher 4.0.2 (third_party/monocypher, BSD-2-Clause OR CC0-1.0). ninja -C out/gpu party-link-test
target_sources(bbgpu PRIVATE third_party/monocypher/monocypher.c)
if (WIN32)
    target_link_libraries(bbgpu PUBLIC ole32 oleaut32)
endif()
# The link layer only: the other shim/party files (runtime wiring, game-side co-op) need the game.
set(PARTY_LINK_SOURCES shim/party/party_link.cpp shim/party/party_rudp.cpp shim/party/party_code.cpp shim/party/party_addr.cpp
    shim/party/party_crypto.cpp shim/party/party_lan.cpp shim/party/upnp_win.cpp)
add_executable(party-link-test EXCLUDE_FROM_ALL ../tests/test_party_link.cpp ${PARTY_LINK_SOURCES}
    shim/net/net_stun.cpp shim/net/netsim.cpp third_party/monocypher/monocypher.c)
target_compile_options(party-link-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-link-test PRIVATE shim)
set_target_properties(party-link-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
if (WIN32)
    target_link_libraries(party-link-test PRIVATE ws2_32 iphlpapi ole32 oleaut32)
endif()
# NAT traversal (F2): the NAT model against simulated NATs, the netsim NAT kinds, the DHT
# rendezvous (fake DHT nodes on loopback only) and the route table (member relay, IPv6; 0.7.1).
# ninja -C out/gpu party-nat-test
add_executable(party-nat-test EXCLUDE_FROM_ALL ../tests/test_party_nat.cpp ${PARTY_LINK_SOURCES}
    shim/party/party_nat.cpp shim/party/party_rdv.cpp shim/party/party_netpath.cpp shim/net/party_route.cpp
    shim/net/party_udp.cpp shim/net/net_stun.cpp shim/net/netsim.cpp
    third_party/monocypher/monocypher.c)
target_compile_options(party-nat-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-nat-test PRIVATE shim)
set_target_properties(party-nat-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
if (WIN32)
    target_link_libraries(party-nat-test PRIVATE ws2_32 iphlpapi ole32 oleaut32)
endif()
# The in-game party flow's pure parts (shim/party/party_flow.cpp: party.json with the DPAPI-protected
# password, the action planner, the relaunch file). ninja -C out/gpu party-flow-test
add_executable(party-flow-test EXCLUDE_FROM_ALL ../tests/test_party_flow.cpp shim/party/party_flow.cpp shim/net/json.cpp)
target_compile_options(party-flow-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-flow-test PRIVATE shim)
set_target_properties(party-flow-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Party campaign start (C1, shim/party/party_start.cpp; in bbgpu through SHIM_SOURCES): readiness,
# bell grant plan, lobby ring decisions. ninja -C out/gpu party-start-test
add_executable(party-start-test EXCLUDE_FROM_ALL ../tests/test_party_start.cpp shim/party/party_start.cpp)
target_compile_options(party-start-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-start-test PRIVATE shim)
set_target_properties(party-start-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Stability fixes' pure decisions (shim/bbport_stability_logic.h). ninja -C out/gpu stability-logic-test
add_executable(stability-logic-test EXCLUDE_FROM_ALL ../tests/test_stability_logic.cpp)
target_compile_options(stability-logic-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(stability-logic-test PRIVATE shim)
set_target_properties(stability-logic-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Direct summon delivery (shim/party/party_dsummon.cpp): wire format, type-1 payload, decisions.
add_executable(party-dsummon-test EXCLUDE_FROM_ALL ../tests/test_party_dsummon.cpp shim/party/party_dsummon.cpp)
target_compile_options(party-dsummon-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-dsummon-test PRIVATE shim)
set_target_properties(party-dsummon-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Test aids (shim/party/test_newgame.cpp, BB_TEST_NEWGAME / BB_TEST_PROLOGUE_SKIP): the scripted
# new game driver against a model of the title menus. ninja -C out/gpu party-newgame-test
add_executable(party-newgame-test EXCLUDE_FROM_ALL ../tests/test_party_newgame.cpp shim/party/test_newgame.cpp)
target_compile_definitions(party-newgame-test PRIVATE BB_TEST_NEWGAME_NO_GAME)
target_compile_options(party-newgame-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-newgame-test PRIVATE shim)
set_target_properties(party-newgame-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Host session watchdog (shim/party/party_watchdog.cpp): when a stuck host session is stopped.
# ninja -C out/gpu party-watchdog-test
add_executable(party-watchdog-test EXCLUDE_FROM_ALL ../tests/test_party_watchdog.cpp shim/party/party_watchdog.cpp)
target_compile_options(party-watchdog-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-watchdog-test PRIVATE shim)
set_target_properties(party-watchdog-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Party logging levels, areas, tags, rate limits, marks (shim/party/party_log.cpp; in bbgpu through
# SHIM_SOURCES). ninja -C out/gpu party-log-test
add_executable(party-log-test EXCLUDE_FROM_ALL ../tests/test_party_log.cpp shim/party/party_log.cpp
    shim/party/party_async_io.cpp)
target_compile_options(party-log-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-log-test PRIVATE shim)
set_target_properties(party-log-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# The shared BB_PARTY on/off rule (party_env.h). ninja -C out/gpu party-env-test
add_executable(party-env-test EXCLUDE_FROM_ALL ../tests/test_party_env.cpp)
target_compile_options(party-env-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
set_target_properties(party-env-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# guest_stick test-step arguments (party_stick_args.h). ninja -C out/gpu party-stick-test
add_executable(party-stick-test EXCLUDE_FROM_ALL ../tests/test_party_stick.cpp)
target_compile_options(party-stick-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-stick-test PRIVATE shim)
set_target_properties(party-stick-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# C5 spectate target (party_spectate.h). ninja -C out/gpu party-spectate-test
add_executable(party-spectate-test EXCLUDE_FROM_ALL ../tests/test_party_spectate.cpp)
target_compile_options(party-spectate-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-spectate-test PRIVATE shim)
set_target_properties(party-spectate-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# I5 gather the party (party_gather_rules.h). ninja -C out/gpu party-gather-test
add_executable(party-gather-test EXCLUDE_FROM_ALL ../tests/test_party_gather.cpp)
target_compile_options(party-gather-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-gather-test PRIVATE shim)
set_target_properties(party-gather-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Remote player chrs shown by the port only after their member's data (party_remote_show_rules.h).
# ninja -C out/gpu party-remote-show-test
add_executable(party-remote-show-test EXCLUDE_FROM_ALL ../tests/test_party_remote_show.cpp)
target_compile_options(party-remote-show-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-remote-show-test PRIVATE shim)
set_target_properties(party-remote-show-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Deaths, rests and same-map lamps (party_death_rules.h). ninja -C out/gpu party-death-rules-test
add_executable(party-death-rules-test EXCLUDE_FROM_ALL ../tests/test_party_death_rules.cpp)
target_compile_options(party-death-rules-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-death-rules-test PRIVATE shim)
set_target_properties(party-death-rules-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Lifts in a party: the table and the decisions (party_lift_rules.h). ninja -C out/gpu party-lift-rules-test
add_executable(party-lift-rules-test EXCLUDE_FROM_ALL ../tests/test_party_lift_rules.cpp)
target_compile_options(party-lift-rules-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-lift-rules-test PRIVATE shim)
set_target_properties(party-lift-rules-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# A guest the party keeps where it stands (party_stay_rules.h). ninja -C out/gpu party-stay-rules-test
add_executable(party-stay-rules-test EXCLUDE_FROM_ALL ../tests/test_party_stay_rules.cpp)
target_compile_options(party-stay-rules-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-stay-rules-test PRIVATE shim)
set_target_properties(party-stay-rules-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# A party member's own values (party_guestvals_rules.h). ninja -C out/gpu party-guestvals-test
add_executable(party-guestvals-test EXCLUDE_FROM_ALL ../tests/test_party_guestvals.cpp)
target_compile_options(party-guestvals-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-guestvals-test PRIVATE shim)
set_target_properties(party-guestvals-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Companions without the phantom glow (party_glow_rules.h). ninja -C out/gpu party-glow-test
add_executable(party-glow-test EXCLUDE_FROM_ALL ../tests/test_party_glow.cpp)
target_compile_options(party-glow-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-glow-test PRIVATE shim)
set_target_properties(party-glow-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Direct lamp travel (lamp_travel_rules.h). ninja -C out/gpu lamp-travel-test
add_executable(lamp-travel-test EXCLUDE_FROM_ALL ../tests/test_lamp_travel.cpp)
target_compile_options(lamp-travel-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(lamp-travel-test PRIVATE shim)
set_target_properties(lamp-travel-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# The world warper's rules (world_warper_rules.h). ninja -C out/gpu world-warper-test
add_executable(world-warper-test EXCLUDE_FROM_ALL ../tests/test_world_warper.cpp)
target_compile_options(world-warper-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(world-warper-test PRIVATE shim)
set_target_properties(world-warper-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# H4 enemy census (party_enemy_census.h). ninja -C out/gpu party-enemy-census-test
add_executable(party-enemy-census-test EXCLUDE_FROM_ALL ../tests/test_party_enemy_census.cpp)
target_compile_options(party-enemy-census-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-enemy-census-test PRIVATE shim)
set_target_properties(party-enemy-census-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Fast loading (shim/bbport_fast_loading.h): the pure rules. ninja -C out/gpu fast-loading-test
add_executable(fast-loading-test EXCLUDE_FROM_ALL ../tests/test_fast_loading.cpp)
target_compile_options(fast-loading-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(fast-loading-test PRIVATE shim)
set_target_properties(fast-loading-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Shader cache header (vk_cache_header.h): versions, translator id, GPU profile. ninja -C out/gpu shader-cache-header-test
add_executable(shader-cache-header-test EXCLUDE_FROM_ALL ../tests/test_shader_cache_header.cpp)
target_compile_options(shader-cache-header-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(shader-cache-header-test PRIVATE shadps4)
set_target_properties(shader-cache-header-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Boot watchdog (shim/bbport_boot_watch.h, BB_BOOT_WATCHDOG): the rules. ninja -C out/gpu boot-watch-test
add_executable(boot-watch-test EXCLUDE_FROM_ALL ../tests/test_boot_watch.cpp)
target_compile_options(boot-watch-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(boot-watch-test PRIVATE shim)
set_target_properties(boot-watch-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Frame rate by hooks (shim/bbport_frame_rate_logic.h): the pure rules. ninja -C out/gpu frame-rate-test
add_executable(frame-rate-test EXCLUDE_FROM_ALL ../tests/test_frame_rate.cpp)
target_compile_options(frame-rate-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(frame-rate-test PRIVATE shim)
set_target_properties(frame-rate-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# C6 crash rejoin: the crashed member's connections (party_nr_drop.h). ninja -C out/gpu party-nr-drop-test
add_executable(party-nr-drop-test EXCLUDE_FROM_ALL ../tests/test_party_nr_drop.cpp)
target_compile_options(party-nr-drop-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-nr-drop-test PRIVATE shim)
set_target_properties(party-nr-drop-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)


# Party progress sync (shim/party/party_progress, C2): keys, categories, policy, diff, JSON, BBPF
# against a fake flag store (the game glue is compiled out). ninja -C out/gpu party-progress-test
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_party_progress.cpp")  # not committed yet
add_executable(party-progress-test EXCLUDE_FROM_ALL ../tests/test_party_progress.cpp
    shim/party/party_progress.cpp shim/net/json.cpp)
target_compile_definitions(party-progress-test PRIVATE BB_PARTY_PROGRESS_NO_GAME
    BB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(party-progress-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-progress-test PRIVATE shim)
set_target_properties(party-progress-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
endif()
# Party desync detector (shim/party/party_desync.cpp, F5): digests, the guest's flag model, the
# checker, resync round trips. cmake --build out/gpu --target party-desync-test
add_executable(party-desync-test EXCLUDE_FROM_ALL ../tests/test_party_desync.cpp shim/party/party_desync.cpp
    shim/party/party_progress.cpp shim/net/json.cpp)
target_compile_definitions(party-desync-test PRIVATE BB_PARTY_PROGRESS_NO_GAME)
target_compile_options(party-desync-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-desync-test PRIVATE shim)
set_target_properties(party-desync-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Party host harness (tests/party_host_harness.cpp): PartyLink host + PartyHostService + FromApi +
# the party UDP port in one process with no game, for tools/party/simguest.py.
# ninja -C out/gpu party-host-harness
add_executable(party-host-harness EXCLUDE_FROM_ALL ../tests/party_host_harness.cpp ${PARTY_NET_SOURCES}
    ${PARTY_LINK_SOURCES} third_party/monocypher/monocypher.c)
target_compile_options(party-host-harness PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-host-harness PRIVATE shim shim/net ..)
set_target_properties(party-host-harness PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
if (WIN32)
    target_link_libraries(party-host-harness PRIVATE ws2_32 iphlpapi synchronization ole32 oleaut32)
endif()
# Party status board (shim/party/party_status.cpp; in bbgpu through SHIM_SOURCES): the overlay's
# Party tab <-> director snapshot and command queue. ninja -C out/gpu party-status-test
add_executable(party-status-test EXCLUDE_FROM_ALL ../tests/test_party_status.cpp shim/party/party_status.cpp)
target_compile_options(party-status-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-status-test PRIVATE shim)
set_target_properties(party-status-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Party transport soak (tests/party_soak.cpp): a host and three guests in one process over
# loopback under the network simulator's profiles and burst outages; a verdict per scenario.
# cmake --build out/gpu --target party-soak && out/gpu/party-soak.exe [scenario...]
add_executable(party-soak EXCLUDE_FROM_ALL ../tests/party_soak.cpp ${PARTY_NET_SOURCES} ${PARTY_LINK_SOURCES}
    third_party/monocypher/monocypher.c)
target_compile_options(party-soak PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-soak PRIVATE shim/net shim ..)
set_target_properties(party-soak PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
if (WIN32)
    target_link_libraries(party-soak PRIVATE ws2_32 iphlpapi synchronization ole32 oleaut32 winmm)
endif()
# Party security fuzzing (tests/party_fuzz.cpp): every parser of peer bytes (JSON, PartyLink
# frames over loopback both ways, STUN / relay / vport framing, party codes, the host service and
# FromApi, the guest's event decoders and sanitizers) under random and structure-aware inputs,
# plus the security regressions. party-fuzz: 60 s by default ([--seconds S] [target...]);
# party-security-test: the regressions and a 20 s run; party-fuzz-ubsan: party-fuzz with UBSan in
# trap mode and libstdc++ assertions (MinGW GCC has no ASan).
set(PARTY_FUZZ_SOURCES ../tests/party_fuzz.cpp ${PARTY_NET_SOURCES} ${PARTY_LINK_SOURCES}
    third_party/monocypher/monocypher.c shim/party/party_travel.cpp shim/party/party_story.cpp
    shim/party/party_items.cpp shim/party/party_phantom.cpp shim/party/party_progress.cpp
    shim/party/party_guestsvc.cpp shim/party/party_worldact.cpp shim/party/party_balance_rules.cpp
    shim/party/party_chalice.cpp shim/party/party_droproll.cpp shim/party/party_dsummon.cpp
    shim/party/party_desync.cpp shim/party/party_npctalk.cpp shim/party/party_ping.cpp
    shim/party/party_gestures.cpp)
foreach(fuzz_target party-fuzz party-security-test party-fuzz-ubsan)
    add_executable(${fuzz_target} EXCLUDE_FROM_ALL ${PARTY_FUZZ_SOURCES})
    target_compile_definitions(${fuzz_target} PRIVATE BB_PARTY_TRAVEL_NO_GAME BB_PARTY_STORY_NO_GAME
        BB_PARTY_ITEMS_NO_GAME BB_PARTY_PHANTOM_NO_GAME BB_PARTY_PROGRESS_NO_GAME BB_PARTY_GUESTSVC_NO_GAME
        BB_PARTY_WORLDACT_NO_GAME BB_PARTY_CHALICE_NO_GAME BB_PARTY_DROPROLL_NO_GAME BB_PARTY_NPCTALK_NO_GAME
        BB_PARTY_GESTURES_NO_GAME)
    target_compile_options(${fuzz_target} PRIVATE -UNDEBUG -Wall -Wextra)
    target_include_directories(${fuzz_target} PRIVATE shim shim/net ..)
    set_target_properties(${fuzz_target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
    if (WIN32)
        target_link_libraries(${fuzz_target} PRIVATE ws2_32 iphlpapi synchronization ole32 oleaut32)
    endif()
endforeach()
target_compile_definitions(party-security-test PRIVATE PARTY_FUZZ_QUICK)
target_compile_options(party-fuzz-ubsan PRIVATE -fsanitize=undefined -fsanitize-undefined-trap-on-error
    -fno-sanitize-recover=all -D_GLIBCXX_ASSERTIONS -O1)
target_link_options(party-fuzz-ubsan PRIVATE -fsanitize=undefined -fsanitize-undefined-trap-on-error)
# Party balance rules (shim/party/party_balance_rules.cpp; in bbgpu through SHIM_SOURCES): scaling
# rows, rot, the "balance" event JSON. cmake --build out/gpu --target party-balance-test
add_executable(party-balance-test EXCLUDE_FROM_ALL ../tests/test_party_balance.cpp shim/party/party_balance_rules.cpp
    shim/net/json.cpp)
target_compile_options(party-balance-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-balance-test PRIVATE shim)
set_target_properties(party-balance-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# C4 rot cleanses (0.7.2, shim/party/party_rot_rules.h, header only) with the rules / party_rot.txt
# fields of party_balance_rules. cmake --build out/gpu --target party-rot-test
add_executable(party-rot-test EXCLUDE_FROM_ALL ../tests/test_party_rot.cpp shim/party/party_balance_rules.cpp
    shim/net/json.cpp)
target_compile_options(party-rot-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-rot-test PRIVATE shim)
set_target_properties(party-rot-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Friendly fire / PvP (I1, shim/party/party_pvp_rules.h; the game side party_pvp.cpp is in bbgpu
# through SHIM_SOURCES): session gate, the guest's rule, team pairs, test steps.
# cmake --build out/gpu --target party-pvp-test
add_executable(party-pvp-test EXCLUDE_FROM_ALL ../tests/test_party_pvp.cpp shim/party/party_balance_rules.cpp
    shim/net/json.cpp)
target_compile_options(party-pvp-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-pvp-test PRIVATE shim)
set_target_properties(party-pvp-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Party HUD board (shim/party/party_hud.cpp; in bbgpu through SHIM_SOURCES): same-world rules,
# composition from roster / VITALS / local reads, geometry. cmake --build out/gpu --target party-hud-test
add_executable(party-hud-test EXCLUDE_FROM_ALL ../tests/test_party_hud.cpp shim/party/party_hud.cpp
    ${PARTY_LINK_SOURCES} shim/net/net_stun.cpp shim/net/netsim.cpp third_party/monocypher/monocypher.c)
target_compile_options(party-hud-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-hud-test PRIVATE shim)
set_target_properties(party-hud-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
if (WIN32)
    target_link_libraries(party-hud-test PRIVATE ws2_32 iphlpapi ole32 oleaut32)
endif()
# NPC presence mirror (shim/party/party_npc_mirror.h, header only): the guest's plan, the event
# body. cmake --build out/gpu --target party-npc-mirror-test
add_executable(party-npc-mirror-test EXCLUDE_FROM_ALL ../tests/test_party_npc_mirror.cpp)
target_compile_options(party-npc-mirror-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-npc-mirror-test PRIVATE shim)
# NPC hunter fights with the whole party (shim/party/party_npcfight_rules.h, header only, D11):
# the guest copies of the host-only transitions, assassins, slots. --target party-npcfight-test
add_executable(party-npcfight-test EXCLUDE_FROM_ALL ../tests/test_party_npcfight.cpp)
target_compile_options(party-npcfight-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-npcfight-test PRIVATE shim)
# Party status bridge (shim/party/party_status_bridge.cpp; in bbgpu through SHIM_SOURCES): link /
# game facts -> the Party tab's state, roster, events, commands. ninja -C out/gpu party-status-bridge-test
add_executable(party-status-bridge-test EXCLUDE_FROM_ALL ../tests/test_party_status_bridge.cpp
    shim/party/party_status_bridge.cpp shim/party/party_status.cpp shim/party/party_async_io.cpp
    ${PARTY_LINK_SOURCES} shim/net/net_stun.cpp shim/net/netsim.cpp third_party/monocypher/monocypher.c)
target_compile_definitions(party-status-bridge-test PRIVATE BB_PARTY_BRIDGE_NO_GAME)
target_compile_options(party-status-bridge-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-status-bridge-test PRIVATE shim)
set_target_properties(party-status-bridge-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
if (WIN32)
    target_link_libraries(party-status-bridge-test PRIVATE ws2_32 iphlpapi ole32 oleaut32)
endif()
# Party travel (B1, shim/party/party_travel.cpp; in bbgpu through SHIM_SOURCES): its pure logic
# only (BB_PARTY_TRAVEL_NO_GAME). ninja -C out/gpu party-travel-test
add_executable(party-travel-test EXCLUDE_FROM_ALL ../tests/test_party_travel.cpp shim/party/party_travel.cpp
    shim/party/party_chalice.cpp shim/net/json.cpp)
target_compile_definitions(party-travel-test PRIVATE BB_PARTY_TRAVEL_NO_GAME BB_PARTY_CHALICE_NO_GAME)
target_compile_options(party-travel-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-travel-test PRIVATE shim)
set_target_properties(party-travel-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Party story (C4, shim/party/party_story.cpp): its pure logic only (BB_PARTY_STORY_NO_GAME).
# ninja -C out/gpu party-story-test
add_executable(party-story-test EXCLUDE_FROM_ALL ../tests/test_party_story.cpp shim/party/party_story.cpp
    shim/net/json.cpp)
target_compile_definitions(party-story-test PRIVATE BB_PARTY_STORY_NO_GAME)
target_compile_options(party-story-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-story-test PRIVATE shim)
set_target_properties(party-story-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Party gestures (shim/party/party_gestures.cpp; in bbgpu through SHIM_SOURCES): ids, JSON, the
# owed store and the decision (BB_PARTY_GESTURES_NO_GAME). ninja -C out/gpu party-gestures-test
add_executable(party-gestures-test EXCLUDE_FROM_ALL ../tests/test_party_gestures.cpp shim/party/party_gestures.cpp
    shim/net/json.cpp)
target_compile_definitions(party-gestures-test PRIVATE BB_PARTY_GESTURES_NO_GAME)
target_compile_options(party-gestures-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-gestures-test PRIVATE shim)
set_target_properties(party-gestures-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Party items (C3, shim/party/party_items.cpp; in bbgpu through SHIM_SOURCES): tables, JSON,
# queues and the apply gate (BB_PARTY_ITEMS_NO_GAME). ninja -C out/gpu party-items-test
add_executable(party-items-test EXCLUDE_FROM_ALL ../tests/test_party_items.cpp shim/party/party_items.cpp
    shim/net/json.cpp)
target_compile_definitions(party-items-test PRIVATE BB_PARTY_ITEMS_NO_GAME)
target_compile_options(party-items-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-items-test PRIVATE shim)
set_target_properties(party-items-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Party phantoms (shim/party/party_phantom.cpp; in bbgpu through SHIM_SOURCES): its pure logic
# and party_travel's guest redirect choice. ninja -C out/gpu party-phantom-test
add_executable(party-phantom-test EXCLUDE_FROM_ALL ../tests/test_party_phantom.cpp shim/party/party_phantom.cpp
    shim/party/party_travel.cpp shim/net/json.cpp)
target_compile_definitions(party-phantom-test PRIVATE BB_PARTY_PHANTOM_NO_GAME BB_PARTY_TRAVEL_NO_GAME)
target_compile_options(party-phantom-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-phantom-test PRIVATE shim)
set_target_properties(party-phantom-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Party guest services (shim/party/party_guestsvc.cpp; in bbgpu through SHIM_SOURCES): the gate and
# record logic (BB_PARTY_GUESTSVC_NO_GAME). ninja -C out/gpu party-guestsvc-test
add_executable(party-guestsvc-test EXCLUDE_FROM_ALL ../tests/test_party_guestsvc.cpp shim/party/party_guestsvc.cpp
    shim/party/party_phantom.cpp shim/party/party_travel.cpp shim/net/json.cpp)
target_compile_definitions(party-guestsvc-test PRIVATE BB_PARTY_GUESTSVC_NO_GAME BB_PARTY_PHANTOM_NO_GAME
    BB_PARTY_TRAVEL_NO_GAME)
target_compile_options(party-guestsvc-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-guestsvc-test PRIVATE shim)
set_target_properties(party-guestsvc-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Guest world actions (shim/party/party_worldact.cpp; in bbgpu through SHIM_SOURCES): whitelist,
# request JSON, rate limit, NPC talk allowance (BB_PARTY_WORLDACT_NO_GAME).
# cmake --build out/gpu --target party-worldact-test
add_executable(party-worldact-test EXCLUDE_FROM_ALL ../tests/test_party_worldact.cpp shim/party/party_worldact.cpp
    shim/party/party_progress.cpp shim/net/json.cpp)
target_compile_definitions(party-worldact-test PRIVATE BB_PARTY_WORLDACT_NO_GAME BB_PARTY_PROGRESS_NO_GAME
    BB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(party-worldact-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-worldact-test PRIVATE shim)
set_target_properties(party-worldact-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Boss fights after a party load (party_bossfog_rules.h), over every boss fog (party_worldact.cpp).
# cmake --build out/gpu --target party-bossfog-rules-test
add_executable(party-bossfog-rules-test EXCLUDE_FROM_ALL ../tests/test_party_bossfog_rules.cpp
    shim/party/party_worldact.cpp shim/party/party_progress.cpp shim/net/json.cpp)
target_compile_definitions(party-bossfog-rules-test PRIVATE BB_PARTY_WORLDACT_NO_GAME BB_PARTY_PROGRESS_NO_GAME
    BB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(party-bossfog-rules-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-bossfog-rules-test PRIVATE shim)
set_target_properties(party-bossfog-rules-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Shared NPC dialogue (shim/party/party_npctalk.cpp; in bbgpu through SHIM_SOURCES): event JSON,
# speaker names, range decision, dedupe, talk menu choice tracking (BB_PARTY_NPCTALK_NO_GAME).
# cmake --build out/gpu --target party-npctalk-test
add_executable(party-npctalk-test EXCLUDE_FROM_ALL ../tests/test_party_npctalk.cpp shim/party/party_npctalk.cpp
    shim/net/json.cpp)
target_compile_definitions(party-npctalk-test PRIVATE BB_PARTY_NPCTALK_NO_GAME)
target_compile_options(party-npctalk-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-npctalk-test PRIVATE shim)
set_target_properties(party-npctalk-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Pings (I2, shim/party/party_ping.cpp; in bbgpu through SHIM_SOURCES): EVENT map_ping JSON, the
# relay rate, the marker board, the aim pick and the screen-edge clamp. cmake --build out/gpu --target party-ping-test
add_executable(party-ping-test EXCLUDE_FROM_ALL ../tests/test_party_ping.cpp shim/party/party_ping.cpp
    shim/net/json.cpp)
target_compile_options(party-ping-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-ping-test PRIVATE shim)
set_target_properties(party-ping-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Shared enemy drop roll (shim/party/party_droproll.cpp; in bbgpu through SHIM_SOURCES): seed,
# MT19937 seeding, EVENT drop_seed (BB_PARTY_DROPROLL_NO_GAME). cmake --build out/gpu --target party-droproll-test
add_executable(party-droproll-test EXCLUDE_FROM_ALL ../tests/test_party_droproll.cpp shim/party/party_droproll.cpp
    shim/net/json.cpp)
target_compile_definitions(party-droproll-test PRIVATE BB_PARTY_DROPROLL_NO_GAME)
target_compile_options(party-droproll-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-droproll-test PRIVATE shim)
set_target_properties(party-droproll-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Four-player rules (shim/party/party_fourp.cpp; in bbgpu through SHIM_SOURCES): the pure parts
# (BB_PARTY_FOURP_NO_GAME). ninja -C out/gpu party-fourp-test
add_executable(party-fourp-test EXCLUDE_FROM_ALL ../tests/test_party_fourp.cpp shim/party/party_fourp.cpp)
target_compile_definitions(party-fourp-test PRIVATE BB_PARTY_FOURP_NO_GAME)
target_compile_options(party-fourp-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-fourp-test PRIVATE shim)
set_target_properties(party-fourp-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Save policy (C5, src/runtime_savepolicy.c; in bbport through RUNTIME_SOURCES): party save folder,
# rotating backups, save check, on temporary folders. ninja -C out/gpu save-policy-test
add_executable(save-policy-test EXCLUDE_FROM_ALL ../tests/test_save_policy.c ../src/runtime_savepolicy.c)
target_compile_options(save-policy-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(save-policy-test PRIVATE ../src)
set_target_properties(save-policy-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
