// Audio Phase 0 capture (docs/audio-roadmap.md). With X2W_AUDIO_TRACE=<prefix> every audio stand-in
// call appends one fixed 256-byte record to <prefix>.<module>.bin: QPC time, thread, export, six
// argument registers, result, return address and up to 176 payload bytes the call points at.
// Logging only: no argument, result or memory the caller sees is changed. Unset means no cost beyond
// one environment read per call. tools/startup/audio_trace.py decodes and summarises the files.
#pragma once

#include "startup_stubs/local_log.h"

#include <cstdint>
#include <cstring>
#include <intrin.h>

namespace x2w_audio_trace {

constexpr std::uint32_t magic = 0x52544158;  // "XATR"
constexpr std::size_t payload_max = 176;

#pragma pack(push, 1)
struct Record {
    std::uint64_t qpc;
    std::uint32_t thread;
    std::uint8_t module;    // X2W_TRAP_MODULE_ID: 1 AcpHal, 2 chad, 3 xapu
    std::uint8_t function;  // per-module export number; 0xFF is the file header
    std::uint16_t payload_bytes;
    std::uint64_t args[6];
    std::int64_t result;
    std::uint64_t return_address;
    std::uint8_t payload[payload_max];
};
#pragma pack(pop)
static_assert(sizeof(Record) == 256, "the decoder reads 256-byte records");

struct File {
    INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    HANDLE handle = INVALID_HANDLE_VALUE;
    SRWLOCK lock = SRWLOCK_INIT;
};

inline File& file() noexcept
{
    static File f;
    return f;
}

inline BOOL CALLBACK open_file(PINIT_ONCE, PVOID module_name, PVOID*) noexcept
{
    char prefix[MAX_PATH - 32];
    const DWORD n = GetEnvironmentVariableA("X2W_AUDIO_TRACE", prefix, sizeof(prefix));
    if (!n || n >= sizeof(prefix)) return TRUE;
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s.%s.bin", prefix, static_cast<const char*>(module_name));
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        x2w_local::logf("x2w-audio trace: cannot create %s (error %lu)", path, GetLastError());
        return TRUE;
    }
    // Header: args[0] QPC frequency, args[1] main image base, args[2] this module's base, args[3] magic.
    Record header{};
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    header.qpc = static_cast<std::uint64_t>(now.QuadPart);
    header.thread = GetCurrentThreadId();
    header.function = 0xFF;
    header.args[0] = static_cast<std::uint64_t>(frequency.QuadPart);
    header.args[1] = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&open_file), &self);
    header.args[2] = reinterpret_cast<std::uint64_t>(self);
    header.args[3] = magic;
    DWORD written = 0;
    WriteFile(h, &header, sizeof(header), &written, nullptr);
    file().handle = h;
    x2w_local::logf("x2w-audio trace: writing %s", path);
    return TRUE;
}

inline bool enabled() noexcept
{
    const DWORD saved = GetLastError();
    const bool on = GetEnvironmentVariableA("X2W_AUDIO_TRACE", nullptr, 0) != 0;
    SetLastError(saved);
    return on;
}

// payload may be null or unreadable; only the readable prefix (up to payload_max) is copied.
// shortcut: one WriteFile per call under a lock, about microseconds; fine for the observed rates
// (hundreds of calls per second), move to a ring and writer thread if a run shows it perturbs timing.
inline void emit(const char* module_name, std::uint8_t module, std::uint8_t function, std::uint64_t a0, std::uint64_t a1,
                 std::uint64_t a2, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::int64_t result,
                 void* return_address, const void* payload = nullptr, std::size_t payload_bytes = 0) noexcept
{
    if (!enabled()) return;
    const DWORD saved = GetLastError();
    File& f = file();
    InitOnceExecuteOnce(&f.once, open_file, const_cast<char*>(module_name), nullptr);
    if (f.handle != INVALID_HANDLE_VALUE) {
        Record r{};
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        r.qpc = static_cast<std::uint64_t>(now.QuadPart);
        r.thread = GetCurrentThreadId();
        r.module = module;
        r.function = function;
        r.args[0] = a0, r.args[1] = a1, r.args[2] = a2, r.args[3] = a3, r.args[4] = a4, r.args[5] = a5;
        r.result = result;
        r.return_address = reinterpret_cast<std::uint64_t>(return_address);
        if (payload && payload_bytes) {
            const std::size_t want = payload_bytes < payload_max ? payload_bytes : payload_max;
            const std::size_t have = x2w_local::probe_readable(reinterpret_cast<std::uintptr_t>(payload), want);
            std::memcpy(r.payload, payload, have);
            r.payload_bytes = static_cast<std::uint16_t>(have);
        }
        AcquireSRWLockExclusive(&f.lock);
        DWORD written = 0;
        WriteFile(f.handle, &r, sizeof(r), &written, nullptr);
        ReleaseSRWLockExclusive(&f.lock);
    }
    SetLastError(saved);
}

}  // namespace x2w_audio_trace

// Per-module shorthand; X2W_TRAP_MODULE / X2W_TRAP_MODULE_ID come from the including stub.
#define X2W_AUDIO_TRACE(function, a0, a1, a2, a3, a4, a5, result, ...)                                         \
    x2w_audio_trace::emit(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, function, (std::uint64_t)(a0), (std::uint64_t)(a1), \
                          (std::uint64_t)(a2), (std::uint64_t)(a3), (std::uint64_t)(a4), (std::uint64_t)(a5),      \
                          (std::int64_t)(result), _ReturnAddress(), ##__VA_ARGS__)
