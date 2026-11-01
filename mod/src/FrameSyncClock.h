#pragma once

#include <cstdint>
#include <deque>
#include <optional>

// Candidate CPU frame bindings: SM2 title-screen ordering is captured; gameplay/GPU mapping is pending.
// Standard PCL phases are 0..5. Out-of-band/frame-generation markers never allocate an eye.
class FrameSyncClock {
public:
    enum class Policy { standard, sm2_cached_start };
    explicit FrameSyncClock(Policy policy = Policy::standard) : m_policy{policy} {}
    struct Frame {
        uint32_t token{};
        uint64_t render_id{};
        int eye{};
        unsigned phases{};
        bool invalid{};
    };
    struct Snapshot {
        std::optional<Frame> simulation, rendering, presenting;
        uint64_t duplicates{}, errors{}, ignored{};
    };

    void marker(uint32_t phase, uint32_t token, bool accepted = true) {
        if (phase > 5) { ++m_state.ignored; return; }
        // SM2's start marker uses the cached previous token; render/present/end use the next token.
        // Verified by title-screen capture; do not apply this convention to other engines.
        if (m_policy == Policy::sm2_cached_start && phase == 0) { ++token; }
        auto found = find(token);
        if (!accepted) {
            ++m_state.errors;
            if (found != m_frames.end()) { found->invalid = true; invalidate_bindings(token); }
            return;
        }
        if (phase == 0 && found == m_frames.end()) {
            if (m_last_token) {
                const auto delta = uint32_t(token-*m_last_token);
                if (delta == 0 || delta >= 0x80000000u) { ++m_state.errors; return; }
                if (delta != 1) { m_next_eye = 0; }
            }
            if (m_frames.size() == 8) { m_frames.pop_front(); }
            m_frames.push_back({token, ++m_render_id, m_next_eye, 0, false});
            m_next_eye ^= 1;
            m_last_token = token;
            found = find(token);
        }
        if (found == m_frames.end() && m_policy == Policy::sm2_cached_start && !m_last_token) { ++m_state.ignored; return; }
        if (found == m_frames.end() || found->invalid) { ++m_state.errors; return; }
        const auto bit = 1u << phase;
        if (found->phases & bit) { ++m_state.duplicates; return; }
        // SM2 simulation-end is emitted by a different thread after the present markers; it
        // cannot be a prerequisite for render-submit. Render/present phases still retain order.
        const auto required = m_policy == Policy::standard ? bit-1 :
            phase <= 1 ? (phase == 1 ? 1u : 0u) : (bit-1) & ~2u;
        if ((found->phases & required) != required) {
            found->invalid = true;
            invalidate_bindings(token);
            ++m_state.errors;
            return;
        }
        found->phases |= bit;
        if (phase == 0) { m_state.simulation = *found; }
        if (phase == 2) { m_state.rendering = *found; }
        if (phase == 4) { m_state.presenting = *found; }
    }
    void reset() {
        m_frames.clear();
        m_last_token.reset();
        m_next_eye = 0;
        m_state.simulation.reset(); m_state.rendering.reset(); m_state.presenting.reset();
        // Keep render_id monotonic across device resets; consumers explicitly clear their bindings.
    }
    Snapshot snapshot() const { return m_state; }
    std::optional<Frame> lookup(uint32_t token) const {
        for (const auto& frame : m_frames) { if (frame.token == token && !frame.invalid) { return frame; } }
        return std::nullopt;
    }
    // Camera jobs may migrate between workers. Only the single open render-submit interval
    // can own a camera job; never inherit a worker's old simulation-start history.
    std::optional<Frame> camera_frame() const {
        std::optional<Frame> result;
        for (const auto& frame : m_frames) {
            if (!frame.invalid && (frame.phases & (1u << 2)) && !(frame.phases & (1u << 3))) {
                if (result) { return std::nullopt; }
                result = frame;
            }
        }
        return result;
    }

private:
    std::deque<Frame>::iterator find(uint32_t token) {
        for (auto it = m_frames.begin(); it != m_frames.end(); ++it) { if (it->token == token) { return it; } }
        return m_frames.end();
    }
    void invalidate_bindings(uint32_t token) {
        for (auto* binding : {&m_state.simulation, &m_state.rendering, &m_state.presenting}) {
            if (*binding && (*binding)->token == token) { binding->reset(); }
        }
    }
    std::deque<Frame> m_frames;
    std::optional<uint32_t> m_last_token;
    uint64_t m_render_id{};
    int m_next_eye{};
    Snapshot m_state;
    Policy m_policy;
};

// The clock's marker source when the game sends none. Stereo takes each camera job's identity from the
// PCL markers, which only arrive through Streamline; a card without DLSS may get none, and then no job
// ever has an identity and stereo never starts (docs/TAA_STEREO.md). After AFTER_JOBS camera jobs with no
// new marker the mod numbers the jobs itself; any real marker hands the clock straight back.
class FrameSyncOwnClock {
public:
    static constexpr unsigned AFTER_JOBS=240;
    enum class Step {markers,advance,started,stopped};
    // At a main camera job. markers: PCL markers seen so far.
    Step job(uint64_t markers) {
        if(markers!=m_markers){m_markers=markers;m_jobs=0;const bool was=m_active;m_active=false;return was?Step::stopped:Step::markers;}
        if(m_active)return Step::advance;
        if(++m_jobs<AFTER_JOBS)return Step::markers;
        m_active=true;return Step::started;
    }
    bool active() const {return m_active;}
private:
    uint64_t m_markers{};unsigned m_jobs{};bool m_active{};
};
