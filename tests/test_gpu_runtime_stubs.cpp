// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <setjmp.h>
// Renderer tests have no guest process. Clock/host-thread services work; guest accesses abort.
extern "C" {
#ifdef _WIN32
thread_local jmp_buf* runtime_fault_recover = nullptr;
#else
thread_local sigjmp_buf* runtime_fault_recover = nullptr;
#endif
// gpu/shim/common/assert.cpp reads it (a weak reference, which PE/COFF cannot leave unresolved).
volatile int runtime_restarting = 0;
uint64_t runtime_disabled_optimizations = 0;  // uint64_t as in runtime_memory.c and bbport_toggles.h
uint64_t runtime_experiment_bits = 0;
uint64_t runtime_tsc_frequency() { return 1000000000; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
const char* runtime_file_user_dir(void) { return "."; }
uint64_t runtime_memory_clamp(uintptr_t, uint64_t) { std::abort(); }
uint64_t runtime_process_time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int runtime_memory_region(uintptr_t, uintptr_t*, uintptr_t*, int*) { std::abort(); }
void runtime_memory_set_gpu_hooks(void (*)(uintptr_t, uint64_t),
    void (*)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t)) { std::abort(); }
void runtime_thread_attach_host(const char*) {}
int runtime_win_thread_name(uint32_t, char*, size_t) { return -1; }  // party_fourp A7 logs
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) { std::abort(); }
void runtime_restart() { std::abort(); }
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
int32_t* runtime_errno() { std::abort(); }
int runtime_memory_write_backing(uintptr_t, const void*, uint64_t) { std::abort(); }
void runtime_memory_read_backing(uintptr_t, void*, uint64_t) { std::abort(); }
const uint64_t* runtime_memory_generation() { static const uint64_t generation = 0; return &generation; }
void runtime_memory_note_write(uintptr_t, uint64_t) { std::abort(); }
void runtime_memory_set_note_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_cpu_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_write_watch(uintptr_t, uint64_t, int) { std::abort(); }
void runtime_memory_set_guest_chunk_allocator(int (*)(uint64_t, uint64_t)) {}
void runtime_memory_set_guest_chunk_whole(int) {}
int runtime_memory_vma_info(uintptr_t, int*, int*, uintptr_t*) { std::abort(); }
void runtime_memory_trap(uintptr_t, uint64_t, unsigned, int) { std::abort(); }
unsigned runtime_memory_trap_reasons(uintptr_t) { std::abort(); }
int runtime_memory_direct_phys(uintptr_t, uint64_t*, uintptr_t*) { std::abort(); }
void* runtime_memory_backing_pointer(uint64_t) { std::abort(); }
uintptr_t runtime_memory_resolve(const char*) { std::abort(); }
void runtime_wait_report(double) {}
void runtime_sleep_stats(uint64_t* calls, uint64_t* ns) { *calls = 0; *ns = 0; }
uint64_t runtime_heap_growths() { return 0; }
// The party/net glue in bbgpu (bbnet_glue.cpp, game_state.cpp) runs only with a guest process.
__attribute__((sysv_abi)) void restore_guest_fs() { std::abort(); }
int runtime_setjmp(void*) { std::abort(); }
// Without LTO the linker keeps whole bbgpu objects (menu, stability, party director, equeue), so
// their runtime references need a definition too; none of them runs without the game.
void* runtime_low_map(size_t, int) { std::abort(); }
uint64_t runtime_gfx_heap_more_mib() { return 0; }
uint64_t runtime_memory_pool_size() { return 0; }
int runtime_extra_direct_map(uint64_t, uint64_t, void**) { std::abort(); }
void runtime_memory_grow_flexible(uint64_t) { std::abort(); }
void runtime_pad_director_press(uint32_t) { std::abort(); }
void runtime_pad_director_sticks(int, int, int, int) { std::abort(); }
uint32_t runtime_pad_last_buttons() { return 0; }
uint32_t runtime_pad_last_left() { return 0x8080; }
uint32_t runtime_pad_cross_presses() { return 0; }
// Button prompts (bbport_button_prompts.cpp): nothing sampled, the game's icons.
unsigned runtime_pad_prompt_generation() { return 0; }
int runtime_pad_prompt_set() { return -1; }
int runtime_pad_mouse_look() { return 0; }
int runtime_pad_prompt(const char*, int, char*, int) { return 0; }
const char* runtime_symbol(const char*) { return nullptr; }
void runtime_thread_set_blocked(const char*, uint64_t) {}
void runtime_thread_clear_blocked() {}
// The party flow / new-game test / save policy glue in bbgpu (party_flow_game.cpp, test_newgame.cpp).
void runtime_relaunch(int) { std::abort(); }
int runtime_ime_dialog_count() { return 0; }
int runtime_savedata_mount_count() { return 0; }
const char* runtime_savepolicy_dir() { return "."; }
int runtime_savepolicy_check(const char*, const char*, int, void*) { return -1; }
}
