// Experimental (docs/startup-runtime.md#input-script): parser and playback of X2W_INPUT_SCRIPT for the
// GameInput stand-in (src/startup_stubs/gameinput.cpp). Pure C++, no Windows calls, so the
// checks in tests/startup_gameinput_script.cpp can drive it directly.
//
// A script is a comma-separated list. Each non-empty item is one of:
//   <mask hex>@<s>[+<d>]          press these GameInput button bits at <s> for <d> s (default 0.3)
//   <axis>=<value>@<s>[+<d>]      hold one axis at <value>: lx ly rx ry in [-1, 1], lt rt in [0, 1]
//   base=poll | base=start        time base of every entry (first item only; default poll)
// Active button entries OR their masks; among active entries on one axis the later item wins.
// Items that cannot be accepted are kept as rejections (1-based item index and reason).
#pragma once

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace x2w_input_script {

constexpr std::size_t max_entries = 256;  // as the stand-in kept before holds and axes (#1859)
constexpr double default_duration = 0.3;
// Same order as the floats of the stand-in's GamepadState after its buttons.
enum Axis : int { axis_left_trigger, axis_right_trigger, axis_left_x, axis_left_y, axis_right_x, axis_right_y, axis_count };
inline const char* const axis_names[axis_count] = {"lt", "rt", "lx", "ly", "rx", "ry"};

struct Entry {
    int axis;  // -1 for a button entry
    std::uint32_t mask;
    float value;
    double at, duration;
};
struct Rejection {
    std::size_t index;  // 1-based position among the non-empty items
    std::string reason, text;
};
struct Script {
    bool from_process_start = false;
    std::size_t items = 0;  // non-empty items, accepted or not
    std::vector<Entry> entries;
    std::vector<Rejection> rejections;
};
struct Output {
    std::uint32_t buttons = 0;
    float axes[axis_count] = {};
    bool active[axis_count] = {};
};

namespace detail {
inline bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// "@<seconds>[+<duration>]" up to the end of the item.
inline const char* parse_when(const char* p, Entry& entry)
{
    if (*p != '@') return "missing @<seconds>";
    char* stop = nullptr;
    entry.at = std::strtod(++p, &stop);
    if (stop == p || !std::isfinite(entry.at)) return "bad time";
    p = stop;
    entry.duration = default_duration;
    if (*p == '+') {
        entry.duration = std::strtod(++p, &stop);
        if (stop == p || !std::isfinite(entry.duration) || !(entry.duration > 0)) return "bad duration";
        p = stop;
    }
    return *p ? "trailing text" : nullptr;
}

// One trimmed, non-empty item. Returns the rejection reason, or nullptr with `entry` filled
// (or `directive` set for base=).
inline const char* parse_item(const std::string& item, bool first, Script& script, Entry& entry, bool& directive)
{
    directive = false;
    const std::size_t equals = item.find('='), at = item.find('@');
    if (equals != std::string::npos && (at == std::string::npos || equals < at)) {
        std::string name = item.substr(0, equals);
        while (!name.empty() && blank(name.back())) name.pop_back();
        const char* rest = item.c_str() + equals + 1;
        if (name == "base") {
            std::string value = rest;
            while (!value.empty() && blank(value.front())) value.erase(0, 1);
            if (!first) return "base= must be the first item";
            if (value != "poll" && value != "start") return "unknown base (use poll or start)";
            script.from_process_start = value == "start";
            directive = true;
            return nullptr;
        }
        int axis = -1;
        for (int i = 0; i < axis_count; ++i)
            if (name == axis_names[i]) axis = i;
        if (axis < 0) return "unknown token";
        char* stop = nullptr;
        const double value = std::strtod(rest, &stop);
        if (stop == rest || !std::isfinite(value)) return "bad value";
        const bool trigger = axis == axis_left_trigger || axis == axis_right_trigger;
        if (value > 1 || value < (trigger ? 0 : -1)) return trigger ? "value out of range [0, 1]" : "value out of range [-1, 1]";
        entry.axis = axis;
        entry.mask = 0;
        entry.value = static_cast<float>(value);
        return parse_when(stop, entry);
    }
    // Button entry: strtoul and strtod accept what the earlier sscanf("%x@%lf") form accepted.
    const char* p = item.c_str();
    char* stop = nullptr;
    errno = 0;
    const unsigned long long mask = std::strtoull(p, &stop, 16);
    if (stop == p || errno == ERANGE || (mask > 0xFFFFFFFFull && mask < 0xFFFFFFFF00000000ull)) return "bad mask";
    entry.axis = -1;
    entry.mask = static_cast<std::uint32_t>(mask);
    entry.value = 0;
    return parse_when(stop, entry);
}
}  // namespace detail

inline Script parse(const char* text)
{
    Script script;
    if (!text) return script;
    std::size_t index = 0;
    for (const char* p = text;;) {
        const char* end = p;
        while (*end && *end != ',') ++end;
        const char* begin = p;
        const char* last = end;
        while (begin < last && detail::blank(*begin)) ++begin;
        while (last > begin && detail::blank(last[-1])) --last;
        if (begin < last) {
            const std::string item(begin, last);
            ++index;
            script.items = index;
            Entry entry{};
            bool directive = false;
            const char* reason = detail::parse_item(item, index == 1, script, entry, directive);
            if (!reason && !directive && script.entries.size() >= max_entries) reason = "over 256 entries";
            if (reason)
                script.rejections.push_back({index, reason, item});
            else if (!directive)
                script.entries.push_back(entry);
        }
        if (!*end) break;
        p = end + 1;
    }
    return script;
}

// State at `t` seconds on the script's time base.
inline Output play(const Script& script, double t)
{
    Output out;
    for (const Entry& entry : script.entries) {
        if (!(t >= entry.at && t < entry.at + entry.duration)) continue;
        if (entry.axis < 0) {
            out.buttons |= entry.mask;
        } else {
            out.axes[entry.axis] = entry.value;
            out.active[entry.axis] = true;
        }
    }
    return out;
}

inline double first_start(const Script& script)
{
    double first = script.entries.empty() ? 0 : script.entries.front().at;
    for (const Entry& entry : script.entries) first = entry.at < first ? entry.at : first;
    return first;
}
inline double last_end(const Script& script)
{
    double last = 0;
    for (const Entry& entry : script.entries) last = entry.at + entry.duration > last ? entry.at + entry.duration : last;
    return last;
}

}  // namespace x2w_input_script
