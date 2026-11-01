#pragma once
#include <windows.h>
#include <Xinput.h>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "LockedClockRead.h"

struct VrControllerSample {
    float move_x{}, move_y{}, turn_x{};
    bool confirm{}, interact{}, sprint{}, menu{}, recenter{};
    float fire{},aim{},melee{},parry{},grenade{},utility_x{},utility_y{},utility_press{};
    bool finisher{};
    bool swing_melee{}; // Bounded motion request, independent of grip-force hysteresis.
    bool swing_power{}; // Modifier latched at the swing edge; D-pad up is reserved by the VR pack.
    bool fire_touch{},fire_touch_valid{}; // Right trigger touch; gates physical swings.
    bool select_primary{},select_secondary{},salute{}; // State-checked holsters cycle (Down); salute emotes (Left).
    bool guard{}; // Left-hand guard pose: held native parry/block (LB), alongside left grip.
    bool grenade_throw{}; // Left-hand grenade gesture: native equipment (X), alongside right trackpad.
    float chainsword_rev{}; // Held right trigger in pistol mode; attacks still require a sword swing.
    bool ability{}; // Left A tap: class ability (native Y) pulse.
    float turn_y{}; // Right stick Y: viewer flight and the Touch stim. Last, so positional initialisers stay valid.
    bool menu_navigation{}; // Native menu input (pause panel): A is a plain confirm, never a neutral dodge; the right trackpad is native B (back).
    bool left_push{}; // Left-arm push (VrLeftPush): the native held strike, knockback (D-pad up).
    bool menu_x{}; // Left A on a menu (menu_navigation): native X, held.
    bool touch_layout{}; // Touch controllers (Quest, Rift S): no trackpads, so stim, scoreboard and the held ability move (docs/QUEST_STEAM_LINK.md).
    bool cinematic{}; // The game's cutscene flag: a held right stick click doesn't detonate.
};

// Cached pad reads never call SteamVR or advance snap-turn state on the game's input thread.
class VrControllerInput {
public:
    using TimeSource = std::chrono::steady_clock::time_point(*)();
    explicit VrControllerInput(TimeSource clock=std::chrono::steady_clock::now):m_clock(clock) {}
    static constexpr auto NEUTRAL_DODGE_DIRECTION_HOLD=std::chrono::milliseconds(450);
    // The game's dodge button is two actions (dodge_action_command_binding.sso): let go in time it
    // dodges, held on it becomes CommandSprint. In Joe's runs presses up to 385 ms dash (9-12 m/s) and
    // those of 667 ms or more run (7.8 m/s; walking is 5.2). 31 Oct 16:14:21: A held 3 s with the stick
    // centred ran him backwards along the synthesized stick. So in play native A lets go at
    // DODGE_MAX_PRESS even with A still down: a held press is the same one dodge as a tap and never
    // runs. Shorter presses are untouched. Menus keep a plain A, held for as long as it is held.
    static constexpr auto DODGE_MAX_PRESS=std::chrono::milliseconds(350);
    // The game detonates a placed melta charge on X held 0.3 s (keymap.sso AbilityRoutineDetonate:
    // InputActionHold, time 0.3, moved to X by the controls pack); throwing is X pressed (UseEquipment).
    // A quick trackpad click is shorter than that, so it throws but can't detonate (26 Oct: "we have no
    // way to detonate the melter"). Joe, 31 Oct: hold the right stick click. At 17:49 he read the prompt
    // "right stick down" as pressing the stick in and held the click 2.7, 2.8 and 2 s to no effect.
    // Held DETONATE_HOLD, well past that run's taps (0.09-0.2 s), the click also holds native X for at
    // least EQUIPMENT_MIN_HOLD and for as long as it lasts; the tap stays finisher, sent at once. The
    // trackpad keeps its plain press so a held X never changes how an ordinary grenade throw behaves.
    // Nothing here knows whether a charge is out: with none, a long click presses X like the trackpad
    // does (docs/BUTTON_HINTS.md).
    static constexpr auto EQUIPMENT_MIN_HOLD=std::chrono::milliseconds(400),DETONATE_HOLD=std::chrono::milliseconds(500);
    // Touch controllers (sample.touch_layout, docs/QUEST_STEAM_LINK.md) have no left trackpad,
    // whose right and left presses are stim (D-pad right) and scoreboard (Back). In play, right
    // stick up holds D-pad right with hysteresis (STIM_PRESS, STIM_RELEASE), for at least STIM_MIN_HOLD so a
    // flick still reaches the game; the left stick click held SCOREBOARD_HOLD holds Back while
    // it stays held (the click's sprint toggle still happens, and the scoreboard cancels sprint).
    static constexpr auto STIM_MIN_HOLD=std::chrono::milliseconds(150),SCOREBOARD_HOLD=std::chrono::milliseconds(1000);
    static constexpr float STIM_PRESS=.7f,STIM_RELEASE=.4f; // right stick Y, upwards
    // Re-arming after a disarm (docs/CONTROL_CUTOUT_2026-10-26.md). Until 26 Oct every disarm
    // waited for the whole pad to be neutral; a player in a fight is almost never neutral, so a
    // 0.1-0.4 s pause in the stereo frames locked the stick and trigger out for up to 10 s.
    // A stall (no fresh tracking or poll, the game still in front and the controls still read)
    // that began from gameplay and ends within stall_resume picks up the held controls at once.
    // Any other disarm (focus or tracking loss, a recenter, a menu, a setting change) arms on the
    // next sample, but each control held at that moment stays blocked until it is released, so a
    // trigger held through a focus change still never fires on recovery. whole_pad restores the
    // old rule (SM2VR_INPUT_NEUTRAL_REARM=1); stall_resume 0 turns the resume off.
    struct Rearm {
        std::chrono::milliseconds stall_resume{1000}; bool whole_pad{};
        static constexpr auto MAX_STALL_RESUME=std::chrono::milliseconds(10000);
        bool valid() const { return stall_resume.count()>=0 && stall_resume<=MAX_STALL_RESUME; }
        // SM2VR_INPUT_STALL_RESUME_MS / SM2VR_INPUT_NEUTRAL_REARM values (null: unset). A bad
        // number gives nullopt so the caller can say so and keep the default.
        static std::optional<Rearm> parse(const char* stall_ms,const char* neutral) {
            Rearm value;
            if(stall_ms) {
                char* end{};const long ms=std::strtol(stall_ms,&end,10);
                if(end==stall_ms || *end) return std::nullopt;
                value.stall_resume=std::chrono::milliseconds(ms);
            }
            value.whole_pad=neutral && std::strcmp(neutral,"1")==0;
            return value.valid()?std::optional{value}:std::nullopt;
        }
    };
    // One bit per control the neutral check reads; held() names them for logs and the trace.
    enum Control : uint32_t { MOVE,TURN,CONFIRM,INTERACT,SPRINT,MENU,MENU_X,RECENTER,ABILITY,FINISHER,SWING,LEFT_PUSH,
        SELECT_PRIMARY,SELECT_SECONDARY,SALUTE,GUARD,GRENADE_THROW,FIRE,AIM,MELEE,PARRY,GRENADE,UTILITY,CONTROLS };
    static constexpr std::array<const char*,CONTROLS> CONTROL_NAMES{"move","turn","confirm","interact","sprint","menu","menu_x","recenter","ability",
        "finisher","swing","left_push","select_primary","select_secondary","salute","guard","grenade_throw","fire","aim","melee","parry","grenade","utility"};
    // Raw grips VrSession's gestures mask while they own them (grenade, support, holsters).
    static constexpr uint32_t GESTURE_GRIPS=(1u<<MELEE)|(1u<<PARRY);
    // The controls in value that are not at rest (the same thresholds as the old neutral check).
    static uint32_t held(const VrControllerSample& value) {
        uint32_t mask{};const auto set=[&](Control c,bool on){if(on)mask|=1u<<c;};
        set(MOVE,std::hypot(value.move_x,value.move_y)>0.2f);set(TURN,std::abs(value.turn_x)>0.3f);
        set(CONFIRM,value.confirm);set(INTERACT,value.interact);set(SPRINT,value.sprint);set(MENU,value.menu);set(MENU_X,value.menu_x);
        set(RECENTER,value.recenter);set(ABILITY,value.ability);set(FINISHER,value.finisher);set(SWING,value.swing_melee);set(LEFT_PUSH,value.left_push);
        set(SELECT_PRIMARY,value.select_primary);set(SELECT_SECONDARY,value.select_secondary);set(SALUTE,value.salute);set(GUARD,value.guard);
        set(GRENADE_THROW,value.grenade_throw);set(FIRE,value.fire>0.05f);set(AIM,value.aim>0.05f);set(MELEE,value.melee>0.35f);
        set(PARRY,value.parry>0.35f);set(GRENADE,value.grenade>0.35f);set(UTILITY,value.utility_press>0.35f);
        return mask;
    }
    // Puts the controls in mask at rest in value.
    static void rest(VrControllerSample& value,uint32_t mask) {
        const auto on=[&](Control c){return (mask&(1u<<c))!=0;};
        if(on(MOVE)) value.move_x=value.move_y=0;
        if(on(TURN)) value.turn_x=0;
        if(on(CONFIRM)) value.confirm=false;
        if(on(INTERACT)) value.interact=false;
        if(on(SPRINT)) value.sprint=false;
        if(on(MENU)) value.menu=false;
        if(on(MENU_X)) value.menu_x=false;
        if(on(RECENTER)) value.recenter=false;
        if(on(ABILITY)) value.ability=false;
        if(on(FINISHER)) value.finisher=false;
        if(on(SWING)) value.swing_melee=value.swing_power=false;
        if(on(LEFT_PUSH)) value.left_push=false;
        if(on(SELECT_PRIMARY)) value.select_primary=false;
        if(on(SELECT_SECONDARY)) value.select_secondary=false;
        if(on(SALUTE)) value.salute=false;
        if(on(GUARD)) value.guard=false;
        if(on(GRENADE_THROW)) value.grenade_throw=false;
        if(on(FIRE)) value.fire=0;
        if(on(AIM)) value.aim=0;
        if(on(MELEE)) value.melee=0;
        if(on(PARRY)) value.parry=0;
        if(on(GRENADE)) value.grenade=0;
        if(on(UTILITY)) value.utility_press=0;
    }
    struct Snapshot { bool enabled{}, armed{}; XINPUT_STATE pad{}; uint64_t snaps{}; float snap_degrees{30}; const char* reason{"disabled"}; float chainsword_rev{};
        uint32_t blocked{};           // Controls held when input last armed, still waiting for their release.
        uint64_t stall_resumes{};     // Stalls that picked the held controls up again without a wait.
        double last_stall_ms{};       // Length of the latest of those stalls.
        const char* disarm{""};       // What last disarmed input (a gate reason, or "stall").
    };
    bool set_rearm(Rearm value) { if(!value.valid()) return false; std::lock_guard lock{m_mutex};m_rearm=value;if(!value.stall_resume.count()) m_resume_until={};return true; }
    Rearm rearm() const { std::lock_guard lock{m_mutex};return m_rearm; }
    bool set_snap_degrees(float degrees) {
        if (!std::isfinite(degrees) || degrees < 15 || degrees > 90) { return false; }
        std::lock_guard lock{m_mutex}; m_snap_degrees = degrees; return true;
    }
    void enable(bool enabled) {
        std::lock_guard lock{m_mutex};
        if (m_enabled != enabled) { m_enabled = enabled; disarm(enabled ? awaiting() : "disabled"); publish({}); }
    }
    // Grip melee is opt-in at the application boundary, independently of finger curls and
    // the other native combat buttons. Physical swings have their own opt-in in VrSession.
    void configure(bool runtime,bool controllers,bool synthetic,bool melee=false) { set_melee_enabled(melee);enable(runtime && controllers && !synthetic); }
    // Retain the user's motion-control preference while third person reads the real pad.
    void native_passthrough(bool value) {
        std::lock_guard lock{m_mutex};
        if(value!=m_native_passthrough) {m_native_passthrough=value;disarm(value?"native passthrough":awaiting());publish({});m_native_back_down=false;m_native_recenter=false;m_native_dpad_down=m_native_dpad_hidden=0;m_native_scale_steps=0;}
    }
    bool needs_dispatch() const { std::lock_guard lock{m_mutex};return m_enabled || m_native_passthrough; }
    bool motion_preference() const { std::lock_guard lock{m_mutex};return m_enabled; }
    bool take_native_recenter() { std::lock_guard lock{m_mutex};const bool result=m_native_recenter;m_native_recenter=false;return result; }
    // World scale presses since the last take: positive makes the world bigger (VrViewSettings::resize_world).
    int take_native_scale_steps() { std::lock_guard lock{m_mutex};const int result=m_native_scale_steps;m_native_scale_steps=0;return result; }
    // Only the engine dispatch invokes this, after generic XInput wrappers return.
    // Processing our masked Back bit twice would incorrectly rearm a held button.
    // Back recenters on its press. Held, it makes D-pad up/down world scale steps the game never
    // sees; a direction pressed under Back stays hidden until it is released, even after Back.
    void native_actions(uint32_t result,uint32_t index,XINPUT_STATE* state) {
        std::lock_guard lock{m_mutex};
        if(!m_native_passthrough || index!=0 || !state) return;
        constexpr WORD SCALE=XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_DOWN;
        const WORD buttons=result==ERROR_SUCCESS?state->Gamepad.wButtons:0;
        const bool pressed=(buttons&XINPUT_GAMEPAD_BACK)!=0;
        if(pressed && !m_native_back_down) m_native_recenter=true;
        m_native_back_down=pressed;
        const WORD dpad=buttons&SCALE,fresh=dpad&~m_native_dpad_down;
        if(pressed) {
            if(fresh&XINPUT_GAMEPAD_DPAD_UP) ++m_native_scale_steps;
            if(fresh&XINPUT_GAMEPAD_DPAD_DOWN) --m_native_scale_steps;
            m_native_dpad_hidden|=fresh;
        }
        m_native_dpad_hidden&=dpad;m_native_dpad_down=dpad;
        if(result==ERROR_SUCCESS) state->Gamepad.wButtons&=~((pressed?XINPUT_GAMEPAD_BACK:0)|m_native_dpad_hidden);
    }
    // Diagnostic: hold one native button for 150 ms regardless of VR input, so a D-pad
    // direction's in-game action can be identified from the desktop overlay.
    void test_pulse(WORD button,std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now()) {
        inject(button,now+std::chrono::milliseconds(150));
    }
    // Hold exact native buttons until a deadline on top of whatever update() publishes
    // (viewer mode drives the pause menu and native photo mode this way). inject(0,now) ends it.
    void inject(WORD buttons,std::chrono::steady_clock::time_point until) {
        std::lock_guard lock{m_mutex};
        if(buttons!=m_test_button || until!=m_test_until) {m_test_button=buttons;m_test_until=until;++m_test_packet;}
    }
    bool melee_enabled() const { std::lock_guard lock{m_mutex};return m_melee_enabled; }
    void set_melee_enabled(bool value) {
        std::lock_guard lock{m_mutex};if(value==m_melee_enabled) return;
        m_melee_enabled=value;m_melee=false;
        auto pad=m_state.Gamepad;pad.wButtons&=~XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if(value) {disarm(awaiting());pad={};}
        publish(pad);
    }
    // A deliberate release (a recenter): disarm with this reason; held controls wait for release.
    void release(const char* reason,std::chrono::steady_clock::time_point now) {
        std::lock_guard lock{m_mutex};m_received=now;disarm(reason);publish({});
    }
    // The frame pipeline has no fresh tracking (VrSession: the head or the newest stereo frame is
    // over 100 ms old) while nothing says the player left: release the pad like any loss, and if
    // input was armed for gameplay, let the next gameplay sample within Rearm::stall_resume pick
    // the held controls up again. Not foreground or disabled is a plain disarm.
    void stall(bool foreground,std::chrono::steady_clock::time_point now) {
        std::lock_guard lock{m_mutex};
        if(!m_enabled || m_native_passthrough || !foreground) { m_received=now;disarm(!m_enabled?"disabled":m_native_passthrough?"native passthrough":"game not foreground");publish({});return; }
        if(now<m_received) { m_received=now;disarm("stale cached input");publish({});return; }
        begin_stall(now);m_received=now;publish({});
    }
    // Heading is clockwise relative to the game's camera; turn delta is clockwise radians.
    // look: native right-stick deflection (gaze camera steering, VrGazeCamera.h), x>0 turns
    // right, y>0 looks up. Applied only to armed gameplay pads; never part of the neutral check.
    float update(const std::optional<VrControllerSample>& sample, float heading, bool foreground,
        std::chrono::steady_clock::time_point now, std::optional<std::array<float,2>> look = std::nullopt) {
        std::lock_guard lock{m_mutex};
        // No poll for over 100 ms (a long frame) is a stall; time running backwards is not.
        if (now < m_received) disarm("stale cached input");
        else if (now-m_received > std::chrono::milliseconds(100)) begin_stall(m_received);
        m_received = now;
        XINPUT_GAMEPAD pad{};
        if (!m_enabled || m_native_passthrough || !foreground || !sample || !std::isfinite(heading) ||
            !std::isfinite(sample->move_x) || !std::isfinite(sample->move_y) || !std::isfinite(sample->turn_x) ||
            std::abs(sample->move_x) > 1.01f || std::abs(sample->move_y) > 1.01f || std::abs(sample->turn_x) > 1.01f ||
            !valid_combat(*sample)) {
            disarm(!m_enabled ? "disabled" : m_native_passthrough ? "native passthrough" : !foreground ? "game not foreground" : !sample ? "tracking/actions unavailable" : "invalid action data");
            publish(pad); return 0;
        }
        auto value = *sample;
        m_chainsword_rev=value.chainsword_rev;
        if(!m_melee_enabled) value.melee=0;
        if (!m_armed) {
            m_neutral_dodge=false;m_dodge_down=false;m_dodge_direction_until={};
            const bool resume=m_resume_until!=std::chrono::steady_clock::time_point{} && now<=m_resume_until && !value.menu_navigation;
            const auto stalled_from=m_stall_from;
            m_resume_until={};m_stall_from={};
            if(resume) {
                // The same controls, a moment later: carry on with this sample. The grips are
                // the exception: a gesture may have owned the squeeze until the stall reset it
                // (a grenade held at the belt, a support grip, a holster draw), so a grip that
                // passed nothing before the stall waits for its release instead of becoming a
                // parry or a melee. A held turn does not snap again.
                m_armed=true;m_blocked=held(value)&~m_last_held&GESTURE_GRIPS;m_snap_ready=std::abs(value.turn_x)<=0.3f;
                m_dodge_down=value.confirm && (m_last_held&(1u<<CONFIRM)); // A held through the stall is not a new press, so not a new dodge.
                ++m_stall_resumes;m_last_stall_ms=std::chrono::duration<double,std::milli>(now-stalled_from).count();
            } else {
                m_sprinting=false;m_sprint_down=value.sprint;
                if(m_rearm.whole_pad) { if(!held(value)) {m_armed=true;m_snap_ready=true;m_blocked=0;} }
                else { m_armed=true;m_snap_ready=true;m_blocked=held(value); }
                m_reason=!m_armed?"awaiting neutral controls":m_blocked?"active, held controls wait for release":"active";
                publish(pad); return 0;
            }
        }
        // A control held when input armed stays at rest until it has been let go once.
        if(m_blocked) { m_blocked&=held(value);rest(value,m_blocked); }
        m_reason = m_blocked?"active, held controls wait for release":"active";
        m_last_gameplay=!value.menu_navigation;m_last_held=held(value);
        const auto magnitude = std::hypot(value.move_x,value.move_y);
        float delta{};
        if (std::abs(value.turn_x) <= 0.3f) { m_snap_ready = true; }
        // On a menu the right stick is the native right stick, not snap turn.
        if (value.menu_navigation) {
            m_snap_ready = false;
            const auto axis=[](float v){return std::isfinite(v)?SHORT(std::lround(std::clamp(v,-1.0f,1.0f)*32767)):SHORT(0);};
            pad.sThumbRX = axis(value.turn_x); pad.sThumbRY = axis(value.turn_y);
        }
        else if (m_snap_ready && std::abs(value.turn_x) >= 0.7f) {
            delta = std::copysign(m_snap_degrees*0.01745329252f,value.turn_x);
            m_snap_ready = false; ++m_snaps;
        }
        heading += delta;
        // A neutral-stick A in gameplay is a backwards dodge with a synthesized stick. In a
        // menu that stick would move the cursor down with every confirm: A stays a plain A.
        if(value.menu_navigation) {m_neutral_dodge=false;m_dodge_direction_until={};}
        else if(value.confirm && !m_dodge_down) {
            m_dodge_press_until=now+DODGE_MAX_PRESS;
            if(magnitude<=.2f) {m_neutral_dodge=true;m_dodge_direction_until=now+NEUTRAL_DODGE_DIRECTION_HOLD;}
        }
        m_dodge_down=value.confirm;
        const bool native_a=value.confirm && (value.menu_navigation || now<m_dodge_press_until);
        if(magnitude>.2f || (!native_a && now>=m_dodge_direction_until))m_neutral_dodge=false;
        const bool neutral_dodge=m_neutral_dodge;
        if (magnitude > 0.2f || neutral_dodge) {
            // Native neutral A reuses the gameplay-camera direction, which does
            // not follow VR snap/head yaw. Give the native dodge an explicit
            // backwards direction through the same view-to-camera mapping.
            // Native roll direction is sampled after the initial A edge. Retain
            // that intent through its 400ms startup (measured with the driver).
            const auto amount = neutral_dodge?1.0f:(std::min(magnitude,1.0f)-0.2f)/0.8f;
            const auto x = neutral_dodge?0.0f:value.move_x/magnitude*amount;
            const auto y = neutral_dodge?-1.0f:value.move_y/magnitude*amount;
            const auto sine = std::sin(heading), cosine = std::cos(heading);
            pad.sThumbLX = SHORT(std::lround(std::clamp(x*cosine+y*sine,-1.0f,1.0f)*32767));
            pad.sThumbLY = SHORT(std::lround(std::clamp(y*cosine-x*sine,-1.0f,1.0f)*32767));
        }
        if (native_a) { pad.wButtons |= XINPUT_GAMEPAD_A; }
        // Right B is native B everywhere, so a prompt that says B means this button: interact and
        // reload in play (moved there by the controls pack), back on a menu, hold to skip a cutscene.
        if (value.interact) { pad.wButtons |= XINPUT_GAMEPAD_B; }
        if (!value.menu_navigation && look && std::isfinite((*look)[0]) && std::isfinite((*look)[1])) {
            pad.sThumbRX = SHORT(std::lround(std::clamp((*look)[0],-1.0f,1.0f)*32767));
            pad.sThumbRY = SHORT(std::lround(std::clamp((*look)[1],-1.0f,1.0f)*32767));
        }
        const bool sprint_edge=value.sprint && !m_sprint_down;
        m_sprint_down=value.sprint;
        const bool touch_play=value.touch_layout && !value.menu_navigation;
        if(sprint_edge) m_sprint_from=now;
        const bool scoreboard=touch_play && value.sprint && m_sprint_from!=std::chrono::steady_clock::time_point{} && now-m_sprint_from>=SCOREBOARD_HOLD;
        // Native camera yaw is independent of the VR body. The controls pack
        // removes its camera-angle veto; enforce forward eligibility here before
        // heading conversion, retaining click-to-sprint until stopping/cancelling.
        const bool sprint_forward=magnitude>(.2f+.8f*.4f) && value.move_y/magnitude>=.766044443f;
        if(sprint_edge) m_sprinting=!m_sprinting;
        if(!sprint_forward || value.confirm || value.fire>.05f || value.aim>.05f || value.parry>.35f || value.grenade>.35f || value.guard || value.grenade_throw || value.swing_melee || value.left_push || value.finisher || value.menu || scoreboard) m_sprinting=false;
        if(m_sprinting) pad.wButtons|=XINPUT_GAMEPAD_LEFT_THUMB;
        if (value.menu) { pad.wButtons |= XINPUT_GAMEPAD_START; }
        pad.bRightTrigger=BYTE(std::lround(std::clamp(value.fire,0.0f,1.0f)*255));
        pad.bLeftTrigger=BYTE(std::lround(std::clamp(value.aim,0.0f,1.0f)*255));
        latch(value.melee,m_melee);latch(value.parry,m_parry);latch(value.grenade,m_grenade);
        // Detonate (right stick click held DETONATE_HOLD, play only): native X held at least
        // EQUIPMENT_MIN_HOLD. A menu or cutscene restarts the hold, so a click held through one
        // doesn't detonate the moment it ends.
        const bool detonate_was=m_detonate,no_detonate=value.menu_navigation || value.cinematic;
        if((value.finisher && !m_finisher_down) || no_detonate) m_finisher_from=now;
        m_finisher_down=value.finisher;
        m_detonate=value.finisher && !no_detonate && now-m_finisher_from>=DETONATE_HOLD;
        if(m_detonate && !detonate_was) m_equipment_until=now+EQUIPMENT_MIN_HOLD;
        if(no_detonate) m_equipment_until={};
        const bool stim_was=m_stim;
        if(!touch_play || !std::isfinite(value.turn_y)) m_stim=false;
        else if(value.turn_y>=STIM_PRESS) m_stim=true;
        else if(value.turn_y<STIM_RELEASE) m_stim=false;
        if(m_stim && !stim_was) m_stim_until=now+STIM_MIN_HOLD;
        if(!touch_play) m_stim_until={};
        if(m_stim || now<m_stim_until) pad.wButtons|=XINPUT_GAMEPAD_DPAD_RIGHT;
        if(scoreboard) pad.wButtons|=XINPUT_GAMEPAD_BACK;
        if(m_melee || (value.swing_melee && !value.swing_power)) pad.wButtons|=XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if((value.swing_melee && value.swing_power) || value.left_push) pad.wButtons|=XINPUT_GAMEPAD_DPAD_UP;
        // Parry is also a native melee-abort input. A deliberate sword swing
        // owns this short pulse even if the support arm remains raised. A parry
        // grip held through the attack stays released until it is let go:
        // pressing LB again when the pulse ends would be a new native parry that
        // also aborts the attack. (The guard gesture yields the same way, VrGuard::yield.)
        if(value.swing_melee && m_parry) m_parry_yielded=true;
        if(!m_parry) m_parry_yielded=false;
        if(!value.swing_melee && !value.left_push && ((m_parry && !m_parry_yielded) || value.guard)) pad.wButtons|=XINPUT_GAMEPAD_LEFT_SHOULDER;
        // Equipment is native X in play. On a menu the right trackpad is back, native B.
        if(m_grenade || value.grenade_throw || m_detonate || now<m_equipment_until) pad.wButtons|=value.menu_navigation?XINPUT_GAMEPAD_B:XINPUT_GAMEPAD_X;
        if(value.finisher) pad.wButtons|=XINPUT_GAMEPAD_RIGHT_THUMB;
        if(value.select_primary || value.select_secondary) pad.wButtons|=XINPUT_GAMEPAD_DPAD_DOWN;
        if(value.salute) pad.wButtons|=XINPUT_GAMEPAD_DPAD_LEFT;
        if(value.utility_press<=0.35f) m_utility=0;
        else if(value.utility_press>=0.65f && !m_utility && std::hypot(value.utility_x,value.utility_y)>=0.3f) {
            // Lock the quadrant until release so a held trackpad cannot select multiple actions.
            m_utility=std::abs(value.utility_y)>=std::abs(value.utility_x) ?
                (value.utility_y>=0 ? XINPUT_GAMEPAD_Y : XINPUT_GAMEPAD_DPAD_DOWN) :
                (value.utility_x>=0 ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_BACK);
        }
        pad.wButtons|=m_utility;
        if(value.ability) pad.wButtons|=XINPUT_GAMEPAD_Y;
        if(value.menu_navigation && value.menu_x) pad.wButtons|=XINPUT_GAMEPAD_X;
        publish(pad);
        return delta;
    }
    Snapshot snapshot() const { return snapshot_current(m_clock); }
    Snapshot snapshot_current(TimeSource clock) const {
        return locked_clock_read(m_mutex,clock,[this](auto now){return snapshot_locked(now);});
    }
    // Explicit-time reads intentionally retain strict future/stale rejection.
    Snapshot snapshot(std::chrono::steady_clock::time_point now) const {
        std::lock_guard lock{m_mutex};
        return snapshot_locked(now);
    }
    // Evaluate after the action poll. Reusing its pre-poll time makes a newly received
    // sample appear to come from the future and permanently closes the native body gate.
    bool allows_body_motion(bool foreground,TimeSource clock = std::chrono::steady_clock::now) const {
        return foreground && snapshot_current(clock).armed;
    }
    void get_state(uint32_t* result,uint32_t index,XINPUT_STATE* state) const {
        get_state_from(snapshot(),result,index,state);
    }
    void get_state(uint32_t* result,uint32_t index,XINPUT_STATE* state,
        std::chrono::steady_clock::time_point now) const {
        get_state_from(snapshot(now),result,index,state);
    }
    void get_capabilities(uint32_t* result,uint32_t index,XINPUT_CAPABILITIES* capabilities) const {
        std::lock_guard lock{m_mutex};
        if (!m_enabled || m_native_passthrough || index != 0 || !result || !capabilities || (*result != ERROR_SUCCESS && *result != ERROR_DEVICE_NOT_CONNECTED)) { return; }
        *capabilities = {}; capabilities->Type = XINPUT_DEVTYPE_GAMEPAD; capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        capabilities->Gamepad.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y |
            XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK |
            XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT |
            XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT;
        capabilities->Gamepad.bLeftTrigger=capabilities->Gamepad.bRightTrigger=255;
        capabilities->Gamepad.sThumbLX = capabilities->Gamepad.sThumbLY = 32767;
        *result = ERROR_SUCCESS;
    }
private:
    friend struct VrFreshnessTestAccess;
    TimeSource m_clock;
    Snapshot snapshot_locked(std::chrono::steady_clock::time_point now) const {
        auto pad = m_state;
        const bool fresh = now >= m_received && now-m_received <= std::chrono::milliseconds(100);
        if (!fresh || !m_enabled || m_native_passthrough) {
            if (pad.Gamepad.wButtons || pad.Gamepad.sThumbLX || pad.Gamepad.sThumbLY || pad.Gamepad.bLeftTrigger || pad.Gamepad.bRightTrigger) { ++pad.dwPacketNumber; }
            pad.Gamepad = {};
        }
        if(now<m_test_until) {pad.Gamepad.wButtons|=m_test_button;pad.dwPacketNumber+=m_test_packet<<24;}
        return {m_enabled && !m_native_passthrough,m_armed && fresh && !m_native_passthrough,pad,m_snaps,m_snap_degrees,
            !m_enabled ? "disabled" : !fresh ? "stale cached input" : m_reason,
            m_enabled && m_armed && fresh && !m_native_passthrough?m_chainsword_rev:0.f,
            m_armed?m_blocked:0u,m_stall_resumes,m_last_stall_ms,m_disarm};
    }
    const char* awaiting() const { return m_rearm.whole_pad?"awaiting neutral controls":"awaiting controls"; }
    // Every disarm except a stall: no resume, and the next sample re-arms by the Rearm rule.
    void disarm(const char* reason) {
        if(m_armed || stalling()) m_disarm=reason; // The first cause, not the gates that follow it.
        m_armed=false;m_snap_ready=false;m_resume_until={};m_stall_from={};m_reason=reason;
    }
    // since: when the stall began. Only a pad armed for gameplay may resume; a second stall
    // before the first resumes keeps the first one's deadline.
    void begin_stall(std::chrono::steady_clock::time_point since) {
        const bool was_armed=m_armed;
        m_armed=false;m_snap_ready=false;
        if(was_armed || stalling()) m_reason="stalled: no fresh tracking";
        if(!was_armed) return;
        m_disarm="stall";
        if(m_rearm.stall_resume.count()>0 && m_last_gameplay) {m_stall_from=since;m_resume_until=since+m_rearm.stall_resume;}
        else {m_resume_until={};m_stall_from={};}
    }
    static void get_state_from(const Snapshot& current,uint32_t* result,uint32_t index,XINPUT_STATE* state) {
        if (!current.enabled || index != 0 || !result || !state || (*result != ERROR_SUCCESS && *result != ERROR_DEVICE_NOT_CONNECTED)) { return; }
        *state = current.pad; *result = ERROR_SUCCESS;
    }
    static bool valid_combat(const VrControllerSample& value) {
        for(float axis:{value.fire,value.aim,value.melee,value.parry,value.grenade,value.utility_press,value.chainsword_rev})
            if(!std::isfinite(axis) || axis<0 || axis>1.01f) return false;
        return std::isfinite(value.utility_x) && std::isfinite(value.utility_y) && std::abs(value.utility_x)<=1.01f && std::abs(value.utility_y)<=1.01f;
    }
    static void latch(float force,bool& held) { if(force<=0.35f) held=false;else if(force>=0.65f) held=true; }
    void publish(const XINPUT_GAMEPAD& pad) {
        // A stall that may resume keeps the sprint toggle: running on after a long frame.
        if(!m_armed && !stalling()) {m_sprinting=m_sprint_down=false;m_sprint_from={};}
        if(!m_armed) { m_chainsword_rev=0;m_melee=m_parry=m_grenade=m_parry_yielded=m_detonate=m_finisher_down=m_stim=false;m_equipment_until=m_stim_until={};m_utility=0;m_neutral_dodge=m_dodge_down=false;m_dodge_direction_until=m_dodge_press_until={}; }
        if (pad.wButtons != m_state.Gamepad.wButtons || pad.sThumbLX != m_state.Gamepad.sThumbLX || pad.sThumbLY != m_state.Gamepad.sThumbLY ||
            pad.bLeftTrigger!=m_state.Gamepad.bLeftTrigger || pad.bRightTrigger!=m_state.Gamepad.bRightTrigger) {
            ++m_state.dwPacketNumber;
        }
        m_state.Gamepad = pad;
    }
    mutable std::mutex m_mutex;
    bool m_enabled{},m_armed{},m_snap_ready{},m_melee_enabled{true};
    bool m_native_passthrough{};
    mutable bool m_native_back_down{},m_native_recenter{};
    WORD m_native_dpad_down{},m_native_dpad_hidden{};int m_native_scale_steps{};
    bool m_melee{},m_parry{},m_grenade{},m_parry_yielded{},m_detonate{},m_finisher_down{},m_stim{};
    bool m_neutral_dodge{},m_dodge_down{};
    bool m_sprinting{},m_sprint_down{};
    float m_chainsword_rev{};
    std::chrono::steady_clock::time_point m_dodge_direction_until{},m_dodge_press_until{},m_equipment_until{},m_stim_until{},m_sprint_from{},m_finisher_from{};
    WORD m_utility{},m_test_button{};
    DWORD m_test_packet{};
    std::chrono::steady_clock::time_point m_test_until{};
    XINPUT_STATE m_state{};
    uint64_t m_snaps{};
    float m_snap_degrees{30};
    const char* m_reason{"disabled"};
    std::chrono::steady_clock::time_point m_received{};
    bool stalling() const { return m_resume_until!=std::chrono::steady_clock::time_point{}; }
    Rearm m_rearm{};
    uint32_t m_blocked{},m_last_held{}; // m_last_held: what the last armed update passed on.
    bool m_last_gameplay{}; // The last armed update was gameplay, not a menu.
    std::chrono::steady_clock::time_point m_stall_from{},m_resume_until{};
    uint64_t m_stall_resumes{};
    double m_last_stall_ms{};
    const char* m_disarm{""};
};
