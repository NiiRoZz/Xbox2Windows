// XMA context plumbing of the AcpHal prototype (X2W_ACPHAL_XMA=1, built with X2W_FFMPEG_DIR).
// No XMA encoder exists, so this checks the context state machine, not decoded audio: a started context
// consumes its input buffer and hands it back (valid bit cleared, current buffer switched), the output
// write offset stays inside the ring, a consume command moves the read offset, a stopped context leaves a
// fresh buffer alone, and with the switch unset nothing is consumed (negative control).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
using u64 = std::uint64_t;
using CreateHeapFn = long(__cdecl*)(u64, u64);
using AllocFn = long(__cdecl*)(void**, std::uint32_t*, u64, u64, u64);
using ShapeAllocFn = long(__cdecl*)(void*);
using HalCreateFn = long(__cdecl*)(void**);
using HalSubmitFn = long(__stdcall*)(void*, unsigned, u64, unsigned, const unsigned*, u64);

int failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) ++failures, std::printf("FAIL: %s\n", what);
}

volatile std::uint32_t* ctx_at(unsigned char* contexts, unsigned i) { return reinterpret_cast<volatile std::uint32_t*>(contexts + i * 64); }

// Polls until the predicate holds or the timeout passes.
template <class F>
bool wait_for(F f, DWORD ms)
{
    for (DWORD waited = 0; waited < ms; waited += 10) {
        if (f()) return true;
        Sleep(10);
    }
    return f();
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 3) return std::printf("usage: startup_acphal_xma_check <AcpHal.dll> <ffmpeg bin dir>\n"), 2;
    SetEnvironmentVariableA("X2W_AUDIO_PERMISSIVE", "1");
    SetEnvironmentVariableA("X2W_FFMPEG_BIN", argv[2]);
    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) return std::printf("FAIL: cannot load %s\n", argv[1]), 2;
    auto create_heap = reinterpret_cast<CreateHeapFn>(GetProcAddress(m, "ApuCreateHeap"));
    auto alloc = reinterpret_cast<AllocFn>(GetProcAddress(m, "ApuAlloc"));
    auto shape_alloc = reinterpret_cast<ShapeAllocFn>(GetProcAddress(m, "AcpHalAllocateShapeContexts"));
    auto hal_create = reinterpret_cast<HalCreateFn>(GetProcAddress(m, "AcpHalCreate"));
    if (!create_heap || !alloc || !shape_alloc || !hal_create) return std::printf("FAIL: missing export\n"), 2;

    check(create_heap(0x100000, 0x100000) == 0, "ApuCreateHeap");
    unsigned char block[0x60]{};
    const std::uint32_t xma_contexts = 3;
    std::memcpy(block + 0x10, &xma_contexts, 4);
    check(shape_alloc(block) == 0, "AcpHalAllocateShapeContexts (3 XMA contexts)");
    unsigned char* contexts = nullptr;
    std::memcpy(&contexts, block + 0x38, 8);
    void* hal = nullptr;
    check(hal_create(&hal) == 0 && hal, "AcpHalCreate");
    if (!contexts || !hal) return 1;
    auto submit = reinterpret_cast<HalSubmitFn>((*reinterpret_cast<void* const* const*>(hal))[2]);

    // Two packets of input (zeros: not valid XMA, so the decoder may produce nothing) and a 23-block ring.
    void* input = nullptr;
    void* ring = nullptr;
    std::uint32_t input_apu = 0, ring_apu = 0;
    check(alloc(&input, &input_apu, 2 * 2048, 0x800, 0) == 0, "ApuAlloc input");
    check(alloc(&ring, &ring_apu, 23 * 256, 0x100, 0) == 0, "ApuAlloc ring");
    auto setup = [&](unsigned i) {
        volatile std::uint32_t* c = ctx_at(contexts, i);
        c[0] = 2u | (1u << 20) | (23u << 22);  // 2 packets, input 0 valid, 23 output blocks, write offset 0
        c[1] = (3u << 27) | (1u << 31);       // 48 kHz, mono, output valid
        c[4] = 0;                              // current buffer 0
        c[5] = input_apu;
        c[7] = ring_apu;
        c[9] = 0;
    };

    // Negative control: switch unset, a start must not consume anything.
    setup(1);
    const unsigned one[2] = {1, 0};
    submit(hal, 5, 0xF6F6F6F6, 0xFFFFFFFF, one, 0);
    Sleep(300);
    check((ctx_at(contexts, 1)[0] >> 20) & 1, "switch unset: input buffer stays valid");

    SetEnvironmentVariableA("X2W_ACPHAL_XMA", "1");
    setup(0);
    const unsigned zero[2] = {0, 0};
    submit(hal, 5, 0xF6F6F6F6, 0xFFFFFFFF, zero, 0);
    volatile std::uint32_t* c0 = ctx_at(contexts, 0);
    check(wait_for([&] { return ((c0[0] >> 20) & 1) == 0; }, 3000), "started: input buffer 0 drained and handed back");
    check((c0[4] >> 31) == 1, "started: current buffer switched to 1");
    const unsigned write = (c0[0] >> 27) & 31;
    check(write < 23, "write offset stays inside the ring");

    const unsigned consume[2] = {0, write};
    submit(hal, 0x12, 0xF1F1F1F1, 0xFFFFFFFF, consume, 0);
    check((c0[9] & 31) == write % 23, "consume moves the read offset by the consumed blocks");
    check((c0[1] >> 31) == 1, "consume leaves the output valid (room in the ring)");

    submit(hal, 7, 1, 0xFFFFFFFF, zero, 0);
    c0[0] |= 1u << 20;
    c0[4] = 0;
    Sleep(300);
    check((c0[0] >> 20) & 1, "stopped: a refilled input buffer is left alone");

    std::printf(failures ? "startup_acphal_xma_check: %d failure(s)\n" : "startup_acphal_xma_check: ok\n", failures);
    return failures ? 1 : 0;
}
