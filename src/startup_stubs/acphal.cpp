// Desktop AcpHal.dll trap stubs.
// Every export traps; see trap.h. Scaffolding, not an ABI claim.
#define X2W_TRAP_MODULE "AcpHal.dll"
#define X2W_TRAP_MODULE_ID 1
#include "startup_stubs/trap.h"
#include "startup_stubs/local_log.h"
#include "startup_stubs/audio_trace.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <new>
#include <utility>

X2W_TRAP_EXPORT(2, AcpHalReleaseShapeContexts)

// Experimental (docs/startup-runtime.md). With X2W_AUDIO_PERMISSIVE=1 the exports
// below stand in for the audio hardware layer; otherwise they trap. Nothing decodes or mixes: the
// goal is that the title's audio start-up succeeds.
namespace {
using u64 = std::uint64_t;

bool permissive() noexcept { return x2w_local::env_on("X2W_AUDIO_PERMISSIVE"); }

// APU heap: one committed region; an APU address is an offset into it. A table of extents, sorted by
// offset, tiles [heap_start, heap_bytes) and each extent is free or used. Allocation is first-fit in
// address order, so while nothing has been freed it hands out the same offsets a bump allocator would;
// a freed extent is merged with free neighbours. The table is static: when it is full a request that
// needs a split fails like an exhausted heap. All state is guarded by heap_lock.
constexpr u64 heap_start = 0x10000;  // APU address 0 stays unused
constexpr std::size_t max_extents = 4096;
struct Extent {
    u64 start, size;
    bool used;
};
unsigned char* heap_base;
u64 heap_bytes;
Extent extents[max_extents];
std::size_t extent_count;
u64 high_water;  // offsets below this were handed out once and may hold old data
SRWLOCK heap_lock = SRWLOCK_INIT;

void heap_reset() noexcept
{
    extent_count = 0;
    if (heap_bytes > heap_start) extents[extent_count++] = {heap_start, heap_bytes - heap_start, false};
    high_water = heap_start;
}

// bytes and alignment (a power of two) are checked by the callers; both stay far below 2^63.
unsigned char* heap_take(u64 bytes, u64 alignment)
{
    unsigned char* result = nullptr;
    u64 zero_from = 0, zero_bytes = 0;
    AcquireSRWLockExclusive(&heap_lock);
    for (std::size_t i = 0; heap_base && i < extent_count && !result; ++i) {
        const Extent e = extents[i];
        const u64 address_low = (reinterpret_cast<std::uintptr_t>(heap_base) + e.start) & (alignment - 1);
        const u64 pad = (alignment - address_low) & (alignment - 1);
        if (e.used || pad > e.size || bytes > e.size - pad) continue;
        const u64 tail = e.size - pad - bytes;
        const std::size_t extra = (pad ? 1 : 0) + (tail ? 1 : 0);
        if (extent_count + extra > max_extents) continue;
        std::memmove(&extents[i + 1 + extra], &extents[i + 1], (extent_count - i - 1) * sizeof(Extent));
        std::size_t at = i;
        if (pad) extents[at++] = {e.start, pad, false};
        extents[at++] = {e.start + pad, bytes, true};
        if (tail) extents[at] = {e.start + pad + bytes, tail, false};
        extent_count += extra;
        result = heap_base + e.start + pad;
        const u64 end = e.start + pad + bytes;
        if (e.start + pad < high_water) {
            zero_from = e.start + pad;
            zero_bytes = (end < high_water ? end : high_water) - zero_from;
        }
        if (end > high_water) high_water = end;
    }
    ReleaseSRWLockExclusive(&heap_lock);
    if (zero_bytes) std::memset(heap_base + zero_from, 0, static_cast<std::size_t>(zero_bytes));  // fresh pages are zero
    return result;
}

// Gives back the used extent that starts exactly at virtual_address. Anything else (outside the heap,
// inside a block, already free) is refused and leaves the table unchanged.
bool heap_give(const void* virtual_address)
{
    bool done = false;
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(virtual_address);
    AcquireSRWLockExclusive(&heap_lock);
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(heap_base);
    if (heap_base && address >= base + heap_start && address - base < heap_bytes) {
        const u64 offset = address - base;
        std::size_t lo = 0, hi = extent_count;
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            if (extents[mid].start < offset) lo = mid + 1; else hi = mid;
        }
        if (lo < extent_count && extents[lo].start == offset && extents[lo].used) {
            extents[lo].used = false;
            if (lo + 1 < extent_count && !extents[lo + 1].used) {
                extents[lo].size += extents[lo + 1].size;
                std::memmove(&extents[lo + 1], &extents[lo + 2], (extent_count - lo - 2) * sizeof(Extent));
                --extent_count;
            }
            if (lo > 0 && !extents[lo - 1].used) {
                extents[lo - 1].size += extents[lo].size;
                std::memmove(&extents[lo], &extents[lo + 1], (extent_count - lo - 1) * sizeof(Extent));
                --extent_count;
            }
            done = true;
        }
    }
    ReleaseSRWLockExclusive(&heap_lock);
    return done;
}

// AcpHal object (not COM: slot 0 is the first method). Seen: slot 0 (0x1000, 0x1000) -> HRESULT, a
// connect with two queue lengths; slot 2 (type, id, frame, &value, 0) -> HRESULT, a command submit.
// Both succeed and are logged; every other slot traps and names itself.
template <int N>
__declspec(noinline) u64 hal_trap(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    char name[48];
    std::snprintf(name, sizeof(name), "AcpHal::slot%d", N);
    x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, name, 0x40 + N, rcx, rdx, r8, r9, _ReturnAddress());
}
long __stdcall hal_connect(void* self, unsigned commands, unsigned messages)
{
    x2w_local::logf("x2w-audio AcpHal::slot0(connect) command_queue=%u message_queue=%u -> S_OK", commands, messages);
    X2W_AUDIO_TRACE(0x40, self, commands, messages, 0, 0, 0, 0);
    return 0;
}

// Trace only: the XMA context array from AcpHalAllocateShapeContexts, so each submit can record
// whether the title has written the contexts (count of nonzero contexts, FNV-1a hash) and, the first
// 32 times the hash changes, a full copy as function 0x80 records (128 bytes each; args: snapshot,
// offset, total bytes).
unsigned char* xma_contexts;
std::uint32_t xma_count;
SRWLOCK xma_lock = SRWLOCK_INIT;
u64 xma_last_hash;
unsigned xma_snapshots;

struct SubmitPayload {
    std::uint32_t nonzero_contexts;
    std::uint32_t contexts;
    std::uint64_t hash;
    unsigned char input[0x40];
};

void trace_submit(void* self, unsigned type, u64 id, unsigned frame, const unsigned* input, u64 extra, void* return_address)
{
    SubmitPayload payload{};
    if (x2w_local::readable(input, sizeof(payload.input))) std::memcpy(payload.input, input, sizeof(payload.input));
    AcquireSRWLockExclusive(&xma_lock);
    const unsigned char* contexts = xma_contexts;
    const std::uint32_t count = xma_count;
    payload.contexts = count;
    u64 hash = 0xcbf29ce484222325ull;
    for (std::uint32_t c = 0; contexts && c < count; ++c) {
        bool nonzero = false;
        for (unsigned i = 0; i < 64; ++i) {
            const unsigned char b = contexts[c * 64 + i];
            nonzero |= b != 0;
            hash = (hash ^ b) * 0x100000001b3ull;
        }
        payload.nonzero_contexts += nonzero;
    }
    payload.hash = hash;
    if (contexts && hash != xma_last_hash && xma_snapshots < 32) {
        const unsigned index = xma_snapshots++;
        xma_last_hash = hash;
        for (u64 offset = 0; offset < u64{count} * 64; offset += 128)
            x2w_audio_trace::emit(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, 0x80, index, offset, u64{count} * 64, 0, 0, 0, 0,
                                  return_address, contexts + offset, 128);
    }
    ReleaseSRWLockExclusive(&xma_lock);
    x2w_audio_trace::emit(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, 0x42, reinterpret_cast<u64>(self), type, id, frame,
                          reinterpret_cast<u64>(input), extra, 0, return_address, &payload, sizeof(payload));
    if (type == 5 || type == 7) {  // which title path started or stopped the context (function 0x87)
        void* frames[8]{};
        RtlCaptureStackBackTrace(2, 8, frames, nullptr);
        x2w_audio_trace::emit(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, 0x87, reinterpret_cast<u64>(frames[0]),
                              reinterpret_cast<u64>(frames[1]), reinterpret_cast<u64>(frames[2]), reinterpret_cast<u64>(frames[3]),
                              reinterpret_cast<u64>(frames[4]), reinterpret_cast<u64>(frames[5]), type, return_address,
                              payload.input, 8);
    }
}

#ifdef X2W_HAVE_FFMPEG
}  // namespace
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}
namespace {
// Prototype (docs/audio-roadmap.md Phase 3), X2W_ACPHAL_XMA=1: runs the SHAPE XMA contexts with FFmpeg's
// xma2 decoder (LGPL; the DLLs are delay-loaded from X2W_FFMPEG_BIN, so AcpHal.dll loads without them).
// The 64-byte context matches the Xbox 360 XMA context the title's own code was read against:
//   dw0: input 0 packet count [0,12), input 0 valid bit 20, input 1 valid bit 21, output block count [22,27),
//        output write offset [27,32);  dw1: input 1 packet count [0,12), sample rate [27,29), stereo bit 29,
//        output valid bit 31;  dw2: input read offset in bits [0,26);  dw4: current input buffer bit 31;
//   dw5/dw6: input buffer 0/1 APU addresses (2048-byte packets);  dw7: output ring APU address;
//   dw9: output read offset [0,5). Output blocks are 256 bytes of little-endian interleaved int16.
// HAL submits drive it: type 5 {ctx} starts, type 7 {ctx} stops, type 0x12 {ctx, blocks} consumes blocks.
// The title reads "write == read" as empty while the output valid bit is set and as full while it is clear.
// shortcut: loops (loop_count/loop_start/loop_end) and sub-packet read offsets are ignored: a buffer is
// decoded once from its first packet. Upgrade when a run shows looping sounds cut short.
namespace xma {
constexpr unsigned block_bytes = 256, packet_bytes = 2048;
struct Voice {
    bool running = false;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int channels = 0;
    unsigned packet_index = 0;  // next packet in the current input buffer
    unsigned filled = 0;        // ring blocks written and not consumed yet
    std::int16_t* pending = nullptr;
    std::size_t pending_samples = 0, pending_capacity = 0;
    // Diagnostics (logged at stop): what the title's packet headers promise vs what was decoded and written.
    int rate = 0;
    unsigned header_frames = 0, packets_fed = 0, blocks_written = 0, seek_bits = 0, serial = 0, packets_total = 0;
    u64 decoded_frames = 0;
    std::size_t skip_samples = 0;  // decoded samples before the title's start frame, dropped
    bool flushed = false;          // the decoder was drained at end of input; reset it before new packets
    std::int16_t* dump = nullptr;  // X2W_ACPHAL_XMA_DUMP: every decoded sample of this start, for listening
    std::size_t dump_samples = 0, dump_capacity = 0;
    unsigned tone_phase = 0;  // X2W_ACPHAL_XMA_TONE: sample counter of the test tone
};
LONG stop_logs, dumps;
unsigned next_serial;
Voice* voices;
unsigned voice_count;
SRWLOCK lock = SRWLOCK_INIT;  // order: lock, then xma_lock (never the reverse)
HANDLE worker;
LONG events;

bool enabled() noexcept { return x2w_local::env_on("X2W_ACPHAL_XMA"); }

volatile LONG* field(unsigned ctx, unsigned dw) noexcept
{
    return reinterpret_cast<volatile LONG*>(xma_contexts + ctx * 64 + dw * 4);
}
std::uint32_t get(unsigned ctx, unsigned dw) noexcept { return static_cast<std::uint32_t>(*field(ctx, dw)); }
// The title writes other bits of the same dwords concurrently, so every update is a compare-exchange.
void put(unsigned ctx, unsigned dw, std::uint32_t mask, std::uint32_t bits) noexcept
{
    volatile LONG* p = field(ctx, dw);
    for (;;) {
        const LONG old = *p;
        const LONG next = static_cast<LONG>((static_cast<std::uint32_t>(old) & ~mask) | (bits & mask));
        if (InterlockedCompareExchange(p, next, old) == old) return;
    }
}
unsigned char* apu(std::uint32_t address, u64 bytes) noexcept
{
    return heap_base && address >= heap_start && address + bytes <= heap_bytes ? heap_base + address : nullptr;
}

bool ffmpeg_loaded;
BOOL CALLBACK load_once(PINIT_ONCE, PVOID, PVOID*) noexcept
{
    wchar_t dir[MAX_PATH - 32];
    const DWORD n = GetEnvironmentVariableW(L"X2W_FFMPEG_BIN", dir, MAX_PATH - 32);
    bool loaded = n && n < MAX_PATH - 32;
    for (const wchar_t* name : {L"avutil-61.dll", L"swresample-7.dll", L"avcodec-63.dll"}) {
        if (!loaded) break;
        wchar_t path[MAX_PATH];
        std::swprintf(path, MAX_PATH, L"%s\\%s", dir, name);
        loaded = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) != nullptr;
        if (!loaded) x2w_local::logf("x2w-audio XMA: cannot load %ls (error %lu)", path, GetLastError());
    }
    ffmpeg_loaded = loaded;  // the InitOnce context value would need its low two bits clear
    return TRUE;
}
bool ffmpeg_ready() noexcept
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, load_once, nullptr, nullptr);
    return ffmpeg_loaded;
}

void write_dump(const Voice& v, unsigned ctx) noexcept
{
    char dir[MAX_PATH - 48];
    const DWORD n = GetEnvironmentVariableA("X2W_ACPHAL_XMA_DUMP", dir, sizeof(dir));
    if (!n || n >= sizeof(dir) || !v.dump_samples || InterlockedIncrement(&dumps) > 256) return;
    char path[MAX_PATH];
    CreateDirectoryA(dir, nullptr);  // the run folder may not have it yet
    std::snprintf(path, sizeof(path), "%s\\xma_%04u_ctx%03u.wav", dir, v.serial, ctx);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const std::uint32_t data = static_cast<std::uint32_t>(v.dump_samples * 2), ch = static_cast<std::uint32_t>(v.channels),
                        rate = static_cast<std::uint32_t>(v.rate);
    unsigned char h[44];
    std::memcpy(h, "RIFF", 4);
    const std::uint32_t riff = 36 + data, fmt = 16, byte_rate = rate * ch * 2;
    const std::uint16_t pcm = 1, chans = static_cast<std::uint16_t>(ch), align = static_cast<std::uint16_t>(ch * 2), bits = 16;
    std::memcpy(h + 4, &riff, 4);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    std::memcpy(h + 16, &fmt, 4);
    std::memcpy(h + 20, &pcm, 2);
    std::memcpy(h + 22, &chans, 2);
    std::memcpy(h + 24, &rate, 4);
    std::memcpy(h + 28, &byte_rate, 4);
    std::memcpy(h + 32, &align, 2);
    std::memcpy(h + 34, &bits, 2);
    std::memcpy(h + 36, "data", 4);
    std::memcpy(h + 40, &data, 4);
    DWORD written = 0;
    WriteFile(f, h, sizeof(h), &written, nullptr);
    WriteFile(f, v.dump, data, &written, nullptr);
    CloseHandle(f);
}

void close(Voice& v, unsigned ctx) noexcept
{
    if (v.codec && x2w_local::first_calls(stop_logs, 200))
        x2w_local::logf("x2w-audio XMA ctx %u stop #%u: packets %u, header frames %u (%u samples), decoded %llu frames, packets fed %u, "
                        "blocks written %u (%u samples), still filled %u, pending %zu, title seek bits %u",
                        ctx, v.serial, v.packets_total, v.header_frames, v.header_frames * 512u, static_cast<unsigned long long>(v.decoded_frames),
                        v.packets_fed, v.blocks_written, v.blocks_written * 128u / (v.channels ? v.channels : 1), v.filled,
                        v.pending_samples, v.seek_bits);
    if (v.codec) write_dump(v, ctx);
    std::free(v.dump);
    v.dump = nullptr;
    v.dump_samples = v.dump_capacity = 0;
    v.header_frames = v.packets_fed = v.blocks_written = v.packets_total = 0;
    v.skip_samples = 0;
    v.flushed = false;
    v.decoded_frames = 0;
    if (v.codec) avcodec_free_context(&v.codec);
    if (v.packet) av_packet_free(&v.packet);
    if (v.frame) av_frame_free(&v.frame);
    v.running = false;
    v.packet_index = v.filled = 0;
    v.pending_samples = 0;
}

bool open(Voice& v, int channels, int rate) noexcept
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    if (!codec || !(v.codec = avcodec_alloc_context3(codec))) return false;
    v.codec->sample_rate = rate;
    v.codec->block_align = packet_bytes;  // FFmpeg reads each frame length as log2(block_align) + 4 = 15 bits
    av_channel_layout_default(&v.codec->ch_layout, channels);
    // 34 bytes = XMA2WAVEFORMATEX tail: FFmpeg then takes one stream of `channels` channels.
    v.codec->extradata = static_cast<std::uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!v.codec->extradata) return false;
    v.codec->extradata_size = 34;
    v.codec->extradata[0] = 1;  // NumStreams
    v.channels = channels;
    v.packet = av_packet_alloc();
    v.frame = av_frame_alloc();
    return v.packet && v.frame && avcodec_open2(v.codec, codec, nullptr) == 0;
}

bool append(Voice& v, const AVFrame* f) noexcept
{
    const int ch = v.channels;
    int n = f->nb_samples;
    if (n <= 0 || f->format != AV_SAMPLE_FMT_FLTP || f->ch_layout.nb_channels != ch) return false;
    const std::size_t need = v.pending_samples + std::size_t(n) * ch;
    if (need > v.pending_capacity) {
        auto* grown = static_cast<std::int16_t*>(std::realloc(v.pending, need * 2 * sizeof(std::int16_t)));
        if (!grown) return false;
        v.pending = grown;
        v.pending_capacity = need * 2;
    }
    int first = 0;
    if (v.skip_samples) {
        const std::size_t drop = v.skip_samples < std::size_t(n) ? v.skip_samples : std::size_t(n);
        v.skip_samples -= drop;
        first = static_cast<int>(drop);
    }
    for (int i = first; i < n; ++i)
        for (int c = 0; c < ch; ++c) {
            const float s = reinterpret_cast<const float*>(f->extended_data[c])[i];
            const float scaled = std::nearbyint((s < -1.f ? -1.f : s > 1.f ? 1.f : s) * 32767.f);
            v.pending[v.pending_samples++] = static_cast<std::int16_t>(scaled);
        }
    v.decoded_frames += static_cast<u64>(n);
    n -= first;
    static const bool dumping = GetEnvironmentVariableA("X2W_ACPHAL_XMA_DUMP", nullptr, 0) != 0;
    if (dumping && v.dump_samples + std::size_t(n) * ch <= std::size_t(v.rate) * ch * 60) {  // 60 s cap per start
        const std::size_t want = v.dump_samples + std::size_t(n) * ch;
        if (want > v.dump_capacity) {
            auto* grown = static_cast<std::int16_t*>(std::realloc(v.dump, want * 2 * sizeof(std::int16_t)));
            if (!grown) return true;
            v.dump = grown;
            v.dump_capacity = want * 2;
        }
        std::memcpy(v.dump + v.dump_samples, v.pending + v.pending_samples - std::size_t(n) * ch, std::size_t(n) * ch * 2);
        v.dump_samples = want;
    }
    return true;
}

void drain(Voice& v) noexcept
{
    while (avcodec_receive_frame(v.codec, v.frame) == 0) {
        append(v, v.frame);
        av_frame_unref(v.frame);
    }
}

// Feeds the next packet of the current input buffer. False when no valid input is left. FFmpeg's XMA decoder
// holds about eight frames of look-ahead (measured: a constant ~4200 samples behind an offline decode of the same
// packets), so when both input buffers are empty the decoder is flushed once to release a sound's tail.
bool decode_next(unsigned ctx, Voice& v) noexcept
{
    for (int tries = 0; tries < 3; ++tries) {
        const std::uint32_t d0 = get(ctx, 0), d1 = get(ctx, 1), d4 = get(ctx, 4);
        const unsigned cur = d4 >> 31;
        if (!((d0 >> (20 + cur)) & 1)) {
            if (!((d0 >> (21 - cur)) & 1)) {
                if (v.flushed) return false;
                v.flushed = true;
                const std::size_t before = v.pending_samples;
                avcodec_send_packet(v.codec, nullptr);
                drain(v);
                return v.pending_samples > before;
            }
            put(ctx, 4, 1u << 31, (cur ^ 1u) << 31);  // the other buffer is ready: switch to it
            v.packet_index = 0;
            continue;
        }
        const unsigned packets = cur ? (d1 & 0xFFF) : (d0 & 0xFFF);
        unsigned char* p = apu(get(ctx, 5 + cur) + v.packet_index * packet_bytes, packet_bytes);
        if (v.packet_index >= packets || !p) {
            put(ctx, 0, 1u << (20 + cur), 0);  // buffer drained: hand it back to the title
            put(ctx, 4, 1u << 31, (cur ^ 1u) << 31);
            v.packet_index = 0;
            continue;
        }
        ++v.packet_index;
        ++v.packets_fed;
        put(ctx, 2, 0x3FFFFFFu, (v.packet_index * packet_bytes * 8u) & 0x3FFFFFFu);
        if (v.flushed) {
            avcodec_flush_buffers(v.codec);
            v.flushed = false;
        }
        if (av_new_packet(v.packet, packet_bytes) < 0) return false;
        std::memcpy(v.packet->data, p, packet_bytes);
        const int sent = avcodec_send_packet(v.codec, v.packet);
        av_packet_unref(v.packet);
        if (sent < 0 && x2w_local::first_calls(events, 64))
            x2w_local::logf("x2w-audio XMA ctx %u packet %u rejected (%d)", ctx, v.packet_index - 1, sent);
        drain(v);
        return true;
    }
    return false;
}

void pump(unsigned ctx, Voice& v) noexcept
{
    const unsigned count = (get(ctx, 0) >> 22) & 31;
    unsigned char* ring = count ? apu(get(ctx, 7), u64{count} * block_bytes) : nullptr;
    if (!ring) return;
    constexpr std::size_t block_samples = block_bytes / sizeof(std::int16_t);
    while (v.filled < count) {
        bool more = true;
        while (v.pending_samples < block_samples && (more = decode_next(ctx, v))) {
        }
        if (v.pending_samples == 0) break;
        if (v.pending_samples < block_samples) {
            if (more) break;
            // Input exhausted: pad the tail of the sound with silence to a whole block.
            std::memset(v.pending + v.pending_samples, 0, (block_samples - v.pending_samples) * sizeof(std::int16_t));
            v.pending_samples = block_samples;
        }
        const unsigned write = (get(ctx, 0) >> 27) & 31;
        std::memcpy(ring + std::size_t(write) * block_bytes, v.pending, block_bytes);
        if (x2w_local::env_on("X2W_ACPHAL_XMA_TONE")) {
            // Diagnostic: replace the block with a 1 kHz sine at -12 dBFS (per output sample, mono or stereo
            // interleaved alike) to see whether and how loud ring contents reach the title's output.
            auto* out = reinterpret_cast<std::int16_t*>(ring + std::size_t(write) * block_bytes);
            const int ch = v.channels > 0 ? v.channels : 1;
            for (std::size_t k = 0; k < block_samples; ++k)
                out[k] = static_cast<std::int16_t>(
                    8191.0 * std::sin(6.283185307179586 * 1000.0 * double(v.tone_phase + k / ch) / double(v.rate ? v.rate : 32000)));
            v.tone_phase += static_cast<unsigned>(block_samples / ch);
        }
        v.pending_samples -= block_samples;
        std::memmove(v.pending, v.pending + block_samples, v.pending_samples * sizeof(std::int16_t));
        put(ctx, 0, 31u << 27, ((write + 1) % count) << 27);
        ++v.filled;
        ++v.blocks_written;
    }
    put(ctx, 1, 1u << 31, v.filled < count ? 1u << 31 : 0);
}

DWORD WINAPI run(void*)
{
    for (;;) {
        Sleep(2);
        AcquireSRWLockExclusive(&lock);
        for (unsigned i = 0; i < voice_count; ++i)
            if (voices[i].running) pump(i, voices[i]);
        ReleaseSRWLockExclusive(&lock);
    }
}

// Called for every HAL submit; acts only on the three context commands.
void command(unsigned type, const unsigned* input) noexcept
{
    if ((type != 5 && type != 7 && type != 0x12) || !enabled() || !x2w_local::readable(input, 8)) return;
    const unsigned ctx = input[0], blocks = input[1];
    AcquireSRWLockExclusive(&lock);
    AcquireSRWLockShared(&xma_lock);
    const bool known = xma_contexts && ctx < xma_count;
    if (known && !voices) {
        voices = new (std::nothrow) Voice[xma_count];
        voice_count = voices ? xma_count : 0;
    }
    ReleaseSRWLockShared(&xma_lock);
    if (known && ctx < voice_count && ffmpeg_ready()) {
        Voice& v = voices[ctx];
        if (type == 5) {
            close(v, ctx);
            const std::uint32_t d1 = get(ctx, 1);
            static const int rates[4] = {24000, 32000, 44100, 48000};
            const int channels = (d1 >> 29) & 1 ? 2 : 1, rate = rates[(d1 >> 27) & 3];
            // What the title's packet headers promise (frame count in byte 0 bits 2..7), as its setup sums them.
            const std::uint32_t d0 = get(ctx, 0);
            for (unsigned b = 0; b < 2; ++b) {
                if (!((d0 >> (20 + b)) & 1)) continue;
                const unsigned packets = b ? (d1 & 0xFFF) : (d0 & 0xFFF);
                v.packets_total += packets;
                for (unsigned k = 0; k < packets; ++k)
                    if (const unsigned char* pk = apu(get(ctx, 5 + b) + k * packet_bytes, 4)) v.header_frames += pk[0] >> 2;
            }
            v.seek_bits = get(ctx, 2) & 0x3FFFFFFu;
            // The title's setup points the read offset at the frame holding its start sample: packet index from the
            // offset, then frames before it counted the way its setup walks them (first frame at the 15-bit offset in
            // header bits 0..1 of byte 0, byte 1, bits 3..7 of byte 2, plus the 32-bit header; each frame starts
            // with its own 15-bit length).
            v.packet_index = v.seek_bits / (packet_bytes * 8);
            const unsigned target = v.seek_bits % (packet_bytes * 8);
            unsigned frames_before = 0;
            const unsigned cur = get(ctx, 4) >> 31;
            if (const unsigned char* pk = apu(get(ctx, 5 + cur) + v.packet_index * packet_bytes, packet_bytes)) {
                unsigned bit = ((pk[0] & 3u) << 13 | unsigned(pk[1]) << 5 | pk[2] >> 3) + 32;
                while (bit < target && bit + 15 <= packet_bytes * 8) {
                    const unsigned byte = bit >> 3, shift = bit & 7;
                    const std::uint32_t window = (std::uint32_t(pk[byte]) << 16 | std::uint32_t(pk[byte + 1]) << 8 |
                                                  (byte + 2 < packet_bytes ? pk[byte + 2] : 0u));
                    const unsigned length = (window >> (9 - shift)) & 0x7FFF;
                    if (!length) break;
                    bit += length;
                    ++frames_before;
                }
            }
            v.skip_samples = std::size_t(frames_before) * 512u;
            v.rate = rate;
            v.serial = next_serial++;
            if (x2w_audio_trace::enabled() && v.serial < 64) {
                const unsigned cur_buf = get(ctx, 4) >> 31;
                const unsigned packets = cur_buf ? (d1 & 0xFFF) : (d0 & 0xFFF);
                for (unsigned k = 0; k < 2 && k < packets; ++k)
                    if (const unsigned char* pk = apu(get(ctx, 5 + cur_buf) + k * packet_bytes, packet_bytes))
                        for (unsigned off = 0; off < packet_bytes; off += 128)
                            x2w_audio_trace::emit(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, 0x81, v.serial, ctx, k, off,
                                                  (u64{packets} << 32) | unsigned(channels), rate, 0, nullptr, pk + off, 128);
            }
            put(ctx, 0, 31u << 27, (get(ctx, 9) & 31) << 27);  // empty ring: write offset = read offset
            v.running = open(v, channels, rate);
            if (!v.running) close(v, ctx);
            if (!worker && v.running) worker = CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
            if (x2w_local::first_calls(events, 64))
                x2w_local::logf("x2w-audio XMA ctx %u start: %d ch, %d Hz, input packets %u/%u -> %s", ctx, channels, rate,
                                get(ctx, 0) & 0xFFF, d1 & 0xFFF, v.running ? "running" : "decoder unavailable");
        } else if (type == 7) {
            close(v, ctx);
        } else {
            const unsigned count = (get(ctx, 0) >> 22) & 31;
            const unsigned used = blocks < v.filled ? blocks : v.filled;
            v.filled -= used;
            if (count) put(ctx, 9, 31u, ((get(ctx, 9) & 31) + used) % count);
            put(ctx, 1, 1u << 31, 1u << 31);
        }
    }
    ReleaseSRWLockExclusive(&lock);
}
}  // namespace xma
#endif

long __stdcall hal_submit(void* self, unsigned type, u64 id, unsigned frame, const unsigned* input, u64 extra)
{
    static LONG calls;
    if (x2w_local::first_calls(calls, 16))
        x2w_local::logf("x2w-audio AcpHal::slot2(submit) type=%u id=0x%llx frame=0x%x input=%p [0x%x] extra=0x%llx -> S_OK", type,
                        id, frame, static_cast<const void*>(input),
                        input && x2w_local::readable(input, 4) ? *input : 0u, extra);
    if (x2w_audio_trace::enabled()) trace_submit(self, type, id, frame, input, extra, _ReturnAddress());
#ifdef X2W_HAVE_FFMPEG
    xma::command(type, input);
#endif
    return 0;
}

template <std::size_t... I>
auto hal_table(std::index_sequence<I...>)
{
    return std::array<void*, sizeof...(I)>{reinterpret_cast<void*>(&hal_trap<static_cast<int>(I)>)...};
}
}  // namespace

// ApuCreateHeap(cachedSize, nonCachedSize) -> HRESULT (the caller tests for failure).
extern "C" __declspec(noinline) long ApuCreateHeap(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuCreateHeap", 4, rcx, rdx, r8, r9, _ReturnAddress());
    const u64 cached = static_cast<std::uint32_t>(rcx), non_cached = static_cast<std::uint32_t>(rdx);
    long hr = 0;
    // Creation and publication are one locked step: base, size and the initial free extent become
    // visible together, and a repeat call finds the heap and leaves live blocks alone.
    AcquireSRWLockExclusive(&heap_lock);
    if (!heap_base) {
        const u64 bytes = ((cached + non_cached) + 0xFFFF) & ~u64{0xFFFF};
        void* memory = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (memory) {
            heap_bytes = bytes;
            heap_reset();
            heap_base = static_cast<unsigned char*>(memory);
        } else {
            hr = static_cast<long>(0x8007000E);  // E_OUTOFMEMORY
        }
    }
    unsigned char* const published = heap_base;
    ReleaseSRWLockExclusive(&heap_lock);
    x2w_local::logf("x2w-audio ApuCreateHeap cached=0x%llx non_cached=0x%llx -> 0x%08lx base=%p", cached, non_cached,
                    static_cast<unsigned long>(hr), static_cast<void*>(published));
    X2W_AUDIO_TRACE(4, rcx, rdx, r8, r9, published, 0, hr);
    return hr;
}

// AcpHalAllocateShapeContexts(contexts*) -> HRESULT. Layout from the caller: six UINT32 counts at
// +0x00 (the fifth, at +0x10, is the XMA count: 350), then six array pointers at +0x18; the caller
// clears count << 6 bytes at the pointer at +0x38, so an XMA context is 64 bytes. The six UINT32
// after the pointers (+0x48) are taken to be the arrays' APU addresses. Only the XMA element size is
// known; a nonzero count of another type fails so the gap is visible.
extern "C" __declspec(noinline) long AcpHalAllocateShapeContexts(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "AcpHalAllocateShapeContexts", 0, rcx, rdx, r8, r9,
                       _ReturnAddress());
    auto* block = reinterpret_cast<unsigned char*>(rcx);
    if (!block || !x2w_local::writable(block, 0x60)) return static_cast<long>(0x80004003);
    const auto* counts = reinterpret_cast<const std::uint32_t*>(block);
    auto** arrays = reinterpret_cast<unsigned char**>(block + 0x18);
    auto* apu = reinterpret_cast<std::uint32_t*>(block + 0x48);
    x2w_local::logf("x2w-audio AcpHalAllocateShapeContexts counts=%u,%u,%u,%u,%u,%u", counts[0], counts[1], counts[2],
                    counts[3], counts[4], counts[5]);
    bool allocated[6]{};
    auto rollback = [&]() {
        for (int j = 0; j < 6; ++j) {
            if (!allocated[j]) continue;
            heap_give(arrays[j]);
            arrays[j] = nullptr;
            apu[j] = 0;
            allocated[j] = false;
        }
    };
    for (int i = 0; i < 6; ++i) {
        if (!counts[i]) continue;
        if (i != 4) {
            x2w_local::logf("x2w-audio   context type %d requested (%u): element size unknown -> E_NOTIMPL", i, counts[i]);
            rollback();
            return static_cast<long>(0x80004001);
        }
        const u64 bytes = u64{counts[i]} * 64;
        unsigned char* memory = heap_take(bytes, 64);
        if (!memory) {
            rollback();
            return static_cast<long>(0x8007000E);
        }
        const u64 offset = static_cast<u64>(memory - heap_base);
        if (offset > 0xFFFFFFFFull) {
            heap_give(memory);
            rollback();
            return static_cast<long>(0x8007000E);
        }
        arrays[i] = memory;
        apu[i] = static_cast<std::uint32_t>(offset);
        allocated[i] = true;
        x2w_local::logf("x2w-audio   XMA contexts: %u x 64 bytes at %p (apu 0x%x)", counts[i], static_cast<void*>(memory),
                        apu[i]);
        AcquireSRWLockExclusive(&xma_lock);
        xma_contexts = memory;
        xma_count = counts[i];
        ReleaseSRWLockExclusive(&xma_lock);
    }
    X2W_AUDIO_TRACE(0, rcx, rdx, r8, r9, 0, 0, 0, block, 0x60);
    return 0;
}

// AcpHalCreate(out**) -> HRESULT.
extern "C" __declspec(noinline) long AcpHalCreate(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "AcpHalCreate", 1, rcx, rdx, r8, r9, _ReturnAddress());
    static auto table = hal_table(std::make_index_sequence<48>{});
    static bool ready = false;
    if (!ready) {
        table[0] = reinterpret_cast<void*>(&hal_connect);
        table[2] = reinterpret_cast<void*>(&hal_submit);
        ready = true;
    }
    static void* const* const object = table.data();
    if (!rcx || !x2w_local::writable(reinterpret_cast<void*>(rcx), sizeof(void*))) return static_cast<long>(0x80004003);
    *reinterpret_cast<void* const* const**>(rcx) = &object;
    x2w_local::logf("x2w-audio AcpHalCreate -> stand-in object %p", static_cast<const void*>(&object));
    X2W_AUDIO_TRACE(1, rcx, rdx, r8, r9, &object, 0, 0);
    return 0;
}

// ApuAlloc(&virtualAddress, apuAddress* or null, size, alignment, flags) -> HRESULT (first call:
// size 0x222e00, alignment 0x800, null APU-address output). Size and alignment are taken as 32 bits.
// First fit; ApuFree returns blocks, and a reused block is zeroed as fresh heap memory is.
// A null output is E_POINTER, size 0 is E_INVALIDARG; an alignment below 16 or not a power of two means 16, and no room is E_OUTOFMEMORY; a failure leaves the heap as it was.
extern "C" __declspec(noinline) long ApuAlloc(void** virtual_address, std::uint32_t* apu_address, u64 size, u64 alignment,
                                              u64 flags)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuAlloc", 3, reinterpret_cast<u64>(virtual_address),
                       reinterpret_cast<u64>(apu_address), size, alignment, _ReturnAddress());
    const u64 bytes = static_cast<std::uint32_t>(size);
    u64 align = static_cast<std::uint32_t>(alignment);
    if (align < 16 || (align & (align - 1))) align = 16;
    const bool valid = bytes != 0;
    unsigned char* memory = virtual_address && valid ? heap_take(bytes, align) : nullptr;
    const long hr = memory ? 0 : static_cast<long>(!virtual_address ? 0x80004003 : valid ? 0x8007000E : 0x80070057);
    if (virtual_address) *virtual_address = memory;
    if (memory && apu_address) *apu_address = static_cast<std::uint32_t>(memory - heap_base);
    static LONG calls;
    if (FAILED(hr) || x2w_local::first_calls(calls, 8))
        x2w_local::logf("x2w-audio ApuAlloc size=0x%llx align=0x%llx flags=0x%llx apu_out=%p -> 0x%08lx %p", bytes, align,
                        static_cast<u64>(static_cast<std::uint32_t>(flags)), static_cast<void*>(apu_address),
                        static_cast<unsigned long>(hr), static_cast<void*>(memory));
    X2W_AUDIO_TRACE(3, virtual_address, apu_address, size, alignment, flags, memory, hr);
    return hr;
}

// ApuFree(virtualAddress) -> HRESULT (every known caller discards it). Null is a no-op and S_OK; an address
// that is not the start of a live block (unknown, inside a block, already freed) is E_INVALIDARG.
extern "C" __declspec(noinline) long ApuFree(void* virtual_address)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuFree", 5, reinterpret_cast<u64>(virtual_address), 0, 0, 0,
                       _ReturnAddress());
    if (!virtual_address) {
        X2W_AUDIO_TRACE(5, 0, 0, 0, 0, 0, 0, 0);
        return 0;
    }
    const long hr = heap_give(virtual_address) ? 0 : static_cast<long>(0x80070057);
    X2W_AUDIO_TRACE(5, virtual_address, 0, 0, 0, 0, 0, hr);
    static LONG calls;
    if (FAILED(hr) || x2w_local::first_calls(calls, 4))
        x2w_local::logf("x2w-audio ApuFree %p -> 0x%08lx", virtual_address, static_cast<unsigned long>(hr));
    return hr;
}

// The three below take their shapes from their names; none has been seen yet, so each logs its first calls.

extern "C" __declspec(noinline) int ApuIsVirtualAddressValid(void* virtual_address)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuIsVirtualAddressValid", 6,
                       reinterpret_cast<u64>(virtual_address), 0, 0, 0, _ReturnAddress());
    auto* p = static_cast<unsigned char*>(virtual_address);
    const int valid = heap_base && p >= heap_base && p < heap_base + heap_bytes;
    static LONG calls;
    if (x2w_local::first_calls(calls, 4)) x2w_local::logf("x2w-audio ApuIsVirtualAddressValid %p -> %d", virtual_address, valid);
    X2W_AUDIO_TRACE(6, virtual_address, 0, 0, 0, 0, 0, valid);
    return valid;
}

extern "C" __declspec(noinline) void* ApuMapApuAddress(u64 apu_address)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuMapApuAddress", 7, apu_address, 0, 0, 0, _ReturnAddress());
    const u64 offset = static_cast<std::uint32_t>(apu_address);
    void* result = heap_base && offset && offset < heap_bytes ? heap_base + offset : nullptr;
    static LONG calls;
    if (x2w_local::first_calls(calls, 4)) x2w_local::logf("x2w-audio ApuMapApuAddress 0x%llx -> %p", offset, result);
    X2W_AUDIO_TRACE(7, apu_address, 0, 0, 0, 0, 0, result);
    return result;
}

extern "C" __declspec(noinline) std::uint32_t ApuMapVirtualAddress(void* virtual_address)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuMapVirtualAddress", 8, reinterpret_cast<u64>(virtual_address),
                       0, 0, 0, _ReturnAddress());
    auto* p = static_cast<unsigned char*>(virtual_address);
    const std::uint32_t result = heap_base && p >= heap_base && p < heap_base + heap_bytes
        ? static_cast<std::uint32_t>(p - heap_base) : 0;
    static LONG calls;
    if (x2w_local::first_calls(calls, 4)) x2w_local::logf("x2w-audio ApuMapVirtualAddress %p -> 0x%x", virtual_address, result);
    X2W_AUDIO_TRACE(8, virtual_address, 0, 0, 0, 0, 0, result);
    return result;
}
