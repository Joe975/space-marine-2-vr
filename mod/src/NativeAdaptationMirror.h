#pragma once
#include "NativeOrdinarySceneScope.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

// The hangar right-eye glow (docs/PLAYTEST_2026-10-23_0836.md). The HDR core (reference 0x4f9d010,
// the tone pass's first argument) keeps one 0x198-byte state per view index. Both eyes run the
// native grading step on index 0 (every grading record 17-23 Oct), but the right eye's view draws
// with its own index (view+0x33 = 1), so whatever reads index 1's eye-adaptation fields
// (+0x174..+0x194: flare intensity, lights and emissive passes) sees the never-stepped initial state.
// The right eye's light cores reached the DLSS input up to 1.75x hotter than the left's at the same
// pose, which bloom turned into halos. Copying index 0's adaptation fields into index 1 made the
// same-pose pair match (over 0.0383 -> 0.0053, 23 Oct 12:01). The mirror runs after every main tone
// call of the ordinary pair (index 0 then holds the left eye's single step for the frame:
// NativeOrdinaryGrading undoes the right eye's second one). On with SM2VR_ADAPTATION_MIRROR=1; a control file
// (SM2VR_ADAPTATION_MIRROR_CONTROL, polled once a second: "left" or "native") switches it for A/B.
class NativeAdaptationMirror {
public:
    static constexpr size_t STRIDE=0x198,BEGIN=0x174,END=0x194;
    static NativeAdaptationMirror& get(){static auto* value=new NativeAdaptationMirror;return *value;}
    void enable(bool value){m_enabled.store(value);}
    bool enabled()const{return m_enabled.load();}
    bool configured()const{return m_configured.load();}
    void configure(bool enabled,std::filesystem::path control={}){
        std::lock_guard lock{m_mutex};m_enabled.store(enabled);m_control=std::move(control);m_next=std::chrono::steady_clock::time_point{};m_configured.store(true);
    }
    // Index 1's adaptation fields take index 0's. Returns true when it wrote.
    bool mirror(uintptr_t core){
        poll();if(!m_enabled.load() || !core)return false;std::array<uint8_t,END-BEGIN> left{},right{};
        if(!ordinary_scene_scope::read(core+BEGIN,left) || !ordinary_scene_scope::read(core+STRIDE+BEGIN,right)){m_unreadable.fetch_add(1,std::memory_order_relaxed);return false;}
        m_calls.fetch_add(1,std::memory_order_relaxed);if(left==right)return false;
        std::memcpy(reinterpret_cast<void*>(core+STRIDE+BEGIN),left.data(),left.size());m_writes.fetch_add(1,std::memory_order_relaxed);return true;
    }
    // One log line: mode and counts since the last line.
    std::string line(){
        std::ostringstream out;out<<"mode="<<(m_enabled.load()?"left":"native")<<" calls="<<m_calls.exchange(0)<<" writes="<<m_writes.exchange(0)<<" unreadable="<<m_unreadable.exchange(0);return out.str();
    }
private:
    void poll(){
        const auto now=std::chrono::steady_clock::now();if(now<m_next.load())return;
        std::unique_lock lock{m_mutex,std::try_to_lock};if(!lock || m_control.empty())return;m_next=now+std::chrono::seconds(1);
        std::ifstream in{m_control};std::string word;if(in>>word){if(word=="left")m_enabled.store(true);else if(word=="native")m_enabled.store(false);}
    }
    std::atomic_bool m_enabled{},m_configured{};std::mutex m_mutex;std::filesystem::path m_control;
    std::atomic<std::chrono::steady_clock::time_point> m_next{};std::atomic<uint64_t> m_calls{},m_writes{},m_unreadable{};
};
