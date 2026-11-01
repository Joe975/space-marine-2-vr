#pragma once
#include <atomic>
#include <cstdint>
#include <optional>

// Owned stereo needs a private history for the right eye, and a receipt for it: the game's DLSS
// evaluation on a private viewport (OrdinaryDlssContext), or the game's TAA pass on a private pair
// (NativeOrdinaryTaa, docs/TAA_STEREO.md). With neither (FSR, 31 Oct 19:00 with TAA before that pair
// existed) no pair can be accepted, and each attempt stops the stack: three cold restarts in 40 s ended
// stereo for the session, and the setting would have done the same on every later launch. So stereo
// waits (docs/FREEZE_REHOOK_2026-10-27.md).
//
// held(): the last job made no evaluation and no TAA selection, so add no pair on the next one. It
// costs one mono job when a menu or a load made none and clears on the first job that does. A change
// between DLSS and TAA holds one job too: the next pair must not chain to one proved the other way.
// missing(): REPORT_JOBS jobs in a row in play made neither. That is the setting, not a load: in five
// DLSS runs and the 19:00 run before the change no 5 s log span had a scene job and no Streamline
// call; the TAA minute had five. Title and load jobs don't count: they may run no DLSS with it on.
// Pure logic: the render thread calls complete(), the producer and the present thread read.
class OrdinaryDlssPresence {
public:
    static constexpr uint64_t REPORT_JOBS=120;
    static OrdinaryDlssPresence& get(){static auto* value=new OrdinaryDlssPresence;return *value;}
    // At each job's completion. evaluations: DLSS evaluations the job made. playing: gameplay or the hub.
    // taa: its selections of the TAA targets while TAA stereo can run (NativeOrdinaryTaa::take_calls).
    void complete(uint64_t evaluations,bool playing,uint64_t taa=0){
        if(evaluations || taa){const auto method=evaluations?Method::dlss:Method::taa;const bool changed=m_method!=Method::none && m_method!=method;m_method=method;
            m_held.store(changed,std::memory_order_release);m_jobs.store(0,std::memory_order_relaxed);m_missing.store(false,std::memory_order_release);return;}
        m_held.store(true,std::memory_order_release);if(!playing)return;
        if(m_jobs.fetch_add(1,std::memory_order_relaxed)+1>=REPORT_JOBS)m_missing.store(true,std::memory_order_release);
    }
    bool held()const{return m_held.load(std::memory_order_acquire);}
    bool missing()const{return m_missing.load(std::memory_order_acquire);}
    uint64_t jobs()const{return m_jobs.load(std::memory_order_relaxed);}
    // Present thread: missing() when it differs from the last value taken, once per change.
    std::optional<bool> take_change(){const auto value=missing();if(value==m_reported)return std::nullopt;m_reported=value;return value;}
private:
    enum class Method {none,dlss,taa};
    std::atomic_bool m_held{true},m_missing{};std::atomic_uint64_t m_jobs{};bool m_reported{};Method m_method{}; // Held until a job proves one runs.
};
