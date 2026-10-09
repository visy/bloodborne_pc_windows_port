// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// bbport: small host helpers for diagnostics code shared by the Linux and Windows builds:
// reading memory that may be unmapped without faulting, the calling thread's id and name,
// naming a host code address (dladdr), and the registers of a fault context.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace BbHost {

/// Copies up to `bytes` from `src` (memory of this process that may be unmapped) without
/// faulting. Returns the number of bytes copied (Linux: process_vm_readv on ourselves; Windows:
/// ReadProcessMemory on the current process).
inline std::size_t ReadNoFault(void* dst, const void* src, std::size_t bytes) {
#ifdef _WIN32
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), src, dst, bytes, &read)) {
        return std::size_t(read); // a partial copy reports what it read
    }
    return std::size_t(read);
#else
    iovec local{dst, bytes}, remote{const_cast<void*>(src), bytes};
    const ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return got > 0 ? std::size_t(got) : 0;
#endif
}

/// The calling thread's id (Linux: gettid; Windows: GetCurrentThreadId).
inline std::uint32_t ThreadId() {
#ifdef _WIN32
    return std::uint32_t(GetCurrentThreadId());
#else
    return std::uint32_t(gettid());
#endif
}

/// The calling thread's name into `out` (at most `size` bytes with the terminator).
inline void CurrentThreadName(char* out, std::size_t size) {
    if (size == 0) {
        return;
    }
    out[0] = '\0';
#ifdef _WIN32
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                               "GetThreadDescription")));
    PWSTR description = nullptr;
    if (get_description && SUCCEEDED(get_description(GetCurrentThread(), &description)) &&
        description) {
        WideCharToMultiByte(CP_UTF8, 0, description, -1, out, int(size), nullptr, nullptr);
        out[size - 1] = '\0';
        LocalFree(description);
    }
#else
    pthread_getname_np(pthread_self(), out, size);
#endif
}

/// What dladdr tells about a host code address: the module (file name, base) and, where
/// available (Linux), the nearest exported symbol.
struct AddressInfo {
    std::string module;      ///< full path, empty if unknown
    std::uintptr_t base = 0; ///< module load address
    std::string symbol;      ///< empty if unknown (always on Windows)
    std::uintptr_t symbol_address = 0;
};

inline bool DescribeAddress(const void* address, AddressInfo& info) {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(address), &module) ||
        !module) {
        return false;
    }
    char path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
    info.module.assign(path, length);
    info.base = reinterpret_cast<std::uintptr_t>(module);
    return true;
#else
    Dl_info dl{};
    if (!dladdr(address, &dl)) {
        return false;
    }
    info.module = dl.dli_fname ? dl.dli_fname : "";
    info.base = reinterpret_cast<std::uintptr_t>(dl.dli_fbase);
    info.symbol = dl.dli_sname ? dl.dli_sname : "";
    info.symbol_address = reinterpret_cast<std::uintptr_t>(dl.dli_saddr);
    return true;
#endif
}

/// Registers of a fault context as passed to the access violation handlers (Linux: ucontext_t*;
/// Windows: EXCEPTION_POINTERS*).
struct FaultRegisters {
    std::uint64_t rip = 0, rsp = 0, rbp = 0;
};

inline FaultRegisters GetFaultRegisters(const void* context) {
    FaultRegisters regs;
#ifdef _WIN32
    const auto* record = static_cast<const EXCEPTION_POINTERS*>(context)->ContextRecord;
    regs.rip = record->Rip;
    regs.rsp = record->Rsp;
    regs.rbp = record->Rbp;
#else
    const auto* g = static_cast<const ucontext_t*>(context)->uc_mcontext.gregs;
    regs.rip = std::uint64_t(g[REG_RIP]);
    regs.rsp = std::uint64_t(g[REG_RSP]);
    regs.rbp = std::uint64_t(g[REG_RBP]);
#endif
    return regs;
}

} // namespace BbHost
