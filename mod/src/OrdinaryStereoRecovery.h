#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>
#include "OwnedStereoReadiness.h"

// Recovery policy for a recoverable owned-stereo refusal (for example a changed
// native bloom texture contract). Pure logic: no native or GPU access.
// While suspended the producer renders native mono and the headset shows the
// flat fallback panel. A stereo attempt is allowed only after a backoff AND an
// independent read-only probe says the refused condition has cleared; the attempt
// itself is still fully authenticated by its consumer. Each refused attempt
// doubles the backoff up to max_backoff, where it stays. Recovery is unlimited
// (Joe, 21 Oct): no episode or attempt count ever ends stereo for the session.
// Only exhaust() (another, permanent refusal) does.
// A level transition (suspend(now,true): the tracked packet's refusals 4/12/13) holds
// longer: the probe must stay clear for transition_settle without a break, measured
// across every job, before the first attempt. 23 Oct 19:55: resuming 330 ms into the
// hangar -> training load was followed 1.6 s later by a fault in the game's own scene
// render (PLAYTEST_2026-10-23_1948); 0.1.1, which never resumed, did not crash there.
// A cutscene start raises the same refusal (24 Oct, PLAYTEST_2026-10-24_1720: three
// cutscenes, each 8 s of flat panel with the probe clear throughout). When the game's
// cutscene flag is up during a hold (may_attempt's cutscene), the episode is a cutscene
// cut for the rest of it and settles after cutscene_settle instead. A load with the flag
// down (the 0.1.4 crash: scripted=false) keeps the long settle.
// A hold whose owned pair has been renewed (may_attempt's renewed: the bloom retired the pair after
// the pool was rebuilt, so the next job is a fresh bootstrap) settles after renewal_settle instead
// (Joe, 26 Oct: shorten the load hold). The 8 s settle was a timer for the 0.1.4 crash, whose cause
// is now known (LOAD_CRASH_2026-10-26: a game PIP view behind the pair) and guarded separately by
// ForeignViewHold, which keeps stereo off until 5 s after a PIP. Every load hold in the 26 Oct 14:46
// run ended exactly 8.1 s after its renewal; now about 1 s after it.
class OrdinaryStereoRecovery {
public:
    using Clock=std::chrono::steady_clock;
    struct Settings {std::chrono::milliseconds first_backoff{250},max_backoff{8000},transition_settle{8000},cutscene_settle{500},renewal_settle{1000};};
    struct Stats {
        uint64_t episodes{},attempts{},refused_attempts{},recoveries{},probe_refusals{};
        uint64_t transition_episodes{},settle_breaks{},cutscene_holds{};
        bool suspended{},exhausted{},attempting{},transition{},cutscene{}; // cutscene: this transition hold saw the cutscene flag.
        Clock::time_point since{},next_attempt{},clear_since{}; // clear_since: start of the current clear run (transition holds).
    };
    OrdinaryStereoRecovery()=default;
    explicit OrdinaryStereoRecovery(Settings settings):m_settings(settings){}
    // A recoverable refusal. Returns false only after exhaust(): the caller must
    // then fail closed. A refusal of the attempt in flight counts against that
    // attempt; a refusal while already suspended without an attempt (an in-flight
    // job admitted before the suspension took effect) is not new.
    // transition=true: a level-transition refusal; it turns the episode (new or current)
    // into a transition hold and restarts the settle run.
    bool suspend(Clock::time_point now,bool transition=false){
        if(m_stats.exhausted)return false;
        if(transition){if(!m_stats.transition || !m_stats.suspended)increment(m_stats.transition_episodes);m_stats.clear_since={};}
        if(!m_stats.suspended){
            increment(m_stats.episodes);m_stats.suspended=true;m_stats.attempting=false;m_stats.since=now;m_backoff=m_settings.first_backoff;
            m_stats.transition=transition;m_stats.cutscene=false;m_stats.clear_since={};m_stats.next_attempt=now+m_backoff;return true;
        }
        if(transition)m_stats.transition=true;
        if(!m_stats.attempting)return true;
        m_stats.attempting=false;increment(m_stats.refused_attempts);m_stats.clear_since={};
        m_backoff=(std::min)(m_backoff*2,m_settings.max_backoff);m_stats.next_attempt=now+m_backoff;return true;
    }
    // Producer gate, evaluated once per native job while suspended. cutscene: the game's
    // cutscene flag on this job (EngineCinematicState). renewed: the owned pair was renewed in
    // this episode and a fresh bootstrap is due (NativeOrdinaryBloom).
    bool may_attempt(Clock::time_point now,bool probe_clear,bool cutscene=false,bool renewed=false){
        if(!m_stats.suspended || m_stats.exhausted)return false;
        if(m_stats.attempting)return true; // Admission already granted; keep it until the consumer reports.
        if(m_stats.transition){
            if(cutscene && !m_stats.cutscene){m_stats.cutscene=true;increment(m_stats.cutscene_holds);}
            // Every job counts, backoff or not: one unclear probe restarts the settle run.
            if(!probe_clear){if(m_stats.clear_since!=Clock::time_point{})increment(m_stats.settle_breaks);m_stats.clear_since={};}
            else if(m_stats.clear_since==Clock::time_point{})m_stats.clear_since=now;
            if(now<m_stats.next_attempt)return false;
            if(!probe_clear){increment(m_stats.probe_refusals);return false;}
            const auto settle=m_stats.cutscene?m_settings.cutscene_settle:renewed?(std::min)(m_settings.renewal_settle,m_settings.transition_settle):m_settings.transition_settle;
            if(now-m_stats.clear_since<settle)return false;
        }
        if(now<m_stats.next_attempt)return false;
        if(!probe_clear){increment(m_stats.probe_refusals);return false;}
        increment(m_stats.attempts);m_stats.attempting=true;return true;
    }
    // The consumer accepted a complete resumed stereo job.
    void recovered(){
        if(!m_stats.suspended)return;
        m_stats.suspended=false;m_stats.attempting=false;m_stats.transition=false;m_stats.cutscene=false;m_stats.clear_since={};increment(m_stats.recoveries);
    }
    // Permanent stop for another reason: no further attempts.
    void exhaust(){m_stats.exhausted=true;m_stats.attempting=false;}
    std::chrono::milliseconds backoff()const{return m_backoff;}
    // Per-episode log lines over a whole session: every one of the first 32, then
    // every 64th (episodes are numbered from 1). Counters saturate, never wrap.
    static bool log_episode(uint64_t episode){return episode && (episode<=32 || episode%64==0);}
    static void increment(uint64_t& value){if(value!=UINT64_MAX)++value;}
    // Headset content while owned stereo is lost: the flat game image on the
    // head-locked panel instead of the runtime's empty/frozen view. A stop applies
    // from the moment the producer is armed, even before its first pair: a stop
    // during loading (22 Oct: a swap-chain reset) otherwise leaves the runtime
    // with no frames at all. A suspension is a short recoverable episode, so it
    // only falls back once stereo has run. A native menu already uses the same
    // panel, so it needs no fallback.
    // waiting (24 Oct evening): an armed producer that has submitted no pair for STALL while
    // neither suspended nor stopped: owned readiness blocking every job (no player actor through a
    // load or a cutscene), or no pair yet at the title screen. Before, the headset then got
    // nothing at all; now it always gets either stereo or the flat game.
    enum class Fallback:uint32_t {none,suspended,stopped,waiting};
    static constexpr auto STALL=std::chrono::milliseconds(500);
    static const char* name(Fallback value){switch(value){case Fallback::none:return "none";case Fallback::suspended:return "suspended";case Fallback::stopped:return "stopped";case Fallback::waiting:return "waiting";}return "unknown";}
    static Fallback fallback(bool producer_armed,bool producer_stopped,bool producer_suspended,uint64_t submitted_pairs,bool menu_panel,bool stalled=false){
        if(menu_panel || !producer_armed)return Fallback::none;
        if(producer_stopped)return Fallback::stopped;
        if(producer_suspended && submitted_pairs)return Fallback::suspended;
        return stalled?Fallback::waiting:Fallback::none;
    }
    bool suspended()const{return m_stats.suspended;}
    bool exhausted()const{return m_stats.exhausted;}
    bool attempting()const{return m_stats.attempting;}
    Stats stats()const{return m_stats;}
private:
    Settings m_settings;Stats m_stats;std::chrono::milliseconds m_backoff{};
};

// The last rung (docs/GENERIC_GUARDS_2026-10-26.md): a stop of owned stereo for any reason the
// rungs above don't recover (an unknown refusal code, the producer's own refusal, a poisoned
// private DLSS eye, a stopped pair retention) restarts the whole owned-stereo stack from its
// startup state instead of ending stereo for the session. 22-25 Oct each new stop cause got its
// own recovery (swap-chain reset, packet refusals 4/12/13, pool rebuild, chain restart, refusals 5
// and 6); this one doesn't need to know the cause. Pure policy, present thread.
// The stack restarts once the stop has lasted SETTLE and the game is not loading (the 0.1.4
// crash followed stereo resuming inside a load); the producer's warmup and owned readiness then
// settle as at startup. More than BUDGET restarts in WINDOW is a real fault: stop for the session.
// A device-reset re-arm in progress owns the restart (Sm2Probe::rearm_ordinary_headset).
class OrdinaryColdRestart {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr auto SETTLE=std::chrono::seconds(3),WINDOW=std::chrono::minutes(10);
    static constexpr unsigned BUDGET=3,ATTEMPTS=600; // ATTEMPTS: presents to retry a busy stack, about ten seconds.
    enum class Action:uint32_t {none,attempt,give_up};
    static const char* name(Action value){switch(value){case Action::none:return "none";case Action::attempt:return "attempt";case Action::give_up:return "give_up";}return "unknown";}
    // stopped: owned stereo is stopped (the producer, the bloom chain or the pair retention).
    // rearm: a device-reset re-arm is pending. loading: the game's loading screen is up.
    Action observe(Clock::time_point now,bool stopped,bool rearm,bool loading){
        if(m_exhausted)return Action::none;
        if(!stopped || rearm){m_since={};m_attempts=0;return Action::none;}
        if(m_since==Clock::time_point{})m_since=now;
        if(loading || now-m_since<SETTLE)return Action::none;
        while(!m_restarts.empty() && now-m_restarts.front()>WINDOW)m_restarts.erase(m_restarts.begin());
        if(m_restarts.size()>=BUDGET || ++m_attempts>ATTEMPTS){m_exhausted=true;return Action::give_up;}
        return Action::attempt;
    }
    // The stack restarted: counts against the budget and waits for the next stop.
    void restarted(Clock::time_point now){m_restarts.push_back(now);++m_total;m_since={};m_attempts=0;}
    bool exhausted()const{return m_exhausted;}
    uint64_t total()const{return m_total;}
    unsigned attempts()const{return m_attempts;}
    size_t in_window()const{return m_restarts.size();}
private:
    std::vector<Clock::time_point> m_restarts;Clock::time_point m_since{};unsigned m_attempts{};uint64_t m_total{};bool m_exhausted{};
};

// The other way stereo can be gone for good without anything stopping (26 Oct playtest, 11:19):
// the producer runs, the game is in plain play, and owned readiness refuses every job for a
// player-state reason (that time actor_missing: the body probe's full tracker table had dropped
// the reloaded player's tracker). Nothing is stopped or suspended, so neither the recovery
// rungs nor the cold restart act, and the headset shows the flat panel for the rest of the run.
// When that has lasted LIMIT, the observation probes forget what they hold and learn the
// current level's objects afresh (EngineBodyProbe/EngineWeaponProbe::forget); again every LIMIT
// up to TRIES times, then it gives up for this stall (logged once). Ready jobs end the stall.
// Pure policy; present thread.
class ReadinessStall {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr auto LIMIT=std::chrono::seconds(10);
    static constexpr unsigned TRIES=3;
    enum class Action:uint32_t {none,rediscover,give_up};
    static const char* name(Action value){switch(value){case Action::none:return "none";case Action::rediscover:return "rediscover";case Action::give_up:return "give_up";}return "unknown";}
    // expect_player: the scene has a player (SceneMode gameplay or hub). running: the producer is
    // armed, neither stopped nor suspended. blocked: readiness's reason is a player-state one.
    // ready_jobs: readiness's count of ready jobs (any advance ends the stall).
    Action observe(Clock::time_point now,bool expect_player,bool running,bool blocked,uint64_t ready_jobs){
        const bool progressed=m_started && ready_jobs!=m_ready_jobs;m_ready_jobs=ready_jobs;m_started=true; // the first poll is a baseline
        if(!expect_player || !running || !blocked || progressed){m_since={};m_tries=0;m_gave_up=false;return Action::none;}
        if(m_since==Clock::time_point{} || now<m_since){m_since=now;m_next=now+LIMIT;}
        if(m_gave_up || now<m_next)return Action::none;
        if(m_tries>=TRIES){m_gave_up=true;return Action::give_up;}
        ++m_tries;++m_total;m_next=now+LIMIT;return Action::rediscover;
    }
    // The readiness reasons that mean the player's state isn't being read (OwnedStereoReadiness).
    static bool player_reason(OwnedStereoReadiness::Reason r){
        using R=OwnedStereoReadiness::Reason;
        return r==R::actor_missing || r==R::pose_missing || r==R::owner_invalid || r==R::world_invalid || r==R::stale;
    }
    std::chrono::milliseconds stalled(Clock::time_point now)const{return m_since==Clock::time_point{} || now<m_since?std::chrono::milliseconds{}:std::chrono::duration_cast<std::chrono::milliseconds>(now-m_since);}
    unsigned tries()const{return m_tries;}
    uint64_t total()const{return m_total;}
private:
    Clock::time_point m_since{},m_next{};unsigned m_tries{};uint64_t m_total{},m_ready_jobs{};bool m_gave_up{},m_started{};
};

// What the eyes get (docs/STEREO_PAUSES_2026-10-26.md): the time between two pairs submitted to
// the runtime. Nothing is submitted between pairs, so the runtime keeps reprojecting the last one;
// over Fallback STALL the flat panel shows. A pause in the log (pairs=0 to OrdinaryHeadsetResumed)
// measures the producer, not this: on 26 Oct 18:49:00 two 130 ms pauses 0.5 s apart showed the
// flat panel for 0.65 s. take() returns the window's counts and longest gap, then starts a new one.
class SubmitGaps {
public:
    using Clock=std::chrono::steady_clock;
    struct Window {uint64_t submits{},over_100ms{},over_stall{};std::chrono::milliseconds longest{};};
    void submit(Clock::time_point now){
        if(m_last!=Clock::time_point{} && now>=m_last){const auto gap=std::chrono::duration_cast<std::chrono::milliseconds>(now-m_last);
            m_window.longest=std::max(m_window.longest,gap);if(gap>std::chrono::milliseconds(100))++m_window.over_100ms;
            if(gap>=OrdinaryStereoRecovery::STALL)++m_window.over_stall;}
        m_last=now;++m_window.submits;
    }
    Window take(){const auto value=m_window;m_window={};return value;}
private:
    Clock::time_point m_last{};Window m_window;
};
