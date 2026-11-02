// Left-hand gestures (guard, grenade) and the playtest trace: startup, overlay, logging.
// Kept out of Sm2Probe.cpp so the gesture work stays a small, separate diff.
#include "PathText.h"
#include "Sm2Probe.h"
#include "EngineBodyFollowing.h"
#include "EngineWeaponProbe.h"
#include "EngineCinematicState.h"
#include "EngineControllerRegistration.h"
#include "VrGazeCamera.h"
#include <imgui.h>
#include <spdlog/spdlog.h>

namespace {
std::filesystem::path env_path(const wchar_t* name) {
    wchar_t value[32768]{};const auto length=GetEnvironmentVariableW(name,value,32768);
    if(!length || length>=32768) return {};
    std::filesystem::path path{value};return path.is_absolute()?path:std::filesystem::path{};
}
bool env_flag(const char* name) { char value[32]{};return GetEnvironmentVariableA(name,value,sizeof(value)) && std::atoi(value)==1; }
template<class Settings,class Apply> bool load_settings(const std::filesystem::path& path,Apply&& apply) {
    if(path.empty()) return false;const auto loaded=Settings::load(path);return loaded && apply(*loaded);
}
}

void Sm2Probe::setup_gestures_and_trace() {
    m_vr_session->enable_guard(env_flag("SM2VR_GUARD"));
    m_guard_settings_path=env_path(L"SM2VR_GUARD_CONFIG");
    const bool guard_loaded=load_settings<VrGuard::Settings>(m_guard_settings_path,[&](const auto& s){return m_vr_session->set_guard_settings(s);});
    m_vr_session->set_guard_belt_latch(env_flag("SM2VR_GUARD_BELT_LATCH"));
    spdlog::info("VrGuard: enabled={} config_loaded={} belt_latch={}",m_vr_session->guard_enabled(),guard_loaded,m_vr_session->guard_belt_latch());
    // Guard guide: SM2VR_GUARD_GUIDE=1 (launcher, default with -Controls) and enabled=1 in its config.
    m_vr_session->enable_guard_guide(env_flag("SM2VR_GUARD_GUIDE"));
    m_guard_guide_settings_path=env_path(L"SM2VR_GUARD_GUIDE_CONFIG");
    const bool guide_loaded=load_settings<VrGuardGuide::Settings>(m_guard_guide_settings_path,[&](const auto& s){return m_vr_session->set_guard_guide_settings(s);});
    {const auto look=m_vr_session->guard_guide_settings();const auto target=VrGuardGuide::target(m_vr_session->guard_settings());
     spdlog::info("GuardGuide: enabled={} config_enabled={} config_loaded={} target=[{},{},{}] bar_length={} thickness={} grip={} opacity={}",m_vr_session->guard_guide_enabled(),look.enabled(),guide_loaded,
        target.lateral,target.height,target.forward,look[VrGuardGuide::LENGTH],look[VrGuardGuide::THICKNESS],look[VrGuardGuide::GRIP],look[VrGuardGuide::OPACITY]);}
    m_vr_session->enable_left_push(env_flag("SM2VR_LEFT_PUSH"));
    spdlog::info("VrLeftPush: enabled={} (open left hand thrust forward = native held strike, knockback)",m_vr_session->left_push_enabled());
    {   // Game rumble on the controllers (docs/HAPTICS.md): SM2VR_HAPTICS=1, optional SM2VR_HAPTICS_GAIN (0..2).
        // Headset A/B: SM2VR_HAPTICS_FREQUENCY (Hz, 0 = runtime default) and SM2VR_HAPTICS_TEST=1 (one
        // half-second pulse on both hands at the start of the session).
        vr_rumble::Settings rumble;char gain[32]{},frequency[32]{};
        if(GetEnvironmentVariableA("SM2VR_HAPTICS_GAIN",gain,sizeof(gain))) rumble.gain=static_cast<float>(std::atof(gain));
        if(GetEnvironmentVariableA("SM2VR_HAPTICS_FREQUENCY",frequency,sizeof(frequency))) rumble.frequency=static_cast<float>(std::atof(frequency));
        rumble.test_pulse=env_flag("SM2VR_HAPTICS_TEST");
        const bool wanted=env_flag("SM2VR_HAPTICS");
        const bool enabled=wanted && m_vr_session->enable_rumble(true,rumble);
        EngineControllerRegistration::get().forward_rumble(enabled?&vr_rumble::Mailbox::get():nullptr);
        spdlog::info("VrRumble: requested={} enabled={} gain={} cross={} slot_available={}",wanted,enabled,rumble.gain,rumble.cross,
            EngineControllerRegistration::get().rumble_available());
    }
    m_vr_session->enable_grenade_gesture(env_flag("SM2VR_GRENADE_GESTURE"));
    // SM2VR_GRENADE_AIM_CANDIDATE is the 19 October name of the same switch.
    m_grenade_aim_candidate=env_flag("SM2VR_GRENADE_AIM") || env_flag("SM2VR_GRENADE_AIM_CANDIDATE");
    m_grenade_aim_settings_path=env_path(L"SM2VR_GRENADE_AIM_CONFIG");
    const bool aim_loaded=load_settings<VrGrenadeAim::Settings>(m_grenade_aim_settings_path,[&](const auto& s){return m_vr_session->set_grenade_aim_settings(s);});
    const auto aim=m_vr_session->grenade_aim_settings();
    spdlog::info("VrGrenadeAim: enabled={} config_loaded={} hand_yaw_weight={} max_yaw={} hand_pitch_weight={} base_pitch={} pitch=[{},{}] reference_speed={} speed_fraction=[{},{}] (exact equipment user; one native initial velocity override per release)",
        m_grenade_aim_candidate,aim_loaded,aim[VrGrenadeAim::HAND_YAW_WEIGHT],aim[VrGrenadeAim::MAX_YAW],aim[VrGrenadeAim::HAND_PITCH_WEIGHT],aim[VrGrenadeAim::BASE_PITCH],aim[VrGrenadeAim::MIN_PITCH],aim[VrGrenadeAim::MAX_PITCH],
        aim[VrGrenadeAim::REFERENCE_SPEED],aim[VrGrenadeAim::MIN_SPEED],aim[VrGrenadeAim::MAX_SPEED]);
    m_grenade_settings_path=env_path(L"SM2VR_GRENADE_CONFIG");
    const bool grenade_loaded=load_settings<VrGrenadeThrow::Settings>(m_grenade_settings_path,[&](const auto& s){return m_vr_session->set_grenade_settings(s);});
    spdlog::info("VrGrenade: enabled={} config_loaded={} mode={}",m_vr_session->grenade_gesture_enabled(),grenade_loaded,
        m_vr_session->grenade_settings().hold_mode()?"hold":"tap on throw");
    {   // Gaze camera candidate: the native interaction camera follows the headset gaze.
        auto& steering=vr_gaze::Steering::get();
        steering.enable(env_flag("SM2VR_GAZE_CAMERA"));
        m_gaze_camera_settings_path=env_path(L"SM2VR_GAZE_CAMERA_CONFIG");
        const bool loaded=load_settings<vr_gaze::Steering::Settings>(m_gaze_camera_settings_path,[&](const auto& s){return steering.set_settings(s);});
        const auto& v=steering.settings().values.values;
        spdlog::info("GazeCamera: enabled={} config_loaded={} response_per_second={} deadband={} reengage={} boost={} max_stick={} rate_scale={} pitch_sign={}",
            steering.enabled(),loaded,v[0],v[1],v[2],v[3],v[4],v[5],v[6]);
    }
    if(env_flag("SM2VR_TRACE")) {
        char rate[32]{};double hz=30;
        if(GetEnvironmentVariableA("SM2VR_TRACE_HZ",rate,sizeof(rate))) hz=std::atof(rate);
        auto path=env_path(L"SM2VR_TRACE_PATH");
        if(path.empty()) { wchar_t local[32768]{};if(GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768)) path=std::filesystem::path{local}/"sm2vr"/"sm2vr_trace.jsonl"; }
        std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);
        const bool opened=!path.empty() && vr_trace::Recorder::get().open(path,hz);
        spdlog::info("VrTrace: enabled={} hz={} path={}",opened,hz,path_text(path));
    }
}

void Sm2Probe::trace_camera(const vr_camera::Matrix& game,const vr_camera::Matrix& anchor,const EngineCameraProbe::VrEye& eye) {
    if(!vr_trace::Recorder::get().enabled() || eye.synthetic || eye.eye<0 || eye.eye>1) return;
    vr_camera::Matrix world{},view{};
    // The same composition EngineCameraProbe renders with, so this is the eye the player sees.
    if(!vr_camera::compose(anchor,eye.relative_eye,eye.units_per_metre,world,view,eye.view_settings.mode!=VrViewMode::diorama)) return;
    const auto follow=EngineBodyFollowing::get().snapshot();
    vr_trace::Camera record;record.time=std::chrono::steady_clock::now();record.game=game;record.anchor=anchor;record.units_per_metre=eye.units_per_metre;
    record.actor=follow.follow.actor;record.camera_mode=eye.viewer_anchor?5:static_cast<int>(follow.follow.camera_mode);record.anchored=follow.follow.anchored && !eye.viewer_anchor;
    record.body_yaw=follow.follow.yaw;record.facing_error_degrees=follow.facing_error_degrees;record.melee_facing=follow.follow.melee_facing;record.room_error=follow.follow.room_error;
    record.room_steps=follow.follow.room_steps;record.room_blocked=follow.follow.room_blocked;record.room_held=follow.follow.room_held;record.room_rebases=follow.follow.room_rebases;record.room_rebased=follow.follow.room_rebased;record.room_consumed=follow.follow.consumed;
    record.riding=follow.follow.riding;record.travel_speed=follow.follow.travel_speed;record.vignette=follow.follow.vignette;record.released=follow.follow.released;record.combo_camera=follow.follow.combo_camera;
    record.eye_forward=vr_trace::Vector{world[8],world[9],world[10]};
    vr_trace::Recorder::get().camera(record,eye.eye,{world[12],world[13],world[14]});
}

void Sm2Probe::observe_gaze_camera(const vr_camera::Matrix& game,const vr_camera::Matrix& anchor,const EngineCameraProbe::VrEye& eye) {
    auto& steering=vr_gaze::Steering::get();
    if(!steering.enabled() || eye.synthetic) return;
    const auto follow=EngineBodyFollowing::get().snapshot().follow;
    // Only the anchored first-person body camera: in cutscenes and external cameras the
    // native camera belongs to the game.
    if(!follow.anchored || follow.camera_mode!=VrBodyFollowing::CameraMode::body || eye.view_settings.mode!=VrViewMode::first_person) {steering.clear();return;}
    vr_camera::Matrix world{},view{};
    if(!vr_camera::compose(anchor,eye.relative_eye,eye.units_per_metre,world,view,true)) {steering.clear();return;}
    steering.observe(game,{world[8],world[9],world[10]},std::chrono::steady_clock::now());
}

void Sm2Probe::draw_gesture_overlay() {
    if(ImGui::TreeNode("Left-hand guard (parry / block)")) {
        auto enabled=m_vr_session->guard_enabled();
        if(ImGui::Checkbox("Raise left forearm across or up to parry/block",&enabled)) m_vr_session->enable_guard(enabled);
        auto settings=m_vr_session->guard_settings();bool changed=false;
        for(size_t i=0;i<VrGuard::PARAMETERS.size();++i) {
            const auto& p=VrGuard::PARAMETERS[i];
            changed|=ImGui::SliderFloat(p.name,&settings.values.values[i],p.minimum,p.maximum,"%.2f");
        }
        if(ImGui::Button("Reset guard zone")) { settings={};changed=true; }
        {   // Guard guide: the ghost bar placed from the zone above.
            auto look=m_vr_session->guard_guide_settings();bool look_changed=false;
            bool shown=look.enabled();
            if(ImGui::Checkbox("Guard guide (ghost bar while the sword is out)",&shown)) { look.values.values[VrGuardGuide::ENABLED]=shown?1.f:0.f;look_changed=true; }
            for(size_t i=1;i<VrGuardGuide::PARAMETERS.size();++i) {
                const auto& p=VrGuardGuide::PARAMETERS[i];
                look_changed|=ImGui::SliderFloat(p.name,&look.values.values[i],p.minimum,p.maximum,"%.3f");
            }
            if(look_changed && m_vr_session->set_guard_guide_settings(look) && !m_guard_guide_settings_path.empty()) look.save(m_guard_guide_settings_path);
            const auto s=m_vr_session->guard_guide_status();
            ImGui::Text("Guide %s (launcher %s): %s",look.enabled()?"on":"off",m_vr_session->guard_guide_enabled()?"on":"off",VrGuardGuide::name(s.state));
        }
        if(changed && m_vr_session->set_guard_settings(settings) && !m_guard_settings_path.empty()) settings.save(m_guard_settings_path);
        const auto g=m_vr_session->guard_statistics();
        ImGui::Text("Left hand: height %.2f, lateral %.2f, forward %.2f m; angle from gaze %.0f deg",g.hand.height,g.hand.lateral,g.hand.forward,g.angle_degrees);
        ImGui::Text("In zone %s, guard %s%s; guards %llu, forward-pointing rejections %llu, yielded to sword %llu, last %.0f ms",g.in_zone?"yes":"no",g.held?"HELD":"off",
            g.aim_latched?" (waiting: trigger was pulled here)":"",static_cast<unsigned long long>(g.guards),static_cast<unsigned long long>(g.angle_rejections),
            static_cast<unsigned long long>(g.yields),g.last_duration_ms);
        ImGui::TreePop();
    }
    if(ImGui::TreeNode("Left-arm push (knockback)")) {
        auto enabled=m_vr_session->left_push_enabled();
        if(ImGui::Checkbox("Thrust the open left hand straight out: native held strike",&enabled)) m_vr_session->enable_left_push(enabled);
        const auto p=m_vr_session->left_push_statistics();
        ImGui::Text("Left hand: forward %.2f, height %.2f, lateral %.2f m; thrust %.2f m, peak %.1f m/s; pushes %llu%s",p.forward,p.height,p.lateral,p.travel,p.peak,
            static_cast<unsigned long long>(p.pushes),p.ready?"":" (bring the arm back first)");
        ImGui::TreePop();
    }
    if(ImGui::TreeNode("Grenade throw (left hand)")) {
        auto enabled=m_vr_session->grenade_gesture_enabled();
        if(ImGui::Checkbox("Squeeze left grip near waist, release while throwing",&enabled)) m_vr_session->enable_grenade_gesture(enabled);
        auto settings=m_vr_session->grenade_settings();bool changed=false;
        for(size_t i=0;i<VrGrenadeThrow::PARAMETERS.size();++i) {
            const auto& p=VrGrenadeThrow::PARAMETERS[i];
            changed|=ImGui::SliderFloat(p.name,&settings.values.values[i],p.minimum,p.maximum,"%.2f");
        }
        if(ImGui::Button("Reset grenade gesture")) { settings={};changed=true; }
        if(changed && m_vr_session->set_grenade_settings(settings) && !m_grenade_settings_path.empty()) settings.save(m_grenade_settings_path);
        ImGui::TextUnformatted(m_grenade_aim_candidate?"Launch direction (native frag override on):":"Launch direction (native override off: grenades follow the game camera):");
        auto aim=m_vr_session->grenade_aim_settings();bool aim_changed=false;
        for(size_t i=0;i<VrGrenadeAim::PARAMETERS.size();++i) {
            const auto& p=VrGrenadeAim::PARAMETERS[i];
            aim_changed|=ImGui::SliderFloat(p.name,&aim.values.values[i],p.minimum,p.maximum,"%.2f");
        }
        if(ImGui::Button("Reset grenade aim")) { aim={};aim_changed=true; }
        if(aim_changed && m_vr_session->set_grenade_aim_settings(aim) && !m_grenade_aim_settings_path.empty()) aim.save(m_grenade_aim_settings_path);
        const auto g=m_vr_session->grenade_statistics();
        ImGui::Text("Left hand: height %.2f, lateral %.2f, forward %.2f m; in belt zone %s; speed %.2f m/s",g.hand.height,g.hand.lateral,g.hand.forward,g.in_zone?"yes":"no",g.speed);
        ImGui::Text("Grabs %llu, throws %llu, put back %llu; last release %.2f m/s, forward fraction %.2f",static_cast<unsigned long long>(g.grabs),
            static_cast<unsigned long long>(g.throws),static_cast<unsigned long long>(g.cancels),g.last_release_speed,g.last_forward_fraction);
        ImGui::Text("Last throw: yaw %.0f deg from gaze (hand %.0f), pitch %.0f deg (hand %.0f), hand %.2f m/s = %.2fx native speed",g.last_aim.yaw_degrees,g.last_aim.hand_yaw_degrees,g.last_aim.pitch_degrees,g.last_aim.hand_pitch_degrees,g.last_aim.hand_speed,g.last_aim.speed_scale);
        ImGui::TextUnformatted(!enabled?"Grenade gesture disabled":g.holding?"Grenade gesture held (native inventory checked on throw)":g.recovering?"Release grip and lower left hand to re-arm guard/support":"Left waist grip ready; no held-grenade model");
        ImGui::TreePop();
    }
    if(ImGui::TreeNode("Gaze camera (interaction aim)")) {
        auto& steering=vr_gaze::Steering::get();
        bool enabled=steering.enabled();
        if(ImGui::Checkbox("Turn the native camera toward the gaze (right stick)",&enabled)) steering.enable(enabled);
        auto settings=steering.settings();bool changed=false;
        for(size_t i=0;i<vr_gaze::Steering::PARAMETERS.size();++i) {
            const auto& p=vr_gaze::Steering::PARAMETERS[i];
            changed|=ImGui::SliderFloat(p.name,&settings.values.values[i],p.minimum,p.maximum,"%.3f");
        }
        if(settings.values.values[vr_gaze::Steering::PITCH_SIGN]!=0) settings.values.values[vr_gaze::Steering::PITCH_SIGN]=settings.values.values[vr_gaze::Steering::PITCH_SIGN]<0?-1.f:1.f;
        if(ImGui::Button("Reset gaze camera")) { settings={};changed=true; }
        if(changed && steering.set_settings(settings) && !m_gaze_camera_settings_path.empty()) settings.save(m_gaze_camera_settings_path);
        const auto now=std::chrono::steady_clock::now();
        if(const auto a=steering.latest(now)) ImGui::Text("Gaze vs native camera: yaw %+.1f, pitch %+.1f deg",a->yaw_degrees,a->pitch_degrees);
        else ImGui::TextUnformatted("Gaze vs native camera: unavailable (needs anchored first person)");
        if(const auto s=steering.last()) ImGui::Text("Right stick sent: %+.2f, %+.2f (%llu polls)",s->x,s->y,static_cast<unsigned long long>(steering.steered()));
        ImGui::TreePop();
    }
    auto& trace=vr_trace::Recorder::get();
    if(trace.enabled()) {
        ImGui::Text("Playtest trace: %llu samples -> %s",static_cast<unsigned long long>(trace.samples()),path_text(trace.path()).c_str());
        if(ImGui::Button("Mark moment in trace")) trace.note("marker","overlay button");
    } else ImGui::TextUnformatted("Playtest trace: off (set SM2VR_TRACE=1)");
}

void Sm2Probe::log_gestures() {
    const auto g=m_vr_session->guard_statistics();
    spdlog::info("VrGuard: enabled={} guards={} angle_rejections={} yields={} aim_latched={} held={} in_zone={} hand=[{},{},{}] angle_degrees={} last_ms={}",m_vr_session->guard_enabled(),
        g.guards,g.angle_rejections,g.yields,g.aim_latched,g.held,g.in_zone,g.hand.lateral,g.hand.height,g.hand.forward,g.angle_degrees,g.last_duration_ms);
    if(m_vr_session->guard_guide_enabled()) {
        const auto s=m_vr_session->guard_guide_status();
        spdlog::info("GuardGuide: state={} changes={} polls hidden={} idle={} turn={} blocked={} active={}",VrGuardGuide::name(s.state),s.changes,s.polls[0],s.polls[1],s.polls[2],s.polls[3],s.polls[4]);
    }
    const auto push=m_vr_session->left_push_statistics();
    spdlog::info("VrLeftPush: enabled={} pushes={} ready={} forward={} height={} lateral={} travel={} peak={}",m_vr_session->left_push_enabled(),push.pushes,push.ready,push.forward,push.height,push.lateral,push.travel,push.peak);
    if(const auto r=m_vr_session->rumble_statistics();r.enabled) {
        const auto& registration=EngineControllerRegistration::get();
        // amplitude_bins: applied pulses under 0.1, 0.25, 0.5 and above (vr_rumble::Driver::bins).
        spdlog::info("VrRumble: slot_healthy={} binds={} native_calls={} slot_zero_calls={} applies={} stops={} failures={} peak={} amplitude_bins=[{}] test_pulses={} frequency={}",registration.rumble_healthy(),
            registration.rumble_binds(),registration.rumble_calls(),r.game_calls,r.applies,r.stops,r.failures,r.peak,fmt::join(r.amplitude_bins,","),r.test_pulses,r.frequency);
    }
    const auto n=m_vr_session->grenade_statistics();
    spdlog::info("VrGrenade: enabled={} grabs={} throws={} cancels={} holding={} in_zone={} recovering={} hand=[{},{},{}] last_speed={} last_forward_fraction={}",
        m_vr_session->grenade_gesture_enabled(),n.grabs,n.throws,n.cancels,n.holding,n.in_zone,n.recovering,n.hand.lateral,n.hand.height,n.hand.forward,n.last_release_speed,n.last_forward_fraction);
    if(n.throws) spdlog::info("VrGrenadeAim: last yaw_from_gaze={} pitch={} hand_yaw_from_gaze={} hand_pitch={} hand_speed={} speed_scale={}",n.last_aim.yaw_degrees,n.last_aim.pitch_degrees,n.last_aim.hand_yaw_degrees,n.last_aim.hand_pitch_degrees,n.last_aim.hand_speed,n.last_aim.speed_scale);
    if(vr_trace::Recorder::get().enabled()) spdlog::info("VrTrace: samples={}",vr_trace::Recorder::get().samples());
    auto& steering=vr_gaze::Steering::get();
    if(steering.enabled()) {
        const auto now=std::chrono::steady_clock::now();
        const auto pad=m_controller_input.snapshot(now).pad.Gamepad;
        spdlog::info("GazeCamera: {}",vr_gaze::status_line(steering.steered(),steering.latest(now),steering.age_ms(now),steering.last(),pad.sThumbRX,pad.sThumbRY));
    }
}

// The game's own firearm aim ray: the newest owned CtrlTarget sample (what the shot uses)
// and the aim controller target, so a floating weapon can be compared with where it aims.
void Sm2Probe::trace_weapon() {
    if(!vr_trace::Recorder::get().enabled() || !EngineWeaponProbe::get().installed()) return;
    const auto snapshot=EngineWeaponProbe::get().snapshot();
    const EngineWeaponProbe::Sample* best{};
    for(const auto& s:snapshot.targets) if(s.valid && s.owned && (!best || s.received>best->received)) best=&s;
    if(!best) return;
    vr_trace::WeaponRay ray;ray.time=best->received;ray.owned=best->owned;ray.overridden=best->overridden;
    ray.origin={best->world[12],best->world[13],best->world[14]};ray.forward={best->world[8],best->world[9],best->world[10]};
    ray.target=best->target;ray.max_distance=best->max_distance;
    if(snapshot.aim && snapshot.aim->target_valid) {
        ray.aim_target=vr_arm::Vector{snapshot.aim->input_target[12],snapshot.aim->input_target[13],snapshot.aim->input_target[14]};ray.aimed=snapshot.aim->aimed;
    }
    vr_trace::Recorder::get().weapon(ray);
}

// Native cinematic/pause flags, already sampled every Present for the camera handover.
void Sm2Probe::trace_game() {
    if(!vr_trace::Recorder::get().enabled()) return;
    const auto state=EngineCinematicState::get().snapshot();
    vr_trace::Recorder::get().game({std::chrono::steady_clock::now(),state.available,state.scripted,state.paused,state.published,state.requested});
}
