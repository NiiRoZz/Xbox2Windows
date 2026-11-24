// Verifies X2W_INPUT_SCRIPT playback of the GameInput.dll stand-in: holds, overlapping masks, stick
// and trigger values, both time bases, legacy equivalence, the 256-entry and 4,095-character limits
// and rejected items. tests/startup_gameinput_script.cpp keeps its own long and entries cases. The parser is checked directly
// (src/startup_stubs/input_script.h); playback is checked through IGameInput::GetCurrentReading in one
// child process per case, because the stand-in reads the script once per process. The children set
// X2W_INPUT_CLOCK_MS (milliseconds since process start) before every poll.
//
// usage: startup_gameinput_script_syntax <GameInput.dll>   (exit 77 when XInput pad 0 is connected: its
// values would merge into every reading; the parser checks still run first)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "startup_stubs/input_script.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kind_gamepad = 0x00040000;

struct GamepadState {
    std::uint32_t buttons;
    float leftTrigger, rightTrigger;
    float leftThumbstickX, leftThumbstickY, rightThumbstickX, rightThumbstickY;
};
static_assert(sizeof(GamepadState) == 28, "GamepadState layout");

struct PadShared {
    std::uint32_t magic, buttons, title_read, reads;
};

struct IGameInputReading {
    const void* const* vtbl;
    std::uint32_t Release()
    {
        using Fn = std::uint64_t(__stdcall*)(IGameInputReading*);
        return static_cast<std::uint32_t>(reinterpret_cast<Fn>(vtbl[2])(this));
    }
    bool GetGamepadState(GamepadState* state)
    {
        using Fn = bool(__stdcall*)(IGameInputReading*, GamepadState*);
        return reinterpret_cast<Fn>(vtbl[22])(this, state);
    }
};
struct IGameInput {
    const void* const* vtbl;
    HRESULT GetCurrentReading(std::uint32_t kind, void* device, IGameInputReading** reading)
    {
        using Fn = HRESULT(__stdcall*)(IGameInput*, std::uint32_t, void*, IGameInputReading**);
        return reinterpret_cast<Fn>(vtbl[4])(this, kind, device, reading);
    }
};

int failures = 0;
void check(bool ok, const char* what, double at = -1)
{
    if (ok) return;
    ++failures;
    if (at >= 0)
        std::printf("FAIL: %s (at %.3f s)\n", what, at);
    else
        std::printf("FAIL: %s\n", what);
}

// ---- legacy reference: the parser and playback this stand-in shipped before holds and axes ----------
struct LegacyScript {
    std::uint32_t mask[32];
    double at[32];
    int count;
};
LegacyScript legacy_parse(const char* script)
{
    LegacyScript sc{{}, {}, 0};
    char text[512]{};
    std::strncpy(text, script, sizeof(text) - 1);
    char* context = nullptr;
    for (char* item = strtok_s(text, ",", &context); item && sc.count < 32; item = strtok_s(nullptr, ",", &context)) {
        unsigned mask = 0;
        double at = 0;
        if (std::sscanf(item, "%x@%lf", &mask, &at) == 2) {
            sc.mask[sc.count] = mask;
            sc.at[sc.count++] = at;
        }
    }
    return sc;
}
std::uint32_t legacy_buttons(const LegacyScript& sc, std::uint64_t ms_since_zero)
{
    const double t = ms_since_zero / 1000.0;
    std::uint32_t buttons = 0;
    for (int i = 0; i < sc.count; ++i)
        if (t >= sc.at[i] && t < sc.at[i] + 0.3) buttons |= sc.mask[i];
    return buttons;
}

const char* const legacy_scripts[] = {
    "4@60,1@70,20@80",
    "4@70",
    "4@1,,1@2.5,20@3,0x10@3.1, 8@4 ,2@4.2,",
    "4@1,4@1.1,4@1.25,8@1.2",
    "200@0,400@0.3,800@0.6,1000@0.9,2000@1.2",
};

std::uint32_t lcg(std::uint32_t& state) { return state = state * 1664525u + 1013904223u; }
std::string random_legacy_script(std::uint32_t& state)
{
    std::string text;
    const int count = 1 + static_cast<int>(lcg(state) % 32);
    for (int i = 0; i < count; ++i) {
        char item[48];
        const unsigned mask = 1 + lcg(state) % 0x3FFF;
        const unsigned ms = lcg(state) % 20000;
        switch (lcg(state) % 4) {
        case 0: std::snprintf(item, sizeof(item), "%x@%u", mask, ms / 1000); break;
        case 1: std::snprintf(item, sizeof(item), "0x%X@%.1f", mask, ms / 1000.0); break;
        case 2: std::snprintf(item, sizeof(item), " %x@%.3f", mask, ms / 1000.0); break;
        default: std::snprintf(item, sizeof(item), "%x@%.2f ", mask, ms / 1000.0); break;
        }
        text += (i ? "," : "") + std::string(item);
    }
    return text;
}

// Old-form against new-form parsing: identical buttons at every millisecond.
void check_legacy_equivalence()
{
    std::vector<std::string> scripts(std::begin(legacy_scripts), std::end(legacy_scripts));
    std::uint32_t state = 0x5EED;
    for (int i = 0; i < 60; ++i) scripts.push_back(random_legacy_script(state));
    for (const std::string& text : scripts) {
        const LegacyScript old_form = legacy_parse(text.c_str());
        const x2w_input_script::Script new_form = x2w_input_script::parse(text.c_str());
        if (new_form.entries.size() != static_cast<std::size_t>(old_form.count) || !new_form.rejections.empty() ||
            new_form.from_process_start) {
            std::printf("FAIL: legacy script \"%s\": %d old entries, %zu new entries, %zu rejected\n", text.c_str(),
                        old_form.count, new_form.entries.size(), new_form.rejections.size());
            ++failures;
            continue;
        }
        double last = 0;
        for (int i = 0; i < old_form.count; ++i) last = old_form.at[i] > last ? old_form.at[i] : last;
        const auto limit = static_cast<std::uint64_t>(last * 1000) + 1000;
        for (std::uint64_t ms = 0; ms <= limit; ++ms) {
            const x2w_input_script::Output out = x2w_input_script::play(new_form, ms / 1000.0);
            bool axes = false;
            for (bool active : out.active) axes |= active;
            if (out.buttons != legacy_buttons(old_form, ms) || axes) {
                std::printf("FAIL: legacy script \"%s\" at %llu ms: old 0x%x, new 0x%x\n", text.c_str(),
                            static_cast<unsigned long long>(ms), legacy_buttons(old_form, ms), out.buttons);
                ++failures;
                break;
            }
        }
    }
}

void check_rejections()
{
    struct Case {
        const char* script;
        std::size_t accepted, index;
        const char* reason;
    };
    const Case cases[] = {
        {"4", 0, 1, "missing @<seconds>"},
        {"4 @1", 0, 1, "missing @<seconds>"},
        {"4@", 0, 1, "bad time"},
        {"4@x", 0, 1, "bad time"},
        {"4@1+0", 0, 1, "bad duration"},
        {"4@1+-1", 0, 1, "bad duration"},
        {"4@1+", 0, 1, "bad duration"},
        {"4@1junk", 0, 1, "trailing text"},
        {"4@1,zz@2,8@3", 2, 2, "bad mask"},
        {"1FFFFFFFF@1", 0, 1, "bad mask"},
        {"lx=2@1", 0, 1, "value out of range [-1, 1]"},
        {"lt=-0.5@1", 0, 1, "value out of range [0, 1]"},
        {"qq=1@1", 0, 1, "unknown token"},
        {"lx=@1", 0, 1, "bad value"},
        {"ly=0.5", 0, 1, "missing @<seconds>"},
        {"4@1,base=start", 1, 2, "base= must be the first item"},
        {"base=later,4@1", 1, 1, "unknown base (use poll or start)"},
    };
    for (const Case& c : cases) {
        const x2w_input_script::Script s = x2w_input_script::parse(c.script);
        const bool ok = s.entries.size() == c.accepted && s.rejections.size() == 1 && s.rejections[0].index == c.index &&
                        s.rejections[0].reason == c.reason;
        if (!ok) {
            std::printf("FAIL: \"%s\": %zu accepted, %zu rejected, first %zu \"%s\" (want %zu accepted, item %zu \"%s\")\n",
                        c.script, s.entries.size(), s.rejections.size(), s.rejections.empty() ? 0 : s.rejections[0].index,
                        s.rejections.empty() ? "" : s.rejections[0].reason.c_str(), c.accepted, c.index, c.reason);
            ++failures;
        }
    }
    // The base directive is an item (it counts for indices) but not an entry.
    const x2w_input_script::Script based = x2w_input_script::parse(" base = start ,4@1,lx=-1@2+0.5,rt=1@3");
    check(based.from_process_start && based.entries.size() == 3 && based.rejections.empty(), "base=start with two axis entries");
    // Limit: 256 entries accepted, the 257th reported with its index.
    std::string text;
    for (int i = 0; i < 257; ++i) text += (i ? ",1@" : "1@") + std::to_string(i);
    const x2w_input_script::Script full = x2w_input_script::parse(text.c_str());
    check(full.entries.size() == 256 && full.rejections.size() == 1 && full.rejections[0].index == 257 &&
              full.rejections[0].reason == "over 256 entries" && full.items == 257,
          "257-entry script: 256 accepted, item 257 rejected over the limit");
}

// ---- playback through the stand-in, one child process per case --------------------------------------
struct Child {
    IGameInput* input = nullptr;
    std::string log_path;

    GamepadState at(std::uint64_t ms)
    {
        char text[24];
        std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(ms));
        SetEnvironmentVariableA("X2W_INPUT_CLOCK_MS", text);
        GamepadState state{};
        IGameInputReading* reading = nullptr;
        if (FAILED(input->GetCurrentReading(kind_gamepad, nullptr, &reading)) || !reading) {
            check(false, "GetCurrentReading", ms / 1000.0);
            return state;
        }
        check(reading->GetGamepadState(&state), "GetGamepadState", ms / 1000.0);
        reading->Release();
        return state;
    }
    std::string log() const
    {
        std::string text;
        if (FILE* f = std::fopen(log_path.c_str(), "rb")) {
            char buffer[4096];
            for (std::size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;) text.append(buffer, n);
            std::fclose(f);
        }
        return text;
    }
    std::size_t count(const char* needle) const
    {
        const std::string text = log();
        std::size_t n = 0;
        for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
        return n;
    }
};

bool open_child(const char* dll, const std::string& script, Child& child)
{
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    child.log_path = std::string(temp) + "x2w_gameinput_script_" + std::to_string(GetCurrentProcessId()) + ".log";
    DeleteFileA(child.log_path.c_str());
    SetEnvironmentVariableA("X2W_TRAP_LOG", child.log_path.c_str());
    SetEnvironmentVariableA("X2W_INPUT_PRESS", nullptr);
    SetEnvironmentVariableA("X2W_INPUT_KEYS", nullptr);
    SetEnvironmentVariableA("X2W_INPUT_SCRIPT", script.c_str());
    // The external-input block is OR-ed into every reading; this check needs it clear.
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(PadShared), L"Local\\x2w_input_pad");
    if (auto* shared = mapping ? static_cast<PadShared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PadShared))) : nullptr)
        shared->buttons = 0;
    HMODULE module = LoadLibraryA(dll);
    if (!module) {
        std::printf("LoadLibraryA(%s) failed: %lu\n", dll, GetLastError());
        return false;
    }
    using CreateFn = HRESULT(__stdcall*)(IGameInput**);
    const auto create = reinterpret_cast<CreateFn>(GetProcAddress(module, "GameInputCreate"));
    if (!create || FAILED(create(&child.input)) || !child.input) {
        std::printf("GameInputCreate failed\n");
        return false;
    }
    return true;
}

void expect_buttons(Child& child, std::uint64_t ms, std::uint32_t want, const char* what)
{
    const GamepadState s = child.at(ms);
    if (s.buttons != want) {
        std::printf("FAIL: %s at %llu ms: buttons 0x%x, want 0x%x\n", what, static_cast<unsigned long long>(ms), s.buttons, want);
        ++failures;
    }
}

int case_long(const char* dll)
{
    // 200 entries, one every 0.5 s, about 2.5 KB of text (the former limits were 32 entries and 512 bytes).
    std::string script;
    std::vector<std::uint32_t> masks;
    for (int i = 0; i < 200; ++i) {
        masks.push_back((1u << (i % 14)) | (static_cast<std::uint32_t>(i) << 16));
        char item[32];
        std::snprintf(item, sizeof(item), "%s%x@%.3f", i ? "," : "", masks.back(), i * 0.5);
        script += item;
    }
    Child child;
    if (!open_child(dll, script, child)) return 2;
    const std::uint64_t first_poll = 1000;
    expect_buttons(child, first_poll, masks[0], "first poll");
    int delivered = 0;
    for (int i = 0; i < 200; ++i) {
        const std::uint64_t start = first_poll + static_cast<std::uint64_t>(i) * 500;
        const GamepadState on = child.at(start + 100), off = child.at(start + 400);
        if (on.buttons == masks[i] && off.buttons == 0) ++delivered;
    }
    if (delivered != 200) {
        std::printf("FAIL: %d of 200 entries delivered\n", delivered);
        ++failures;
    }
    check(child.count("x2w-input script: 200 entries accepted, 0 rejected, time base first-poll") == 1, "200-entry summary line");
    check(child.count("x2w-input script time passed its last entry (200 entries, ended at 99.800 s") == 1, "last-entry line");
    return 0;
}

int case_legacy(const char* dll)
{
    const char* script = "4@1,,1@2.5,20@3,0x10@3.1, 8@4 ,2@4.2,";
    const LegacyScript old_form = legacy_parse(script);
    Child child;
    if (!open_child(dll, script, child)) return 2;
    const std::uint64_t first_poll = 2345;
    for (std::uint64_t ms = 0; ms <= 6000; ms += 5) {
        const std::uint32_t want = legacy_buttons(old_form, ms);
        const GamepadState s = child.at(first_poll + ms);
        if (s.buttons != want) {
            std::printf("FAIL: legacy script at %llu ms after the first poll: 0x%x, want 0x%x\n",
                        static_cast<unsigned long long>(ms), s.buttons, want);
            ++failures;
            break;
        }
    }
    check(child.count("x2w-input script: 6 entries accepted, 0 rejected, time base first-poll") == 1, "legacy summary line");
    return 0;
}

int case_hold(const char* dll)
{
    Child child;
    if (!open_child(dll, "4@1+2,8@5", child)) return 2;
    const std::uint64_t z = 2000;
    expect_buttons(child, z, 0, "hold before start");
    expect_buttons(child, z + 990, 0, "hold before start");
    expect_buttons(child, z + 1000, 4, "hold starts");
    expect_buttons(child, z + 2990, 4, "hold still held");
    expect_buttons(child, z + 3000, 0, "hold ends after 2 s");
    expect_buttons(child, z + 5000, 8, "default press starts");
    expect_buttons(child, z + 5290, 8, "default press held");
    expect_buttons(child, z + 5310, 0, "default press ends after 0.3 s");
    return 0;
}

int case_overlap(const char* dll)
{
    Child child;
    if (!open_child(dll, "4@1+2,8@2+2,10@2.5", child)) return 2;
    const std::uint64_t z = 500;
    expect_buttons(child, z, 0, "overlap start");
    expect_buttons(child, z + 1500, 0x4, "first hold alone");
    expect_buttons(child, z + 2200, 0xC, "two holds combined");
    expect_buttons(child, z + 2600, 0x1C, "three entries combined");
    expect_buttons(child, z + 2900, 0xC, "press ended, holds remain");
    expect_buttons(child, z + 3500, 0x8, "second hold alone");
    expect_buttons(child, z + 4500, 0, "all released");
    return 0;
}

int case_sticks(const char* dll)
{
    Child child;
    if (!open_child(dll, "lx=-1@1+1,ly=0.5@1.5+1,rt=0.75@2,4@1.2+0.5,rx=1@3+1,rx=-0.25@3.5+0.2,lt=1@3,ry=-0.5@3", child)) return 2;
    struct Want {
        std::uint64_t ms;
        std::uint32_t buttons;
        float lt, rt, lx, ly, rx, ry;
    };
    const Want wants[] = {
        {500, 0, 0, 0, 0, 0, 0, 0},       {1300, 4, 0, 0, -1, 0, 0, 0},    {1600, 4, 0, 0, -1, 0.5f, 0, 0},
        {2100, 0, 0, 0.75f, 0, 0.5f, 0, 0}, {2600, 0, 0, 0, 0, 0, 0, 0},     {3100, 0, 1, 0, 0, 0, 1, -0.5f},
        {3600, 0, 0, 0, 0, 0, -0.25f, 0}, {3800, 0, 0, 0, 0, 0, 1, 0},       {4500, 0, 0, 0, 0, 0, 0, 0},
    };
    const std::uint64_t z = 1000;
    child.at(z);
    for (const Want& w : wants) {
        const GamepadState s = child.at(z + w.ms);
        const bool ok = s.buttons == w.buttons && s.leftTrigger == w.lt && s.rightTrigger == w.rt && s.leftThumbstickX == w.lx &&
                        s.leftThumbstickY == w.ly && s.rightThumbstickX == w.rx && s.rightThumbstickY == w.ry;
        if (!ok) {
            std::printf("FAIL: sticks at %.3f s: buttons 0x%x lt %g rt %g lx %g ly %g rx %g ry %g\n", w.ms / 1000.0, s.buttons,
                        s.leftTrigger, s.rightTrigger, s.leftThumbstickX, s.leftThumbstickY, s.rightThumbstickX, s.rightThumbstickY);
            ++failures;
        }
    }
    return 0;
}

int case_base_start(const char* dll)
{
    Child child;
    if (!open_child(dll, "base=start,4@5", child)) return 2;
    expect_buttons(child, 3000, 0, "process-start base, first poll");
    expect_buttons(child, 4900, 0, "process-start base, before");
    expect_buttons(child, 5100, 4, "process-start base, 5 s after process start");
    expect_buttons(child, 5400, 0, "process-start base, after");
    expect_buttons(child, 8100, 0, "process-start base, not first-poll relative");
    check(child.count("x2w-input script: 1 entries accepted, 0 rejected, time base process-start") == 1, "process-start summary");
    check(child.count("x2w-input script time base process-start: first poll at 3.000 s, first entry at 5.000 s since process start") == 1,
          "process-start resolved time line");
    return 0;
}

int case_base_poll(const char* dll)
{
    Child child;
    if (!open_child(dll, "4@5", child)) return 2;
    expect_buttons(child, 3000, 0, "first-poll base, first poll");
    expect_buttons(child, 5100, 0, "first-poll base, not process-start relative");
    expect_buttons(child, 8100, 4, "first-poll base, 5 s after the first poll");
    expect_buttons(child, 8400, 0, "first-poll base, after");
    check(child.count("x2w-input script time base first-poll: first poll at 3.000 s, first entry at 8.000 s since process start") == 1,
          "first-poll resolved time line");
    return 0;
}

int case_malformed(const char* dll)
{
    Child child;
    if (!open_child(dll, "4@1,zz@2,8@3", child)) return 2;
    const std::uint64_t z = 100;
    expect_buttons(child, z, 0, "malformed script, first poll");
    expect_buttons(child, z + 1100, 4, "valid item before the malformed one");
    expect_buttons(child, z + 2100, 0, "malformed item delivers nothing");
    expect_buttons(child, z + 3100, 8, "valid item after the malformed one");
    check(child.count("x2w-input script: 2 entries accepted, 1 rejected, time base first-poll") == 1, "malformed summary counts");
    check(child.count(" rejected (") == 1, "exactly one rejection line");
    check(child.count("x2w-input X2W_INPUT_SCRIPT: 2 of 3 entries used (1 malformed, at most 256)") == 1, "entries-used line");
    check(child.count("x2w-input script item 2 rejected (bad mask): \"zz@2\"") == 1, "rejection names item 2 and the reason");
    return 0;
}

// Over 4,095 characters the value is ignored as a whole and logged; nothing is pressed.
int case_too_long(const char* dll)
{
    std::string script = "4@0";
    while (script.size() < 4096) script += ",1@9000";
    Child child;
    if (!open_child(dll, script, child)) return 2;
    expect_buttons(child, 1000, 0, "too-long script, first poll");
    expect_buttons(child, 1100, 0, "too-long script presses nothing");
    char line[96];
    std::snprintf(line, sizeof(line), "x2w-input X2W_INPUT_SCRIPT ignored: %zu characters, at most 4095", script.size());
    check(child.count(line) == 1, "ignored line with the length");
    check(child.count("x2w-input script: ") == 0, "no summary for an ignored script");
    return 0;
}

// 257 entries: the 257th is reported over the limit and counted; the 256 others play.
int case_over_limit(const char* dll)
{
    std::string script;
    for (int i = 0; i < 257; ++i) script += (i ? ",4@" : "4@") + std::to_string(i);
    Child child;
    if (!open_child(dll, script, child)) return 2;
    expect_buttons(child, 0, 4, "over limit: entry 1");
    expect_buttons(child, 255100, 4, "over limit: entry 256");
    expect_buttons(child, 256100, 0, "over limit: entry 257 not played");
    check(child.count("x2w-input X2W_INPUT_SCRIPT: 256 of 257 entries used (0 malformed, at most 256)") == 1, "entries-used line");
    check(child.count("x2w-input script item 257 rejected (over 256 entries): \"4@256\"") == 1, "item 257 named");
    return 0;
}

struct NamedCase {
    const char* name;
    int (*run)(const char*);
};
const NamedCase cases[] = {
    {"long", case_long},         {"legacy", case_legacy},         {"hold", case_hold},
    {"overlap", case_overlap},   {"sticks", case_sticks},         {"base_start", case_base_start},
    {"base_poll", case_base_poll}, {"malformed", case_malformed},
    {"too_long", case_too_long}, {"over_limit", case_over_limit},
};

bool xinput_pad_connected()
{
    struct State {
        DWORD packet;
        BYTE pad[12];
    };
    HMODULE module = LoadLibraryW(L"xinput1_4.dll");
    if (!module) return false;
    using Fn = DWORD(WINAPI*)(DWORD, State*);
    const auto get = reinterpret_cast<Fn>(GetProcAddress(module, "XInputGetState"));
    State state{};
    return get && get(0, &state) == 0;
}

int run_child_case(const char* self, const char* dll, const char* name)
{
    std::string command = std::string("\"") + self + "\" \"" + dll + "\" " + name;
    STARTUPINFOA si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::printf("CreateProcess(%s) failed: %lu\n", name, GetLastError());
        return 2;
    }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        std::printf("case %s timed out\n", name);
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: %s <GameInput.dll> [case]\n", argv[0]);
        return 2;
    }
    if (argc >= 3) {
        for (const NamedCase& c : cases)
            if (std::strcmp(c.name, argv[2]) == 0) {
                const int status = c.run(argv[1]);
                return status ? status : (failures ? 1 : 0);
            }
        std::printf("unknown case %s\n", argv[2]);
        return 2;
    }

    check_legacy_equivalence();
    check_rejections();
    std::printf("parser checks: %s\n", failures ? "FAIL" : "pass");
    if (xinput_pad_connected()) {
        std::printf("SKIP: XInput pad 0 is connected; its values merge into every reading, playback cases not run\n");
        return failures ? 1 : 77;
    }
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    for (const NamedCase& c : cases) {
        const int code = run_child_case(self, argv[1], c.name);
        std::printf("case %s: %s (exit %d)\n", c.name, code == 0 ? "pass" : "FAIL", code);
        if (code != 0) ++failures;
    }
    return failures ? 1 : 0;
}
