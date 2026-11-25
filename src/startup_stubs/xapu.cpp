// Desktop xapu.dll trap stubs.
// Every export traps; see trap.h. Scaffolding, not an ABI claim.
#define X2W_TRAP_MODULE "xapu.dll"
#define X2W_TRAP_MODULE_ID 3
#include "startup_stubs/trap.h"
#include "startup_stubs/local_log.h"
#include "startup_stubs/audio_trace.h"
#include <timeapi.h>
#include <cstring>
#include <new>
#ifdef X2W_HAVE_OPUS
#include <opus.h>
#endif

// Experimental (docs/startup-runtime.md). With X2W_AUDIO_PERMISSIVE=1 the exports
// stand in for the audio command queue; otherwise they trap. Call shapes from the title:
//   XApuConnect(input*, output*, handle*) -> HRESULT (the handle is later passed to the others)
//   XApuGetResultCount(handle, 0) -> count
//   XApuDequeueResult(handle, result*) -> HRESULT; the caller zeroes 0x58 bytes and reads a type at
//     +0 (1..5 seen in its switch), a slot index at +4 and a value at +0x34
//   XApuEnqueueCommand(handle, command*, ...) -> HRESULT
//   XApuDisconnect(handle)
// Under X2W_AUDIO_PERMISSIVE=1 each active connection tracks its pending completion count so
// enqueued commands can be retired cleanly without fabricating unmeasured result fields.
// Experiment (docs/audio-roadmap.md Phase 0), X2W_XAPU_SLOT_RESULTS=1: commands are read as the title
// builds them ({u32 type, u32 slot, ...}). Types 1 (open), 2 (close) and 5 (reset) complete with a
// result {type at +0, slot at +4}, the fields the title's result handler matches. Type 3 (decode a
// packet) and unknown types get no result: there is no decoder, so nothing is claimed for them.
// In that mode XApuConnect also provides the shared buffer: the title reads one pointer from the first
// qword of the 16-byte output and places packets at base + slot * 640 and decoded floats from
// base + 0x14000. Its size is the u32 at input +0x0C (0x104000 seen). The second qword is never read.
// Prototype (X2W_HAVE_OPUS, a build with X2W_OPUS_DIR): type 3 decodes its packet with libopus into the
// command's output buffer and completes with {3, slot, decoded bytes at +0x34}; the title reads that byte
// count as float samples. Packets observed in a run carry an Opus TOC byte (20 ms CELT frames).
namespace {
using u64 = std::uint64_t;
constexpr long hr_ok = 0;
constexpr long hr_pointer = static_cast<long>(0x80004003);  // E_POINTER
constexpr long hr_fail = static_cast<long>(0x80004005);     // E_FAIL
constexpr long hr_handle = static_cast<long>(0x80070006);   // E_HANDLE
constexpr long hr_out_of_memory = static_cast<long>(0x8007000E); // E_OUTOFMEMORY
constexpr std::size_t result_size = 0x58;

bool permissive() noexcept { return x2w_local::env_on("X2W_AUDIO_PERMISSIVE"); }

bool slot_results() noexcept { return x2w_local::env_on("X2W_XAPU_SLOT_RESULTS"); }

constexpr std::uint32_t result_ring = 4096;
struct SlotResult {
    std::uint32_t type, slot;
    std::uint32_t value = 0;  // written at result +0x34
};

struct Connection {
    Connection* next = nullptr;
    u64 handle = 0;
    std::uint64_t pending_results = 0;  // placeholder mode: count of zero results owed
    SlotResult ring[result_ring]{};     // slot-result mode: FIFO of completions
    std::uint32_t ring_head = 0, ring_count = 0;
    void* buffer = nullptr;             // slot-result mode: shared buffer returned by XApuConnect
};
constexpr std::uint32_t max_buffer_bytes = 64u << 20;

SRWLOCK connection_lock = SRWLOCK_INIT;
Connection* connections = nullptr;
u64 next_handle = 0x5841500000000001ULL;
bool timer_active = false;

Connection* find_connection(u64 handle) noexcept
{
    for (auto* conn = connections; conn; conn = conn->next) {
        if (conn->handle == handle) return conn;
    }
    return nullptr;
}

void hex(const void* data, std::size_t bytes, char* out, std::size_t size)
{
    out[0] = 0;
    if (!data || !x2w_local::readable(data, bytes)) return;
    const auto* p = static_cast<const unsigned char*>(data);
    std::size_t used = 0;
    for (std::size_t i = 0; i < bytes && used + 4 < size; ++i) used += std::snprintf(out + used, size - used, "%02x ", p[i]);
}
// Trace payload for XApuConnect: the 0x18-byte input then the 0x10-byte output as the caller sees it after the call.
struct ConnectPayload {
    unsigned char bytes[0x28]{};
    ConnectPayload(u64 input, u64 output)
    {
        if (x2w_local::readable(reinterpret_cast<const void*>(input), 0x18)) std::memcpy(bytes, reinterpret_cast<const void*>(input), 0x18);
        if (x2w_local::readable(reinterpret_cast<const void*>(output), 0x10))
            std::memcpy(bytes + 0x18, reinterpret_cast<const void*>(output), 0x10);
    }
};
#ifdef X2W_HAVE_OPUS
// One decoder per title slot (the title's pool has 256). Guarded by decoder_lock.
constexpr std::uint32_t max_slots = 256;
struct SlotDecoder {
    OpusDecoder* decoder = nullptr;
    int channels = 0, rate = 0;
};
SlotDecoder decoders[max_slots];
SRWLOCK decoder_lock = SRWLOCK_INIT;

void drop_decoder(std::uint32_t slot, bool destroy) noexcept
{
    if (slot >= max_slots) return;
    AcquireSRWLockExclusive(&decoder_lock);
    SlotDecoder& d = decoders[slot];
    if (d.decoder && destroy) {
        opus_decoder_destroy(d.decoder);
        d = SlotDecoder{};
    } else if (d.decoder) {
        opus_decoder_ctl(d.decoder, OPUS_RESET_STATE);
    }
    ReleaseSRWLockExclusive(&decoder_lock);
}

// Decodes a type 3 command; returns the decoded byte count for result +0x34 (0 when nothing was decoded).
// shortcut: decodes synchronously on the enqueuing thread (about 0.1 ms per 20 ms frame); move to a worker
// that completes on the audio clock if a run shows the title depends on completion timing.
std::uint32_t decode_packet(const unsigned char* c) noexcept
{
    std::uint32_t slot, frames, bytes, out_bytes;
    float rate_f;
    u64 packet, out;
    std::memcpy(&slot, c + 4, 4);
    std::memcpy(&rate_f, c + 0x1C, 4);
    std::memcpy(&frames, c + 0x24, 4);
    std::memcpy(&bytes, c + 0x28, 4);
    std::memcpy(&packet, c + 0x2C, 8);
    std::memcpy(&out_bytes, c + 0x34, 4);
    std::memcpy(&out, c + 0x38, 8);
    const int channels = static_cast<int>(out_bytes / 0xF00);  // the builder stores channels * 0xF00
    const int rate = static_cast<int>(rate_f);
    const bool rate_ok = rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
    if (slot >= max_slots || (channels != 1 && channels != 2) || !rate_ok || frames == 0 ||
        u64{frames} * channels * 4 > out_bytes || bytes == 0 ||
        !x2w_local::readable(reinterpret_cast<const void*>(packet), bytes) ||
        !x2w_local::writable(reinterpret_cast<const void*>(out), out_bytes))
        return 0;
    int decoded = OPUS_INTERNAL_ERROR;
    AcquireSRWLockExclusive(&decoder_lock);
    SlotDecoder& d = decoders[slot];
    if (d.decoder && (d.channels != channels || d.rate != rate)) {
        opus_decoder_destroy(d.decoder);
        d = SlotDecoder{};
    }
    if (!d.decoder) {
        int error = OPUS_OK;
        d.decoder = opus_decoder_create(rate, channels, &error);
        if (error != OPUS_OK) d.decoder = nullptr;
        d.channels = channels;
        d.rate = rate;
    }
    if (d.decoder)
        decoded = opus_decode_float(d.decoder, reinterpret_cast<const unsigned char*>(packet), static_cast<opus_int32>(bytes),
                                    reinterpret_cast<float*>(out), static_cast<int>(frames), 0);
    ReleaseSRWLockExclusive(&decoder_lock);
    static LONG errors;
    if (decoded < 0) {
        if (x2w_local::first_calls(errors, 8))
            x2w_local::logf("x2w-audio XApu decode slot=%u bytes=%u -> opus error %d", slot, bytes, decoded);
        return 0;
    }
    return static_cast<std::uint32_t>(decoded) * static_cast<std::uint32_t>(channels) * 4u;
}
#endif

// Decode command (type 3) as its builder fills it: +0x24 frames, +0x28 input bytes, +0x2C input pointer.
// Logs the first few and traces the packet bytes as function 0x83 (args: slot, bytes, frames, pointer).
void trace_decode(u64 command, void* return_address)
{
    unsigned char c[0x40];
    if (!x2w_local::readable(reinterpret_cast<const void*>(command), sizeof(c))) return;
    std::memcpy(c, reinterpret_cast<const void*>(command), sizeof(c));
    std::uint32_t slot, frames, bytes;
    u64 packet;
    std::memcpy(&slot, c + 4, 4);
    std::memcpy(&frames, c + 0x24, 4);
    std::memcpy(&bytes, c + 0x28, 4);
    std::memcpy(&packet, c + 0x2C, 8);
    static LONG calls;
    if (x2w_local::first_calls(calls, 8)) {
        char head[3 * 16 + 1];
        hex(reinterpret_cast<const void*>(packet), bytes < 16 ? bytes : 16, head, sizeof(head));
#ifdef X2W_HAVE_OPUS
        x2w_local::logf("x2w-audio XApu decode slot=%u frames=%u bytes=%u packet=[%s] (libopus)", slot, frames, bytes, head);
#else
        x2w_local::logf("x2w-audio XApu decode slot=%u frames=%u bytes=%u packet=[%s] (no decoder: no result)", slot, frames,
                        bytes, head);
#endif
    }
    x2w_audio_trace::emit(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, 0x83, slot, bytes, frames, packet, 0, 0, 0, return_address,
                          reinterpret_cast<const void*>(packet), bytes);
}
}  // namespace

extern "C" __declspec(noinline) long XApuConnect(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XApuConnect", 0, rcx, rdx, r8, r9, _ReturnAddress());
    char input[3 * 0x18 + 1], output[3 * 0x10 + 1];
    hex(reinterpret_cast<const void*>(rcx), 0x18, input, sizeof(input));
    hex(reinterpret_cast<const void*>(rdx), 0x10, output, sizeof(output));
    if (!r8 || !x2w_local::writable(reinterpret_cast<const void*>(r8), sizeof(void*))) {
        x2w_local::logf("x2w-audio XApuConnect input=[%s] output(before)=[%s] -> E_POINTER", input, output);
        if (x2w_audio_trace::enabled()) X2W_AUDIO_TRACE(0, rcx, rdx, r8, r9, 0, 0, hr_pointer, ConnectPayload(rcx, rdx).bytes, 0x28);
        return hr_pointer;
    }
    void* buffer = nullptr;
    if (slot_results()) {
        std::uint32_t bytes = 0;
        if (!x2w_local::readable(reinterpret_cast<const void*>(rcx), 0x10) ||
            !x2w_local::writable(reinterpret_cast<const void*>(rdx), 0x10)) {
            x2w_local::logf("x2w-audio XApuConnect input=%p output=%p -> E_POINTER (slot results)", reinterpret_cast<void*>(rcx),
                            reinterpret_cast<void*>(rdx));
            X2W_AUDIO_TRACE(0, rcx, rdx, r8, r9, 0, 0, hr_pointer);
            return hr_pointer;
        }
        std::memcpy(&bytes, reinterpret_cast<const unsigned char*>(rcx) + 0x0C, 4);
        if (bytes == 0 || bytes > max_buffer_bytes) {
            x2w_local::logf("x2w-audio XApuConnect buffer bytes 0x%x out of range -> E_INVALIDARG", bytes);
            X2W_AUDIO_TRACE(0, rcx, rdx, r8, r9, 0, 0, static_cast<long>(0x80070057));
            return static_cast<long>(0x80070057);
        }
        buffer = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buffer) {
            X2W_AUDIO_TRACE(0, rcx, rdx, r8, r9, 0, 0, hr_out_of_memory);
            return hr_out_of_memory;
        }
    }
    auto* conn = new (std::nothrow) Connection;
    if (!conn) {
        if (buffer) VirtualFree(buffer, 0, MEM_RELEASE);
        X2W_AUDIO_TRACE(0, rcx, rdx, r8, r9, 0, 0, hr_out_of_memory);
        return hr_out_of_memory;
    }
    conn->buffer = buffer;
    if (buffer) *reinterpret_cast<void**>(rdx) = buffer;
    AcquireSRWLockExclusive(&connection_lock);
    const u64 handle = next_handle++;
    if (next_handle == 0) next_handle = 0x5841500000000001ULL;
    conn->handle = handle;
    conn->next = connections;
    connections = conn;
    if (!conn->next) {
        // The mix worker sleeps for 2 ms between refills. A coarse Windows tick can
        // stretch that wait beyond the time represented by the queued samples.
        timer_active = timeBeginPeriod(1) == TIMERR_NOERROR;
        if (!timer_active) x2w_local::logf("x2w-audio 1 ms mixer timer request unavailable");
    }
    ReleaseSRWLockExclusive(&connection_lock);
    *reinterpret_cast<void**>(r8) = reinterpret_cast<void*>(handle);
    x2w_local::logf("x2w-audio XApuConnect input=[%s] output(before)=[%s] -> handle %p, S_OK", input, output,
                    reinterpret_cast<void*>(handle));
    if (x2w_audio_trace::enabled()) X2W_AUDIO_TRACE(0, rcx, rdx, r8, r9, handle, 0, hr_ok, ConnectPayload(rcx, rdx).bytes, 0x28);
    return hr_ok;
}

extern "C" __declspec(noinline) std::uint32_t XApuGetResultCount(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XApuGetResultCount", 4, rcx, rdx, r8, r9, _ReturnAddress());
    AcquireSRWLockShared(&connection_lock);
    const Connection* conn = find_connection(rcx);
    const std::uint32_t count = !conn ? 0u
        : slot_results() ? conn->ring_count
        : static_cast<std::uint32_t>(conn->pending_results > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : conn->pending_results);
    ReleaseSRWLockShared(&connection_lock);
    static LONG calls;
    if (x2w_local::first_calls(calls, 2))
        x2w_local::logf("x2w-audio XApuGetResultCount handle=0x%llx rdx=0x%llx -> %u", rcx, rdx, count);
    X2W_AUDIO_TRACE(4, rcx, rdx, r8, r9, 0, 0, count);
    return count;
}

extern "C" __declspec(noinline) long XApuDequeueResult(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XApuDequeueResult", 1, rcx, rdx, r8, r9, _ReturnAddress());
    if (!rdx || !x2w_local::writable(reinterpret_cast<const void*>(rdx), result_size)) {
        static LONG bad_out_calls;
        if (x2w_local::first_calls(bad_out_calls, 2))
            x2w_local::logf("x2w-audio XApuDequeueResult handle=0x%llx -> E_POINTER", rcx);
        X2W_AUDIO_TRACE(1, rcx, rdx, r8, r9, 0, 0, hr_pointer);
        return hr_pointer;
    }
    AcquireSRWLockExclusive(&connection_lock);
    Connection* conn = find_connection(rcx);
    long hr = hr_handle;
    SlotResult popped{};
    if (conn && slot_results()) {
        if (conn->ring_count > 0) {
            popped = conn->ring[conn->ring_head];
            conn->ring_head = (conn->ring_head + 1) % result_ring;
            --conn->ring_count;
            hr = hr_ok;
        } else {
            hr = hr_fail;
        }
    } else if (conn) {
        if (conn->pending_results > 0) {
            --conn->pending_results;
            hr = hr_ok;
        } else {
            hr = hr_fail;
        }
    }
    ReleaseSRWLockExclusive(&connection_lock);
    if (hr == hr_ok) {
        std::memset(reinterpret_cast<void*>(rdx), 0, result_size);
        std::memcpy(reinterpret_cast<void*>(rdx), &popped, 8);  // type, slot; zero in placeholder mode
        std::memcpy(reinterpret_cast<unsigned char*>(rdx) + 0x34, &popped.value, 4);
    }
    static LONG calls;
    if (x2w_local::first_calls(calls, 2))
        x2w_local::logf("x2w-audio XApuDequeueResult handle=0x%llx -> 0x%08lx", rcx, static_cast<unsigned long>(hr));
    X2W_AUDIO_TRACE(1, rcx, rdx, r8, r9, 0, 0, hr);
    return hr;
}

extern "C" __declspec(noinline) long XApuEnqueueCommand(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XApuEnqueueCommand", 3, rcx, rdx, r8, r9, _ReturnAddress());
    const bool by_slot = slot_results();
    SlotResult head{};
    const bool have_head = rdx && x2w_local::readable(reinterpret_cast<const void*>(rdx), 8);
    if (have_head) std::memcpy(&head, reinterpret_cast<const void*>(rdx), 8);
#ifdef X2W_HAVE_OPUS
    const bool decodes = true;
    if (by_slot && have_head && head.type == 3) {
        unsigned char c[0x40];
        if (x2w_local::readable(reinterpret_cast<const void*>(rdx), sizeof(c))) {
            std::memcpy(c, reinterpret_cast<const void*>(rdx), sizeof(c));
            head.value = decode_packet(c);
        }
    }
    if (by_slot && have_head && (head.type == 2 || head.type == 5)) drop_decoder(head.slot, head.type == 2);
#else
    const bool decodes = false;
#endif
    AcquireSRWLockExclusive(&connection_lock);
    Connection* conn = find_connection(rcx);
    long hr = hr_handle;
    if (conn && by_slot) {
        if (!have_head) {
            hr = hr_pointer;
        } else if (head.type != 1 && head.type != 2 && head.type != 5 && !(head.type == 3 && decodes)) {
            hr = hr_ok;  // accepted, never completed (see the header comment)
        } else if (conn->ring_count == result_ring) {
            hr = hr_out_of_memory;
        } else {
            conn->ring[(conn->ring_head + conn->ring_count) % result_ring] = head;
            ++conn->ring_count;
            hr = hr_ok;
        }
    } else if (conn) {
        ++conn->pending_results;
        hr = hr_ok;
    }
    ReleaseSRWLockExclusive(&connection_lock);
    if (by_slot && have_head && head.type == 3) trace_decode(rdx, _ReturnAddress());
    static LONG calls;
    if (x2w_local::first_calls(calls, 24)) {
        char command[3 * 0x60 + 1];
        hex(reinterpret_cast<const void*>(rdx), 0x60, command, sizeof(command));
        const auto offset = reinterpret_cast<u64>(_ReturnAddress()) - reinterpret_cast<u64>(GetModuleHandleW(nullptr));
        x2w_local::logf("x2w-audio XApuEnqueueCommand handle=0x%llx r8=0x%llx r9=0x%llx ret=+0x%llx command=[%s] -> 0x%08lx",
                        rcx, r8, r9, offset, command, static_cast<unsigned long>(hr));
    }
    X2W_AUDIO_TRACE(3, rcx, rdx, r8, r9, 0, 0, hr, reinterpret_cast<const void*>(rdx), x2w_audio_trace::payload_max);
    return hr;
}

extern "C" __declspec(noinline) long XApuDisconnect(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XApuDisconnect", 2, rcx, rdx, r8, r9, _ReturnAddress());
    AcquireSRWLockExclusive(&connection_lock);
    Connection** slot = &connections;
    while (*slot && (*slot)->handle != rcx) slot = &(*slot)->next;
    Connection* conn = *slot;
    if (conn) *slot = conn->next;
    if (conn && !connections && timer_active) {
        timeEndPeriod(1);
        timer_active = false;
    }
    ReleaseSRWLockExclusive(&connection_lock);
    if (!conn) {
        x2w_local::logf("x2w-audio XApuDisconnect handle=0x%llx -> E_HANDLE", rcx);
        X2W_AUDIO_TRACE(2, rcx, rdx, r8, r9, 0, 0, hr_handle);
        return hr_handle;
    }
    if (conn->buffer) VirtualFree(conn->buffer, 0, MEM_RELEASE);
    delete conn;
    x2w_local::logf("x2w-audio XApuDisconnect handle=0x%llx -> S_OK", rcx);
    X2W_AUDIO_TRACE(2, rcx, rdx, r8, r9, 0, 0, hr_ok);
    return hr_ok;
}
