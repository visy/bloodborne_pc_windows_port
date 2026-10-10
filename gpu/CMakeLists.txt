# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(bbgpu LANGUAGES C CXX)
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
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(fsr4_color.comp fsr4_decode_comp)
add_host_shader(fsr4_color.comp fsr4_encode_comp ENCODE=1)
add_host_shader(fsr4_reactive.comp fsr4_reactive_comp)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(occlusion_resolve.comp occlusion_resolve_comp)
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
if (WIN32)
    add_library(bbgpu STATIC ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES} ${LAYER_SOURCES}
        shadps4/core/libraries/gnmdriver/gnmdriver.cpp
        shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
        shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
        shadps4/core/libraries/videodec/video_utils.cpp)
else()
    add_library(bbgpu SHARED ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES} ${LAYER_SOURCES}
        shadps4/core/libraries/gnmdriver/gnmdriver.cpp
        shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
        shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
        shadps4/core/libraries/videodec/video_utils.cpp)
endif()
add_dependencies(bbgpu host_shaders)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf"
    BB_SERIF_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSerif.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf;${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSerif.ttf")
# Code guest threads run in place of the game's (System V functions): no red zone (Windows
# exception frames from the write watch would land on it), and the ribbon writers compute the
# game's bits exactly (no fused multiply-adds).
set_source_files_properties(shim/bbport_stability.cpp PROPERTIES COMPILE_OPTIONS "-mno-red-zone")
# party_fourp.cpp: a top-level asm stub (H4) refers to C symbols, which LTO would discard.
set_source_files_properties(shim/party/party_fourp.cpp PROPERTIES COMPILE_OPTIONS "-fno-lto")
set_source_files_properties(shim/bbport_ribbons.cpp PROPERTIES COMPILE_OPTIONS
    "-mno-red-zone;-ffp-contract=off;-fno-fast-math")
target_include_directories(bbgpu PRIVATE shim layer shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include)
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
target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan)
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
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ..)
add_executable(settings-test EXCLUDE_FROM_ALL ../tests/test_settings.cpp shim/bbport_settings.cpp)
target_compile_options(settings-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(settings-test PRIVATE .. shim)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../tests/test_scene_resolution.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_options(scene-resolution-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE bbgpu)
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../tests/test_taa_shader.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(taa-shader-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_taa_shader.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(taa-shader-test PRIVATE bbgpu)
add_dependencies(taa-shader-test host_shaders)

add_executable(camera-motion-test EXCLUDE_FROM_ALL ../tests/test_camera_motion.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(camera-motion-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(camera-motion-test PRIVATE bbgpu)
add_dependencies(camera-motion-test host_shaders)

add_executable(translation-memory-test EXCLUDE_FROM_ALL ../tests/test_translation_memory.cpp
    ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(translation-memory-test PRIVATE shim shadps4 third_party/gcn/include)
target_compile_options(translation-memory-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_translation_memory.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(translation-memory-test PRIVATE bbgpu)

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
target_include_directories(bbport PRIVATE .. ../src)
target_link_libraries(bbport PRIVATE bbgpu "${CMAKE_CURRENT_SOURCE_DIR}/../out/libatrac9.a" Vulkan::Vulkan)
if (WIN32)
    target_link_libraries(bbport PRIVATE ws2_32 iphlpapi synchronization dbghelp ntdll winmm)
    target_link_options(bbport PRIVATE "-Wl,--disable-dynamicbase" "-Wl,--disable-high-entropy-va")
endif()
set_target_properties(bbport PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BB_OUT_DIR}")

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
set(PARTY_LINK_SOURCES shim/party/party_link.cpp shim/party/party_code.cpp shim/party/party_addr.cpp
    shim/party/party_crypto.cpp shim/party/upnp_win.cpp)
add_executable(party-link-test EXCLUDE_FROM_ALL ../tests/test_party_link.cpp ${PARTY_LINK_SOURCES}
    shim/net/net_stun.cpp third_party/monocypher/monocypher.c)
target_compile_options(party-link-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-link-test PRIVATE shim)
set_target_properties(party-link-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
if (WIN32)
    target_link_libraries(party-link-test PRIVATE ws2_32 iphlpapi ole32 oleaut32)
endif()

# Party progress sync (shim/party/party_progress, C2): keys, categories, policy, diff, JSON, BBPF
# against a fake flag store (the game glue is compiled out). ninja -C out/gpu party-progress-test
add_executable(party-progress-test EXCLUDE_FROM_ALL ../tests/test_party_progress.cpp
    shim/party/party_progress.cpp shim/net/json.cpp)
target_compile_definitions(party-progress-test PRIVATE BB_PARTY_PROGRESS_NO_GAME
    BB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(party-progress-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-progress-test PRIVATE shim)
set_target_properties(party-progress-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
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
# Party travel (B1, shim/party/party_travel.cpp; in bbgpu through SHIM_SOURCES): its pure logic
# only (BB_PARTY_TRAVEL_NO_GAME). ninja -C out/gpu party-travel-test
add_executable(party-travel-test EXCLUDE_FROM_ALL ../tests/test_party_travel.cpp shim/party/party_travel.cpp
    shim/net/json.cpp)
target_compile_definitions(party-travel-test PRIVATE BB_PARTY_TRAVEL_NO_GAME)
target_compile_options(party-travel-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-travel-test PRIVATE shim)
set_target_properties(party-travel-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Four-player rules (shim/party/party_fourp.cpp; in bbgpu through SHIM_SOURCES): the pure parts
# (BB_PARTY_FOURP_NO_GAME). ninja -C out/gpu party-fourp-test
add_executable(party-fourp-test EXCLUDE_FROM_ALL ../tests/test_party_fourp.cpp shim/party/party_fourp.cpp)
target_compile_definitions(party-fourp-test PRIVATE BB_PARTY_FOURP_NO_GAME)
target_compile_options(party-fourp-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-fourp-test PRIVATE shim)
set_target_properties(party-fourp-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
