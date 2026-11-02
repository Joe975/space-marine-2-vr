#include "OrdinaryStereoRecovery.h"
#include "ForeignViewHold.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
using R=OrdinaryStereoRecovery;using namespace std::chrono_literals;
const auto T0=R::Clock::time_point{}+100s;

void backoff_and_probe(){
    R r{R::Settings{250ms,1000ms}};
    require(!r.suspended() && !r.may_attempt(T0,true),"running stereo never needs an admission");
    require(r.suspend(T0) && r.suspended() && r.stats().episodes==1,"first refusal suspends instead of stopping");
    require(r.suspend(T0+1ms) && r.stats().refused_attempts==0,"in-flight refusal before any attempt is not a new failure");
    require(!r.may_attempt(T0+249ms,true),"no attempt before the backoff");
    require(!r.may_attempt(T0+300ms,false) && r.stats().probe_refusals==1 && !r.attempting(),"backoff over but probe still refuses: stay native");
    require(r.may_attempt(T0+301ms,true) && r.attempting() && r.stats().attempts==1,"cleared probe after backoff admits one attempt");
    require(r.may_attempt(T0+302ms,false),"granted admission is held until the consumer reports, even if the probe flickers");
    require(r.suspend(T0+310ms) && r.stats().refused_attempts==1 && !r.attempting(),"refused attempt counted");
    require(!r.may_attempt(T0+310ms+499ms,true) && r.may_attempt(T0+310ms+500ms,true),"backoff doubles after a refused attempt");
    require(r.suspend(T0+900ms) && !r.may_attempt(T0+900ms+999ms,true) && r.may_attempt(T0+1900ms,true),"backoff is capped");
    r.recovered();require(!r.suspended() && r.stats().recoveries==1 && !r.attempting(),"accepted resumed job ends the episode");
    require(r.suspend(T0+5s) && r.stats().episodes==2 && !r.may_attempt(T0+5s+249ms,true) && r.may_attempt(T0+5s+250ms,true),"new episode restarts at the first backoff");
}
// 23 Oct 19:55 crash (PLAYTEST_2026-10-23_1948): a level transition resumed after 250 ms,
// inside the load. A transition episode waits for an unbroken clear run of transition_settle.
void transition_hold(){
    R r{R::Settings{250ms,1000ms,8000ms}};
    require(r.suspend(T0,true) && r.stats().transition && r.stats().transition_episodes==1,"level-transition refusal starts a transition hold");
    require(!r.may_attempt(T0+330ms,true),"the 0.1.4 resume point (330 ms, clear probe) stays on the flat panel");
    require(!r.may_attempt(T0+8329ms,true) && r.may_attempt(T0+8330ms,true),"attempt once the probe has been clear for the whole settle window, counted from the first clear job");
    r.recovered();require(!r.stats().transition && r.stats().recoveries==1,"recovery ends the hold");
    // The load churns: a single unclear job (the 1.6 s contract change) restarts the run.
    require(r.suspend(T0+20s,true) && r.stats().transition_episodes==2,"second transition");
    require(!r.may_attempt(T0+21s,true) && !r.may_attempt(T0+26s,false) && r.stats().settle_breaks==1,"unclear probe mid-hold breaks the settle run");
    require(!r.may_attempt(T0+27s,true) && !r.may_attempt(T0+34999ms,true) && r.may_attempt(T0+35s,true),"settle restarts from the next clear job");
    require(r.suspend(T0+35100ms) && !r.attempting() && r.stats().refused_attempts==1,"refused attempt during a hold");
    require(r.stats().transition && !r.may_attempt(T0+36s,true) && !r.may_attempt(T0+43999ms,true) && r.may_attempt(T0+44s,true),"a refused attempt needs a whole new settle run");
    r.recovered();
    // An ordinary episode that a transition refusal reaches mid-way becomes a hold.
    require(r.suspend(T0+60s) && !r.stats().transition && r.suspend(T0+60100ms,true) && r.stats().transition && r.stats().transition_episodes==3,"transition upgrades a running ordinary episode");
    require(!r.may_attempt(T0+61s,true) && r.may_attempt(T0+69s,true),"the upgraded episode holds for the settle window");
    r.recovered();require(r.suspend(T0+80s) && r.may_attempt(T0+80250ms,true),"an ordinary contract episode keeps the 250 ms backoff");
}
// 24 Oct (PLAYTEST_2026-10-24_1720): cutscene starts raise the transition refusal too, and
// each held 8 s. The game's cutscene flag during the hold makes it a cutscene cut.
void cutscene_hold(){
    R r{R::Settings{250ms,1000ms,8000ms,500ms}};
    // 17:52:47: suspended, the flag up about 1.4 s later, the probe clear throughout.
    require(r.suspend(T0,true),"cutscene start suspends as a transition");
    require(!r.may_attempt(T0+300ms,true,false) && !r.may_attempt(T0+1300ms,true,false),"before the flag it is an ordinary transition hold");
    require(r.may_attempt(T0+1400ms,true,true) && r.stats().cutscene && r.stats().cutscene_holds==1,"the flag ends the hold at once once the probe has been clear for 0.5 s");
    r.recovered();require(!r.stats().cutscene,"recovery clears the cutscene mark");
    // Flag up at the start: 0.5 s of clear probe, the backoff still first.
    require(r.suspend(T0+10s,true) && !r.may_attempt(T0+10s,true,true) && !r.may_attempt(T0+10499ms,true,true) && r.may_attempt(T0+10500ms,true,true),"flag from the first job: 0.5 s settle");
    r.recovered();
    // The mark sticks for the episode: the flag dropping again does not bring back 8 s.
    require(r.suspend(T0+20s,true) && !r.may_attempt(T0+20100ms,true,true) && r.may_attempt(T0+20600ms,true,false),"a cutscene hold stays short when the flag drops");
    r.recovered();
    // An unclear probe still restarts the run, short or long.
    require(r.suspend(T0+30s,true) && !r.may_attempt(T0+30100ms,true,true) && !r.may_attempt(T0+30400ms,false,true) && r.stats().settle_breaks==1 && !r.may_attempt(T0+30800ms,true,true) && r.may_attempt(T0+31300ms,true,true),"an unclear probe restarts the cutscene settle");
    r.recovered();
    // A level load without the flag (the 0.1.4 crash) keeps the long settle; a new episode starts unmarked.
    require(r.suspend(T0+40s,true) && !r.stats().cutscene && !r.may_attempt(T0+40300ms,true,false) && !r.may_attempt(T0+48299ms,true,false) && r.may_attempt(T0+48300ms,true,false) && r.stats().cutscene_holds==4,"no flag: the 8 s level-load settle");
    r.recovered();
    // The flag only shortens transition holds; an ordinary episode keeps its backoff.
    require(r.suspend(T0+60s) && !r.may_attempt(T0+60100ms,true,true) && !r.stats().cutscene && r.may_attempt(T0+60250ms,true,true),"ordinary episodes ignore the flag");
}
// Joe, 21 Oct: recovery is unlimited. Default settings, as in the game.
void unlimited(){
    auto t=T0;R episodes;
    for(int i=0;i<1000;++i){require(episodes.suspend(t),"every episode suspends instead of stopping");t+=1s;
        require(episodes.may_attempt(t,true),"every episode gets its attempt");episodes.recovered();}
    require(!episodes.exhausted() && episodes.stats().episodes==1000 && episodes.stats().recoveries==1000,"no per-session episode cap");
    R attempts;require(attempts.suspend(t),"suspended");
    for(int i=0;i<1000;++i){t+=9s;require(attempts.may_attempt(t,true) && attempts.suspend(t),"a refused attempt never ends stereo");}
    require(!attempts.exhausted() && attempts.stats().refused_attempts==1000 && attempts.backoff()==8000ms,"backoff stays at the 8 s maximum indefinitely");
    require(!attempts.may_attempt(t+7999ms,true) && attempts.may_attempt(t+8000ms,true),"retry every 8 s for the rest of the session");
    attempts.recovered();require(attempts.suspend(t+10s) && attempts.backoff()==250ms,"a new episode starts at the first backoff again");
    R stopped;require(stopped.suspend(T0),"suspended");stopped.exhaust();require(!stopped.may_attempt(T0+1h,true) && !stopped.suspend(T0),"only another permanent stop ends recovery");
}
void counters_and_logs(){
    uint64_t n=UINT64_MAX-1;R::increment(n);R::increment(n);require(n==UINT64_MAX,"counters saturate instead of wrapping");
    unsigned logged{};for(uint64_t e=0;e<=10000;++e)logged+=R::log_episode(e);
    require(!R::log_episode(0) && R::log_episode(1) && R::log_episode(32) && !R::log_episode(33) && R::log_episode(64) && !R::log_episode(65) && R::log_episode(128),"first 32 episodes, then every 64th");
    require(logged==32+10000/64,"bounded per-episode log volume over a long session");
}
void fallback(){
    using F=R::Fallback;
    require(R::fallback(true,false,true,10,false)==F::suspended,"suspended after stereo ran: flat panel");
    require(R::fallback(true,true,true,10,false)==F::stopped,"stop wins");
    require(R::fallback(true,false,false,10,false)==F::none,"running stereo: no fallback");
    require(R::fallback(true,true,false,0,false)==F::stopped,"stopped before the first pair (22 Oct swap-chain reset during loading): flat panel, not an empty runtime");
    require(R::fallback(true,false,true,0,false)==F::none,"suspended before the first pair: a short episode, no fallback yet");
    require(R::fallback(true,false,false,0,false)==F::none,"armed and warming up: no fallback");
    require(R::fallback(false,true,false,10,false)==F::none,"no producer: nothing to fall back from");
    require(R::fallback(false,true,false,0,false)==F::none,"never armed: nothing to fall back from");
    require(R::fallback(true,true,false,10,true)==F::none,"native menu already owns the panel");
    // 24 Oct evening: readiness blocking every job (no player actor through a load or cutscene).
    require(R::fallback(true,false,false,10,false,true)==F::waiting,"no pair for the stall time, neither suspended nor stopped: flat panel");
    require(R::fallback(true,false,false,0,false,true)==F::waiting,"no pair yet at the title screen: flat panel");
    require(R::fallback(true,false,false,10,false,false)==F::none,"stereo flowing: no fallback");
    require(R::fallback(true,false,true,10,false,true)==F::suspended && R::fallback(true,true,false,10,false,true)==F::stopped,"suspended and stopped keep their own reasons");
    require(R::fallback(true,false,false,10,true,true)==F::none && R::fallback(false,false,false,0,false,true)==F::none,"menu panel or no producer: none");
    require(R::STALL>=std::chrono::milliseconds(150) && std::string_view(R::name(F::waiting))=="waiting","stall is longer than the panel's own 150 ms gap");
}
// Joe, 26 Oct: shorten the load hold. A hold whose owned pair was renewed (a fresh bootstrap is due)
// settles after renewal_settle (1 s) instead of 8 s; the 14:46 run's holds all ended 8.1 s after the
// renewal. ForeignViewHold separately keeps stereo off until 5 s after a game PIP view.
void renewal_hold(){
    R r{R::Settings{250ms,1000ms,8000ms,500ms,1000ms}};
    require(r.suspend(T0,true) && r.stats().transition,"a load hold");
    require(!r.may_attempt(T0,true,false,true) && !r.may_attempt(T0+500ms,true,false,true) && !r.may_attempt(T0+999ms,true,false,true) && r.may_attempt(T0+1000ms,true,false,true),"renewed: 1 s of clear probe, counted from the first clear job");
    r.recovered();
    require(r.suspend(T0+20s,true) && !r.may_attempt(T0+20s,true,false,true) && !r.may_attempt(T0+20500ms,false,false,true) && r.stats().settle_breaks==1,"an unclear job breaks a renewed settle too");
    require(!r.may_attempt(T0+21s,true,false,true) && !r.may_attempt(T0+21999ms,true,false,true) && r.may_attempt(T0+22s,true,false,true),"then 1 s again from the next clear job");
    r.recovered();
    require(r.suspend(T0+40s,true) && !r.may_attempt(T0+40s,true) && !r.may_attempt(T0+47999ms,true) && r.may_attempt(T0+48s,true),"not renewed: the 8 s settle is unchanged");
    r.recovered();
    require(r.suspend(T0+60s,true) && !r.may_attempt(T0+60s,true,true,true) && r.may_attempt(T0+60500ms,true,true,true),"a cutscene cut keeps its 0.5 s");
    R slow{R::Settings{250ms,1000ms,800ms,500ms,1000ms}};
    require(slow.suspend(T0,true) && !slow.may_attempt(T0,true,false,true) && slow.may_attempt(T0+800ms,true,false,true),"never longer than the transition settle");
}
// The last rung (GENERIC_GUARDS): any stop restarts the stack after SETTLE outside a load, within a budget.
void cold_restart(){
    using C=OrdinaryColdRestart;using A=C::Action;C c;
    require(c.observe(T0,false,false,false)==A::none,"running stereo: nothing to do");
    require(c.observe(T0,true,false,false)==A::none && c.observe(T0+2999ms,true,false,false)==A::none,"a fresh stop waits SETTLE (in-flight jobs drain, the scene settles)");
    require(c.observe(T0+3s,true,false,false)==A::attempt && c.attempts()==1,"after SETTLE: restart the stack");
    require(c.observe(T0+3001ms,true,false,false)==A::attempt && c.attempts()==2,"a busy stack is retried on the next present");
    c.restarted(T0+3001ms);require(c.total()==1 && c.in_window()==1 && c.attempts()==0,"a restart counts against the budget");
    require(c.observe(T0+3002ms,false,false,false)==A::none,"stereo running again");
    // 0.1.4: stereo resumed inside a load and the game crashed. Never restart behind the loading screen.
    require(c.observe(T0+10s,true,false,true)==A::none && c.observe(T0+20s,true,false,true)==A::none,"no restart while the loading screen is up, however long");
    require(c.observe(T0+20001ms,true,false,false)==A::attempt,"the load ends: the stop has lasted SETTLE already");
    c.restarted(T0+20001ms);
    // A device reset's re-arm owns its own restart.
    require(c.observe(T0+30s,true,true,false)==A::none && c.observe(T0+40s,true,true,false)==A::none,"a pending re-arm defers the cold restart");
    require(c.observe(T0+40001ms,true,false,false)==A::none,"the re-arm gave up: SETTLE starts from here");
    require(c.observe(T0+43001ms,true,false,false)==A::attempt,"then the cold restart takes over");
    c.restarted(T0+43001ms);require(c.in_window()==3,"three restarts in the window");
    // A fourth stop within WINDOW is a real fault.
    require(c.observe(T0+50s,true,false,false)==A::none && c.observe(T0+53s,true,false,false)==A::give_up && c.exhausted(),"over budget: stopped for the session");
    require(c.observe(T0+60s,true,false,false)==A::none && c.observe(T0+20min,true,false,false)==A::none,"give_up is reported once; nothing after it");
    // Old restarts leave the window.
    C d;d.restarted(T0);d.restarted(T0+1s);d.restarted(T0+2s);
    require(d.observe(T0+11min,true,false,false)==A::none && d.observe(T0+11min+3s,true,false,false)==A::attempt,"restarts older than WINDOW no longer count");
    // A stack that never comes free gives up after ATTEMPTS presents.
    C e;e.observe(T0,true,false,false);A last{};for(unsigned i=0;i<C::ATTEMPTS+1;++i)last=e.observe(T0+3s+std::chrono::milliseconds(i),true,false,false);
    require(last==A::give_up && e.exhausted(),"a stack busy for ATTEMPTS presents gives up");
    require(std::string_view(C::name(A::give_up))=="give_up" && C::SETTLE>=R::STALL,"names; SETTLE outlasts the flat-panel stall");
}
// 26 Oct 11:19: plain play, producer running, readiness actor_missing on every job for good.
void readiness_stall(){
    using S=ReadinessStall;using A=S::Action;using Reason=OwnedStereoReadiness::Reason;S s;
    require(S::player_reason(Reason::actor_missing) && S::player_reason(Reason::stale) && !S::player_reason(Reason::menu) && !S::player_reason(Reason::loading) && !S::player_reason(Reason::cutscene) && !S::player_reason(Reason::settling),"only player-state reasons count");
    require(s.observe(T0,true,true,true,100)==A::none,"a stall starts");
    require(s.observe(T0+9999ms,true,true,true,100)==A::none && s.observe(T0+10s,true,true,true,100)==A::rediscover && s.tries()==1,"LIMIT of a stall: rediscover");
    require(s.observe(T0+15s,true,true,true,100)==A::none && s.observe(T0+20s,true,true,true,100)==A::rediscover && s.observe(T0+30s,true,true,true,100)==A::rediscover && s.tries()==3,"again every LIMIT");
    require(s.observe(T0+40s,true,true,true,100)==A::give_up && s.observe(T0+50s,true,true,true,100)==A::none && s.total()==3,"after TRIES: give up once, then quiet");
    require(s.observe(T0+51s,true,true,true,101)==A::none && s.tries()==0 && s.stalled(T0+51s).count()==0,"a ready job ends the stall");
    require(s.observe(T0+52s,true,true,true,101)==A::none && s.observe(T0+62s,true,true,true,101)==A::rediscover && s.total()==4,"a new stall starts over");
    S menu;require(menu.observe(T0,true,true,false,5)==A::none && menu.observe(T0+60s,true,true,false,5)==A::none,"a menu or a load (not a player reason) is never a stall");
    S cutscene;require(cutscene.observe(T0,false,true,true,5)==A::none && cutscene.observe(T0+60s,false,true,true,5)==A::none,"no player expected (title, loading, cutscene): never a stall");
    S suspended;require(suspended.observe(T0,true,false,true,5)==A::none && suspended.observe(T0+60s,true,false,true,5)==A::none,"a stopped or suspended producer belongs to the recovery rungs");
    S flicker;flicker.observe(T0,true,true,true,5);require(flicker.observe(T0+5s,false,true,true,5)==A::none && flicker.observe(T0+12s,true,true,true,5)==A::none && flicker.observe(T0+22s,true,true,true,5)==A::rediscover,"leaving play restarts the clock");
}
// 26 Oct 14:44 and 23 Oct 19:55: a game PIP view behind the owned pair (slot 2) faults the game
// (docs/LOAD_CRASH_2026-10-26.md). No pair while one shows, and SETTLE after the last.
void foreign_view_hold(){
    using F=ForeignViewHold;F f;
    require(F::foreign(0x10043) && F::foreign(0x04010043) && !F::foreign(0x3f7c013) && !F::foreign(0x410001400) && !F::foreign(0x80000),"only a PIP view's flags count: not main, shadow or UI views");
    require(!f.held(T0) && !f.stats().sightings,"nothing seen: no hold");
    require(f.seen(T0,1,false) && f.held(T0) && f.stats().holds==1,"a PIP add starts a hold");
    require(f.held(T0+3720ms),"gaps inside one PIP window (3.7 s, 23 Oct 08:46) keep holding");
    require(!f.seen(T0+3720ms,1,false) && f.stats().holds==1 && f.stats().sightings==2,"a sighting inside the hold extends it, not a new hold");
    require(f.held(T0+3720ms+F::SETTLE-1ms) && !f.held(T0+3720ms+F::SETTLE),"SETTLE after the last sighting, then stereo may return");
    require(f.held(T0),"a sighting stamped after now (another thread's clock read) holds");
    require(f.seen(T0+60s,2,true) && f.stats().behind_pair==1 && f.stats().slot==2 && f.stats().holds==2,"a PIP behind the pair is counted, and starts a new hold after the last ended");
    // The 26 Oct crash dispatch: the pair (0x7f7c013) in slots 0 and 1, a shadow view, the class selector's PIP in slot 2.
    const std::array<F::View,4> stereo{{{0,0x7f7c013},{1,0x7f7c013},{2,0x04010043},{3,0x410001400}}};
    require(F::behind_pair(stereo)==2,"the PIP in slot 2 behind the pair is found");
    const std::array<F::View,3> mono{{{0,0x3f7c013},{1,0x04010043},{2,0x410001400}}};
    require(!F::behind_pair(mono),"mono: the PIP is in slot 1 with index 1, as the game expects");
    const std::array<F::View,3> plain{{{0,0x7f7c013},{1,0x7f7c013},{2,0x410001400}}};
    require(!F::behind_pair(plain),"stereo with only shadow/UI views after the pair is normal play");
    require(!F::behind_pair(std::span<const F::View>{}),"no views: nothing");
    const std::array<F::View,4> crash{{{0,0x7f7c013,0},{1,0x7f7c013,0},{2,0x10043,2},{3,0x410001400,3}}};
    require(F::hdr_remap(crash)==2,"the armoury and 26 Oct frame: the PIP in slot 2 still holds index 2");
    auto done=crash;done[2].hdr=1;require(!F::hdr_remap(done),"already rewritten: nothing to do");
    auto eye=crash;eye[1].hdr=1;require(!F::hdr_remap(eye),"a pair not on index 0 is not what the crash read: left alone");
    auto two=crash;two[3].flags=0x10043;require(!F::hdr_remap(two),"two PIPs can't share the one PIP index: left alone");
    const std::array<F::View,3> mono_hdr{{{0,0x3f7c013,0},{1,0x10043,1},{2,0x410001400,2}}};
    require(!F::hdr_remap(mono_hdr),"mono is never touched");
    const auto before=f.stats().remapped;f.remapped();require(f.stats().remapped==before+1,"a rewrite is counted");
    require(f.admit_grading(0) && f.admit_grading(1) && !f.stats().grading_refused,"main and the PIP run their grading");
    require(!f.admit_grading(2) && f.stats().grading_refused==1 && f.stats().grading_index==2,"a slot-2 index never reaches the two-entry tables");
    require(!f.admit_grading(-1) && f.stats().grading_refused==2 && f.stats().grading==4,"nor one before them");
}
}
// docs/STEREO_PAUSES_2026-10-26.md: the time between pairs sent to the runtime, per log window.
void submit_gaps(){
    using namespace std::chrono_literals;SubmitGaps gaps;const auto t=SubmitGaps::Clock::time_point{}+100s;
    gaps.submit(t);gaps.submit(t+40ms);gaps.submit(t+80ms);
    auto w=gaps.take();require(w.submits==3 && w.longest==40ms && !w.over_100ms && !w.over_stall,"steady pairs: no long gaps");
    gaps.submit(t+210ms);gaps.submit(t+250ms);gaps.submit(t+900ms);
    w=gaps.take();require(w.submits==3 && w.longest==650ms && w.over_100ms==2 && w.over_stall==1,"a 130 ms pause and a 650 ms one (the 18:49:00 flat panel)");
    w=gaps.take();require(!w.submits && w.longest==0ms,"take starts a new window");
    gaps.submit(t+950ms);require(gaps.take().longest==50ms,"the gap spanning two windows lands in the second");
    gaps.submit(t);require(gaps.take().longest==0ms,"time running backwards is not a gap");
}
int main(){try{backoff_and_probe();transition_hold();cutscene_hold();unlimited();counters_and_logs();fallback();renewal_hold();cold_restart();readiness_stall();foreign_view_hold();submit_gaps();std::cout<<"ordinary stereo recovery: bounded backoff, probe gate, level-transition and cutscene holds, unlimited episodes, log limits, fallback and the foreign-view hold PASS\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
