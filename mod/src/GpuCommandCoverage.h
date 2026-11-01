#pragma once
#include <array>
#include <cstddef>

// Actual implementation addresses, including lifecycle and every base writer/
// barrier used by the observer. API support alone does not establish coverage.
struct GpuCommandCoverage {
    inline static constexpr std::array<size_t,15> slots{10,9,12,13,48,17,16,19,26,14,59,27,49,50,11};
    using Methods=std::array<void*,slots.size()>;
    static bool matches(const Methods& installed,void* const* table) {
        if(!table) return false;
        for(size_t i=0;i<slots.size();++i) if(!installed[i] || table[slots[i]]!=installed[i]) return false;
        return true;
    }
};
