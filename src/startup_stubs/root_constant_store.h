// Experimental (docs/startup-runtime.md, #870): the 32-bit root arguments a recorded command list keeps for
// translated draws and dispatches. Plain standard C++ with no Windows types, so it can be tested on its own.
//
// A list keeps 16 root parameters of 8 words each. A write that starts past those limits is dropped and one
// that runs past them is cut off at the end of the parameter, so the caller can log either case.
#pragma once

#include <cstddef>
#include <cstdint>

namespace x2w_root_constants {

inline constexpr unsigned parameter_count = 16;
inline constexpr unsigned words_per_parameter = 8;

enum class Result { stored, truncated, dropped };

struct Store {
    std::uint32_t words[parameter_count][words_per_parameter] = {};

    void clear() noexcept
    {
        for (auto& row : words)
            for (auto& word : row) word = 0;
    }

    // SetXxxRoot32BitConstants(parameter, count, data, offset). `data` must hold `count` words.
    Result set(unsigned parameter, unsigned count, const std::uint32_t* data, unsigned offset) noexcept
    {
        if (!count || !data || parameter >= parameter_count || offset >= words_per_parameter) return Result::dropped;
        const bool cut = count > words_per_parameter - offset;
        const unsigned n = cut ? words_per_parameter - offset : count;
        for (unsigned i = 0; i < n; ++i) words[parameter][offset + i] = data[i];
        return cut ? Result::truncated : Result::stored;
    }

    // SetXxxRoot32BitConstant(parameter, value, offset).
    Result set_one(unsigned parameter, std::uint32_t value, unsigned offset) noexcept { return set(parameter, 1, &value, offset); }

    // The word a shader reads; 0 outside the kept range.
    std::uint32_t word(int parameter, int offset) const noexcept
    {
        return parameter >= 0 && parameter < static_cast<int>(parameter_count) && offset >= 0 &&
                       offset < static_cast<int>(words_per_parameter)
                   ? words[parameter][offset]
                   : 0;
    }
};

}  // namespace x2w_root_constants
