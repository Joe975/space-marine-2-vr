#pragma once
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>

// Controls-mode startup invariant, independent of VR input arming/tracking.
// Three distinct camera jobs must observe the same exact native player owner.
// This establishes owned pose readiness; it does not certify server tracker lifetime.
class OwnedStereoReadiness {
public:
    using Clock=std::chrono::steady_clock;
    using Identity=std::array<uintptr_t,7>; // actor, entity, controller, wrapper, network, definition, instance
    enum class Reason:uint32_t {disabled,actor_missing,pose_missing,owner_invalid,world_invalid,future,stale,owner_changed,settling,ready,job,viewer,menu,cutscene,loading};
    // viewer: the free camera owns the view while the game is frozen in photo mode, so the
    // (stopped) player pose is irrelevant. It earns no first-person credit: leaving the viewer
    // settles over three jobs again.
    // cutscene: the game's cutscene flag is up. The view is the held trigger spot or the game's
    // cinematic camera (VrBodyFollowing), not the player's pose, which story cutscenes remove
    // (24 Oct 20:00: actor_missing for two minutes, no stereo). Like the viewer: no first-person credit.
    // menu: a native menu that still renders the world behind it (24 Oct: mission select,
    // 0x800) shows on the menu panel, so owned stereo stands down until it closes.
    // loading: the game's loading screen is up (EngineCinematicState::LOADING). It carries the
    // cutscene bit too (0x4010, 0x4410, 0x5410, 0x4c10 loading into a mission, 25 Oct 18:57), but
    // what renders behind the opaque screen is the half-streamed level (Joe: "white snow"), so
    // stereo stands down and the headset shows the flat loading screen. Wins over cutscene.
    struct Observation {uintptr_t actor{};Identity owner{};Clock::time_point received{},owner_received{};bool pose{},current{},world{},viewer{},menu{},cutscene{},loading{};};
    struct Snapshot {Reason reason{Reason::disabled};Identity owner{};uint64_t checks{},blocked{},ready_jobs{},owner_changes{},last_job{};unsigned consecutive{};
        uint64_t resumes{};}; // jobs that resumed at once after a brief same-owner refusal (resume window)
    // Brief refusals with the owner unchanged (26 Oct, docs/STEREO_PAUSES_2026-10-26.md). In the
    // 18:30 campaign run the pairs paused 59 times, median 108 ms: a game frame over 100 ms (the
    // player pose, its owner or the body tracker past the 100 ms freshness limit at the render
    // job) or a frame-clock skip refused one job, and the three-job settle then refused two more,
    // so each one-job refusal cost three mono frames and the head pose went stale. The settle is a
    // startup invariant (stereo before the player existed crashed, 19 Oct): three jobs with one
    // owner. An owner already proven that way within RESUME and seen again, exactly (all seven
    // parts) and fully revalidated on this job, needs no new proof: the job is ready at once.
    // Only freshness refusals (actor, pose, owner check, stale) and clock skips keep that credit;
    // a menu, loading, viewer, cutscene, invalid world, future timestamp, older job, owner change
    // or any other gap() clears it. resume(0) restores the old rule (SM2VR_STEREO_RESUME_MS=0).
    static constexpr auto RESUME=std::chrono::milliseconds(1000),MAX_RESUME=std::chrono::milliseconds(10000);
    void resume(std::chrono::milliseconds value){m_resume=value<std::chrono::milliseconds::zero()?RESUME:std::min(value,MAX_RESUME);}
    std::chrono::milliseconds resume()const{return m_resume;}
    // SM2VR_STEREO_RESUME_MS: whole milliseconds 0..MAX_RESUME; nullopt for anything else.
    static std::optional<std::chrono::milliseconds> resume_from(const char* text){
        if(!text || !*text)return std::nullopt;char* end{};const long ms=std::strtol(text,&end,10);
        if(*end || ms<0 || ms>MAX_RESUME.count())return std::nullopt;return std::chrono::milliseconds(ms);}
    static bool keeps_credit(Reason value){return value==Reason::actor_missing || value==Reason::pose_missing || value==Reason::owner_invalid || value==Reason::stale;}
    static const char* name(Reason value){switch(value){
        case Reason::disabled:return "disabled";case Reason::actor_missing:return "actor_missing";case Reason::pose_missing:return "pose_missing";
        case Reason::owner_invalid:return "owner_invalid";case Reason::world_invalid:return "world_invalid";case Reason::future:return "future";
        case Reason::stale:return "stale";case Reason::owner_changed:return "owner_changed";case Reason::settling:return "settling";
        case Reason::ready:return "ready";case Reason::job:return "job";case Reason::viewer:return "viewer";case Reason::menu:return "menu";case Reason::cutscene:return "cutscene";case Reason::loading:return "loading";}return "unknown";}
    Snapshot snapshot()const{return m_state;}
    // keep_credit: a frame-clock skip (no unique job: nothing about the owner changed).
    void gap(bool keep_credit=false){m_state.consecutive=0;m_state.owner={};m_state.reason=Reason::job;if(!keep_credit)m_credit={};}
    bool observe(uint64_t job,const Observation& value,Clock::time_point now){
        ++m_state.checks;
        const bool new_job=job && job>m_state.last_job;
        const auto reject=[&](Reason reason){m_state.reason=reason;m_state.consecutive=0;m_state.owner={};++m_state.blocked;if(new_job)m_state.last_job=job;if(!keeps_credit(reason))m_credit={};return false;};
        if(!job || job<m_state.last_job)return reject(Reason::job);
        if(value.loading && !value.viewer)return reject(Reason::loading);
        if(value.viewer || value.cutscene){m_state.reason=value.viewer?Reason::viewer:Reason::cutscene;m_state.consecutive=0;m_state.owner={};m_credit={};if(new_job){m_state.last_job=job;++m_state.ready_jobs;}return true;}
        if(value.menu)return reject(Reason::menu);
        if(!value.actor)return reject(Reason::actor_missing);
        if(!value.pose)return reject(Reason::pose_missing);
        bool complete=true;for(auto id:value.owner)complete=complete && id;
        if(!complete || value.owner[0]!=value.actor || !value.current)return reject(Reason::owner_invalid);
        if(!value.world)return reject(Reason::world_invalid);
        if(value.received>now || value.owner_received>now)return reject(Reason::future);
        if(now-value.received>std::chrono::milliseconds(100) || now-value.owner_received>std::chrono::milliseconds(100))return reject(Reason::stale);
        const bool changed=m_state.consecutive && m_state.owner!=value.owner;
        if(changed){++m_state.owner_changes;m_state.consecutive=0;m_credit={};}
        m_state.owner=value.owner;
        if(new_job){m_state.last_job=job;
            if(!m_state.consecutive && resumable(value.owner,now)){m_state.consecutive=3;++m_state.resumes;}
            else if(m_state.consecutive<3)++m_state.consecutive;}
        // A second check of one job can revoke readiness, but never earn credit.
        if(m_state.consecutive<3){m_state.reason=changed?Reason::owner_changed:Reason::settling;++m_state.blocked;return false;}
        m_state.reason=Reason::ready;if(new_job)++m_state.ready_jobs;m_credit={value.owner,now,true};return true;
    }
private:
    struct Credit {Identity owner{};Clock::time_point at{};bool valid{};};
    bool resumable(const Identity& owner,Clock::time_point now)const{
        return m_resume.count()>0 && m_credit.valid && m_credit.owner==owner && now>=m_credit.at && now-m_credit.at<=m_resume;}
    Snapshot m_state;Credit m_credit;std::chrono::milliseconds m_resume{RESUME};
};

// The menu stand-down outlasts the menu by SETTLE. 24 Oct evening, twice: choosing "return to the
// Battle Barge" closed the pause menu into a load, owned stereo resumed 0.15 s later, and the game's
// render thread then spun for good in its pool release (a binary search over a record array whose
// count read 0xffffffff: the pool torn down under it; hang.dmp, docs/HEADSET_ALWAYS_SHOWS). The
// 0.1.4 crash also followed stereo resuming at a load's start. Held SETTLE past the menu, a load
// has removed the player first (readiness then waits for the next scene); a plain unpause shows
// the flat game for SETTLE.
// 26 Oct timer audit (GENERIC_GUARDS piece 6): the hold did not stop that freeze (0.1.6 froze with
// it); the cause was the second eye's double pool release, fixed by NativePoolReleaseGuard. The hold
// still costs SETTLE of flat view on every unpause. It goes once a scripted quit to title and return
// to the barge pass without it: SM2VR_DIAG_MENU_HOLD_MS=0 (settle()) runs that check.
// 30 Oct: off by default (SETTLE 0). The 29 Oct 20:53 playtest showed 3.06-3.08 s of flat panel at each
// of six unpauses; every level-transition hold since 27 Oct ended about 1 s after gameplay resumed
// (PLAYTEST_2026-10-29_2053). SM2VR_DIAG_MENU_HOLD_MS=3000 restores the old hold (LEGACY_SETTLE).
class MenuHold {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr auto SETTLE=std::chrono::milliseconds(0),LEGACY_SETTLE=std::chrono::milliseconds(3000);
    // gate: the menu panel's gate this frame. True while the menu is up and for the settle after.
    bool update(bool gate,Clock::time_point now){if(gate)m_until=now+m_settle;return gate || now<m_until;}
    void settle(std::chrono::milliseconds value){m_settle=value<std::chrono::milliseconds::zero()?SETTLE:value;}
    std::chrono::milliseconds settle()const{return m_settle;}
private:
    Clock::time_point m_until{};std::chrono::milliseconds m_settle{SETTLE};
};
