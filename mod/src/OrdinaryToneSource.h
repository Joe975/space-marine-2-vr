#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// Diagnostic for the hangar right-eye glow (docs/PLAYTEST_2026-10-23_0836.md): the right eye's tone
// pass reads chosen inputs from the left eye's tone call of the same dispatch instead of its own.
// The engine wrappers are passed, not GPU resources, so the engine keeps doing its own barriers.
// Inputs in native order: 0 scene, 1 bloom (the right eye's owned TAA bloom), 2 luminance.
class OrdinaryToneSource {
public:
    using Inputs=std::array<uintptr_t,3>;
    // "native" (or empty) = 0; otherwise names joined by ',' or '+': scene, bloom, luminance, all.
    static std::optional<uint32_t> parse(std::string_view text){
        uint32_t mask{};
        while(!text.empty()){
            const auto end=text.find_first_of(",+");auto word=text.substr(0,end);text=end==text.npos?std::string_view{}:text.substr(end+1);
            while(!word.empty() && (word.front()==' ' || word.front()=='\t'))word.remove_prefix(1);
            while(!word.empty() && (word.back()==' ' || word.back()=='\t' || word.back()=='\r' || word.back()=='\n'))word.remove_suffix(1);
            if(word.empty() || word=="native")continue;
            if(word=="scene")mask|=1;else if(word=="bloom")mask|=2;else if(word=="luminance")mask|=4;else if(word=="all")mask|=7;else return {};
        }
        return mask;
    }
    struct Stats {uint32_t mask{};uint64_t substituted{},missing{};std::array<uint64_t,3> shared{};};
    void mask(uint32_t value){m_stats.mask=value&7;}
    // Called with the inputs of every main tone call; returns the inputs to invoke it with.
    Inputs apply(uint64_t dispatch,uint32_t slot,Inputs inputs){
        if(slot==0){m_left={dispatch,inputs};return inputs;}
        if(slot!=1)return inputs;
        if(!m_left || m_left->dispatch!=dispatch){if(m_stats.mask)++m_stats.missing;return inputs;}
        // Same wrapper in both slots: the input is re-rendered per eye, so swapping it changes nothing.
        for(size_t k=0;k<3;++k)if(m_left->inputs[k]==inputs[k])++m_stats.shared[k];
        if(!m_stats.mask)return inputs;
        for(size_t k=0;k<3;++k)if(m_stats.mask&(1u<<k) && m_left->inputs[k])inputs[k]=m_left->inputs[k];
        ++m_stats.substituted;return inputs;
    }
    const Stats& stats()const{return m_stats;}
private:
    struct Left {uint64_t dispatch{};Inputs inputs{};};
    std::optional<Left> m_left;Stats m_stats;
};
