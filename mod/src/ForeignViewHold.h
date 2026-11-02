#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

// Owned stereo stands down while the game renders a picture-in-picture view
// (docs/LOAD_CRASH_2026-10-26.md). The game gives each PIP view an HDR core index
// equal to its render-list slot (the camera function that adds the main view, RVA
// 0x1803230 in the reference build, writes the slot returned by the PIP's add into
// the entry's +0x188: build/re/main_camera.c) and has two HDR cores: main 0, PIP 1.
// The owned pair holds slots 0 and 1, so a PIP added in stereo lands in slot 2 and
// its index reads past the game's two-entry table into the next heap block:
// the access violation at Retail.exe+0x26e2d15 (23 Oct 19:55, 26 Oct 14:44, both
// native slot 2, both on the first frame of a PIP: the class selector's and a
// load's). In mono the same view is slot 1, index 1.
// A PIP view's add carries flag 0x40 (NativeRenderProbe::View::pip). The producer
// adds no pair while a PIP was added within SETTLE: a PIP is added every frame or
// every other frame while it shows, with gaps up to 3.7 s inside one window (23 Oct
// 08:46, a load). The first PIP frame can't be held: the pair is added before the
// game adds the PIP in the same frame (behind_pair counts those). hdr_remap() makes that
// frame safe, and the armoury needs it: its preview pauses past SETTLE while you browse
// and comes back with stereo on (1 Nov, two crashes in two tries).
// Thread-safe: seen() runs on the game thread (the add hook) and the render thread
// (a dispatch that holds a PIP behind the pair); held() on the game thread.
class ForeignViewHold {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr auto SETTLE=std::chrono::milliseconds(5000);
    static constexpr uint64_t PIP=0x40;
    static ForeignViewHold& get(){static ForeignViewHold value;return value;}
    // flags: the view flags an add is called with (its third argument) or an entry holds.
    static bool foreign(uint64_t flags){return (flags&PIP)!=0;}
    static constexpr int32_t PIP_HDR_INDEX=1; // Main 0, PIP 1: what a PIP has in mono.
    struct View {int32_t slot{};uint64_t flags{};int32_t hdr{-1};};
    // The first PIP view in a dispatch's list at slot 2 or above while main views hold
    // slots 0 and 1 (the owned pair: the game adds one main view): that dispatch draws a
    // PIP with an out-of-range HDR index.
    static std::optional<int32_t> behind_pair(std::span<const View> views){
        const auto main=[&](int32_t slot){for(const auto& v:views)if(v.slot==slot)return (v.flags&0x50000044)==0;return false;};
        if(!main(0) || !main(1))return std::nullopt;
        for(const auto& v:views)if(v.slot>=2 && foreign(v.flags))return v.slot;
        return std::nullopt;
    }
    // The slot whose HDR index must be rewritten to PIP_HDR_INDEX before the dispatch reads it. Only
    // the case the crash shows: a pair on index 0, one PIP, still holding the index the game
    // gave it from its slot. Anything else is left alone. views need hdr filled in.
    static std::optional<int32_t> hdr_remap(std::span<const View> views){
        const auto slot=behind_pair(views);if(!slot)return std::nullopt;
        int32_t pips{},hdr{-1};
        for(const auto& v:views){
            if(v.slot<2 && v.hdr!=0)return std::nullopt;
            if(foreign(v.flags))++pips;
            if(v.slot==*slot)hdr=v.hdr;
        }
        if(pips!=1 || hdr!=*slot)return std::nullopt;
        return slot;
    }
    void remapped(){m_remapped.fetch_add(1,std::memory_order_relaxed);}
    // Finishing colour grading for a slot: false for one past the game's two entries (NativeGradingIndexGuard.h).
    bool admit_grading(int32_t index){
        m_grading.fetch_add(1,std::memory_order_relaxed);
        if(index>=0 && index<=PIP_HDR_INDEX)return true;
        m_grading_index.store(index,std::memory_order_relaxed);m_grading_refused.fetch_add(1,std::memory_order_relaxed);return false;
    }
    // A game PIP view was added (or seen in a dispatch). slot: the slot it got.
    // behind_pair: it sits after the owned pair. Returns true when this sighting
    // starts a new hold (the previous one is older than SETTLE), for logging.
    bool seen(Clock::time_point now,int32_t slot,bool behind_pair){
        const bool fresh=!held(now);
        m_last.store(now.time_since_epoch().count(),std::memory_order_release);
        m_slot.store(slot,std::memory_order_relaxed);
        m_sightings.fetch_add(1,std::memory_order_relaxed);
        if(fresh)m_holds.fetch_add(1,std::memory_order_relaxed);
        if(behind_pair)m_behind.fetch_add(1,std::memory_order_relaxed);
        return fresh;
    }
    // Within SETTLE of the last sighting. A sighting stamped after now (another
    // thread's clock read raced this one) holds too.
    bool held(Clock::time_point now)const{
        const auto last=m_last.load(std::memory_order_acquire);if(last==NONE)return false;
        const auto at=Clock::time_point{Clock::duration{last}};
        return now<at || now-at<SETTLE;
    }
    struct Stats {uint64_t sightings{},holds{},behind_pair{};int32_t slot{-1};uint64_t remapped{},grading{},grading_refused{};int32_t grading_index{-1};};
    Stats stats()const{return {m_sightings.load(),m_holds.load(),m_behind.load(),m_slot.load(),m_remapped.load(),m_grading.load(),m_grading_refused.load(),m_grading_index.load()};}
private:
    static constexpr Clock::rep NONE=(std::numeric_limits<Clock::rep>::min)();
    std::atomic<Clock::rep> m_last{NONE};std::atomic<int32_t> m_slot{-1};
    std::atomic<uint64_t> m_sightings{},m_holds{},m_behind{},m_remapped{},m_grading{},m_grading_refused{};std::atomic<int32_t> m_grading_index{-1};
};
