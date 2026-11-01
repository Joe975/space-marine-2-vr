#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

// Read-only trace of incoming-attack indications and parry outcomes (VrParryEvents.h), installed
// with -Trace. Three detours, reference build 0x6ac3e716 (build/parry_re/NOTES.md):
// - Wwise AK::SoundEngine::PostEvent, both the name (0x3169490) and the ID (0x3169190) exports:
//   the indicator sounds, the parry sounds and the player's hit sounds become trace notes
//   "parry_sound" (label, name, game object);
// - the Morpheme event dispatch FireMorphemeEvent(ev, track) (0xe3e350): an attacker's
//   "attack_notification" / "attack_indication_dodge" (and _all) events become "attack_event"
//   notes with the attacker's animINST, its name and its root position (world units), so the angle
//   to the player's facing can be worked out against the trace's samples.
// Every other call passes straight through.
class EngineParryProbe {
public:
    struct Snapshot { bool installed{}; uint64_t sounds{},indications{},sound_calls{},event_calls{}; };
    static EngineParryProbe& get();
    bool install_in_game(HMODULE);
    bool installed() const { return m_fire!=nullptr; }
    Snapshot snapshot() const { return {installed(),m_sounds.load(),m_indications.load(),m_sound_calls.load(),m_event_calls.load()}; }
private:
    using PostByName=uint32_t(*)(const char*,uint64_t,uint32_t,void*,void*,uint32_t,void*,uint32_t);
    using PostById=uint32_t(*)(uint32_t,uint64_t,uint32_t,void*,void*,uint32_t,void*,uint32_t);
    using Fire=void(*)(void*,void*);
    static uint32_t post_name_hook(const char*,uint64_t,uint32_t,void*,void*,uint32_t,void*,uint32_t);
    static uint32_t post_id_hook(uint32_t,uint64_t,uint32_t,void*,void*,uint32_t,void*,uint32_t);
    static void fire_hook(void*,void*);
    PostByName m_post_name{};PostById m_post_id{};Fire m_fire{};
    std::atomic<uint64_t> m_sounds{},m_indications{},m_sound_calls{},m_event_calls{};
    std::atomic<int> m_names_seen{};std::mutex m_names_mutex;std::vector<uint32_t> m_names;
    std::atomic<int> m_sounds_seen{};std::mutex m_sounds_mutex;std::vector<uint32_t> m_sound_ids;
};
