// Differential check of every decode_lanes path (src/startup_stubs/bcpack_decoder.h) against
// decode_lanes_reference, the original cycle loop: the two repository BC1 vectors, the 16-lane
// mode-6 vector of tests/bcpack_mode6_vector.py and synthetic BC1/BC7 streams from
// tests/bcpack_encoder.h, each whole, truncated and with single bits flipped. Every path must give
// the reference's accept or reject decision and the same symbols (on rejection too), and
// decode_bcpack must leave the same destination bytes, including streams that the reference
// accepts although they are damaged. Inputs end at a no-access page and the destination sits
// between a canary and a no-access page. The model shortcut is compared on all inputs.
//
//   bcpack_lanes_check VECTOR_DIR [--path NAME] [--full]   check (--full: every truncation and bit flip)
//   bcpack_lanes_check VECTOR_DIR --bench [--large]
//
// With --path, only that path is compared with the reference, and the exit code is 77 (capability
// skip) when this CPU does not support it. Without --path, every supported path is compared.
#include "bcpack_encoder.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using namespace x2w_bcpack;

const uint8_t AB1[12] = { 7, 7, 4, 5, 4, 4, 5, 4, 7, 7, 7, 7 };
const uint8_t AB7[16] = { 3, 3, 7, 7, 7, 7, 7, 6, 6, 4, 5, 7, 7, 7, 7, 4 };
const char* const PATH_NAMES[LANE_PATH_COUNT] = { "reference", "scalar", "avx2" };
int only_path = -1;  // --path: the one path compared with the reference

// tests/bcpack_mode6_vector.py (issue #442): one BC7 block.
const char MODE6_HEX[] =
    "101010101010101004040303030304040101010101010140"
    "000000000000024002027546281800000101df5f4b7c0341"
    "020202411010101004040403101010104004100103030304"
    "4001080001010104000000080000000041020994e4db27a4"
    "410511fe6b020005";

struct Stream {
    std::string name;
    unsigned format;  // 1 or 7
    size_t blocks;
    std::vector<uint8_t> bytes;
};

std::vector<uint8_t> from_hex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

bool load_vector(const std::string& path, Stream& s) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    const size_t hex = text.find("\"chunk_hex\":\"");
    const size_t blocks = text.find("\"blocks\":");
    if (hex == std::string::npos || blocks == std::string::npos) return false;
    const size_t start = hex + 13;
    s.bytes = from_hex(text.substr(start, text.find('"', start) - start));
    s.blocks = std::stoul(text.substr(blocks + 9));
    s.format = 1;
    return !s.bytes.empty();
}

// A region whose last byte is followed by a no-access page.
struct Guarded {
    uint8_t* base = nullptr;
    size_t span = 0;
    explicit Guarded(size_t size) {
        span = (size + 0xFFFF) & ~size_t(0xFFFF);
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, span + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old;
        if (!base || !VirtualProtect(base + span, 0x1000, PAGE_NOACCESS, &old)) std::abort();
    }
    ~Guarded() { VirtualFree(base, 0, MEM_RELEASE); }
    uint8_t* tail(size_t size) const { return base + span - size; }
};

struct Checker {
    Guarded in{ 8u << 20 };
    Guarded out{ (4u << 20) + 64 };
    int failures = 0;
    size_t compared = 0, accepted = 0, rejected = 0, damaged_accepted = 0;

    struct Result {
        bool ok = false;
        std::vector<uint16_t> symbols;
        bool bc_ok = false;
        size_t produced = 0;
        std::vector<uint8_t> dest;
    };

    void run(int path, const Stream& s, const uint8_t* data, size_t size, Result& r) {
        const uint8_t* ab = s.format == 7 ? AB7 : AB1;
        const size_t n = s.format == 7 ? 16 : 12, target = s.format == 7 ? 2 * s.blocks : s.blocks;
        r.ok = decode_lanes_on(path, data, size, ab, n, target, r.symbols);
        const size_t dest_size = s.blocks * (s.format == 7 ? 16 : 8);
        uint8_t* dest = out.tail(dest_size);
        std::memset(dest - 64, 0x5A, 64);
        std::memset(dest, 0xA5, dest_size);
        r.produced = 0x12345;
        lane_path_override = path;
        r.bc_ok = decode_bcpack(data, size, s.format, dest, dest_size, &r.produced);
        lane_path_override = -1;
        for (int k = 0; k < 64; ++k) {
            if (dest[k - 64] != 0x5A) fail(s, "canary before the destination overwritten", path, size);
        }
        r.dest.assign(dest, dest + dest_size);
    }

    void fail(const Stream& s, const char* what, int path, size_t size) {
        if (failures++ < 12) std::fprintf(stderr, "%s, %zu bytes, %s path: %s\n", s.name.c_str(), size, PATH_NAMES[path], what);
    }

    // data: the stream as given (whole, truncated or with a flipped bit); intact: true for the encoder's output.
    void compare(const Stream& s, const std::vector<uint8_t>& data, bool intact) {
        uint8_t* p = in.tail(data.size());
        if (!data.empty()) std::memcpy(p, data.data(), data.size());
        Result ref;
        run(LANE_PATH_REFERENCE, s, p, data.size(), ref);
        ++compared;
        ref.ok ? ++accepted : ++rejected;
        if (ref.ok && !intact) ++damaged_accepted;
        if (intact && (!ref.ok || !ref.bc_ok)) fail(s, "the reference rejects an intact stream", LANE_PATH_REFERENCE, data.size());
        for (int path = LANE_PATH_REFERENCE + 1; path < LANE_PATH_COUNT; ++path) {
            if (!lane_path_supported(path) || (only_path >= 0 && path != only_path)) continue;
            Result r;
            run(path, s, p, data.size(), r);
            if (r.ok != ref.ok) fail(s, "accept/reject differs from the reference", path, data.size());
            if (r.symbols != ref.symbols) fail(s, "symbols differ from the reference", path, data.size());
            if (r.bc_ok != ref.bc_ok || r.produced != ref.produced) fail(s, "decode_bcpack result differs", path, data.size());
            if (r.dest != ref.dest) fail(s, "destination bytes differ", path, data.size());
        }
    }

    void sweep(const Stream& s, bool full) {
        compare(s, s.bytes, true);
        const size_t size = s.bytes.size();
        const size_t cut_step = full ? 1 : (size / 48 + 1), bit_step = full ? 1 : (size * 8 / 160 + 1);
        for (size_t len = 0; len < size; len += cut_step) compare(s, std::vector<uint8_t>(s.bytes.begin(), s.bytes.begin() + len), false);
        for (size_t len = size > 16 ? size - 16 : 0; len < size; ++len) compare(s, std::vector<uint8_t>(s.bytes.begin(), s.bytes.begin() + len), false);
        for (size_t bit = 0; bit < size * 8; bit += bit_step) {
            std::vector<uint8_t> flipped = s.bytes;
            flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
            compare(s, flipped, false);
        }
    }
};

std::vector<Stream> streams(const std::string& dir, bool bench, bool large) {
    std::vector<Stream> list;
    for (const char* v : { "synthetic_11", "synthetic_12" }) {
        Stream s;
        s.name = v;
        if (!load_vector(dir + "/" + v + ".json", s)) {
            std::fprintf(stderr, "cannot read %s/%s.json\n", dir.c_str(), v);
            std::exit(1);
        }
        list.push_back(s);
    }
    if (!bench) list.push_back({ "mode6", 7, 1, from_hex(MODE6_HEX) });
    bcpack_test::Rng rng{ bench ? 7u : 1139u };
    auto add = [&](unsigned format, size_t blocks, uint32_t fragment, bool uniform, size_t padding) {
        bcpack_test::Options o;
        o.max_fragment = fragment;
        o.uniform = uniform;
        o.padding = padding;
        o.bc7_lane = format == 7 ? 15 : -1;
        Stream s{ "", format, blocks, {} };
        std::vector<uint16_t> symbols;
        const size_t n = format == 7 ? 16 : 12, target = format == 7 ? 2 * blocks : blocks;
        if (!bcpack_test::make_stream(format == 7 ? AB7 : AB1, n, target, rng, o, s.bytes, symbols)) std::abort();
        char name[96];
        std::snprintf(name, sizeof name, "BC%u %zu blocks, fragments <= %u%s", format, blocks, fragment, uniform ? ", uniform" : "");
        s.name = name;
        list.push_back(s);
    };
    if (bench) {
        for (unsigned format : { 1u, 7u }) {
            const size_t blocks = large ? (4u << 20) / (format == 7 ? 16 : 8) : (512u << 10) / (format == 7 ? 16 : 8);
            add(format, blocks, 150, false, 0);
            add(format, blocks, 4096, false, 0);
        }
    } else {
        for (unsigned format : { 1u, 7u }) {
            add(format, 1, 8, false, 0);
            add(format, 40, 8, false, 2);
            add(format, 300, 150, false, 0);
            add(format, 300, 150, true, 1);
            add(format, 300, 4096, false, 0);
        }
    }
    return list;
}

uint64_t fnv(const void* data, size_t size, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < size; ++i) h = (h ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
    return h;
}

int bench(const std::vector<Stream>& list) {
    using clock = std::chrono::steady_clock;
    for (const Stream& s : list) {
        const uint8_t* ab = s.format == 7 ? AB7 : AB1;
        const size_t n = s.format == 7 ? 16 : 12, target = s.format == 7 ? 2 * s.blocks : s.blocks;
        const size_t syms = n * target, dest_size = s.blocks * (s.format == 7 ? 16 : 8);
        const int reps = static_cast<int>(syms > 1000000 ? 1 : 2000000 / syms);
        double lanes_best[LANE_PATH_COUNT], total_best[LANE_PATH_COUNT];
        uint64_t sums[LANE_PATH_COUNT] = {};
        std::vector<uint8_t> dest(dest_size);
        std::vector<uint16_t> symbols;
        for (int p = 0; p < LANE_PATH_COUNT; ++p) lanes_best[p] = total_best[p] = 1e30;
        for (int round = 0; round < 8; ++round) {          // round 0 is a warm-up
            for (int p = 0; p < LANE_PATH_COUNT; ++p) {     // paths interleaved within each round
                if (!lane_path_supported(p)) continue;
                auto t0 = clock::now();
                for (int k = 0; k < reps; ++k) {
                    if (!decode_lanes_on(p, s.bytes.data(), s.bytes.size(), ab, n, target, symbols)) return 1;
                }
                auto t1 = clock::now();
                lane_path_override = p;
                size_t produced = 0;
                for (int k = 0; k < reps; ++k) {
                    if (!decode_bcpack(s.bytes.data(), s.bytes.size(), s.format, dest.data(), dest.size(), &produced)) return 1;
                }
                lane_path_override = -1;
                auto t2 = clock::now();
                if (round == 0) continue;
                lanes_best[p] = (std::min)(lanes_best[p], std::chrono::duration<double>(t1 - t0).count() / reps);
                total_best[p] = (std::min)(total_best[p], std::chrono::duration<double>(t2 - t1).count() / reps);
                sums[p] = fnv(dest.data(), dest.size(), fnv(symbols.data(), symbols.size() * 2));
            }
        }
        std::printf("%s: %zu stream bytes, %zu symbols, %.2f MiB out\n", s.name.c_str(), s.bytes.size(), syms, dest_size / 1048576.0);
        for (int p = 0; p < LANE_PATH_COUNT; ++p) {
            if (!lane_path_supported(p)) continue;
            std::printf("  %-9s lanes %6.2f ns/symbol  decode_bcpack %7.2f ms %7.1f MiB/s  x%.2f  checksum %016llx\n", PATH_NAMES[p],
                        lanes_best[p] * 1e9 / syms, total_best[p] * 1e3, dest_size / 1048576.0 / total_best[p],
                        total_best[LANE_PATH_REFERENCE] / total_best[p], static_cast<unsigned long long>(sums[p]));
            if (sums[p] != sums[LANE_PATH_REFERENCE]) return 1;
        }
    }
    return 0;
}

// model_inverse_interval against model_inverse_fast + model_forward for every model and low state.
int check_interval() {
    for (uint8_t eab = 0; eab < 9; ++eab) {
        for (uint8_t q = 0; q < 16; ++q) {
            for (uint32_t c = 0; c < 65536; ++c) {
                uint32_t f, cf, rf, rcf;
                const uint16_t s = model_inverse_interval(eab, q, c, f, cf);
                const uint16_t rs = model_inverse_fast(eab, q, static_cast<uint16_t>(c));
                model_forward(eab, q, rs, rf, rcf);
                if (s != rs || f != rf || cf != rcf) {
                    std::fprintf(stderr, "model_inverse_interval differs: eab %u q %u cf %u\n", eab, q, c);
                    return 1;
                }
            }
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool full = false, do_bench = false, large = false, usage = argc < 2;
    for (int k = 2; k < argc; ++k) {
        if (std::strcmp(argv[k], "--full") == 0) full = true;
        else if (std::strcmp(argv[k], "--bench") == 0) do_bench = true;
        else if (std::strcmp(argv[k], "--large") == 0) large = true;
        else if (std::strcmp(argv[k], "--path") == 0 && k + 1 < argc) {
            ++k;
            for (int p = LANE_PATH_REFERENCE + 1; p < LANE_PATH_COUNT; ++p) {
                if (std::strcmp(argv[k], PATH_NAMES[p]) == 0) only_path = p;
            }
            usage |= only_path < 0;
        } else usage = true;
    }
    if (usage) {
        std::fprintf(stderr, "usage: bcpack_lanes_check VECTOR_DIR [--path NAME] [--full | --bench [--large]]\n");
        return 64;
    }
    std::printf("lane paths:");
    for (int p = 0; p < LANE_PATH_COUNT; ++p) std::printf(" %s%s", PATH_NAMES[p], lane_path_supported(p) ? "" : " (not supported)");
    std::printf("; decode_lanes uses %s\n", PATH_NAMES[best_lane_path()]);
    if (only_path >= 0 && !lane_path_supported(only_path)) {
        std::printf("capability skip: the %s path is not supported here\n", PATH_NAMES[only_path]);
        return 77;
    }
    const std::vector<Stream> list = streams(argv[1], do_bench, large);
    if (do_bench) return bench(list);

    if (check_interval() != 0) return 1;
    Checker checker;
    for (const Stream& s : list) checker.sweep(s, full);
    std::printf("%zu streams compared (%zu accepted, %zu rejected by the reference; %zu damaged streams accepted), %d mismatches\n",
                checker.compared, checker.accepted, checker.rejected, checker.damaged_accepted, checker.failures);
    return checker.failures == 0 ? 0 : 1;
}
