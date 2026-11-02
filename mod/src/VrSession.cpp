#include "VrSession.h"
#include "SceneMode.h"
#include "PerformanceTrace.h"
#include <algorithm>

VrSession::~VrSession() {
    enable(false);
    // A failed GPU/runtime shutdown must not free textures still in use by queued work.
    retire();
    if (!m_pending.empty()) {
        static auto* retained = new std::vector<Pending>;
        for (auto& pending : m_pending) { retained->push_back(std::move(pending)); }
    }
}
void VrSession::invalidate() { m_support_contact.reset(); m_reference.reset(); m_relative_controllers={}; m_fingers={}; m_swing_pose.reset();m_swing.reset();m_holster.reset();m_left_pose.reset();m_left_motion.reset();m_two_handed.reset();m_gun_gate.reset();m_power_trigger.reset();m_left_push.reset();m_guard.reset();m_grenade.reset();m_throw_aim.reset(); m_rig = vr_camera::IDENTITY; m_eyes.clear();m_ordinary_frames.clear(); ++m_epoch; ++m_reference_epoch; }
void VrSession::enable(bool enabled) {
    std::lock_guard lock{m_mutex};
    if (m_enabled == enabled) { return; }
    m_enabled = enabled; m_failed = false; invalidate(); m_next_poll = {};
    if (!enabled && m_started) { stop_worker_locked(); m_backend->stop(); m_started = false; }
}
void VrSession::set_submit_thread(bool enabled,StepWrapper wrap) {
    std::lock_guard lock{m_mutex}; m_submit_thread=enabled; m_step_wrap=std::move(wrap);
}
void VrSession::start_worker_locked() {
    if(m_worker || !m_submit_thread || !m_backend->separate_frame_thread()) return;
    m_worker=std::make_unique<Worker>();
    m_worker->tick_fence=m_fence.Get(); m_worker->tick=m_tick; // Continue the fence's values, never repeat them.
    m_worker->thread=std::thread{[this,worker=m_worker.get()]{worker_loop(*worker);}};
}
void VrSession::stop_worker_locked() {
    if(!m_worker) return;
    auto& w=*m_worker;
    { std::lock_guard lock{w.mutex}; w.stop=true; }
    w.wake.notify_one();
    if(w.thread.joinable()) w.thread.join();
    // Its leases and counts carry on in the synchronous bookkeeping.
    for(auto& pending:w.pending) m_pending.push_back(std::move(pending));
    if(w.tick_fence==m_fence.Get()) m_tick=std::max(m_tick,w.tick);
    m_statistics.submitted_pairs+=w.submitted; m_statistics.failed_transfers+=w.failed_transfers; m_statistics.submit_replaced+=w.replaced;
    if(w.submitted) { m_statistics.last_render_id=w.last_render_id; m_statistics.oldest_pose_age_ms=w.oldest_pose_age_ms; }
    if(w.submit_cpu_ms) m_statistics.submit_cpu_ms=w.submit_cpu_ms;
    m_worker.reset();
}
void VrSession::collect_worker_locked() {
    if(!m_worker) return;
    bool failed{};
    {
        auto& w=*m_worker; std::lock_guard lock{w.mutex};
        while(!w.pending.empty()) {
            const auto completed=w.pending.front().fence->GetCompletedValue();
            if(completed==UINT64_MAX || completed<w.pending.front().value) break;
            w.pending.pop_front();
        }
        failed=w.failed;
    }
    if(failed) { stop_worker_locked(); m_failed=true; invalidate(); m_backend->stop(); m_started=false; }
}
size_t VrSession::in_flight_locked() const {
    size_t count=m_pending.size();
    if(m_worker) { std::lock_guard lock{m_worker->mutex}; count+=m_worker->pending.size()+(m_worker->busy?1:0); }
    return count;
}
void VrSession::worker_loop(Worker& w) {
    for(;;) {
        std::optional<Worker::Job> job;
        {
            std::unique_lock lock{w.mutex};
            // Wakes for a pair, else every 5 ms to keep the runtime's events flowing.
            w.wake.wait_for(lock,std::chrono::milliseconds(5),[&]{return w.stop || (w.queued && !w.failed);});
            if(w.stop) return;
            if(w.failed) continue; // The session stops this thread at its next poll.
            if(w.queued) { job=std::move(w.queued); w.queued.reset(); w.busy=true; }
        }
        auto result=VrBackend::Transfer::skipped; double cpu_ms{}; HRESULT signaled{S_OK}; uint64_t value{};
        const auto step=[&]{
            m_backend->tick();
            if(!job) return;
            const auto start=std::chrono::steady_clock::now();
            result=m_backend->transfer(job->pair,job->queue.Get());
            cpu_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            {
                std::lock_guard lock{w.mutex};
                if(w.tick_fence!=job->fence.Get()) { w.tick_fence=job->fence.Get(); w.tick=0; }
                value=++w.tick;
            }
            // Retain even a partially submitted pair until the queue that read it has finished.
            signaled=m_backend->completion_queue(job->queue.Get())->Signal(job->fence.Get(),value);
        };
        try { if(m_step_wrap) m_step_wrap(step); else step(); } catch(...) { if(job) result=VrBackend::Transfer::failed; }
        if(!job) continue;
        const auto left=job->pair.left.metadata.pose_captured,right=job->pair.right.metadata.pose_captured;
        const auto render_id=job->pair.right.metadata.render_id;
        std::lock_guard lock{w.mutex};
        w.busy=false; w.submit_cpu_ms=cpu_ms;
        if(value) w.pending.push_back({std::move(job->pair),job->fence,value});
        if(result==VrBackend::Transfer::failed || !value || FAILED(signaled)) { ++w.failed_transfers; w.failed=true; continue; }
        if(result!=VrBackend::Transfer::accepted) continue;
        ++w.submitted; w.last_render_id=render_id;
        w.oldest_pose_age_ms=std::chrono::duration<double,std::milli>(job->now-std::min(left,right)).count();
    }
}
void VrSession::retire() {
    while (!m_pending.empty()) {
        const auto& pending = m_pending.front();
        const auto completed = pending.fence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < pending.value) { break; }
        m_pending.pop_front();
    }
}
void VrSession::poll(std::chrono::steady_clock::time_point now,ID3D12CommandQueue* queue) {
    std::lock_guard lock{m_mutex};
    retire(); collect_worker_locked();
    if (!m_enabled || m_failed) { return; }
    if(m_started) { if(!m_worker) m_backend->tick(); return; } // The submit thread ticks for itself.
    if(now < m_next_poll) { return; }
    m_next_poll = now + std::chrono::seconds(3);
    if (m_backend->available() && m_backend->prepare_graphics(queue)) { m_started = m_backend->start(); }
    if(m_started) { start_worker_locked(); if(!m_worker) m_backend->tick(); }
}
void VrSession::recenter() { std::lock_guard lock{m_mutex}; invalidate(); m_measure_height=true; }
void VrSession::reset_reference() { std::lock_guard lock{m_mutex}; invalidate(); }
VrSession::BodyScaleStatus VrSession::body_scale() const {
    std::lock_guard lock{m_mutex};
    return {m_body.height(),m_calibration.reference_eye_height_metres,m_body.last(),m_body_readings,m_body_rejected,body_factor_locked(),units_locked()};
}
float VrSession::body_factor_locked() const {
    return m_view_settings.mode==VrViewMode::first_person ? m_body.factor(m_calibration.reference_eye_height_metres) : 1.0f;
}
float VrSession::fit_factor_locked() const {
    return m_view_settings.mode==VrViewMode::first_person ? m_calibration.first_person_factor()*body_factor_locked() : 1.0f;
}
float VrSession::units_locked() const {
    return std::clamp(m_view_settings.scale(m_calibration.units_per_metre)*fit_factor_locked(),VrCalibration::SCALE.minimum,VrCalibration::SCALE.maximum);
}
float VrSession::height_offset_locked() const { return m_calibration.height_offset_metres/fit_factor_locked(); }
void VrSession::set_stereo(bool stereo) {
    std::lock_guard lock{m_mutex};
    if (m_stereo != stereo) { m_stereo = stereo; invalidate(); }
}
bool VrSession::set_render_path(RenderPath path){
    if(path!=RenderPath::afr && path!=RenderPath::ordinary)return false;
    std::lock_guard lock{m_mutex};if(path!=m_render_path){m_render_path=path;invalidate();}return true;
}
VrSession::RenderPath VrSession::render_path()const{std::lock_guard lock{m_mutex};return m_render_path;}
bool VrSession::set_scale(float scale) {
    if (!std::isfinite(scale) || scale < VrCalibration::SCALE.minimum || scale > VrCalibration::SCALE.maximum) { return false; }
    std::lock_guard lock{m_mutex};
    if(m_view_settings.scale(scale)>100) return false;
    if (m_calibration.units_per_metre != scale) { m_calibration.units_per_metre = scale; invalidate(); }
    return true;
}
bool VrSession::set_calibration(const VrCalibration& value) {
    if (!value.valid()) { return false; }
    std::lock_guard lock{m_mutex};
    if(m_view_settings.scale(value.units_per_metre)>100) return false;
    // Sprint offset/blend changes apply live (they ramp); only placement needs a new reference.
    if (value != m_calibration) { const bool placement=!value.same_placement(m_calibration); m_calibration = value; if(placement) invalidate(); }
    return true;
}
VrCalibration VrSession::calibration() const { std::lock_guard lock{m_mutex}; return m_calibration; }
bool VrSession::set_view_settings(const VrViewSettings& value) {
    if(!value.valid()) return false;
    std::lock_guard lock{m_mutex};
    const auto scale=value.scale(m_calibration.units_per_metre);
    if(scale<0.01f || scale>100) return false;
    if(value!=m_view_settings) {m_view_settings=value;invalidate();}
    return true;
}
VrViewSettings VrSession::view_settings() const { std::lock_guard lock{m_mutex};return m_view_settings; }
void VrSession::set_swing_observer(std::function<void(std::chrono::steady_clock::time_point)> observer) { std::lock_guard lock{m_mutex};m_swing_observer=std::move(observer); }
void VrSession::enable_swing_melee(bool value) { std::lock_guard lock{m_mutex};m_swing_enabled=value;m_swing.reset(); }
bool VrSession::swing_melee_enabled() const { std::lock_guard lock{m_mutex};return m_swing_enabled; }
void VrSession::allow_swing_melee(uintptr_t actor) { std::lock_guard lock{m_mutex};if(actor!=m_swing_actor) m_swing.reset();m_swing_actor=actor; }
bool VrSession::set_swing_settings(const VrSwingMelee::Settings& value) { std::lock_guard lock{m_mutex};return m_swing.set_settings(value); }
VrSwingMelee::Settings VrSession::swing_settings() const { std::lock_guard lock{m_mutex};return m_swing.settings(); }
VrSwingMelee::Snapshot VrSession::swing_statistics() const { std::lock_guard lock{m_mutex};return m_swing.snapshot(); }
void VrSession::require_trigger_lifted(bool value) { std::lock_guard lock{m_mutex};m_swing_touch_gate=value;m_swing.reset(); }
bool VrSession::trigger_lift_required() const { std::lock_guard lock{m_mutex};return m_swing_touch_gate; }
void VrSession::enable_holsters(bool value) { std::lock_guard lock{m_mutex};m_holster_enabled=value;m_holster.reset(); }
bool VrSession::holsters_enabled() const { std::lock_guard lock{m_mutex};return m_holster_enabled; }
bool VrSession::set_holster_settings(const VrHolster::Settings& value) { std::lock_guard lock{m_mutex};return m_holster.set_settings(value); }
VrHolster::Settings VrSession::holster_settings() const { std::lock_guard lock{m_mutex};return m_holster.settings(); }
VrHolster::Snapshot VrSession::holster_statistics() const { std::lock_guard lock{m_mutex};return m_holster.snapshot(); }
void VrSession::enable_guard(bool value) { std::lock_guard lock{m_mutex};m_guard_enabled=value;m_guard.reset(); }
bool VrSession::guard_enabled() const { std::lock_guard lock{m_mutex};return m_guard_enabled; }
void VrSession::set_guard_belt_latch(bool value) { std::lock_guard lock{m_mutex};m_guard_belt_latch=value;m_guard.reset(); }
bool VrSession::guard_belt_latch() const { std::lock_guard lock{m_mutex};return m_guard_belt_latch; }
bool VrSession::set_guard_settings(const VrGuard::Settings& value) { std::lock_guard lock{m_mutex};return m_guard.set_settings(value); }
VrGuard::Settings VrSession::guard_settings() const { std::lock_guard lock{m_mutex};return m_guard.settings(); }
VrGuard::Snapshot VrSession::guard_statistics() const { std::lock_guard lock{m_mutex};return m_guard.snapshot(); }
void VrSession::enable_guard_guide(bool value) { std::lock_guard lock{m_mutex};m_guard_guide.enable(value); }
bool VrSession::guard_guide_enabled() const { std::lock_guard lock{m_mutex};return m_guard_guide.enabled(); }
bool VrSession::set_guard_guide_settings(const VrGuardGuide::Settings& value) { std::lock_guard lock{m_mutex};return m_guard_guide.set_settings(value); }
VrGuardGuide::Settings VrSession::guard_guide_settings() const { std::lock_guard lock{m_mutex};return m_guard_guide.settings(); }
std::optional<VrGuardGuide::Draw> VrSession::guard_guide(std::chrono::steady_clock::time_point now) const {
    std::lock_guard lock{m_mutex};auto draw=m_guard_guide.current(now);
    // The guard zone is in body-scaled (Joe's) metres; draw it where this player's hand goes.
    if(draw) { const float body=body_factor_locked();for(auto& quad:draw->quads) for(auto& c:quad.corners) {c.height/=body;c.lateral/=m_left_handed?-body:body;c.forward/=body;} }
    return draw;
}
VrGuardGuide::Tracker::Status VrSession::guard_guide_status() const { std::lock_guard lock{m_mutex};return m_guard_guide.status(); }
void VrSession::enable_left_push(bool value) { std::lock_guard lock{m_mutex};m_left_push_enabled=value;m_left_push.reset(); }
bool VrSession::left_push_enabled() const { std::lock_guard lock{m_mutex};return m_left_push_enabled; }
void VrSession::set_left_handed(bool value) { std::lock_guard lock{m_mutex};m_left_handed=value;invalidate(); }
bool VrSession::left_handed() const { std::lock_guard lock{m_mutex};return m_left_handed; }
VrLeftPush::Snapshot VrSession::left_push_statistics() const { std::lock_guard lock{m_mutex};return m_left_push.snapshot(); }
void VrSession::enable_grenade_gesture(bool value) { std::lock_guard lock{m_mutex};m_grenade_enabled=value;m_grenade.reset();m_throw_aim.reset(); }
bool VrSession::grenade_gesture_enabled() const { std::lock_guard lock{m_mutex};return m_grenade_enabled; }
bool VrSession::set_grenade_settings(const VrGrenadeThrow::Settings& value) { std::lock_guard lock{m_mutex};if(!m_grenade.set_settings(value))return false;m_throw_aim.reset();return true; }
VrGrenadeThrow::Settings VrSession::grenade_settings() const { std::lock_guard lock{m_mutex};return m_grenade.settings(); }
bool VrSession::set_grenade_aim_settings(const VrGrenadeAim::Settings& value) { std::lock_guard lock{m_mutex};return m_grenade.set_aim_settings(value); }
VrGrenadeAim::Settings VrSession::grenade_aim_settings() const { std::lock_guard lock{m_mutex};return m_grenade.aim_settings(); }
VrGrenadeThrow::Snapshot VrSession::grenade_statistics() const { std::lock_guard lock{m_mutex};return m_grenade.snapshot(); }
void VrSession::enable_two_hand(bool value) {std::lock_guard lock{m_mutex};m_two_hand_enabled=value;m_support_contact.reset();m_two_hand_grip.reset();m_support.reset();}
void VrSession::note_support_contact(const std::optional<vr_two_hand::Contact>& value) {std::lock_guard lock{m_mutex};m_support_contact=value && value->epoch==m_epoch?value:std::nullopt;}
vr_two_hand::Request VrSession::two_hand_request(std::chrono::steady_clock::time_point now)const {std::lock_guard lock{m_mutex};const auto s=m_two_hand_grip.state();return {m_epoch,s.generation,m_two_hand_enabled && s.held && m_support_contact && VrWeaponAim::fresh(m_support_contact->captured,now),s.engagements};}
vr_two_hand::Grip::State VrSession::two_hand_state()const {std::lock_guard lock{m_mutex};return m_two_hand_grip.state();}
bool VrSession::set_support_settings(const VrSupportGrip::Settings& value) {std::lock_guard lock{m_mutex};return m_support.set_settings(value);}
VrSupportGrip::Settings VrSession::support_settings() const {std::lock_guard lock{m_mutex};return m_support.settings();}
VrSupportGrip::Snapshot VrSession::support_statistics() const {std::lock_guard lock{m_mutex};return m_support.snapshot();}
std::optional<VrWeaponAim::Pose> VrSession::grenade_aim(const vr_camera::Matrix& anchor,uintptr_t actor,uint64_t epoch,std::chrono::steady_clock::time_point now) {
    std::lock_guard lock{m_mutex};
    if(!m_throw_aim || !actor || epoch!=m_epoch || epoch!=m_throw_aim->epoch || now<m_throw_aim->released || now-m_throw_aim->released>std::chrono::milliseconds(750)) return {};
    if(m_throw_aim->actor && m_throw_aim->actor!=actor) return {};
    if(!m_throw_aim->world) {m_throw_aim->world=vr_arm::wrist_world(anchor,m_throw_aim->relative,vr_camera::IDENTITY,units_locked());m_throw_aim->actor=actor;}
    if(!m_throw_aim->world) return {};
    return VrWeaponAim::Pose{actor,epoch,m_throw_aim->sequence,m_throw_aim->released,*m_throw_aim->world,0,m_throw_aim->speed_scale};
}
void VrSession::poll_controllers(VrControllerInput& input,bool foreground,std::chrono::steady_clock::time_point now,float native_camera_heading_offset,std::optional<bool> secondary_equipped,
    std::optional<uint32_t> native_flags,bool character_native,std::optional<float> native_ground_speed,std::optional<std::array<float,2>> native_look) {
    std::lock_guard lock{m_mutex};
    m_character_native=character_native;
    if (now == std::chrono::steady_clock::time_point{}) { now = std::chrono::steady_clock::now(); }
    m_native_ground_speed=native_ground_speed;
    poll_controllers_locked(input,foreground,now,native_camera_heading_offset,secondary_equipped,native_flags,native_look);
    update_rumble(foreground,now);
    // After every path: the pad just published decides whether the game was told to sprint.
    // The gameplay path already updated before tracing; a repeat at the same time is a no-op.
    update_sprint_camera(input,now);
}
bool VrSession::enable_rumble(bool value,vr_rumble::Settings settings,vr_rumble::Mailbox* source) {
    if(!settings.valid() || (value && !source)) return false;
    std::lock_guard lock{m_mutex};m_rumble_enabled=value;m_rumble_settings=settings;m_rumble_source=source;
    m_rumble_test_pending=value && settings.test_pulse;return true;
}
VrSession::RumbleStatistics VrSession::rumble_statistics() const {
    std::lock_guard lock{m_mutex};
    return {m_rumble_enabled,m_rumble_calls,m_rumble.applies(),m_rumble.stops(),m_rumble_failures,m_rumble.peak(),m_rumble.bins(),m_rumble_tests,m_rumble_settings.frequency};
}
// Every path (gameplay, menu, viewer) plays the game's rumble while the session runs in the
// foreground; otherwise the Driver stops whatever is playing.
void VrSession::update_rumble(bool foreground,std::chrono::steady_clock::time_point now) {
    if(!m_rumble_source) return;
    const auto read=m_rumble_source->read();m_rumble_calls=read.calls;
    const bool allowed=m_rumble_enabled && foreground && m_enabled && m_started && !m_failed;
    // One test pulse, retried each poll until the runtime takes it (it refuses before focus).
    if(allowed && m_rumble_test_pending && m_backend->rumble(vr_rumble::Driver::test(m_rumble_settings))) {m_rumble_test_pending=false;++m_rumble_tests;}
    if(!allowed && !m_rumble.playing()) return;
    auto commands=m_rumble.update(read,allowed,now,m_rumble_settings);
    if(m_left_handed) std::swap(commands[0],commands[1]); // The game's left motor is the support hand's.
    if(std::ranges::any_of(commands,[](const auto& c){return c.op!=vr_rumble::Command::Op::none;}) && !m_backend->rumble(commands)) ++m_rumble_failures;
}
void VrSession::update_sprint_camera(VrControllerInput& input,std::chrono::steady_clock::time_point now) {
    const auto native_ground_speed=m_native_ground_speed;
    VrSprintCamera::Settings settings=m_sprint_camera.settings();
    settings.offset_metres=m_calibration.sprint_forward_offset_metres;settings.blend_seconds=m_calibration.sprint_blend_seconds;
    if(settings!=m_sprint_camera.settings()) m_sprint_camera.set_settings(settings,now);
    const auto pad=input.snapshot(now);
    const bool first_person=m_view_settings.mode==VrViewMode::first_person && !m_viewer.engaged() && m_viewer_sequence.state()==VrViewerSequencer::State::off;
    const bool requested=first_person && pad.armed && (pad.pad.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_THUMB)!=0;
    const float units=m_view_settings.scale(m_calibration.units_per_metre);
    std::optional<float> speed;
    if(native_ground_speed && std::isfinite(units) && units>=0.01f) speed=*native_ground_speed/units;
    m_sprint_camera.update(requested,speed,now);
}
VrSprintCamera::Snapshot VrSession::sprint_camera(std::chrono::steady_clock::time_point now) const { std::lock_guard lock{m_mutex};return m_sprint_camera.snapshot(now); }
float VrSession::forward_offset_locked(std::chrono::steady_clock::time_point now) const {
    return (m_calibration.head_forward_offset_metres+m_sprint_camera.offset_metres(now))/fit_factor_locked();
}
void VrSession::poll_controllers_locked(VrControllerInput& input,bool foreground,std::chrono::steady_clock::time_point now,float native_camera_heading_offset,
    std::optional<bool> secondary_equipped,std::optional<uint32_t> native_flags,std::optional<std::array<float,2>> native_look) {
    // Before the head-freshness gate: in photo mode nothing tracks the head until the viewer
    // itself owns the camera, so the sequence must be able to advance on stale tracking.
    if (m_view_settings.mode==VrViewMode::first_person && m_enabled && m_started && !m_failed && m_viewer_sequence.state()!=VrViewerSequencer::State::off) {
        poll_viewer(input,foreground,now,native_flags);return;
    }
    if (m_view_settings.mode!=VrViewMode::first_person || !m_enabled || !m_started || m_failed || !m_reference || now < m_head_received || now-m_head_received > std::chrono::milliseconds(100)) {
        // Only the head being late is a stall (a long frame, or the stereo pairs pausing for a few
        // jobs). The input may pick its held controls up again and the trigger routing keeps its
        // owner, so a held trigger fires on (docs/CONTROL_CUTOUT_2026-10-26.md); a loadout change
        // across the stall still needs the triggers released (VrCombatRouting::update).
        const bool stall=m_view_settings.mode==VrViewMode::first_person && m_enabled && m_started && !m_failed && m_reference;
        m_throw_aim.reset();if(!stall)m_combat.reset();m_support.reset();m_swing.reset();m_holster.reset();m_guard.reset();m_grenade.reset();
        if(stall) input.stall(foreground,now);else input.update(std::nullopt,0,foreground,now);
        return;
    }
    const auto head = vr_camera::multiply(m_relative_head,m_rig);
    const float heading = std::atan2(-head[8],head[10])+native_camera_heading_offset;
    const bool ordinary=m_render_path==RenderPath::ordinary;
    // A full-screen menu over a rendered world (VrScreenMenu.h) shows on the panel while stereo
    // pairs keep coming: those pairs carry the panel, and menu input applies as on pause.
    const bool panel_over_stereo=ordinary && now<m_panel_over_stereo_until;
    if(ordinary && (panel_over_stereo || m_ordinary_frames.empty() || !m_ordinary_frames.back().valid_at(now) || m_ordinary_frames.back().eyes[0].tracking_epoch!=m_epoch)){
        m_throw_aim.reset();m_support.reset();m_swing.reset();m_holster.reset();m_guard.reset();m_grenade.reset();
        // Native pause/settings: no stereo frames, but the menu panel is live. Pass buttons and
        // the raw stick through for menu navigation; motion gestures stay off.
        auto menu=(panel_over_stereo || now<m_menu_input_until) && foreground && input.snapshot(now).enabled?m_backend->controllers():std::nullopt;
        const auto raw_menu=menu;
        // Keeps sticks, A/B/X/Y routes, start, triggers (menu tabs) and trackpad back; drops turn,
        // sprint, recenter and every motion gesture.
        // Left A (raw, the recenter button in gameplay) is native X on a menu (Joe, 25 Oct: all of
        // A, B, X and Y on the face buttons: right A/B, left A/B).
        // The right stick reaches the game raw on a menu (both sticks navigate, 25 Oct); no snap turn.
        // The grips are the shoulder buttons on a menu (Joe, 26 Oct: tabs): right grip = RB, left
        // grip = LB. In play they are melee and parry; only the guard and grenade gestures are dropped.
        if(menu){auto m=*menu;m.menu_navigation=true;m.menu_x=m.recenter;m.sprint=m.recenter=m.finisher=m.swing_melee=m.swing_power=m.select_primary=m.select_secondary=m.salute=m.guard=m.grenade_throw=m.ability=m.left_push=false;
            m.chainsword_rev=0;menu=m;}
        // Left B on a menu: tap = native Y on release, one-second hold = viewer mode. (It was Start
        // until 25 Oct; right B, native B, backs out of the pause menu.) On the pause menu itself a tap
        // is Start again, which closes it: the same button that opened it gets you out (Joe, 31 Oct).
        const bool pause_menu=native_flags && (*native_flags&scene_mode::PAUSE_MASK)==scene_mode::PAUSE_MASK;
        const auto left_b=menu_button_locked(MenuButtonContext::menu,menu && menu->menu,menu.has_value(),now);
        // Chosen on the tap's first frame: the Start closes the menu inside its own pulse, and the rest of
        // the pulse must not turn into Y, the class ability.
        if(left_b.tap && !m_menu_tap)m_menu_tap_start=pause_menu;
        m_menu_tap=left_b.tap;
        if(menu){menu->menu=left_b.tap && m_menu_tap_start;menu->ability=left_b.tap && !m_menu_tap_start;}
        m_left_a.reset();m_touch_x.reset();
        if(left_b.hold && request_viewer(native_flags,now)){poll_viewer(input,foreground,now,native_flags);return;}
        // No menu and no fresh stereo frame (the head is fresh, so the pairs paused or a snap
        // cleared them): a stall, as above. A menu passes its own sample.
        if(!menu && !panel_over_stereo && now>=m_menu_input_until){input.stall(foreground,now);return;}
        m_combat.reset();input.update(menu,0,foreground,now);
        // Menu input is traced too: twice a pause menu opened mid-fight and "the buttons are always
        // scrolling" (29 Oct 08:19, 30 Oct 20:45), with no record of what the game was sent.
        if(vr_trace::Recorder::get().enabled()) trace_input_locked(input,now,menu,raw_menu,false,false,true);
        return;}
    auto sample = foreground && input.snapshot(now).enabled ? (ordinary?m_ordinary_frames.back().input:m_backend->controllers()) : std::nullopt;
    const auto raw_sample=sample;
    // Left A: tap = class ability (native Y), one-second hold = recenter. On Touch, left X holds
    // native Y while held and X with Y held for a second recenters (VrTouchXButton).
    const bool touch=sample && sample->touch_layout;
    const auto left_a=m_left_a.update(!touch && sample && sample->recenter,input.snapshot(now).armed,now);
    const auto touch_x=m_touch_x.update(touch && sample->recenter,touch && sample->menu,touch && input.snapshot(now).armed,now);
    if (left_a.recenter || touch_x.recenter) { invalidate(); m_measure_height=true; input.release("recenter",now); return; }
    if (sample && (left_a.ability || touch_x.ability)) sample->ability=true;
    // Left B: a tap of at least PAUSE_MIN_PRESS = native Start (pause) on release, one-second hold =
    // viewer mode. A Touch X+Y chord cancels it, so the recenter chord never pauses or opens the viewer.
    // So does Left A down during the press: one thumb on both buttons (30 Oct 20:45:40), not a pause.
    const bool left_a_down=!touch && sample && sample->recenter;
    const auto left_b=menu_button_locked(MenuButtonContext::gameplay,sample && sample->menu,input.snapshot(now).armed && !touch_x.chord && !left_a_down,now,PAUSE_MIN_PRESS);
    if (sample) sample->menu=left_b.tap;
    // EngineCinematicState::CINEMATIC (0x10) in the published or requested flags: a cutscene.
    if (sample && native_flags && (*native_flags&0x10u)) sample->cinematic=true;
    if (left_b.hold && request_viewer(native_flags,now)) {
        m_throw_aim.reset();m_combat.reset();m_support.reset();m_swing.reset();m_holster.reset();m_guard.reset();m_grenade.reset();
        poll_viewer(input,foreground,now,native_flags);return;
    }
    const bool armed=input.snapshot(now).armed;
    const bool hands_fresh=m_swing_pose && now>=m_swing_pose->captured && now-m_swing_pose->captured<=std::chrono::milliseconds(100);
    const bool left_fresh=m_left_pose && now>=m_left_pose->captured && now-m_left_pose->captured<=std::chrono::milliseconds(100);
    const auto left_head=left_fresh?std::optional{m_left_pose->head}:std::nullopt;
    const auto left_hand=left_fresh?std::optional{m_left_pose->hand}:std::nullopt;
    const auto right_hand=hands_fresh?std::optional{m_swing_pose->hand}:std::nullopt;
    // Zone gestures (holsters, grenade belt, guard, support, two hands) read hands body-scaled
    // about the head into Joe's metres, so zones tuned to his body fit any player
    // (docs/BODY_SCALE.md). Swing melee, the left push and the grenade launch point stay raw.
    const float body=body_factor_locked();
    const auto zone_left=left_hand?std::optional{vr_body::scaled_from_head(*left_head,*left_hand,body)}:std::nullopt;
    const auto zone_right=right_hand?std::optional{vr_body::scaled_from_head(m_swing_pose->head,*right_hand,body)}:std::nullopt;
    // A shot/aim or a primary support squeeze owns the hands before a holster
    // can acquire. Check this frame's geometry too, so simultaneous squeezes
    // cannot cycle a lowered supported rifle into the sidearm.
    const bool contact_fresh=m_support_contact && m_support_contact->actor==m_swing_actor && m_support_contact->epoch==m_epoch && VrWeaponAim::fresh(m_support_contact->captured,now);
    const bool actual_contact=m_two_hand_enabled && secondary_equipped!=std::optional{true} && contact_fresh && right_hand && left_hand && vr_two_hand::Grip::within_contact(*m_support_contact,*right_hand,*left_hand);
    const bool support_intent=sample && secondary_equipped==std::optional{false} && sample->parry>VrHolster::RELEASE &&
        (m_two_hand_enabled?(actual_contact || m_two_hand_grip.state().reserved):m_support.contains(zone_right,zone_left));
    // A right thumb button squeezes the Index grip with it: never a holster grab (VrHolster::thumb_squeeze).
    const bool holster_acquisition=sample && sample->fire<=0.05f && sample->aim<=0.05f && !support_intent &&
        !VrHolster::thumb_squeeze(sample->confirm,sample->interact,sample->finisher,sample->grenade);
    // The held weapon is learned from where the gun sits on the hands. Loaded somewhere with the
    // gun in neither hand (25 Oct: the Battle Barge's battle simulator) it never resolves, and
    // the draws and sword swings that would resolve it were refused while unknown: no gun, no
    // attack. Unknown for UNKNOWN_LOADOUT (not a weapon switch in progress) lets them through, and
    // the triggers then route as for the primary (VrCombatRouting): a weapon the attachment check
    // never recognises can still fire (UnknownGrace.h, GENERIC_GUARDS piece 5).
    const bool loadout_unknown=m_loadout_grace.update(secondary_equipped.has_value(),now);
    // Holsters read the raw right grip before grip melee is masked; a squeeze in a zone
    // belongs to the holster, never to grip melee.
    const auto slot=m_holster.update(hands_fresh?std::optional{m_swing_pose->head}:std::nullopt,zone_right,
        sample?sample->melee:0,m_holster_enabled && foreground && sample && armed,now,secondary_equipped,holster_acquisition);
    if(sample && m_holster_enabled) {
        sample->select_primary=slot==VrHolster::Slot::primary;sample->select_secondary=slot==VrHolster::Slot::secondary;sample->salute=slot==VrHolster::Slot::salute;
        if(m_holster.owns_grip()) sample->melee=0;
    }
    // Left hand. The grenade reads the raw left grip before parry sees it; a squeeze at the
    // belt or while a grenade is held never parries, and the guard waits while it is held.
    const bool gestures_allowed=foreground && sample && armed;
    // An already supported primary owns its left grip while lowering through the
    // waist. Do not reset support just because the belt overlaps its current pose.
    // A fresh waist squeeze still belongs to grenade; support must have established
    // outside that zone first. Resetting grenade here also consumes the held squeeze
    // so loss of support geometry cannot become a delayed grenade grab.
    const bool continuing_support=gestures_allowed && secondary_equipped==std::optional{false} &&
        (m_two_hand_enabled?m_two_hand_grip.state().reserved:(m_support.snapshot().held && m_support.contains(zone_right,zone_left))) && sample->parry>VrHolster::RELEASE;
    const auto throws_before=m_grenade.snapshot().throws;
    const bool grenade=m_grenade.update(left_head,zone_left,sample?sample->parry:0,m_grenade_enabled && gestures_allowed && !continuing_support,now,left_fresh?std::optional{m_left_pose->captured}:std::nullopt);
    if(!gestures_allowed || !m_grenade_enabled || !left_fresh) m_throw_aim.reset();
    else if(m_grenade.snapshot().throws!=throws_before && left_head && left_hand && m_reference) {
        // Gaze yaw and hand release velocity choose the direction (VrGrenadeAim). Tracking
        // space here; the rig below carries snap turns into it, the anchor maps it to the world.
        const auto forward=vr_body::yaw_forward(*left_head);
        if(forward) {
            const auto chosen=VrGrenadeAim::tracking(*forward,m_grenade.snapshot().last_aim.direction);
            vr_arm::Vector direction{chosen[0],chosen[1],chosen[2]};
            if(vr_arm::length(direction)<.01f) direction={(*forward)[0],0,(*forward)[1]};
            auto launch=vr_arm::turn({0,0,1},direction);vr_arm::position(launch,vr_arm::position(*left_hand));
            if(m_left_handed) launch=vr_hand::mirror(launch); // The throw leaves the real hand, along the real gaze.
            auto relative=vr_camera::multiply(vr_camera::multiply(launch,vr_camera::inverse(*m_reference)),m_rig);
            relative[13]+=height_offset_locked();
            vr_camera::Matrix level{};
            if(vr_camera::level_anchor(vr_camera::multiply(m_relative_head,m_rig),level)) {
                const float forward=forward_offset_locked(now);
                for(int j:{0,2}) relative[12+j]-=forward*level[8+j];
                m_throw_aim=ThrowAim{relative,{},m_epoch,m_grenade.snapshot().throws,now,0,m_grenade.snapshot().last_aim.speed_scale};
            }
        }
    }
    const bool support_allowed=gestures_allowed && secondary_equipped==std::optional{false} && !m_grenade.owns_grip() &&
        (!m_two_hand_enabled || (m_support_contact && m_support_contact->actor==m_swing_actor));
    const bool supporting=m_two_hand_enabled?
        m_two_hand_grip.update(m_support_contact,right_hand,left_hand,sample?sample->parry:0,support_allowed,m_epoch,now):
        m_support.update(zone_right,zone_left,sample?sample->parry:0,support_allowed);
    // A not-yet-known firearm/model cannot acknowledge a grab. Consume that
    // squeeze until release rather than interpreting the failed grab as LB.
    if(m_two_hand_enabled && gestures_allowed && secondary_equipped!=std::optional{true} && !m_grenade.owns_grip() && (actual_contact || !contact_fresh))
        m_two_hand_grip.reserve(sample->parry);
    const bool support_reserved=m_two_hand_enabled && m_two_hand_grip.state().reserved;
    if(sample && m_grenade_enabled) { sample->grenade_throw=grenade;if(m_grenade.owns_grip()) sample->parry=0; }
    // Joe approved grab-only support grip (3 Nov): raw grip still reaches the
    // gun/grenade detectors above, but gameplay parry comes from forearm guard.
    // Menu shoulder-button routing returned earlier and remains unchanged.
    if(sample && (m_two_hand_enabled || supporting || support_reserved)) sample->parry=0;
    // A grenade put back (squeezed at the belt and let go there, no throw) is one native LB tap as
    // it is let go (Joe, 25 Oct). In the barge that is Reject Mission (keymap ResetSelectedMission),
    // which a relaxed hand at the belt could never reach: every squeeze there was a grenade grab.
    // In combat it is a short parry. A grenade raised and let go slowly elsewhere (an aborted
    // throw), a time-out or lost tracking is not a put back.
    {const auto grenade_state=m_grenade.snapshot();
     if(grenade_state.cancels!=m_grenade_cancels_seen){m_grenade_cancels_seen=grenade_state.cancels;
        if(grenade_state.last==VrGrenadeThrow::Outcome::cancelled && grenade_state.in_zone && gestures_allowed)m_put_back_until=now+PUT_BACK_TAP;}
     if(grenade_state.holding)m_put_back_until={};} // A new grab ends a tap still running.
    // A returned grenade is a grab/release too, so grab-only mode must not
    // synthesize LB from it (or Reject Mission while walking the barge).
    if(m_two_hand_enabled)m_put_back_until={};
    if(sample && now<m_put_back_until) sample->parry=1;
    // Both hands on the sword (Joe, 21 Oct): the left hand is not raising a guard.
    const bool two_handed=m_two_handed.update(zone_right,zone_left,now);
    // A grenade held or thrown pauses and latches the guard; a hand merely resting in the belt zone
    // only pauses it, so a raise straight out of the belt guards at once (VrGuard::poll, 26 Oct).
    const bool grenade_busy=m_grenade_enabled && m_grenade.busy(),at_belt=m_grenade_enabled && m_grenade.owns_grip() && !grenade_busy;
    const bool guard=m_guard.poll(left_head,zone_left,sample?sample->aim:0,m_guard_enabled && gestures_allowed && !supporting && !support_reserved && !actual_contact && !two_handed && !grenade_busy,at_belt,m_guard_belt_latch,now);
    if(sample && m_guard_enabled) sample->guard=guard;
    // Only the confirmed pistol/chainsword loadout owns sword motion. The raw
    // right trigger held (not a resting finger) selects power; the left trigger can still
    // fire the pistol. Light is a swing with the trigger released.
    const bool power=sample && sample->fire>VrSwingMelee::POWER_TRIGGER;
    const bool touch_allowed=sample && (power || !(m_swing_touch_gate && sample->fire_touch_valid && sample->fire_touch));
    // A left punch also twists the right controller; the detector compares both hands,
    // unless both hold the sword. A holster zone only stops a swing starting there (the
    // sword rests at the hip and is raised by the shoulder); a weapon selection and its
    // draw stop swings until the hand settles. A swing whose follow-through reaches the
    // hip keeps its full native pulse and cooldown (playtest 20 Oct).
    const auto swings_before=m_swing.snapshot().swings;
    VrMeleePoll melee;melee.right=m_swing_pose;melee.left=left_fresh?m_left_pose:std::nullopt;melee.trigger=sample?sample->fire:0;melee.two_handed=two_handed;
    melee.trigger_held=m_power_trigger.update(melee.trigger,now);
    // Joe, 21 Oct: the bolter out may melee too, with the stricter gun gate: triggers released
    // for 300 ms, no support grip, strong wrist strokes only, holster zones a hard gate.
    const bool sword=m_combat.sword_out(secondary_equipped,now);
    const bool gun_quiet=m_gun_gate.update(sample?sample->fire:1,sample?sample->aim:1,supporting,now);
    melee.gun=!sword && m_combat.gun_out(secondary_equipped,now);
    melee.allowed=m_swing_enabled && m_swing_actor && foreground && sample && touch_allowed && (sword || loadout_unknown || (melee.gun && gun_quiet)) &&
        std::abs(sample->turn_x)<0.7f && armed && !(m_holster_enabled && (melee.gun?m_holster.quiets_gun_swing(now):m_holster.quiets_swing(now)));
    melee.zone_block=m_holster_enabled && m_holster.blocks_swing();
    const bool swing=vr_melee_poll(m_swing,m_left_motion,melee,now);
    if(m_swing.snapshot().swings!=swings_before) { m_guard.yield(now);if(m_swing_observer) m_swing_observer(now); }
    if(sample) { sample->swing_melee=swing;sample->swing_power=swing && m_swing.snapshot().power; }
    // Left-arm push or punch: knockback (the native held strike), sword out only.
    VrLeftPush::Input push{.head=left_head,.hand=left_hand,.trigger=raw_sample?raw_sample->aim:1.f,.grenade=m_grenade_enabled && m_grenade.owns_grip(),
        .right_speed=m_swing.snapshot().wrist_speed,.guard=m_guard.owns_hand(),.allowed=m_left_push_enabled && gestures_allowed && sword && !two_handed && !swing};
    const bool pushed=m_left_push.update(push,now);
    if(sample) sample->left_push=pushed;
    if(sample) m_combat.update(*sample,secondary_equipped,supporting,now,loadout_unknown);else m_combat.reset();
    m_guard_guide.update(m_guard.settings(),{false,m_guard_enabled,sword,gestures_allowed,m_guard.snapshot(),left_fresh?VrGuardGuide::measure(*left_head,*zone_left):std::nullopt},now);
    const auto delta = input.update(sample,heading,foreground,now,native_look);
    update_sprint_camera(input,now);
    if(vr_trace::Recorder::get().enabled()) trace_input_locked(input,now,sample,raw_sample,supporting,two_handed,false);
    if (delta == 0) { return; }
    const auto sine = std::sin(delta), cosine = std::cos(delta);
    vr_camera::Matrix turn{cosine,0,sine,0, 0,1,0,0, -sine,0,cosine,0, 0,0,0,1};
    // Rotate around the current head, preserving its position when physically leaning.
    turn[12] = head[12]-(head[12]*cosine-head[14]*sine);
    turn[14] = head[14]-(head[12]*sine+head[14]*cosine);
    m_rig = vr_camera::multiply(m_rig,turn);
    m_swing.reset();m_swing_pose.reset();m_holster.reset();
    m_eyes.clear();m_ordinary_frames.clear(); ++m_epoch; // Keep head reference; discard images rendered before this snap.
}
void VrSession::trace_input_locked(const VrControllerInput& input,std::chrono::steady_clock::time_point now,const std::optional<VrControllerSample>& sample,
    const std::optional<VrControllerSample>& raw_sample,bool supporting,bool two_handed,bool menu) {
    const auto pad=input.snapshot(now);
    vr_trace::Input record;record.time=now;record.epoch=m_epoch;record.armed=pad.armed;record.reason=pad.reason;
    record.blocked=pad.blocked;record.stall_resumes=pad.stall_resumes;record.last_stall_ms=pad.last_stall_ms;record.disarm=pad.disarm;
    record.head=m_trace_head;
    // The trace keeps physical hands, as every analysis of it expects; record.left_handed says so.
    const auto traced=[&](const VrSwingMelee::Pose& pose){return m_left_handed?vr_hand::mirror(pose.hand):pose.hand;};
    if(m_left_pose) record.hands[m_left_handed?1:0]=traced(*m_left_pose);
    if(m_swing_pose) record.hands[m_left_handed?0:1]=traced(*m_swing_pose);
    record.left_handed=m_left_handed;
    record.sample=sample;record.raw_sample=raw_sample;record.supporting=supporting;record.two_handed=two_handed;record.menu=menu;
    {const auto routing=m_combat.snapshot();record.loadout_secondary=routing.secondary;record.triggers_rearmed=routing.rearmed;record.rev_rearmed=routing.rev_rearmed;record.rev_holdover=routing.rev_holdover;}
    record.pad=pad.pad.Gamepad;record.snaps=pad.snaps;
    record.guard_enabled=m_guard_enabled;record.grenade_enabled=m_grenade_enabled;record.holster_enabled=m_holster_enabled;record.swing_enabled=m_swing_enabled;
    record.guard=m_guard.snapshot();record.grenade=m_grenade.snapshot();record.holster=m_holster.snapshot();record.swing=m_swing.snapshot();
    record.left_push_enabled=m_left_push_enabled;record.left_push=m_left_push.snapshot();record.fingers=m_fingers;
    record.sprint_camera=m_sprint_camera.snapshot(now);
    record.body_factor=body_factor_locked();record.player_eye_height=m_body.height();record.body_readings=m_body_readings;
    record.body_reading=m_body.last()?VrBodyScale::name(*m_body.last()):"";
    vr_trace::Recorder::get().input(record);
}
bool VrSession::request_viewer(std::optional<uint32_t> native_flags,std::chrono::steady_clock::time_point now) {
    ++m_viewer_requests;
    // The entry anchor maps today's room; a recenter or snap since then would misplace it.
    if(!m_viewer_entry || m_viewer_entry->epoch!=m_epoch || !m_viewer_sequence.request(native_flags,now)) {++m_viewer_refusals;return false;}
    m_viewer_start=m_viewer_entry;m_viewer_polled=now;return true;
}
// The game gets a neutral pad plus the sequencer's menu presses; sticks and grips fly the rig.
VrTapHold::Result VrSession::menu_button_locked(MenuButtonContext context,bool pressed,bool armed,std::chrono::steady_clock::time_point now,
        std::chrono::steady_clock::duration min_tap) {
    if(context!=m_menu_button_context){m_menu_button.end_pulse();m_menu_button_context=context;}
    return m_menu_button.update(pressed,armed,now,min_tap);
}
void VrSession::poll_viewer(VrControllerInput& input,bool foreground,std::chrono::steady_clock::time_point now,std::optional<uint32_t> native_flags) {
    m_throw_aim.reset();m_combat.reset();m_support.reset();m_swing.reset();m_holster.reset();m_guard.reset();m_grenade.reset();m_left_a.reset();
    const bool owned=foreground && input.snapshot(now).enabled;
    const auto raw=owned?m_backend->controllers():std::nullopt;
    // Leave on a tap or a hold of Left B once active or playing; presses mid-sequence are ignored.
    using S=VrViewerSequencer::State;
    const auto left_b=menu_button_locked(MenuButtonContext::viewer,raw && raw->menu,owned,now);
    // Right A: a press lets the game run under the free camera; another press freezes it again.
    const bool play_held=raw && raw->confirm;
    const bool play_press=play_held && !m_play_button;m_play_button=play_held;
    const auto play_mode=m_viewer_play.mode();
    if((left_b.hold || left_b.tap) && (m_viewer_sequence.state()==S::active || m_viewer_sequence.state()==S::playing)) {
        if(m_viewer_sequence.state()==S::playing) {m_play_seconds=std::chrono::duration<float>(now-m_viewer_sequence.play_started()).count();m_play_end=vr_viewer::PlayEnd::left;}
        m_viewer_sequence.request(native_flags,now);
    }
    else if(play_press && play_mode!=vr_viewer::Play::Mode::off && m_viewer_sequence.state()==S::active) m_viewer_sequence.play(now);
    else if(m_viewer_sequence.state()==S::playing) {
        const auto elapsed=now-m_viewer_sequence.play_started();
        auto end=play_press?vr_viewer::PlayEnd::cancelled:
            vr_viewer::play_end(m_viewer_play,elapsed,play_held,m_viewer_motion.moved(),m_viewer_motion.still_for(now));
        // Right A freezes. A play that has run its course closes the viewer as Left B does: the game
        // already runs, so nothing is left to press.
        if(end!=vr_viewer::PlayEnd::none && (vr_viewer::freezes(end)?m_viewer_sequence.refreeze(now):m_viewer_sequence.request(native_flags,now))) {m_play_end=end;m_play_seconds=std::chrono::duration<float>(elapsed).count();}
    }
    const auto step=m_viewer_sequence.update(native_flags,foreground,now,m_character_native);
    // Right stick click: one picture a click, however long it's held. The game gets a neutral pad
    // below, so it is never a finisher here.
    const bool photo_held=raw && raw->finisher;
    if(photo_held && !m_photo_button && vr_viewer::takes_photos(m_viewer_sequence.state())) m_photo_requested=true;
    m_photo_button=photo_held;
    if(step.outcome==VrViewerSequencer::Outcome::playing) {m_viewer_motion.reset(now);m_play_end=vr_viewer::PlayEnd::none;m_play_seconds=0;}
    if(step.outcome==VrViewerSequencer::Outcome::entered && m_viewer_start) {
        // Stage space: the reference head's height is the player's eye height above the floor.
        std::optional<VrViewerRig::Ground> ground;
        if(m_viewer_start->floor && m_reference) ground=VrViewerRig::Ground{*m_viewer_start->floor,-(*m_reference)[13]};
        m_viewer.begin(m_viewer_start->anchor,m_viewer_start->units_per_metre,ground);
    }
    if(!VrViewerSequencer::rig_engaged(m_viewer_sequence.state())) m_viewer.end();
    // The render thread stamps tracking at job time, often just after this present-thread
    // poll's time; refusing those (as first person does) dropped about a quarter of the
    // slide live. Accept a sample within 100 ms either side.
    const auto age=now-m_head_received;
    const bool tracked=m_reference && age<=std::chrono::milliseconds(100) && age>=-std::chrono::milliseconds(100);
    if(m_viewer.engaged() && tracked) {
        VrViewerRig::Input rig;rig.head=vr_camera::multiply(m_relative_head,m_rig);
        for(size_t hand=0;hand<2;++hand) if(m_relative_controllers[hand]) {
            const auto pose=vr_camera::multiply(*m_relative_controllers[hand],m_rig);rig.hands[hand]=vr_viewer::Vec{pose[12],pose[13],pose[14]};
        }
        if(raw) {const auto grips=vr_hand::physical_grips(raw->parry,raw->melee,m_left_handed);rig.left_grip=grips[0];rig.right_grip=grips[1];rig.move_x=raw->move_x;rig.move_y=raw->move_y;rig.turn_x=raw->turn_x;rig.turn_y=raw->turn_y;rig.preset=raw->fire;}
        rig.snap_degrees=input.snapshot(now).snap_degrees;
        m_viewer.update(rig,std::chrono::duration<float>(now-m_viewer_polled).count());
    }
    m_viewer_polled=now;
    input.update(std::nullopt,0,foreground,now);
    input.inject(step.buttons,step.buttons?step.until:now);
}
void VrSession::note_first_person_anchor(const vr_camera::Matrix& anchor,const EngineCameraProbe::VrEye& eye,std::optional<float> floor) {
    if(eye.viewer_anchor || eye.view_settings.mode!=VrViewMode::first_person) return;
    std::lock_guard lock{m_mutex};
    if(eye.tracking_epoch!=m_epoch) return;
    if(const auto entry=vr_viewer::entry_anchor(anchor,eye.relative_head,height_offset_locked(),forward_offset_locked(eye.captured),eye.units_per_metre))
        m_viewer_entry=ViewerEntry{*entry,eye.units_per_metre,eye.tracking_epoch,floor};
}
VrSession::ViewerStatus VrSession::viewer_status() const {
    std::lock_guard lock{m_mutex};return {m_viewer_sequence.state(),m_viewer.snapshot(),m_viewer_requests,m_viewer_refusals,m_viewer_sequence.failures(),m_viewer_sequence.unreleased(),
        m_viewer_sequence.plays(),m_viewer_sequence.refreezes(),m_play_end,m_play_seconds,m_viewer_motion.speed(),m_viewer_motion.moved()};
}
bool VrSession::viewer_owns_camera() const { std::lock_guard lock{m_mutex};return m_viewer.engaged(); }
bool VrSession::viewer_engaged() const { std::lock_guard lock{m_mutex};return m_viewer_sequence.state()!=VrViewerSequencer::State::off; }
bool VrSession::viewer_playing() const { std::lock_guard lock{m_mutex};return VrViewerSequencer::play_phase(m_viewer_sequence.state()); }
void VrSession::set_viewer_play(const vr_viewer::Play& play) { std::lock_guard lock{m_mutex};if(play.valid()) m_viewer_play=play; }
vr_viewer::Play VrSession::viewer_play() const { std::lock_guard lock{m_mutex};return m_viewer_play; }
// Speeds are in the Marine's metres, as the takeover's still speed: not the eyes' first-person scale.
void VrSession::note_character_motion(const vr_viewer::Motion::Sample& sample) {
    std::lock_guard lock{m_mutex};
    if(m_viewer_sequence.state()!=VrViewerSequencer::State::playing) return;
    m_viewer_motion.add(sample,m_view_settings.scale(m_calibration.units_per_metre),m_viewer_play.still_speed());
}
bool VrSession::take_photo_request() { std::lock_guard lock{m_mutex};const bool out=m_photo_requested;m_photo_requested=false;return out; }
bool VrSession::photo_pulse() {
    std::lock_guard lock{m_mutex};
    return m_enabled && m_started && !m_failed && m_backend->rumble(vr_photo::pulse(m_rumble_settings.frequency));
}
std::string VrSession::input_status() const { std::lock_guard lock{m_mutex}; return m_backend->input_status(); }
std::string VrSession::input_bindings() const { std::lock_guard lock{m_mutex}; return m_backend->input_bindings(); }
std::string VrSession::finger_status() const { std::lock_guard lock{m_mutex}; return m_backend->finger_status(); }
std::optional<VrSession::ControllerPoses> VrSession::controller_poses(std::chrono::steady_clock::time_point now) const {
    std::lock_guard lock{m_mutex};
    if(m_render_path==RenderPath::ordinary){
        if(m_ordinary_frames.empty() || !m_ordinary_frames.back().valid_at(now) || m_ordinary_frames.back().eyes[0].tracking_epoch!=m_epoch)return {};
        return m_ordinary_frames.back().controllers;
    }
    return make_controller_poses(now);
}
std::optional<VrSession::ControllerPoses> VrSession::make_controller_poses(std::chrono::steady_clock::time_point now)const{
    if (m_view_settings.mode!=VrViewMode::first_person || m_viewer.engaged() || !m_enabled || !m_started || m_failed || !m_reference || now<m_head_received ||
        now-m_head_received>std::chrono::milliseconds(100)) { return std::nullopt; }
    ControllerPoses poses{m_relative_controllers,m_head_received,m_reference_epoch,m_epoch,units_locked()};
    poses.fingers=m_fingers;poses.fit_factor=fit_factor_locked();
    vr_camera::Matrix heading{};
    if (!vr_camera::level_anchor(vr_camera::multiply(m_relative_head,m_rig),heading)) { return std::nullopt; }
    const float forward=forward_offset_locked(now);
    for (auto& pose:poses.relative) if (pose) {
        *pose=vr_camera::multiply(*pose,m_rig);
        (*pose)[13]+=height_offset_locked();
        for (int j:{0,2}) { (*pose)[12+j]-=forward*heading[8+j]; }
    }
    return poses;
}
std::optional<EngineCameraProbe::VrEye> VrSession::eye(const FrameSyncClock::Frame& frame,std::chrono::steady_clock::time_point now) {
    return acquire_eye(frame,now,false);
}
std::optional<EngineCameraProbe::VrEye> VrSession::shared_eye(const FrameSyncClock::Frame& frame,std::chrono::steady_clock::time_point now) {
    return acquire_eye(frame,now,true);
}
std::optional<EngineCameraProbe::VrEye> VrSession::acquire_eye(const FrameSyncClock::Frame& frame,std::chrono::steady_clock::time_point now,bool reuse) {
    std::lock_guard lock{m_mutex};
    if(m_render_path!=RenderPath::afr)return {};
    if(reuse && m_enabled && m_started && !m_failed && !frame.invalid && frame.render_id && frame.eye>=0 && frame.eye<=1) {
        const auto issued=std::ranges::find_if(m_eyes,[&](const auto& value){return value.render_token==frame.render_id;});
        if(issued!=m_eyes.end()) return issued->eye==frame.eye && issued->tracking_epoch==m_epoch && issued->valid_at(now) ? std::optional{*issued} : std::nullopt;
    }
    if (!m_enabled || !m_started || m_failed || frame.invalid || frame.render_id == 0 || frame.render_id <= m_last_eye || frame.eye < 0 || frame.eye > 1) { return std::nullopt; }
    m_last_eye = frame.render_id;
    const auto tracking = m_backend->tracking();
    if (!tracking || !vr_camera::rigid(tracking->head) || !vr_camera::rigid(tracking->eyes[frame.eye]) || !tracking->fov[frame.eye].valid()) {
        invalidate(); return std::nullopt;
    }
    if(!update_tracking(*tracking,now)){invalidate();return {};}
    auto eye=make_eye(*tracking,frame.eye,frame.render_id,now);if(!eye){invalidate();return {};}
    if (m_eyes.size() == 8) { m_eyes.pop_front(); }
    m_eyes.push_back(*eye);return eye;
}
bool VrSession::update_tracking(const VrBackend::Tracking& tracking,std::chrono::steady_clock::time_point now){
    // Putting the headset on references again, as a recenter that does not measure: the
    // launch's first reference is often taken with the headset still in the player's hands.
    if (tracking.worn) {
        if (*tracking.worn && m_worn==false && m_reference) { invalidate(); ++m_statistics.worn_references; }
        m_worn=tracking.worn;
    }
    if (!m_reference) {
        vr_camera::Matrix anchor{};
        if (!vr_camera::level_anchor(tracking.head,anchor))return false;
        // Only a first-person, player-initiated recenter measures; automatic resets can happen
        // crouched. Third person references where the head is (often seated) and never measures:
        // body scale is first person only, and a seated reading would carry over to first person.
        // Standing space: the head's Y is its height above the floor.
        if (m_view_settings.mode!=VrViewMode::first_person) {
            m_measure_height=false;
        } else if (m_measure_height) {
            m_measure_height=false;++m_body_readings;
            if (m_body.measure(anchor[13])!=VrBodyScale::Reading::accepted) { ++m_body_rejected; }
        } else {
            // An automatic reference stands at the player's eye height, not wherever the
            // headset was (a desk, the player's hands), so the eye starts at standing height.
            auto standing=m_body.height();
            if (!standing) { standing=m_calibration.reference_eye_height_metres; }
            if (standing) { anchor[13]=*standing; }
        }
        m_reference = anchor;
    }
    if (!vr_camera::relative_eye(*m_reference,tracking.head,vr_camera::IDENTITY,m_relative_head))return false;
    m_relative_controllers={};m_fingers={};
    for (size_t hand=0;hand<m_relative_controllers.size();++hand) if (tracking.controllers[hand]) {
        vr_camera::Matrix relative{};
        if (vr_camera::relative_eye(*m_reference,*tracking.controllers[hand],vr_camera::IDENTITY,relative)) {
            m_relative_controllers[hand]=relative;
            if (tracking.fingers[hand] && vr_finger::valid(*tracking.fingers[hand])) { m_fingers[hand]=tracking.fingers[hand]; }
        }
    }
    m_head_received = now;
    m_swing_pose.reset();m_left_pose.reset();
    const auto player=vr_hand::logical(tracking.head,tracking.controllers,m_left_handed);
    if(player.hands[1] && vr_camera::rigid(*player.hands[1]))
        m_swing_pose=VrSwingMelee::Pose{*player.hands[1],player.head,now,m_epoch};
    if(player.hands[0] && vr_camera::rigid(*player.hands[0]))
        m_left_pose=VrSwingMelee::Pose{*player.hands[0],player.head,now,m_epoch};
    m_trace_head=tracking.head;
    return true;
}
std::optional<EngineCameraProbe::VrEye> VrSession::make_eye(const VrBackend::Tracking& tracking,int slot,uint64_t render_id,std::chrono::steady_clock::time_point now)const{
    EngineCameraProbe::VrEye eye{};
    eye.eye = slot; eye.render_token = render_id; eye.captured = now;
    const auto mode=m_eye_source.mode();const int source=VrEyeSourceDiagnostic::source(mode,slot);
    auto eye_to_head=tracking.eyes[source];eye_to_head[12]+=VrEyeSourceDiagnostic::lateral_offset(mode,slot);
    eye.runtime_head = tracking.head; eye.tracking_epoch = m_epoch; eye.fov = tracking.fov[source];
    eye.runtime_eye_to_head = m_stereo ? eye_to_head : vr_camera::IDENTITY;
    eye.reference_epoch = m_reference_epoch;
    eye.relative_head = vr_camera::multiply(m_relative_head,m_rig);
    eye.view_settings=m_view_settings;
    eye.units_per_metre = units_locked();
    // Viewer: the free camera's anchor and scale. Its entry anchor already holds the
    // first-person offsets, so they are not applied again.
    const bool viewer=m_viewer.engaged();
    eye.fit_factor=fit_factor_locked();
    if(viewer) {const auto rig=m_viewer.snapshot();eye.viewer_anchor=rig.anchor;eye.units_per_metre=rig.units_per_metre;eye.fit_factor=1;}
    const bool first_person=m_view_settings.mode==VrViewMode::first_person && !viewer;
    if (!vr_camera::relative_eye(*m_reference,tracking.head,m_stereo ? eye_to_head : vr_camera::IDENTITY,eye.relative_eye))return {};
    eye.relative_eye = vr_camera::multiply(eye.relative_eye,m_rig);
    if(first_person) eye.relative_eye[13] += height_offset_locked();
    vr_camera::Matrix heading{};
    if (!vr_camera::level_anchor(eye.relative_head,heading))return {};
    // Placement calibration follows level body/head yaw, not gaze pitch or a frozen room axis.
    // Apply the same displacement to both eyes and the controller origins above. The sprint
    // offset rides on the same axis, so hands stay registered to the eyes while it blends.
    if(first_person) { const float forward=forward_offset_locked(now); for (int j:{0,2}) { eye.relative_eye[12+j]-=forward*heading[8+j]; } }
    return eye;
}
bool VrSession::OrdinaryFrame::valid_at(std::chrono::steady_clock::time_point now)const{
    return valid_at(now,std::chrono::milliseconds(100));
}
bool VrSession::OrdinaryFrame::valid_for_submission(std::chrono::steady_clock::time_point now)const{
    return valid_at(now,std::chrono::milliseconds(250));
}
bool VrSession::OrdinaryFrame::valid_at(std::chrono::steady_clock::time_point now,std::chrono::milliseconds max_age)const{
    const auto& left=eyes[0];const auto& right=eyes[1];
    if(!render_id || !left.tracking_epoch || !left.reference_epoch || now<left.captured || now-left.captured>max_age ||
       left.captured!=right.captured || left.runtime_head!=right.runtime_head || left.relative_head!=right.relative_head ||
       !vr_camera::rigid(left.relative_head) || left.tracking_epoch!=right.tracking_epoch || left.reference_epoch!=right.reference_epoch || left.units_per_metre!=right.units_per_metre || left.view_settings!=right.view_settings || left.viewer_anchor!=right.viewer_anchor)return false;
    float separation{};for(size_t axis=12;axis<15;++axis){const float d=left.runtime_eye_to_head[axis]-right.runtime_eye_to_head[axis];separation+=d*d;}
    if(!std::isfinite(separation) || separation<=1e-8f)return false;
    for(size_t slot=0;slot<2;++slot){const auto& eye=eyes[slot];if(eye.eye!=int(slot) || eye.render_token!=render_id || !eye.valid_at(now) || eye.synthetic ||
        !eye.fov || !eye.fov->valid() || !vr_camera::rigid(eye.runtime_head) || !vr_camera::rigid(eye.runtime_eye_to_head))return false;}
    if(controllers){if(left.view_settings.mode!=VrViewMode::first_person || controllers->captured!=left.captured || controllers->tracking_epoch!=left.tracking_epoch ||
        controllers->reference_epoch!=left.reference_epoch || controllers->units_per_metre!=left.units_per_metre)return false;
        for(size_t hand=0;hand<2;++hand){if(controllers->relative[hand] && !vr_camera::rigid(*controllers->relative[hand]))return false;
            if(controllers->fingers[hand] && (!controllers->relative[hand] || !vr_finger::valid(*controllers->fingers[hand])))return false;}}
    else if(left.view_settings.mode==VrViewMode::first_person && !left.viewer_anchor)return false;
    if(left.viewer_anchor && input)return false;
    if(left.view_settings.mode!=VrViewMode::first_person && input)return false;
    return true;
}
std::optional<VrSession::OrdinaryFrame> VrSession::ordinary_frame(const FrameSyncClock::Frame& frame,std::chrono::steady_clock::time_point now){
    PerformanceTrace::Scope timing{"ordinary_tracking_and_lock_cpu"};
    std::lock_guard lock{m_mutex};if(m_render_path!=RenderPath::ordinary || !m_stereo || !m_enabled || !m_started || m_failed)return {};
    if(frame.invalid || !frame.render_id){m_last_eye=std::max(m_last_eye,frame.render_id);invalidate();return {};}
    const auto cached=std::ranges::find(m_ordinary_frames,frame.render_id,&OrdinaryFrame::render_id);
    if(cached!=m_ordinary_frames.end())return cached->game_token==frame.token && cached->eyes[0].tracking_epoch==m_epoch && cached->valid_at(now)?std::optional{*cached}:std::nullopt;
    if(frame.render_id<=m_last_eye || now<m_head_received)return {};m_last_eye=frame.render_id;
    try{const auto tracking=m_backend->tracking();if(!tracking || !vr_camera::rigid(tracking->head)) {invalidate();return {};}
        for(size_t slot=0;slot<2;++slot)if(!vr_camera::rigid(tracking->eyes[slot]) || !tracking->fov[slot].valid()){invalidate();return {};}
        if(!update_tracking(*tracking,now)){invalidate();return {};}
        if(m_eye_source.poll(now)){m_statistics.eye_source=m_eye_source.mode();m_statistics.eye_source_since_render_id=frame.render_id;}
        OrdinaryFrame value;value.game_token=frame.token;value.render_id=frame.render_id;
        for(size_t slot=0;slot<2;++slot){const auto eye=make_eye(*tracking,int(slot),frame.render_id,now);if(!eye){invalidate();return {};}value.eyes[slot]=*eye;}
        value.controllers=make_controller_poses(now);
        if(m_view_settings.mode==VrViewMode::first_person && !m_viewer.engaged())value.input=m_backend->controllers();
        if(!value.valid_at(now)){invalidate();return {};}
        if(m_ordinary_frames.size()==8)m_ordinary_frames.pop_front();m_ordinary_frames.push_back(value);return value;
    }catch(...){invalidate();return {};}
}
bool VrSession::submit(const EyeTextureCapture::Pair& pair,ID3D12CommandQueue* queue,std::chrono::steady_clock::time_point now) {
    std::lock_guard lock{m_mutex};
    retire(); collect_worker_locked();
    if (m_render_path!=RenderPath::afr || !m_enabled || !m_started || m_failed || !queue || in_flight_locked() >= 2) { return false; }
    const auto& left = pair.left; const auto& right = pair.right;
    if (!left.ready() || !right.ready() || left.generation != right.generation || left.metadata.eye != 0 || right.metadata.eye != 1 ||
        left.metadata.render_id+1 != right.metadata.render_id || uint32_t(left.metadata.game_token+1) != right.metadata.game_token ||
        right.metadata.render_id <= m_last_submitted || left.texture() == right.texture()) { return false; }
    for (const auto* image : {&left,&right}) {
        const auto& meta = image->metadata;
        if (!meta.tracked_stereo || meta.tracking_epoch != m_epoch || !vr_camera::rigid(meta.runtime_head) ||
            now < meta.pose_captured || now-meta.pose_captured > std::chrono::milliseconds(100)) { return false; }
        const auto issued = std::ranges::find_if(m_eyes,[&](const auto& eye) { return eye.render_token == meta.render_id; });
        if (issued == m_eyes.end() || issued->eye != meta.eye || issued->tracking_epoch != meta.tracking_epoch ||
            issued->captured != meta.pose_captured || issued->runtime_head != meta.runtime_head ||
            issued->runtime_eye_to_head != meta.runtime_eye_to_head) { return false; }
    }
    return transfer_images(pair,queue,now);
}
bool VrSession::submit_ordinary(const OrdinaryImages& ordinary,ID3D12CommandQueue* queue,std::chrono::steady_clock::time_point now) {
    std::lock_guard lock{m_mutex};retire();collect_worker_locked();
    const auto refuse=[&](size_t reason){++m_statistics.ordinary_refusals[reason];return false;};
    if(m_render_path!=RenderPath::ordinary || !m_enabled || !m_started || m_failed || !queue || (ordinary.queue && ordinary.queue.Get()!=queue) || in_flight_locked()>=2 || !ordinary.dispatch)return refuse(0);
    const auto& pair=ordinary.images;const auto& left=pair.left;const auto& right=pair.right;
    m_statistics.ordinary_generations={left.generation,right.generation};
    m_statistics.ordinary_pose_age_ms=std::chrono::duration<double,std::milli>(now-left.metadata.pose_captured).count();
    if(!left.ready() || !right.ready() || !left.generation || left.generation!=right.generation || left.metadata.eye!=0 || right.metadata.eye!=1 ||
       left.metadata.render_id!=right.metadata.render_id || left.metadata.game_token!=right.metadata.game_token ||
       left.metadata.render_id<=m_last_submitted || left.texture()==right.texture())return refuse(1);
    const auto issued=std::ranges::find(m_ordinary_frames,left.metadata.render_id,&OrdinaryFrame::render_id);
    if(issued==m_ordinary_frames.end() || issued->game_token!=left.metadata.game_token)return refuse(2);
    if(!issued->valid_for_submission(now))return refuse(3);
    for(const auto* image:{&left,&right}) {
        const auto& meta=image->metadata;const auto& eye=issued->eyes[meta.eye];
        if(!meta.tracked_stereo || meta.screen_menu!=left.metadata.screen_menu || meta.tracking_epoch!=m_epoch || eye.tracking_epoch!=meta.tracking_epoch ||
           meta.pose_captured!=eye.captured || meta.runtime_head!=eye.runtime_head || meta.runtime_eye_to_head!=eye.runtime_eye_to_head)return refuse(4);
    }
    const bool sent=transfer_images(pair,queue,now);
    if(sent)m_panel_over_stereo_until=left.metadata.screen_menu?now+std::chrono::milliseconds(250):std::chrono::steady_clock::time_point{};
    return sent;
}
std::optional<std::array<EyeTextureCapture::Metadata,2>> VrSession::menu_eyes(std::chrono::steady_clock::time_point now){
    std::lock_guard lock{m_mutex};
    if(m_render_path!=RenderPath::ordinary || !m_stereo || !m_enabled || !m_started || m_failed)return {};
    try{const auto tracking=m_backend->tracking();if(!tracking || !vr_camera::rigid(tracking->head))return {};
        // Keeps the head sample fresh for menu input while the stereo producer is idle. First,
        // so the epochs below include a reference taken again for putting the headset on.
        if(!update_tracking(*tracking,now))return {};
        std::array<EyeTextureCapture::Metadata,2> out;
        for(int slot=0;slot<2;++slot){auto& meta=out[slot];
            if(!vr_camera::rigid(tracking->eyes[slot]) || !tracking->fov[slot].valid() || !vr_projection::build(tracking->fov[slot],.1f,1000,meta.projection))return {};
            meta.eye=slot;meta.tracked_stereo=true;meta.screen_menu=true;meta.runtime_head=tracking->head;meta.runtime_eye_to_head=tracking->eyes[slot];
            meta.tracking_epoch=m_epoch;meta.pose_captured=now;}
        return out;
    }catch(...){return {};}
}
bool VrSession::submit_menu(const EyeTextureCapture::Pair& pair,ID3D12CommandQueue* queue,std::chrono::steady_clock::time_point now){
    std::lock_guard lock{m_mutex};retire();collect_worker_locked();
    if(m_render_path!=RenderPath::ordinary || !m_enabled || !m_started || m_failed || !queue || in_flight_locked()>=2)return false;
    const auto& left=pair.left;const auto& right=pair.right;
    if(!left.ready() || !right.ready() || left.metadata.eye!=0 || right.metadata.eye!=1 || left.metadata.render_id!=right.metadata.render_id || left.texture()==right.texture())return false;
    for(const auto* image:{&left,&right}){const auto& meta=image->metadata;
        if(!meta.screen_menu || !meta.tracked_stereo || meta.tracking_epoch!=m_epoch || !vr_camera::rigid(meta.runtime_head) ||
           now<meta.pose_captured || now-meta.pose_captured>std::chrono::milliseconds(100))return false;}
    // Menu frames use their own IDs; they must not advance the ordinary pair sequence.
    const auto last=m_last_submitted;const bool sent=transfer_images(pair,queue,now);m_last_submitted=last;
    if(sent)m_menu_input_until=now+std::chrono::milliseconds(250);
    return sent;
}
bool VrSession::transfer_images(const EyeTextureCapture::Pair& pair,ID3D12CommandQueue* queue,std::chrono::steady_clock::time_point now) {
    const auto& left=pair.left;const auto& right=pair.right;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT || device->GetNodeCount() != 1) { return false; }
    for (const auto* image : {&left,&right}) {
        Microsoft::WRL::ComPtr<ID3D12Device> image_device;
        if (FAILED(image->texture()->GetDevice(IID_PPV_ARGS(&image_device))) || image_device.Get() != device.Get()) { return false; }
    }
    const bool queued=m_worker && [&]{std::lock_guard lock{m_worker->mutex};return m_worker->queued.has_value();}();
    if (m_queue && m_queue.Get() != queue && (in_flight_locked() || queued)) { return false; }
    if (!m_queue || m_queue.Get() != queue) {
        m_queue = queue; m_device = device;
        if (FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&m_fence)))) { m_queue.Reset(); return false; }
        m_tick = 0;
    }
    if (m_worker) {
        // Hand over; the submit thread transfers, leases and counts it. A pair it has not taken yet is replaced.
        auto& w=*m_worker;
        { std::lock_guard lock{w.mutex}; if (w.queued) { ++w.replaced; } w.queued=Worker::Job{pair,queue,m_fence,now}; }
        w.wake.notify_one();
        m_last_submitted = right.metadata.render_id;
        return true;
    }
    // Retain even a partially submitted pair until the queue has finished runtime transfer work.
    const auto submit_start = std::chrono::steady_clock::now();
    const auto transfer = m_backend->transfer(pair,queue);
    m_statistics.submit_cpu_ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-submit_start).count();
    m_pending.push_back({pair,m_fence,++m_tick});
    // The lease ends when the queue that read the images is done (the backend's own queue, if it has one).
    const auto signaled = m_backend->completion_queue(queue)->Signal(m_fence.Get(),m_tick);
    m_last_submitted = right.metadata.render_id;
    if (transfer == VrBackend::Transfer::failed || FAILED(signaled)) {
        ++m_statistics.failed_transfers;
        m_failed = true; invalidate(); m_backend->stop(); m_started = false;
        return false;
    }
    if(transfer == VrBackend::Transfer::skipped) { return false; }
    ++m_statistics.submitted_pairs;
    m_statistics.last_render_id = right.metadata.render_id;
    m_statistics.oldest_pose_age_ms = std::chrono::duration<double,std::milli>(now-std::min(left.metadata.pose_captured,right.metadata.pose_captured)).count();
    return true;
}
std::string VrSession::status() const {
    std::lock_guard lock{m_mutex};
    return !m_enabled ? "disabled" : m_failed ? "submission failed; disable/re-enable to retry: " + m_backend->status() : m_backend->status();
}
uint64_t VrSession::epoch() const { std::lock_guard lock{m_mutex}; return m_epoch; }
VrSession::Statistics VrSession::statistics() const {
    std::lock_guard lock{m_mutex}; auto out=m_statistics;
    if(m_worker) {
        const auto& w=*m_worker; std::lock_guard worker_lock{m_worker->mutex};
        out.submit_thread=true; out.submitted_pairs+=w.submitted; out.failed_transfers+=w.failed_transfers; out.submit_replaced+=w.replaced;
        if(w.submitted) { out.last_render_id=w.last_render_id; out.oldest_pose_age_ms=w.oldest_pose_age_ms; }
        if(w.submit_cpu_ms) out.submit_cpu_ms=w.submit_cpu_ms;
    }
    return out;
}
