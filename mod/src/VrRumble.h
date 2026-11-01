#pragma once
// Game rumble -> controller haptics (docs/HAPTICS.md). The engine drives gamepad rumble through
// its resolved XInputSetState (EngineControllerRegistration's rumble slot). Slot-zero motor
// speeds land in the Mailbox on the game's input thread; VrSession's controller poll turns the
// latest speeds into per-hand haptic commands through Driver, and the backend plays them.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace vr_rumble {
using Clock=std::chrono::steady_clock;

// XInput motors: low = left, heavy low-frequency motor; high = right, light high-frequency motor.
struct Motors { uint16_t low{},high{}; bool operator==(const Motors&) const=default; };

// Single writer (the engine's rumble call), single reader (the controller poll). A torn read
// pairs one call's speeds with a neighbouring call's time, which the Driver tolerates.
class Mailbox {
public:
    static Mailbox& get() { static Mailbox value; return value; }
    struct Read { Motors motors; Clock::time_point at; uint64_t calls{}; };
    void publish(Motors motors,Clock::time_point now) {
        m_motors.store(uint32_t(motors.low)|uint32_t(motors.high)<<16,std::memory_order_relaxed);
        m_at.store(now.time_since_epoch().count(),std::memory_order_relaxed);
        m_calls.fetch_add(1,std::memory_order_release);
    }
    Read read() const {
        const auto calls=m_calls.load(std::memory_order_acquire);
        const auto packed=m_motors.load(std::memory_order_relaxed);
        return {{uint16_t(packed&0xffff),uint16_t(packed>>16)},Clock::time_point{Clock::duration{m_at.load(std::memory_order_relaxed)}},calls};
    }
private:
    std::atomic<uint32_t> m_motors{};
    std::atomic<Clock::rep> m_at{};
    std::atomic<uint64_t> m_calls{};
};

struct Settings {
    float gain{1};  // Scales both hands; 0..2, clamped to full amplitude.
    float cross{1}; // How much the other motor drives a hand: 1 = both hands feel both motors, 0 = left/right split.
    // Headset A/B for "no haptics" with every pulse accepted (docs/HAPTICS.md, 27 Oct): an explicit
    // vibration frequency instead of the runtime's default (0 = XR_FREQUENCY_UNSPECIFIED), and one
    // strong half-second pulse on both hands once the session can play it.
    float frequency{0};
    bool test_pulse{};
    bool valid() const {
        return std::isfinite(gain) && gain>=0 && gain<=2 && std::isfinite(cross) && cross>=0 && cross<=1 &&
            std::isfinite(frequency) && (frequency==0 || (frequency>=1 && frequency<=1000));
    }
};

// Left hand follows the low (left) motor, right hand the high (right) motor, each topped up by
// `cross` of the other. A pad's motors differ in frequency, not side, so cross defaults to 1.
inline std::array<float,2> amplitudes(Motors m,const Settings& s) {
    const float low=m.low/65535.f,high=m.high/65535.f;
    const auto scale=[&](float v){return std::clamp(v*s.gain,0.f,1.f);};
    return {scale(std::max(low,s.cross*high)),scale(std::max(high,s.cross*low))};
}

inline constexpr auto PULSE=std::chrono::milliseconds(100);
struct Command {
    enum class Op : uint8_t { none,apply,stop };
    Op op{}; float amplitude{};
    float frequency{}; // Hz; 0 = the runtime's default
    std::chrono::milliseconds duration{PULSE};
};
using Commands=std::array<Command,2>; // Left, right.

// OpenXR/OpenVR pulses have a duration, so a held rumble is re-sent before it runs out, and a
// change is sent at once. A speed the game has not re-sent for STALE is treated as zero, so a
// pause, a focus change or a missed stop never leaves the controllers buzzing.
class Driver {
public:
    static constexpr auto PULSE=vr_rumble::PULSE;
    static constexpr auto TEST_PULSE=std::chrono::milliseconds(500);
    static constexpr auto REFRESH=std::chrono::milliseconds(50);
    static Commands test(const Settings& settings) {
        const Command pulse{Command::Op::apply,1,settings.frequency,TEST_PULSE};
        return {pulse,pulse};
    }
    static constexpr auto STALE=std::chrono::milliseconds(1000);
    static constexpr float FLOOR=.02f,STEP=.05f;
    Commands update(const Mailbox::Read& read,bool allowed,Clock::time_point now,const Settings& settings) {
        const bool fresh=allowed && read.calls && now>=read.at && now-read.at<=STALE;
        const auto target=fresh?amplitudes(read.motors,settings):std::array<float,2>{};
        Commands out{};
        for(size_t hand=0;hand<2;++hand) {
            auto& playing=m_playing[hand];
            if(target[hand]<FLOOR) {
                if(playing>0) { out[hand]={Command::Op::stop,0}; playing=0; ++m_stops; }
                continue;
            }
            if(playing==0 || std::abs(target[hand]-playing)>=STEP || now-m_sent[hand]>=REFRESH) {
                out[hand]={Command::Op::apply,target[hand],settings.frequency}; playing=target[hand]; m_sent[hand]=now; ++m_applies;
                m_peak=std::max(m_peak,target[hand]);++m_bins[bin(target[hand])];
            }
        }
        return out;
    }
    bool playing() const { return m_playing[0]>0 || m_playing[1]>0; }
    uint64_t applies() const { return m_applies; }
    uint64_t stops() const { return m_stops; }
    float peak() const { return m_peak; }
    // Applied pulses by amplitude: under 0.1, 0.25, 0.5, and the rest. Cumulative; the difference
    // between two log lines is what a stretch of play felt like (26 Oct: 'no haptics' while
    // firing the heavy, with ~14 pulses a second sent and no runtime failure).
    static constexpr std::array<float,3> BIN_EDGES{.1f,.25f,.5f};
    static size_t bin(float amplitude) { size_t i=0; while(i<BIN_EDGES.size() && amplitude>=BIN_EDGES[i]) ++i; return i; }
    const std::array<uint64_t,4>& bins() const { return m_bins; }
private:
    std::array<float,2> m_playing{};
    std::array<Clock::time_point,2> m_sent{};
    uint64_t m_applies{},m_stops{};
    float m_peak{};
    std::array<uint64_t,4> m_bins{};
};
}
