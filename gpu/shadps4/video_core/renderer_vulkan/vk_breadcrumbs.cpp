// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <thread>
#include <vector>
#include <cstring>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <fmt/format.h>
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_breadcrumbs.h"
#include "video_core/renderer_vulkan/vk_instance.h"

extern "C" int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end);

#include <vk_mem_alloc.h>

namespace Vulkan::Breadcrumbs {

namespace {

constexpr u32 MaxStreams = 16;
constexpr u32 StreamStride = 64; // bytes of the marker buffer per stream (a cache line)
constexpr u64 RingSize = 1u << 15;
constexpr u32 ArgSlots = 4096;                     // indirect dispatch arguments, 16 bytes each
constexpr u64 ArgsOffset = MaxStreams * StreamStride; // where they start in the buffer

struct Entry {
    u64 id;
    Crumb crumb;
};

struct Stream {
    const char* name = nullptr;
    std::atomic<u64> next{1};
    std::unique_ptr<Entry[]> ring;
};

std::array<Stream, MaxStreams> streams;
std::atomic<u32> num_streams{0};
bool enabled = false;
std::once_flag init_once;
vk::Buffer marker_buffer{};
VmaAllocation marker_allocation{};
volatile u32* markers = nullptr;
volatile u32* args = nullptr;
std::atomic<u32> next_args_slot{0};
/// What each argument slot was last used for.
struct ArgsNote {
    u64 id;
    u32 stream;
    u64 hash;
};
std::array<ArgsNote, ArgSlots> args_notes{};
const Instance* the_instance = nullptr;
/// While a report is written: its copy in out/ (support bundles take it with the hang shaders).
FILE* report_file = nullptr;

/// printf to stdout (the launcher log) and to the report file.
void Out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    if (report_file) {
        va_start(ap, fmt);
        std::vfprintf(report_file, fmt, ap);
        va_end(ap);
    }
}

const char* OutDir() {
    return access("out", W_OK) == 0 ? "out/" : "";
}

void Init(const Instance& instance) {
    the_instance = &instance;
    const char* env = std::getenv("BB_BREADCRUMBS");
    if (env && env[0] == '0') {
        return;
    }
    if (!instance.IsBufferMarkerSupported()) {
        std::printf("GPU breadcrumbs: VK_AMD_buffer_marker unavailable, GPU hangs will not be "
                    "located\n");
        return;
    }
    const VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = ArgsOffset + ArgSlots * 16,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    VkBuffer buffer{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer,
                        &marker_allocation, &info) != VK_SUCCESS ||
        !info.pMappedData) {
        std::printf("GPU breadcrumbs: no host-visible buffer, GPU hangs will not be located\n");
        return;
    }
    marker_buffer = vk::Buffer{buffer};
    markers = static_cast<volatile u32*>(info.pMappedData);
    for (u32 i = 0; i < (ArgsOffset + ArgSlots * 16) / sizeof(u32); ++i) {
        markers[i] = 0;
    }
    args = markers + ArgsOffset / sizeof(u32);
    enabled = true;
}

/// Guest memory that may be gone by now: read only where the runtime has it mapped (its direct
/// memory is a GPU driver mapping, which process_vm_readv cannot read).
size_t ReadGuest(u64 address, void* out, size_t size) {
    int prot = 0, type = 0;
    uintptr_t end = 0;
    if (!address || !runtime_memory_vma_info(address, &prot, &type, &end) || end <= address) {
        return 0;
    }
    size = std::min<size_t>(size, end - address);
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return size;
}

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Pass:
        return "pass";
    case Kind::Draw:
        return "draw";
    case Kind::DrawIndexed:
        return "indexed draw";
    case Kind::DrawIndirect:
        return "indirect draw";
    case Kind::Dispatch:
        return "dispatch";
    case Kind::DispatchIndirect:
        return "indirect dispatch";
    }
    return "?";
}

void PrintEntry(const Stream& stream, u64 id, const char* label) {
    const Entry& entry = stream.ring[id & (RingSize - 1)];
    if (entry.id != id) {
        Out("    #%llu %s: no longer kept\n", (unsigned long long)id, label);
        return;
    }
    const Crumb& c = entry.crumb;
    Out("    #%llu %s: %s", (unsigned long long)id, label, KindName(c.kind));
    switch (c.kind) {
    case Kind::Pass:
        Out(" %s", c.name ? c.name : "?");
        break;
    case Kind::Draw:
    case Kind::DrawIndexed:
        Out(" vs %016llx ps %016llx, %u %s x %u instances", (unsigned long long)c.hash[0],
                    (unsigned long long)c.hash[1], c.count[0],
                    c.kind == Kind::Draw ? "vertices" : "indices", c.count[1]);
        break;
    case Kind::DrawIndirect: {
        Out(" vs %016llx ps %016llx, up to %u draws, stride %u, arguments at %#llx",
                    (unsigned long long)c.hash[0], (unsigned long long)c.hash[1], c.count[0],
                    c.count[1], (unsigned long long)c.address);
        u32 now[5]{};
        if (ReadGuest(c.address, now, sizeof(now)) == sizeof(now)) {
            Out(" (guest memory now %u %u %u %u %u)", now[0], now[1], now[2], now[3], now[4]);
        }
        if (c.args_slot < ArgSlots) {
            const volatile u32* read = args + c.args_slot * 4;
            Out(" (the first command the GPU read: %u %u %u)", read[0], read[1], read[2]);
        }
        break;
    }
    case Kind::Dispatch:
        Out(" cs %016llx, %ux%ux%u groups", (unsigned long long)c.hash[0], c.count[0],
                    c.count[1], c.count[2]);
        break;
    case Kind::DispatchIndirect: {
        Out(" cs %016llx, arguments at %#llx", (unsigned long long)c.hash[0],
                    (unsigned long long)c.address);
        if (c.args_slot < ArgSlots) {
            const volatile u32* read = args + c.args_slot * 4;
            Out(" (the GPU read %ux%ux%u groups)", read[0], read[1], read[2]);
        }
        Out(c.count[0] ? ", in place" : ", a copy");
        u32 now[3]{};
        if (ReadGuest(c.address, now, sizeof(now)) == sizeof(now)) {
            Out(" (guest memory now %ux%ux%u)", now[0], now[1], now[2]);
        }
        break;
    }
    }
    for (u32 i = 0; i < 2; ++i) {
        if (c.program[i]) {
            Out(", code %#llx", (unsigned long long)c.program[i]);
        }
    }
    Out("\n");
}

/// The guest code of the shaders of the stuck command, for disassembly (up to 64 KiB each:
/// the program and what follows, its OrbShdr header among it).
void DumpPrograms(const Stream& stream, u64 id) {
    const Entry& entry = stream.ring[id & (RingSize - 1)];
    if (entry.id != id) {
        return;
    }
    const char* dir = OutDir();
    for (u32 i = 0; i < 2; ++i) {
        const u64 program = entry.crumb.program[i];
        if (!program) {
            continue;
        }
        std::array<u8, 64 * 1024> code{};
        size_t size = 0;
        // Page by page: the read stops at the first page that is not mapped.
        while (size < code.size()) {
            const size_t chunk = std::min<size_t>(4096 - ((program + size) & 4095), code.size() - size);
            const size_t read = ReadGuest(program + size, code.data() + size, chunk);
            size += read;
            if (read != chunk) {
                break;
            }
        }
        char path[256];
        std::snprintf(path, sizeof(path), "%shang_shader_%016llx.bin", dir,
                      (unsigned long long)entry.crumb.hash[i]);
        if (FILE* file = std::fopen(path, "wb")) {
            std::fwrite(code.data(), 1, size, file);
            std::fclose(file);
            Out("    shader %016llx code (%zu bytes from %#llx) saved to %s\n",
                        (unsigned long long)entry.crumb.hash[i], size,
                        (unsigned long long)program, path);
        }
        // And what the recompiler made of it (the shader cache's SPIR-V of each permutation),
        // so a report names the recompiler output too (spirv-dis / spirv-cross).
        auto& db = Storage::DataBase::Instance();
        if (!db.IsOpened()) {
            continue;
        }
        for (u32 perm = 0; perm < 4; ++perm) {
            std::vector<u32> spv;
            db.Load(Storage::BlobType::ShaderBinary,
                    fmt::format("{:#018x}_{}", entry.crumb.hash[i], perm), spv);
            if (spv.empty()) {
                continue;
            }
            std::snprintf(path, sizeof(path), "%shang_shader_%016llx_%u.spv", dir,
                          (unsigned long long)entry.crumb.hash[i], perm);
            if (FILE* file = std::fopen(path, "wb")) {
                std::fwrite(spv.data(), sizeof(u32), spv.size(), file);
                std::fclose(file);
                Out("    shader %016llx permutation %u SPIR-V (%zu bytes) saved to %s\n",
                    (unsigned long long)entry.crumb.hash[i], perm, spv.size() * sizeof(u32), path);
            }
        }
    }
}

void StartReportTest();
} // Anonymous namespace

u32 NewStream(const Instance& instance, const char* name) {
    std::call_once(init_once, [&] {
        Init(instance);
        std::printf("GPU breadcrumbs: %s; device fault info (VK_EXT_device_fault): %s\n",
                    enabled ? "on" : "off", instance.IsDeviceFaultSupported() ? "on" : "off");
        StartReportTest();
    });
    const u32 index = num_streams.fetch_add(1, std::memory_order_relaxed);
    if (index >= MaxStreams) {
        return MaxStreams;
    }
    streams[index].name = name;
    streams[index].ring = std::make_unique<Entry[]>(RingSize);
    return index;
}

bool Enabled() noexcept {
    return enabled;
}

u32 Note(u32 stream_index, const Crumb& crumb) {
    if (stream_index >= MaxStreams) {
        return 0;
    }
    Stream& stream = streams[stream_index];
    const u64 id = stream.next.fetch_add(1, std::memory_order_relaxed);
    Entry& entry = stream.ring[id & (RingSize - 1)];
    entry.crumb = crumb;
    entry.id = id;
    if (crumb.args_slot < ArgSlots && crumb.kind == Kind::DispatchIndirect) {
        args_notes[crumb.args_slot] = {id, stream_index, crumb.hash[0]};
    }
    return u32(id);
}

void Mark(vk::CommandBuffer cmdbuf, u32 stream, u32 id, bool after) {
    if (!enabled || stream >= MaxStreams) {
        return;
    }
    cmdbuf.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eBottomOfPipe, marker_buffer,
                                stream * StreamStride, id * 2 + (after ? 1 : 0));
}

u32 ArgsSlot() {
    return enabled ? next_args_slot.fetch_add(1, std::memory_order_relaxed) % ArgSlots : ~0u;
}

void CopyArgs(vk::CommandBuffer cmdbuf, vk::Buffer source, u64 offset, u32 slot) {
    if (!enabled || slot >= ArgSlots) {
        return;
    }
    cmdbuf.copyBuffer(source, marker_buffer,
                      vk::BufferCopy{offset, ArgsOffset + u64(slot) * 16, 3 * sizeof(u32)});
}

namespace {
void Report(const char* title, const char* where);

/// Opens out/<prefix>_<time>.log for the report's copy.
void OpenReportFile(const char* prefix) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char path[256];
    std::snprintf(path, sizeof(path), "%s%s_%04d%02d%02d_%02d%02d%02d.log", OutDir(), prefix,
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    report_file = std::fopen(path, "w");
    if (report_file) {
        std::printf("GPU report saved to %s\n", path);
    }
}

void CloseReportFile() {
    if (report_file) {
        std::fclose(report_file);
        report_file = nullptr;
    }
    std::fflush(stdout);
}

const char* FaultAddressTypeName(VkDeviceFaultAddressTypeEXT type) {
    switch (type) {
    case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT:
        return "none";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:
        return "invalid read";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:
        return "invalid write";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:
        return "invalid execute";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
        return "instruction pointer (unknown)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
        return "instruction pointer (invalid)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
        return "instruction pointer (fault)";
    default:
        return "?";
    }
}

/// VK_EXT_device_fault: what the driver says the GPU faulted on (a page fault names the address:
/// an out-of-bounds or freed buffer; none and a timeout: a shader or a command that ran too long).
void ReportFault() {
    if (!the_instance || !the_instance->IsDeviceFaultSupported() ||
        !VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT) {
        Out("GPU fault info: VK_EXT_device_fault unavailable\n");
        return;
    }
    const VkDevice device = the_instance->GetDevice();
    VkDeviceFaultCountsEXT counts{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult r = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT(device, &counts, nullptr);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        Out("GPU fault info: unavailable (%d)\n", int(r));
        return;
    }
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
    VkDeviceFaultInfoEXT info{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
    info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
    info.pVendorInfos = vendor.empty() ? nullptr : vendor.data();
    counts.vendorBinarySize = 0; // no vendor crash dump
    r = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT(device, &counts, &info);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        Out("GPU fault info: unavailable (%d)\n", int(r));
        return;
    }
    Out("GPU fault info: \"%s\", %u address(es), %u vendor record(s)\n", info.description,
        counts.addressInfoCount, counts.vendorInfoCount);
    for (u32 i = 0; i < counts.addressInfoCount; ++i) {
        const auto& a = addresses[i];
        const u64 mask = a.addressPrecision ? ~(u64(a.addressPrecision) - 1) : ~0ull;
        Out("    %s at %#llx (within %#llx..%#llx)\n", FaultAddressTypeName(a.addressType),
            (unsigned long long)a.reportedAddress, (unsigned long long)(a.reportedAddress & mask),
            (unsigned long long)((a.reportedAddress & mask) + (a.addressPrecision ? a.addressPrecision - 1 : 0)));
    }
    for (u32 i = 0; i < counts.vendorInfoCount; ++i) {
        Out("    vendor: \"%s\" code %#llx data %#llx\n", vendor[i].description,
            (unsigned long long)vendor[i].vendorFaultCode,
            (unsigned long long)vendor[i].vendorFaultData);
    }
}

/// Video memory in use against the budget (an overcommitted heap pages on every submit, and a
/// long enough page-in is a TDR).
void ReportMemory() {
    if (!the_instance || !the_instance->GetAllocator()) {
        return;
    }
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(the_instance->GetAllocator(), budgets.data());
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(the_instance->GetAllocator(), &props);
    if (!props) {
        return;
    }
    Out("GPU memory at the report:");
    for (u32 h = 0; h < props->memoryHeapCount; ++h) {
        Out(" heap %u%s %llu of %llu MiB budget (%llu MiB allocated by us);", h,
            (props->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (VRAM)" : "",
            (unsigned long long)(budgets[h].usage >> 20),
            (unsigned long long)(budgets[h].budget >> 20),
            (unsigned long long)(budgets[h].statistics.allocationBytes >> 20));
    }
    Out("\n");
}

/// Command buffers cut short by a bad packet header (NoteDroppedCommands).
std::atomic<u64> dropped_buffers{0};
std::atomic<const char*> dropped_queue{nullptr};
std::atomic<u32> dropped_dword{0};
std::atomic<u64> dropped_offset{0}, dropped_submission{0};

void ReportDroppedCommands() {
    const u64 count = dropped_buffers.load();
    if (count == 0) {
        return;
    }
    const char* queue = dropped_queue.load();
    Out("Command buffers cut short at a bad PM4 header: %llu (last: %s, dword %#010x at +%#llx, "
        "submission %llu)\n",
        (unsigned long long)count, queue ? queue : "?", dropped_dword.load(),
        (unsigned long long)dropped_offset.load(), (unsigned long long)dropped_submission.load());
}

/// The device-lost report: fault info, memory, breadcrumbs (and the stuck shaders' code).
void WriteDeviceLostReport(const char* prefix, const char* title, const char* where) {
    OpenReportFile(prefix);
    Out("%s (%s)\n", title, where);
    ReportFault();
    ReportMemory();
    ReportDroppedCommands();
    if (enabled) {
        Report("at device lost", where);
    } else {
        Out("GPU breadcrumbs: off, the stuck command is unknown\n");
    }
    CloseReportFile();
}

/// Test aid BB_DEVICE_LOST_REPORT_TEST=S: S s after the first command stream, the device-lost
/// report of a healthy GPU (out/device_lost_test_*.log), to check the report path end to end
/// (VK_EXT_device_fault answers with no fault, the streams finished all or nearly all).
void StartReportTest() {
    const char* env = std::getenv("BB_DEVICE_LOST_REPORT_TEST");
    if (!env || !env[0]) {
        return;
    }
    const int seconds = std::atoi(env);
    std::thread([seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds > 0 ? seconds : 60));
        WriteDeviceLostReport("device_lost_test", "GPU device lost report test, the device is fine",
                              "BB_DEVICE_LOST_REPORT_TEST");
        // The shader dump of a stuck command, here for the newest dispatch noted.
        const u32 count = std::min(num_streams.load(), MaxStreams);
        for (u32 s = 0; s < count; ++s) {
            const Stream& stream = streams[s];
            for (u64 id = stream.next.load() - 1, n = 0; id > 0 && n < 4096; --id, ++n) {
                const Entry& entry = stream.ring[id & (RingSize - 1)];
                if (entry.id == id && entry.crumb.kind == Kind::Dispatch && entry.crumb.program[0]) {
                    OpenReportFile("device_lost_test_dump");
                    PrintEntry(stream, id, "test dump");
                    DumpPrograms(stream, id);
                    CloseReportFile();
                    return;
                }
            }
        }
    }).detach();
}
} // Anonymous namespace

void NoteDroppedCommands(const char* queue, u32 dword, u64 offset, u64 submission) {
    dropped_queue = queue;
    dropped_dword = dword;
    dropped_offset = offset;
    dropped_submission = submission;
    ++dropped_buffers;
}

void ReportDeviceLost(const char* where) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    static std::atomic<bool> written{false};
    if (reported.test_and_set()) {
        // Another thread writes the report: the caller asserts next, which would end the process
        // with the report half written. Wait for it (bounded).
        for (int i = 0; i < 500 && !written.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return;
    }
    WriteDeviceLostReport("device_lost", "GPU device lost", where);
    written = true;
}

void ReportStuck(const char* where) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (enabled && !reported.test_and_set()) {
        OpenReportFile("gpu_stuck");
        ReportMemory();
        Report("while the GPU makes no progress", where);
        CloseReportFile();
    }
}

namespace {
void Report(const char* title, const char* where) {
    Out("GPU breadcrumbs %s (%s): what each command stream finished\n", title, where);
    const u32 count = std::min(num_streams.load(), MaxStreams);
    for (u32 s = 0; s < count; ++s) {
        const Stream& stream = streams[s];
        const u64 last = stream.next.load() - 1;
        const u32 marker = markers[s * StreamStride / sizeof(u32)];
        Out("  stream %u (%s): %llu commands noted", s, stream.name,
                    (unsigned long long)last);
        if (last == 0) {
            Out("\n");
            continue;
        }
        if (marker == 0) {
            Out(", the GPU finished none\n");
            continue;
        }
        // The marker holds the low 32 bits of 2 * id + after.
        const u64 newest = last * 2 + 1;
        const u64 value = newest - u32(u32(newest) - marker);
        const u64 id = value / 2;
        if (value & 1) {
            if (id == last) {
                Out(", the GPU finished all\n");
                continue;
            }
            Out(", the GPU finished #%llu but did not start #%llu: stuck in the work "
                        "recorded between them (copies, clears, barriers) or before it\n",
                        (unsigned long long)id, (unsigned long long)(id + 1));
            PrintEntry(stream, id, "finished");
            PrintEntry(stream, id + 1, "next");
        } else {
            Out(", the GPU started #%llu and did not finish it (%llu noted after it)\n",
                        (unsigned long long)id, (unsigned long long)(last - id));
            if (id > 1) {
                PrintEntry(stream, id - 1, "finished");
            }
            PrintEntry(stream, id, "STUCK");
            std::fflush(stdout);
            DumpPrograms(stream, id);
        }
        for (u64 next = id + 2; next <= std::min(last, id + 4); ++next) {
            PrintEntry(stream, next, "later");
        }
    }
    std::fflush(stdout);
}
} // Anonymous namespace


void PrintProgress() {
    if (!enabled) {
        return;
    }
    std::printf("GPU breadcrumbs:");
    const u32 count = std::min(num_streams.load(), MaxStreams);
    std::array<u64, MaxStreams> finished{};
    for (u32 s = 0; s < count; ++s) {
        const u64 last = streams[s].next.load(std::memory_order_relaxed) - 1;
        if (last == 0) {
            continue;
        }
        const u32 marker = markers[s * StreamStride / sizeof(u32)];
        const u64 newest = last * 2 + 1;
        const u64 value = marker ? newest - u32(u32(newest) - marker) : 0;
        // 2n + 1: n finished; 2n: n started, n - 1 finished.
        finished[s] = value ? value / 2 - (value & 1 ? 0 : 1) : 0;
        std::printf(" stream %u (%s) %llu noted, %llu unfinished;", s, streams[s].name,
                    (unsigned long long)last, (unsigned long long)(last - finished[s]));
    }
    // The largest indirect dispatches among the last ArgSlots the GPU finished, one per shader:
    // counts that only grow from frame to frame end in a dispatch that never finishes.
    struct Largest {
        u64 hash;
        u64 groups;
        u32 dims[3];
    };
    std::array<Largest, 3> largest{};
    for (u32 slot = 0; slot < ArgSlots; ++slot) {
        const ArgsNote note = args_notes[slot];
        if (note.id == 0 || note.stream >= count || note.id > finished[note.stream]) {
            continue;
        }
        const u32 x = args[slot * 4], y = args[slot * 4 + 1], z = args[slot * 4 + 2];
        const u64 groups = u64(x) * y * z;
        auto it = std::find_if(largest.begin(), largest.end(),
                               [&](const Largest& l) { return l.hash == note.hash; });
        if (it == largest.end()) {
            it = std::min_element(largest.begin(), largest.end(),
                                  [](const Largest& a, const Largest& b) { return a.groups < b.groups; });
            if (it->groups >= groups) {
                continue;
            }
            *it = {note.hash, 0, {}};
        }
        if (groups >= it->groups) {
            *it = {note.hash, groups, {x, y, z}};
        }
    }
    std::sort(largest.begin(), largest.end(),
              [](const Largest& a, const Largest& b) { return a.groups > b.groups; });
    std::printf(" largest indirect dispatches:");
    for (const auto& l : largest) {
        if (l.groups != 0) {
            std::printf(" cs %016llx %ux%ux%u;", (unsigned long long)l.hash, l.dims[0], l.dims[1],
                        l.dims[2]);
        }
    }
    std::printf("\n");
}

} // namespace Vulkan::Breadcrumbs
