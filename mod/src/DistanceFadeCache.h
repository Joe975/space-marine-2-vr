#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

// Bookkeeping for the camera-distance fade fix (EnginePlayerOpacity): one entry per native
// GPU-parameter cache it has evaluated (modified = its distance offset was overridden and must be
// rebuilt to restore). Before 26 Oct a full map took no new cache for the rest of the session: the
// caches of earlier levels kept their entries, so after enough level loads (classes playtest,
// 11:54:53, 65536 entries, capacity_skips climbing to 56876) the new player's own meshes were
// refused, kept the native camera-distance fade, and the hands faded at some angles ("I can only
// see my hand on some angles", the Sniper, 11:57:04).
// The same rule as BoundedTable: a full map first drops every entry not drawn for STALE (a cache
// not drawn for a second belongs to a gone or hidden object; it is re-added when next built). The
// sweep runs at most once per STALE. A cache of the player's own character is never refused: with
// the map still full it takes the place of an entry that is not the player's.
// A dropped modified entry stays overridden until its object's next native rebuild, which in the
// all-objects scope is what is wanted anyway.
class DistanceFadeCache {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr auto STALE=std::chrono::seconds(1);
    struct Entry {bool modified{},owned{};alignas(8) Clock::time_point seen{};};
    enum class Admit {added,swept,evicted,full};
    explicit DistanceFadeCache(size_t capacity):m_capacity{capacity} {}
    Entry* find(uint64_t handle) {const auto it=m_entries.find(handle);return it==m_entries.end()?nullptr:&it->second;}
    void seen(uint64_t handle,Clock::time_point now) {if(auto* entry=find(handle)) entry->seen=now;} // A draw used it.
    // The per-draw path, safe for several threads under a shared lock: find is a read and the
    // time is stored atomically. Every other member needs the exclusive lock. False: not tracked.
    bool touch(uint64_t handle,Clock::time_point now) {
        const auto it=m_entries.find(handle);if(it==m_entries.end()) return false;
        std::atomic_ref<Clock::time_point>{it->second.seen}.store(now,std::memory_order_relaxed);return true;
    }
    bool erase(uint64_t handle) {return m_entries.erase(handle)!=0;}
    void clear() {m_entries.clear();}
    size_t size() const {return m_entries.size();}
    bool full() const {return m_entries.size()>=m_capacity;}
    // Room for one more cache, if a stale entry can go (or, for the player's own cache, any other).
    bool room(bool owned,Clock::time_point now) {
        if(!full()) return true;
        sweep(now);
        if(!full()) return true;
        if(!owned) return false;
        for(auto it=m_entries.begin();it!=m_entries.end();++it) if(!it->second.owned) {m_entries.erase(it);++m_evicted;return true;}
        return false;
    }
    // Reserve the entry for a cache whose parameters are being built (unmodified until written).
    std::pair<Entry*,Admit> admit(uint64_t handle,bool owned,Clock::time_point now) {
        if(auto* entry=find(handle)) {entry->owned=owned;entry->seen=now;return {entry,Admit::added};}
        const auto swept=m_swept;const auto evicted=m_evicted;
        if(!room(owned,now)) return {nullptr,Admit::full};
        auto& entry=m_entries[handle];entry=Entry{false,owned,now};
        return {&entry,m_evicted!=evicted?Admit::evicted:m_swept!=swept?Admit::swept:Admit::added};
    }
    uint64_t swept() const {return m_swept;}     // Stale entries dropped.
    uint64_t evicted() const {return m_evicted;} // Entries given up for the player's own cache.
private:
    void sweep(Clock::time_point now) {
        if(m_last_sweep && now>=*m_last_sweep && now-*m_last_sweep<STALE) return;
        m_last_sweep=now;
        for(auto it=m_entries.begin();it!=m_entries.end();) {
            if(now>=it->second.seen && now-it->second.seen>=STALE) {it=m_entries.erase(it);++m_swept;} else ++it;
        }
    }
    size_t m_capacity;
    std::unordered_map<uint64_t,Entry> m_entries;
    std::optional<Clock::time_point> m_last_sweep;
    uint64_t m_swept{},m_evicted{};
};
