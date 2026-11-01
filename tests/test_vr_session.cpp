#include "GpuTestHost.h"
#include "VrSession.h"
#include "OpenVrConversions.h"
#include <future>

namespace {
bool close(float a,float b) { return std::abs(a-b)<0.00001f; }
struct Backend final : VrBackend {
    bool present{}, valid{true},skip_transfer{}; std::atomic<bool> submit_ok{true};
    unsigned starts{}, stops{},trackings{};
    std::atomic<unsigned> submissions{},ticks{};
    Tracking pose;
    Host* gpu{};
    ComPtr<ID3D12Fence> transfer_gate;
    std::optional<VrControllerSample> controller_sample;
    Backend() {
        pose.head[13] = 1.7f;
        pose.eyes[0][12] = -0.032f; pose.eyes[1][12] = 0.032f;
        pose.fov = {vr_projection::Tangents{-1.1f,0.9f,-0.9f,1.1f},vr_projection::Tangents{-0.9f,1.1f,-1.1f,0.9f}};
    }
    bool available() override { return present; }
    bool start() override { ++starts; return true; }
    std::optional<Tracking> tracking() override { ++trackings;return valid ? std::optional{pose} : std::nullopt; }
    bool submit(const EyeTextureCapture::Pair& pair,ID3D12CommandQueue* queue) override {
        ++submissions;
        require(pair.left.texture() != pair.right.texture() && pair.left.metadata.eye == 0 && pair.right.metadata.eye == 1,
            "backend must receive two independent eye images in correct order");
        require(pair.left.metadata.runtime_head[13] == 1.7f && pair.right.metadata.runtime_head[13] == 1.7f,
            "submitted images must carry absolute runtime head pose without applying game scale");
        if (transfer_gate) { check(queue->Wait(transfer_gate.Get(),1)); }
        return submit_ok;
    }
    void stop() override { ++stops; }
    Transfer transfer(const EyeTextureCapture::Pair& pair,ID3D12CommandQueue* queue) override {
        {   // A runtime whose frame calls block (xrBeginFrame behind a loaded GPU) until released.
            std::unique_lock lock{gate_mutex}; transfer_thread=std::this_thread::get_id(); ++entered; gate_changed.notify_all();
            gate_changed.wait(lock,[&]{return !hold;});
        }
        return skip_transfer?Transfer::skipped:VrBackend::transfer(pair,queue);
    }
    void tick() override { ++ticks; }
    bool frame_thread{};
    bool separate_frame_thread() const override { return frame_thread; }
    std::mutex gate_mutex; std::condition_variable gate_changed; bool hold{}; unsigned entered{}; std::thread::id transfer_thread;
    void set_hold(bool value) { { std::lock_guard lock{gate_mutex}; hold=value; } gate_changed.notify_all(); }
    bool wait_entered(unsigned count) { std::unique_lock lock{gate_mutex}; return gate_changed.wait_for(lock,std::chrono::seconds(5),[&]{return entered>=count;}); }
    ID3D12CommandQueue* completion{};
    ID3D12CommandQueue* completion_queue(ID3D12CommandQueue* caller) override { return completion?completion:caller; }
    std::string status() const override { return present ? "mock active" : "mock absent"; }
    std::optional<VrControllerSample> controllers() override { return controller_sample; }
    std::vector<vr_rumble::Commands> rumbles; bool rumble_ok{true};
    bool rumble(const vr_rumble::Commands& c) override { rumbles.push_back(c); return rumble_ok; }
};
EyeTextureCapture::Metadata tracked_meta(const EngineCameraProbe::VrEye& eye,uint32_t token) {
    auto m = meta(eye.render_token,eye.eye,token);
    m.tracked_stereo = true; m.runtime_head = eye.runtime_head; m.tracking_epoch = eye.tracking_epoch; m.pose_captured = eye.captured;
    m.runtime_eye_to_head = eye.runtime_eye_to_head;
    vr_camera::Matrix view{};
    require(vr_camera::compose(vr_camera::IDENTITY,eye.relative_eye,1,m.world,view),"pose must compose into game world");
    require(vr_projection::build(*eye.fov,0.1f,1000,m.projection),"eye projection must build");
    return m;
}
void skipped_transfers_recover(Host& gpu) {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;fake->skip_transfer=true;
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    const auto now=std::chrono::steady_clock::now();session.poll(now,gpu.queue.Get());
    auto red=gpu.color(1,0,0),blue=gpu.color(0,0,1);EyeTextureCapture capture;
    for(uint64_t first:{1ULL,3ULL}) {
        const auto left=session.eye({uint32_t(first),first,0},now);
        const auto right=session.eye({uint32_t(first+1),first+1,1},now);
        require(left && right,"a skipped runtime frame must leave tracking active");
        const auto lm=tracked_meta(*left,uint32_t(first)),rm=tracked_meta(*right,uint32_t(first+1));
        check(capture.capture(gpu.device.Get(),gpu.queue.Get(),red.Get(),lm));check(capture.capture(gpu.device.Get(),gpu.queue.Get(),blue.Get(),rm));
        const auto image=capture.latest(1);wait(image->fence.Get(),image->fence_value);
        const auto pair=*capture.pair();const bool accepted=session.submit(pair,gpu.queue.Get(),now);
        require(accepted==(first==3),"skips must stay neutral; later fresh frames must recover");
        require(session.statistics().failed_transfers==0,"runtime visibility skips must not poison the session");
        fake->skip_transfer=false;
    }
    require(session.statistics().submitted_pairs==1 && fake->stops==0,"only the recovered accepted frame may count");
    check(gpu.queue->Signal(gpu.fence.Get(),++gpu.tick));wait(gpu.fence.Get(),gpu.tick);
}
void weapon_controls_session(bool ordinary) {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;fake->controller_sample=VrControllerSample{};
    auto right=vr_camera::IDENTITY,left=right;right[12]=.2f;right[13]=1.3f;right[14]=-.3f;left[12]=.1f;left[13]=1.25f;left[14]=-.6f;
    fake->pose.controllers={left,right};
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);if(ordinary)session.set_render_path(VrSession::RenderPath::ordinary);
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(90)};session.poll(now);VrControllerInput input;input.enable(true);uint64_t frame{};
    const auto step=[&](std::optional<bool> secondary){
        ++frame;const auto time=now+std::chrono::milliseconds(frame*20);
        if(ordinary)require(session.ordinary_frame({uint32_t(frame),frame,0},time).has_value(),"ordinary control fixture frame issued");
        else require(session.eye({uint32_t(frame),frame,int(frame%2)},time).has_value(),"AFR control fixture frame issued");
        session.poll_controllers(input,true,time,0,secondary);return input.snapshot(time).pad.Gamepad;
    };
    step(false);step(false);
    auto& s=*fake->controller_sample;s.parry=1;auto pad=step(false);
    require(pad.bLeftTrigger==255 && !(pad.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER),"real session support grip aims instead of parrying");
    s.parry=0;require(step(false).bLeftTrigger==0,"support release reaches native input");
    s.fire=.8f;require(step(false).bRightTrigger>190,"primary physical right trigger fires");
    require(step(true).bRightTrigger==0,"actual session loadout change re-arms triggers");
    s={};step(true);s.aim=.8f;pad=step(true);
    require(pad.bRightTrigger>190 && !pad.bLeftTrigger,"actual session left trigger fires pistol without zoom");
    s.aim=0;s.fire=.8f;pad=step(true);
    require(!pad.bRightTrigger && !(pad.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER),"right trigger rev reservation cannot fire pistol or melee");
    require(!step(std::nullopt).bRightTrigger,"stale gun hand releases fire in production poll");
    {   // GENERIC_GUARDS piece 5: a weapon the attachment check never recognises stays unknown; after
        // UNKNOWN_LOADOUT the triggers route as for the primary (25 Oct: "couldn't fire the heavy gun").
        s={};for(int n=0;n<110;++n)step(std::nullopt); // 2.2 s, triggers released
        s.fire=.8f;s.aim=.5f;pad=step(std::nullopt);
        require(pad.bRightTrigger>190 && pad.bLeftTrigger>120,"a weapon unknown for 2 s fires with the right trigger and aims with the left");
        s={};step(false);
    }
    {   // Gaze camera steering reaches the native right stick through the gameplay poll only.
        ++frame;const auto time=now+std::chrono::milliseconds(frame*20);
        if(ordinary)require(session.ordinary_frame({uint32_t(frame),frame,0},time).has_value(),"look fixture frame issued");
        else require(session.eye({uint32_t(frame),frame,int(frame%2)},time).has_value(),"look fixture frame issued");
        session.poll_controllers(input,true,time,0,true,std::nullopt,true,std::nullopt,std::array<float,2>{-.5f,.25f});
        const auto look=input.snapshot(time).pad.Gamepad;
        require(look.sThumbRX==-16384 && look.sThumbRY==8192,"first-person gameplay pad carries the native look deflection");
        require(step(true).sThumbRX==0,"without a steering value the native right stick is centred");
    }
    {   // Joe, 31 Oct: the right stick click, held, must reach native equipment (X) through the whole
        // gameplay poll, not just VrControllerInput. He held it 2.7, 2.8 and 2 s at 17:49 to no effect.
        s={};step(true);s.finisher=true;pad=step(true);
        require((pad.wButtons&XINPUT_GAMEPAD_RIGHT_THUMB) && !(pad.wButtons&XINPUT_GAMEPAD_X),"the click reaches the game at once, without equipment");
        int waited=0;while(!((pad=step(true)).wButtons&XINPUT_GAMEPAD_X))require(++waited<40,"a held click reaches native X in play");
        require((waited+1)*20>=480 && (pad.wButtons&XINPUT_GAMEPAD_RIGHT_THUMB),"after about half a second, with the finisher still held");
        for(int i=0;i<30;++i)require(step(true).wButtons&XINPUT_GAMEPAD_X,"X lasts as long as the click");
        s={};require(!(step(true).wButtons&XINPUT_GAMEPAD_X),"letting go ends it");
        s.turn_y=-1;for(int i=0;i<40;++i)require(!(step(true).wButtons&XINPUT_GAMEPAD_X),"right stick down no longer detonates");
        s={};step(true);
    }
    session.enable_holsters(true);s={};
    // The recorded physical waist miss. Both hands are close to a lowered rifle;
    // a simultaneous support/right squeeze must prefer support over sidearm draw.
    right[12]=.273f;right[13]=1.238f;right[14]=.064f;
    left=right;left[14]=-.3f;fake->pose.controllers={left,right};session.enable_grenade_gesture(true);
    step(false);step(false);s.parry=s.melee=1;pad=step(false);
    require(pad.bLeftTrigger==255 && !(pad.wButtons&XINPUT_GAMEPAD_DPAD_DOWN),"same-frame support squeeze cannot acquire the right hip holster");
    s.parry=0;require(!(step(false).wButtons&XINPUT_GAMEPAD_DPAD_DOWN),"letting support go with right grip held cannot switch later");
    s.melee=0;step(false);s.melee=1;
    require(step(false).wButtons&XINPUT_GAMEPAD_DPAD_DOWN,"fresh deliberate waist draw works at recorded height after support releases");
    // Re-arm the holster, then let the previous pulse/cooldown expire.
    session.enable_holsters(true);s={};right[13]=1.6f;right[14]=.1f;fake->pose.controllers={left,right};
    for(int n=0;n<25;++n)step(true);s.fire=.8f;s.melee=1;
    require(!(step(true).wButtons&XINPUT_GAMEPAD_DPAD_DOWN),"rev/shot trigger prevents a new right-grip holster acquisition");
    s.fire=0;require(!(step(true).wButtons&XINPUT_GAMEPAD_DPAD_DOWN),"trigger release cannot complete an inhibited draw");
}
// docs/CONTROL_CUTOUT_2026-10-26.md. 26 Oct 18:30: the stereo pairs paused for 65-160 ms in a
// fight; the input then waited for the whole pad to rest and the trigger routing for both
// triggers to be released, so a held stick and trigger stayed dead for up to 10 s.
void stereo_pause_keeps_controls(bool ordinary) {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;fake->controller_sample=VrControllerSample{};
    auto right=vr_camera::IDENTITY,left=right;right[12]=.2f;right[13]=1.3f;right[14]=-.3f;left[12]=-.2f;left[13]=1.0f;left[14]=-.2f;
    fake->pose.controllers={left,right};
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);if(ordinary)session.set_render_path(VrSession::RenderPath::ordinary);
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(95)};session.poll(now);VrControllerInput input;input.enable(true);uint64_t frame{};
    const auto at=[&](int ms){return now+std::chrono::milliseconds(ms);};
    const auto render=[&](int ms){++frame;
        if(ordinary)require(session.ordinary_frame({uint32_t(frame),frame,0},at(ms)).has_value(),"pause fixture frame issued");
        else require(session.eye({uint32_t(frame),frame,int(frame%2)},at(ms)).has_value(),"pause fixture frame issued");};
    const auto poll=[&](int ms,bool foreground=true){session.poll_controllers(input,foreground,at(ms),0,false);return input.snapshot(at(ms)).pad.Gamepad;};
    for(int ms=0;ms<=100;ms+=20){render(ms);poll(ms);}
    auto& s=*fake->controller_sample;s.move_y=1;s.fire=.9f;
    render(120);auto pad=poll(120);
    require(pad.sThumbLY>30000 && pad.bRightTrigger>220,"walking and firing before the pause");
    for(int ms=140;ms<=280;ms+=20) pad=poll(ms); // no frames: the head goes stale
    require(!pad.sThumbLY && !pad.bRightTrigger,"the pause releases the pad: nothing stale reaches the game");
    render(300);pad=poll(300);
    require(pad.sThumbLY>30000 && pad.bRightTrigger>220,"the first fresh frame carries the held stick and trigger again");
    require(input.snapshot(at(300)).stall_resumes==1,"counted as a resumed stall");
    // The protection kept: the trigger held through a focus change stays released until let go.
    render(320);poll(320,false);
    for(int ms=340;ms<=380;ms+=20){render(ms);pad=poll(ms);}
    require(!pad.bRightTrigger && !pad.sThumbLY,"after focus returns the held trigger and stick wait for release");
    s.fire=0;s.move_y=0;render(400);poll(400);s.fire=.9f;s.move_y=1;render(420);pad=poll(420);
    require(pad.bRightTrigger>220 && pad.sThumbLY>30000,"released and pressed again, they work");
}
void primary_support_belt_ownership_session(bool ordinary) {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;fake->controller_sample=VrControllerSample{};
    auto right=vr_camera::IDENTITY,left=right;right[12]=left[12]=.10f;right[13]=left[13]=1.4f;right[14]=-.10f;left[14]=-.25f;
    fake->pose.controllers={left,right};VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    session.enable_grenade_gesture(true);session.enable_guard(true);
    if(ordinary)session.set_render_path(VrSession::RenderPath::ordinary);
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(180)};session.poll(now);
    VrControllerInput input;input.enable(true);uint64_t frame{};
    const auto step=[&](){
        ++frame;const auto time=now+std::chrono::milliseconds(frame*20);
        fake->pose.controllers={left,right};
        if(ordinary)require(session.ordinary_frame({uint32_t(frame),frame,0},time).has_value(),"support/belt ordinary frame issued");
        else require(session.eye({uint32_t(frame),frame,int(frame%2)},time).has_value(),"support/belt AFR frame issued");
        session.poll_controllers(input,true,time,0,false);return input.snapshot(time).pad.Gamepad;
    };
    step();step();auto& sample=*fake->controller_sample;sample.parry=1;
    require(step().bLeftTrigger==255 && session.support_statistics().held,"primary support first establishes outside grenade belt");
    for(int n=1;n<=10;++n){right[13]=left[13]=1.4f-.025f*n;const auto pad=step();
        require(pad.bLeftTrigger==255 && session.support_statistics().held && !session.grenade_statistics().holding,
            "lowering an established supported rifle into belt retains support until release");
        require(!(pad.wButtons&(XINPUT_GAMEPAD_X|XINPUT_GAMEPAD_LEFT_SHOULDER)),"established support never becomes grenade or guard through waist overlap");
    }
    require(session.grenade_statistics().grabs==0,"held support cannot invent a fresh grenade squeeze");
    sample.parry=.4f;require(step().bLeftTrigger==255,"established support retains grip hysteresis inside overlapping waist zone");
    sample.parry=0;auto pad=step();require(!pad.bLeftTrigger && !session.support_statistics().held && !(pad.wButtons&XINPUT_GAMEPAD_X),"support release in belt releases aim without equipment");
    sample.parry=1;pad=step();
    require(session.grenade_statistics().holding && session.grenade_statistics().grabs==1 && !pad.bLeftTrigger &&
        !(pad.wButtons&(XINPUT_GAMEPAD_X|XINPUT_GAMEPAD_LEFT_SHOULDER)),"fresh belt squeeze after support release deliberately belongs to grenade");
    sample.parry=0;pad=step();
    require(session.grenade_statistics().cancels==1 && !session.grenade_statistics().holding && !(pad.wButtons&XINPUT_GAMEPAD_X),"stationary grenade release still cancels after support handover");
    // 25 Oct: the put-back is one LB tap on release (Reject Mission in the barge), then LB lets go.
    require(pad.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER,"a grenade put back taps native LB as it is let go");
    for(int n=0;n<12;++n)pad=step();
    require(!(pad.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER) && !pad.bLeftTrigger,"the tap ends by itself (200 ms) with the grip open");
    sample.parry=1;pad=step();require(session.grenade_statistics().grabs==2 && !(pad.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER),"the next belt squeeze is a grenade grab again, no LB while held");
}
void sword_attack_modes_session(bool ordinary) {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    fake->controller_sample=VrControllerSample{};fake->pose.controllers={vr_camera::IDENTITY,vr_camera::IDENTITY};
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    if(ordinary)session.set_render_path(VrSession::RenderPath::ordinary);
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(150)};session.poll(now);
    session.enable_swing_melee(true);session.allow_swing_melee(42);
    VrControllerInput input;input.configure(true,true,false);uint64_t frame{};int ms{};
    const auto step=[&](float right_x,std::optional<bool> secondary=true,bool foreground=true) {
        ms+=20;++frame;auto time=now+std::chrono::milliseconds(ms);
        (*fake->pose.controllers[1])[12]=right_x;
        if(ordinary)require(session.ordinary_frame({uint32_t(frame),frame,0},time).has_value(),"sword ordinary pose issued");
        else require(session.eye({uint32_t(frame),frame,int(frame%2)},time).has_value(),"sword AFR pose issued");
        session.poll_controllers(input,foreground,time,0,secondary);
        return input.snapshot(time).pad.Gamepad;
    };
    auto& s=*fake->controller_sample;
    step(0);step(0);step(0);step(.06f);step(.12f);auto p=step(.20f);
    require(p.wButtons==XINPUT_GAMEPAD_RIGHT_SHOULDER,"released-trigger sword motion reaches light action on both render paths");
    s.fire=.8f;p=step(.20f);
    require(p.wButtons==XINPUT_GAMEPAD_RIGHT_SHOULDER && !p.bRightTrigger,"trigger press during a light pulse cannot change action or shoot");
    s={};for(int n=0;n<35;++n)step(.20f);
    s.fire=.8f;for(int n=0;n<15;++n)require(!step(.20f).wButtons,"stationary rev must never attack"); // Held 300 ms: power (VrPowerTrigger::HOLD).
    step(.26f);step(.32f);p=step(.40f);
    require(p.wButtons==XINPUT_GAMEPAD_DPAD_UP && !p.bRightTrigger,"right trigger plus sword swing routes power without firing pistol");
    s.fire=0;p=step(.40f);
    require(p.wButtons==XINPUT_GAMEPAD_DPAD_UP,"trigger release cannot split the committed power pulse");
    require(!step(.40f,true,false).wButtons,"focus loss cancels power immediately");
    s={};for(int n=0;n<35;++n)step(0);
    // Left arm movement and left pistol firing are independent of sword detection.
    for(int n=0;n<5;++n){(*fake->pose.controllers[0])[12]+=.07f;require(!step(0).wButtons,"left arm alone cannot request sword melee");}
    s.aim=.8f;step(.06f);step(.12f);p=step(.20f);
    require(p.wButtons==XINPUT_GAMEPAD_RIGHT_SHOULDER && p.bRightTrigger>190,"left pistol fire remains independent of a light sword swing");
    s={};for(int n=0;n<35;++n)step(0,false);
    step(.06f,false);step(.12f,false);require(!step(.2f,false).wButtons,"primary firearm loadout does not own sword gestures");
    for(int n=0;n<35;++n)step(0,std::nullopt);
    step(.06f,std::nullopt);step(.12f,std::nullopt);require(!step(.2f,std::nullopt).wButtons,"briefly unknown weapon ownership (a switch in progress) cannot request a sword attack");
    // 25 Oct: unknown for 2 s (the battle simulator, gun in neither hand) lets the swing through.
    for(int n=0;n<100;++n)step(0,std::nullopt);
    step(.06f,std::nullopt);step(.12f,std::nullopt);p=step(.2f,std::nullopt);
    require(p.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER,"a loadout unknown for 2 s still attacks with a sword swing");
}
void eye_source_diagnostic() {
    using D=VrEyeSourceDiagnostic;
    const auto same_fov=[](const EngineCameraProbe::VrEye& eye,const vr_projection::Tangents& t){return eye.fov && eye.fov->left==t.left && eye.fov->right==t.right;};
    require(D::parse("swap\r\n")==D::Mode::Swap && D::parse("left")==D::Mode::Left && !D::parse("swapped") && !D::parse(""),"eye source control parses exact modes only");
    require(D::source(D::Mode::Native,1)==1 && D::source(D::Mode::Swap,0)==1 && D::source(D::Mode::Swap,1)==0 && D::source(D::Mode::Right,0)==1,"eye source mapping");
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);session.set_render_path(VrSession::RenderPath::ordinary);
    const auto control=std::filesystem::temp_directory_path()/"sm2vr_eye_source_test.txt";
    const auto write=[&](const char* mode){std::ofstream{control,std::ios::binary|std::ios::trunc}<<mode;};
    write("native");session.set_eye_source_control(control);
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(120)};session.poll(now);
    auto frame=session.ordinary_frame({1,1,0},now);
    require(frame && close(frame->eyes[0].runtime_eye_to_head[12],-.032f) && close(frame->eyes[1].runtime_eye_to_head[12],.032f) && same_fov(frame->eyes[0],fake->pose.fov[0]),"native mode keeps runtime eye order");
    write("swap");const auto later=now+std::chrono::seconds(1);frame=session.ordinary_frame({2,2,0},later);
    require(frame && close(frame->eyes[0].runtime_eye_to_head[12],.032f) && close(frame->eyes[1].runtime_eye_to_head[12],-.032f) &&
        same_fov(frame->eyes[0],fake->pose.fov[1]) && same_fov(frame->eyes[1],fake->pose.fov[0]),"swap feeds each slot the other eye's offset and FOV");
    require(session.statistics().eye_source==D::Mode::Swap && session.statistics().eye_source_since_render_id==2,"applied mode and first render are reported");
    write("left");frame=session.ordinary_frame({3,3,0},later+std::chrono::milliseconds(600));
    require(frame && close(frame->eyes[0].runtime_eye_to_head[12],-.032f) && close(frame->eyes[1].runtime_eye_to_head[12],-.032f+D::COLLAPSE_SEPARATION) &&
        same_fov(frame->eyes[1],fake->pose.fov[0]) && frame->valid_at(later+std::chrono::milliseconds(600)),"collapsed mode keeps a minimal valid separation");
    write("bogus");frame=session.ordinary_frame({4,4,0},later+std::chrono::milliseconds(1200));
    require(frame && session.statistics().eye_source==D::Mode::Left,"invalid control keeps the last valid mode");
    std::filesystem::remove(control);
}
void left_a_button() {
    using namespace std::chrono;VrLeftAButton a;const auto t0=steady_clock::time_point{seconds(50)};
    require(!a.update(true,true,t0).ability && !a.update(true,true,t0+milliseconds(100)).ability,"no ability while Left A is down");
    auto r=a.update(false,true,t0+milliseconds(120));require(r.ability && !r.recenter,"tap release uses the ability");
    require(a.update(false,true,t0+milliseconds(250)).ability && !a.update(false,true,t0+milliseconds(271)).ability,"ability is a bounded 150 ms pulse");
    const auto t1=t0+seconds(5);a.update(true,true,t1);require(!a.update(true,true,t1+milliseconds(999)).recenter,"holding under a second does nothing yet");
    r=a.update(true,true,t1+milliseconds(1000));require(r.recenter && !r.ability,"one-second hold recenters");
    require(!a.update(true,true,t1+milliseconds(1500)).recenter,"recenter happens once per hold");
    r=a.update(false,true,t1+milliseconds(1600));require(!r.ability && !r.recenter,"releasing a recenter hold never uses the ability");
    const auto t2=t1+seconds(5);a.update(true,true,t2);a.update(true,false,t2+milliseconds(50));
    r=a.update(false,true,t2+milliseconds(100));require(!r.ability,"a press interrupted by disarming sends nothing on release");
    a.update(true,false,t2+seconds(1));r=a.update(true,true,t2+seconds(3));require(!r.recenter && !r.ability,"a button already held when arming must be released first");
    // Through the real session poll: a tap reaches the native pad as Y.
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;fake->controller_sample=VrControllerSample{};
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    const auto now=time_point_cast<steady_clock::duration>(steady_clock::time_point{seconds(130)});session.poll(now);VrControllerInput input;input.enable(true);uint64_t frame{};
    const auto step=[&](bool down){++frame;const auto time=now+milliseconds(frame*20);fake->controller_sample->recenter=down;
        require(session.eye({uint32_t(frame),frame,int(frame%2)},time).has_value(),"session fixture frame issued");session.poll_controllers(input,true,time,0,true);return input.snapshot(time).pad.Gamepad.wButtons;};
    step(false);step(false);
    require(!(step(true)&XINPUT_GAMEPAD_Y) && !(step(true)&XINPUT_GAMEPAD_Y),"held Left A sends no Y yet");
    require(step(false)&XINPUT_GAMEPAD_Y,"Left A tap sends native Y (ability)");
    for(int i=0;i<10;++i)step(false);require(!(step(false)&XINPUT_GAMEPAD_Y),"Y releases after the pulse");
}
// Touch (docs/QUEST_STEAM_LINK.md): left X holds native Y while held (the Assault leap, aimed
// abilities); X and Y together held a second recenter, and never pause or open the viewer.
void touch_x_button() {
    using namespace std::chrono;using X=VrTouchXButton;X x;const auto t0=steady_clock::time_point{seconds(50)};
    require(!x.update(true,false,true,t0).ability && !x.update(true,false,true,t0+milliseconds(90)).ability,"X waits for a chord before the ability");
    auto r=x.update(true,false,true,t0+X::CHORD);require(r.ability && !r.chord,"then X holds the ability");
    require(x.update(true,false,true,t0+milliseconds(900)).ability && x.update(true,false,true,t0+milliseconds(1500)).ability,"for as long as X is held, past a second (no recenter)");
    require(!x.update(false,false,true,t0+milliseconds(1520)).ability,"releasing X releases it at once");
    const auto t1=t0+seconds(5);x.update(true,false,true,t1);r=x.update(false,false,true,t1+milliseconds(60));
    require(r.ability,"a tap shorter than the chord window is a pulse on release");
    require(x.update(false,false,true,t1+milliseconds(200)).ability && !x.update(false,false,true,t1+milliseconds(211)).ability,"the pulse lasts 150 ms");
    const auto t6=t1+seconds(2);x.update(true,false,true,t6);x.update(true,false,true,t6+X::CHORD);
    require(x.update(false,false,true,t6+milliseconds(120)).ability && x.update(false,false,true,t6+milliseconds(240)).ability &&
        !x.update(false,false,true,t6+milliseconds(260)).ability,"a hold released just after the window still lasts 150 ms");
    const auto t2=t1+seconds(5);x.update(true,false,true,t2);r=x.update(true,true,true,t2+milliseconds(50));
    require(r.chord && !r.ability,"Y within the window makes a chord");
    require(!x.update(true,true,true,t2+milliseconds(1049)).recenter,"the chord recenters after a second of both");
    r=x.update(true,true,true,t2+milliseconds(1050));require(r.recenter && !r.ability,"one second of both recenters");
    require(!x.update(true,true,true,t2+milliseconds(1500)).recenter && !x.update(true,false,true,t2+milliseconds(1600)).ability,"once, and X after it never uses the ability");
    x.update(false,false,true,t2+milliseconds(1700));
    const auto t3=t2+seconds(5);x.update(false,true,true,t3);r=x.update(true,true,true,t3+milliseconds(400));require(r.chord,"Y first, then X, is a chord too");
    r=x.update(true,false,true,t3+milliseconds(500));require(!r.chord && !r.ability && !r.recenter,"letting go of Y early ends the chord without the ability");
    require(!x.update(false,false,true,t3+milliseconds(600)).ability,"nor on release");
    const auto t4=t3+seconds(5);x.update(true,false,true,t4);x.update(true,false,true,t4+milliseconds(200));
    r=x.update(true,true,true,t4+milliseconds(300));require(r.ability && !r.chord,"Y pressed after X is already holding the ability is not a chord");
    x.update(false,false,true,t4+milliseconds(400));
    const auto t5=t4+seconds(5);x.update(true,false,true,t5);x.update(true,false,false,t5+milliseconds(50));
    r=x.update(true,false,true,t5+milliseconds(500));require(!r.ability,"an X held through a disarm waits for release");
    require(!x.update(false,false,true,t5+milliseconds(600)).ability,"and its release sends nothing");
    // Through the real session poll.
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;fake->controller_sample=VrControllerSample{};fake->controller_sample->touch_layout=true;
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    const auto now=time_point_cast<steady_clock::duration>(steady_clock::time_point{seconds(130)});session.poll(now);VrControllerInput input;input.enable(true);uint64_t frame{};
    const auto step=[&](bool x_down,bool y_down){++frame;const auto time=now+milliseconds(frame*20);fake->controller_sample->recenter=x_down;fake->controller_sample->menu=y_down;
        require(session.eye({uint32_t(frame),frame,int(frame%2)},time).has_value(),"session fixture frame issued");session.poll_controllers(input,true,time,0,true);return input.snapshot(time).pad.Gamepad.wButtons;};
    step(false,false);step(false,false);
    for(int i=0;i<5;++i)step(true,false);
    for(int i=0;i<60;++i)require(step(true,false)&XINPUT_GAMEPAD_Y,"Touch X held is native Y held, past a second, with no recenter");
    require(!(step(false,false)&XINPUT_GAMEPAD_Y),"releasing X releases Y");
    for(int i=0;i<10;++i)step(false,false);
    const auto epoch=session.epoch();bool quiet=true;
    for(int i=0;i<45;++i)quiet=!(step(true,true)&(XINPUT_GAMEPAD_Y|XINPUT_GAMEPAD_START)) && quiet;
    require(quiet && session.epoch()==epoch,"X and Y together send nothing before a second");
    for(int i=0;i<10;++i)quiet=!(step(true,true)&(XINPUT_GAMEPAD_Y|XINPUT_GAMEPAD_START)) && quiet;
    require(quiet && session.epoch()>epoch,"a second of X and Y recenters without the ability or pause");
    for(int i=0;i<10;++i)quiet=!(step(false,false)&(XINPUT_GAMEPAD_Y|XINPUT_GAMEPAD_START)) && quiet;
    require(quiet,"releasing the chord sends neither the ability nor pause");
}
// Game rumble (docs/HAPTICS.md) through the real controller poll: off until enabled, played
// while foreground, stopped on focus loss, runtime refusals counted.
void rumble_session() {
    using namespace std::chrono;using Op=vr_rumble::Command::Op;
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    VrSession session{std::move(backend)};session.enable(true);
    const auto now=time_point_cast<steady_clock::duration>(steady_clock::time_point{seconds(200)});session.poll(now);
    VrControllerInput input;input.enable(true);vr_rumble::Mailbox mailbox;
    mailbox.publish({65535,0},now);session.poll_controllers(input,true,now,0,true);
    require(fake->rumbles.empty() && !session.rumble_statistics().enabled,"rumble stays off until enabled");
    require(!session.enable_rumble(true,{3,1},&mailbox) && !session.enable_rumble(true,{},nullptr),"bad gain or no source is refused");
    require(session.enable_rumble(true,{.5f,0},&mailbox),"rumble enables");
    session.poll_controllers(input,true,now,0,true);
    require(fake->rumbles.size()==1 && fake->rumbles[0][0].op==Op::apply && close(fake->rumbles[0][0].amplitude,.5f) && fake->rumbles[0][1].op==Op::none,
        "the low motor at half gain plays on the left hand only (cross 0)");
    session.poll_controllers(input,false,now+milliseconds(10),0,true);
    require(fake->rumbles.size()==2 && fake->rumbles[1][0].op==Op::stop,"losing the foreground stops the controllers");
    session.poll_controllers(input,false,now+milliseconds(20),0,true);
    require(fake->rumbles.size()==2,"nothing is sent while stopped in the background");
    fake->rumble_ok=false;session.poll_controllers(input,true,now+milliseconds(30),0,true);
    const auto s=session.rumble_statistics();
    require(s.enabled && s.game_calls==1 && s.applies==2 && s.stops==1 && s.failures==1 && close(s.peak,.5f),"statistics count plays, stops and runtime refusals");
    {
        // 27 Oct headset A/B (docs/HAPTICS.md): one strong test pulse once the runtime takes it, and
        // an explicit frequency on every pulse.
        auto test_backend=std::make_unique<Backend>();auto* test=test_backend.get();test->present=true;
        VrSession tested{std::move(test_backend)};tested.enable(true);tested.poll(now);
        vr_rumble::Mailbox quiet;vr_rumble::Settings settings;settings.frequency=160;settings.test_pulse=true;
        require(!tested.enable_rumble(true,{1,1,2000},&quiet),"a frequency outside 1-1000 Hz is refused");
        require(tested.enable_rumble(true,settings,&quiet),"test pulse and frequency enable");
        test->rumble_ok=false;tested.poll_controllers(input,true,now,0,true);
        require(test->rumbles.size()==1 && tested.rumble_statistics().test_pulses==0,"a refused test pulse is retried, not counted");
        test->rumble_ok=true;tested.poll_controllers(input,true,now+milliseconds(10),0,true);
        const auto& pulse=test->rumbles.back();
        require(test->rumbles.size()==2 && pulse[0].op==Op::apply && pulse[1].op==Op::apply && close(pulse[0].amplitude,1) &&
            pulse[0].duration==milliseconds(500) && close(pulse[1].frequency,160),"the test pulse is half a second, full strength, both hands");
        tested.poll_controllers(input,true,now+milliseconds(20),0,true);
        require(test->rumbles.size()==2 && tested.rumble_statistics().test_pulses==1,"and is sent once");
        quiet.publish({65535,0},now+milliseconds(30));tested.poll_controllers(input,true,now+milliseconds(30),0,true);
        require(test->rumbles.size()==3 && close(test->rumbles.back()[0].frequency,160) && test->rumbles.back()[0].duration==vr_rumble::PULSE,
            "game rumble carries the chosen frequency at the normal pulse length");
    }
}
// Viewer mode through the real session poll: Left B hold, Start, X on the pause menu, photo
// flags, free-camera eyes, stick and two-grip changes, then a tap leaves via B and Start.
void viewer_mode_session() {
    using namespace std::chrono;using Seq=VrViewerSequencer;
    const auto within=[](float a,float b,float tolerance=1e-3f){return std::abs(a-b)<=tolerance;};
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;fake->controller_sample=VrControllerSample{};
    auto left=vr_camera::IDENTITY,right=left;left[12]=-.2f;left[13]=1.2f;left[14]=-.4f;right[12]=.2f;right[13]=1.2f;right[14]=-.4f;
    fake->pose.controllers={left,right};
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    require(session.set_render_path(VrSession::RenderPath::ordinary),"ordinary path for the viewer fixture");
    const auto base=time_point_cast<steady_clock::duration>(steady_clock::time_point{seconds(400)});session.poll(base);
    VrControllerInput input;input.enable(true);uint64_t frame{};std::optional<VrSession::OrdinaryFrame> last;
    // render: the game issues an ordinary stereo job before the poll (gameplay, or the viewer's own frames).
    const auto step=[&](std::optional<uint32_t> flags,bool render,bool native=true){++frame;const auto time=base+milliseconds(frame*20);
        if(render){last=session.ordinary_frame({uint32_t(frame),frame,0},time);require(last.has_value(),"viewer fixture frame issued");}
        session.poll_controllers(input,true,time,0,true,flags,native);return input.snapshot(time).pad.Gamepad.wButtons;};
    auto& s=*fake->controller_sample;
    step(0,true);step(0,true);
    s.menu=true;for(int i=0;i<55;++i)require(!(step(0,true)&XINPUT_GAMEPAD_START),"held Left B sends no Start");
    require(session.viewer_status().refusals==1 && session.viewer_status().state==Seq::State::off,"a hold with no first-person anchor yet is refused");
    s.menu=false;require(!(step(0,true)&XINPUT_GAMEPAD_START),"releasing a hold never pauses");
    // 31 Oct: a brushed Left B (a 20 ms or 60 ms press) never pauses.
    s.menu=true;step(0,true);s.menu=false;
    {bool started=false;for(int i=0;i<10;++i)started=(step(0,true)&XINPUT_GAMEPAD_START) || started;require(!started,"a brushed Left B never pauses");}
    s.menu=true;for(int i=0;i<3;++i)step(0,true);s.menu=false;
    {bool started=false;for(int i=0;i<10;++i)started=(step(0,true)&XINPUT_GAMEPAD_START) || started;require(!started,"a 60 ms brush never pauses");}
    // 31 Oct 16:16:05 and :07: plain taps of 138 and 133 ms did nothing under the old 150 ms rule.
    s.menu=true;for(int i=0;i<5;++i)step(0,true);s.menu=false;require(step(0,true)&XINPUT_GAMEPAD_START,"a plain 100 ms Left B tap sends native Start on release");
    for(int i=0;i<10;++i)step(0,true);
    require(!(step(0,true)&XINPUT_GAMEPAD_START),"Start is a bounded pulse");
    // 30 Oct 20:45:40: one thumb on Left A and Left B, a pause mid-fight. The ability goes, the pause doesn't.
    s.menu=true;step(0,true);s.recenter=true;for(int i=0;i<5;++i)step(0,true);s.menu=s.recenter=false;
    {bool started=false,ability=false;for(int i=0;i<10;++i){const auto pad=step(0,true);started=(pad&XINPUT_GAMEPAD_START) || started;ability=(pad&XINPUT_GAMEPAD_Y) || ability;}
     require(!started && ability,"a Left B press that also catches Left A is the ability, not a pause");}
    // 24 Oct: a stereo cutscene (0x810) skips on native B held. Right B is native B there and in play.
    s.interact=true;require((step(0x810,true)&(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_X))==XINPUT_GAMEPAD_B,"right B in a cutscene is native B");
    require((step(0x810,true)&(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_X))==XINPUT_GAMEPAD_B,"held through the cutscene, as the skip hold needs");
    require((step(0,true)&(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_X))==XINPUT_GAMEPAD_B,"right B is interact (B) in gameplay");
    s.interact=false;step(0,true);
    // 31 Oct: the held stick click detonates in play only.
    s.finisher=true;for(int i=0;i<40;++i)require(!(step(0x810,true)&XINPUT_GAMEPAD_X),"a held click in a cutscene is never native X");
    require(!(step(0,true)&XINPUT_GAMEPAD_X),"a click held out of a cutscene starts its hold there");
    s.finisher=false;step(0,true);
    auto first_person=vr_camera::IDENTITY;first_person[12]=10;first_person[13]=5;first_person[14]=-3;
    const auto noted=last->eyes[0];session.note_first_person_anchor(first_person,noted);
    s.menu=true;int held{};
    while(!session.viewer_engaged())require(!(step(0,true,false)&XINPUT_GAMEPAD_START) && ++held<60,"a one-second hold starts the viewer");
    require(held>=45 && session.viewer_status().state==Seq::State::releasing,"after a full second the viewer first releases the character");
    for(int i=0;i<5;++i)require(!(step(0,true,false)&XINPUT_GAMEPAD_START),"no pause until the game has republished its own pose");
    require(step(0,true,true)&XINPUT_GAMEPAD_START,"the released character is paused with Start");
    require(!session.viewer_owns_camera() && session.viewer_status().unreleased==0,"Start comes before the free camera, after a confirmed release");
    s.menu=false;require(step(0,false)&XINPUT_GAMEPAD_START,"the Start pulse continues while the game pauses");
    uint16_t buttons{};int waited{};
    while(!((buttons=step(0x908,false))&XINPUT_GAMEPAD_X))require(++waited<30,"X follows the pause menu");
    require(!(buttons&XINPUT_GAMEPAD_START),"X never overlaps Start");
    for(int i=0;i<40;++i)require(step(0x908,false)&XINPUT_GAMEPAD_X,"X stays held on the pause menu");
    require(!step(0x108,false) && session.viewer_owns_camera() && session.viewer_status().state==Seq::State::active,"photo flags hand the camera to the viewer; the pad goes neutral");
    step(0x108,true);
    const auto calibration=session.calibration();
    const auto expected=vr_viewer::entry_anchor(first_person,noted.relative_head,calibration.height_offset_metres,calibration.head_forward_offset_metres,noted.units_per_metre);
    require(expected && last->eyes[0].viewer_anchor && *last->eyes[0].viewer_anchor==*expected && last->eyes[1].viewer_anchor==last->eyes[0].viewer_anchor &&
        last->eyes[0].units_per_metre==noted.units_per_metre,"viewer eyes start from the noted first-person view");
    require(!last->controllers && !last->input && !session.controller_poses(base+milliseconds(frame*20)),"viewer frames carry no hands or game input");
    s.move_y=1;for(int i=0;i<10;++i)require(!step(0x108,true),"the game sees a neutral pad while flying");
    s.move_y=0;step(0x108,true);
    const auto slid=*last->eyes[0].viewer_anchor;const auto forward=vr_viewer::direction(*expected,{0,0,-1});
    const vr_viewer::Vec moved{slid[12]-(*expected)[12],slid[13]-(*expected)[13],slid[14]-(*expected)[14]};
    require(within(vr_viewer::length(moved),2.5f*.2f*noted.units_per_metre,1e-2f) && moved[0]*forward[0]+moved[2]*forward[2]>0,"the left stick slides the viewer forward");
    {   // The render thread can stamp tracking just after the present thread's poll time.
        s.move_y=1;++frame;const auto time=base+milliseconds(frame*20);
        last=session.ordinary_frame({uint32_t(frame),frame,0},time+milliseconds(5));
        const auto before=session.viewer_status().rig.anchor;session.poll_controllers(input,true,time,0,true,0x108);
        require(session.viewer_status().rig.anchor!=before,"tracking stamped just after the poll still flies the viewer");
        s.move_y=0;
    }
    s.parry=s.melee=1;step(0x108,true);
    right[12]=.6f;fake->pose.controllers={left,right};step(0x108,true);step(0x108,true);
    require(within(last->eyes[0].units_per_metre,noted.units_per_metre*.5f),"both grips: hands twice as far apart make the world twice as big");
    require(session.viewer_status().rig.grabs==1,"one two-hand grab");
    s.parry=s.melee=0;step(0x108,true);
    // Back into the viewer from first person: the Left B hold, the pause, X on the pause menu.
    const auto enter_again=[&]{
        session.note_first_person_anchor(first_person,last->eyes[0]);
        s.menu=true;held=0;while(!session.viewer_engaged())require(++held<60,"hold to enter again"),step(0,true,false);
        step(0,true,true);s.menu=false;
        waited=0;while(!(step(0x908,false)&XINPUT_GAMEPAD_X))require(++waited<30,"X");
        step(0x108,true);require(session.viewer_status().state==Seq::State::active,"in the viewer again");
    };
    {   // Right A (Joe, 30 Oct): the game plays out under the free camera. 31 Oct: when the animation
        // ends the player is back in the game; only Right A freezes it again.
        const auto next=[&]{return base+milliseconds((frame+1)*20);}; // the time of the next step
        // The character's root and HEAD from its native publications (before each poll, as Sm2Probe does).
        float x{};const auto publish=[&](float step_units){x+=step_units;vr_viewer::Motion::Sample m;m.time=next();m.actor=7;
            m.points={vr_viewer::Vec{x,0,0},vr_viewer::Vec{x,2,0},std::nullopt,std::nullopt};session.note_character_motion(m);};
        const auto play=[&]{
            s.confirm=true;require(step(0x108,true)&XINPUT_GAMEPAD_B,"Right A presses B to let the game run");s.confirm=false;
            waited=0;while(session.viewer_status().state!=Seq::State::playing) {step(0,true);require(++waited<30,"gameplay: playing");}
        };
        // Everything a normal exit restores: nothing pressed (nothing is paused), first person, armed.
        const auto in_the_game=[&](vr_viewer::PlayEnd end){
            const auto status=session.viewer_status();
            require(status.state==Seq::State::off && !session.viewer_engaged() && !session.viewer_owns_camera() && !session.viewer_playing(),"the play ended the viewer");
            require(status.play_end==end,("why the play ended: "+std::string{vr_viewer::name(status.play_end)}).c_str());
            require(vr_viewer::pose_edits(true,status.state,true)==vr_viewer::PoseEdits{true,true},"the arms and head are first person's again");
            for(int i=0;i<15;++i)require(!(step(0,true)&(XINPUT_GAMEPAD_START|XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_X)),"no pause, photo or menu press: the game already runs");
            require(!last->eyes[0].viewer_anchor && last->controllers && input.snapshot(base+milliseconds(frame*20)).armed,"first person returns with hands, controls armed");
        };
        const auto frozen=*last->eyes[0].viewer_anchor;
        s.confirm=true;require(step(0x108,true)&XINPUT_GAMEPAD_B,"Right A presses B to let the game run");
        require(session.viewer_owns_camera() && session.viewer_playing(),"the free camera stays while the game is let go");
        s.confirm=false;waited=0;
        while(session.viewer_status().state!=Seq::State::playing) {step(0,true);require(++waited<30,"gameplay: playing");}
        require(session.viewer_owns_camera() && last->eyes[0].viewer_anchor && *last->eyes[0].viewer_anchor==frozen && !last->input,"playing: the viewer still draws the eyes, where it was");
        for(int i=0;i<10;++i) {publish(.5f);require(!step(0,true),"the game sees a neutral pad while it plays");}
        int still_steps{};
        while(session.viewer_status().state==Seq::State::playing) {publish(0);require(!step(0,true),"nothing pressed as the animation settles");require(++still_steps<40,"the settled character ends the play");}
        require(still_steps>=24,("half a second after the character settles: "+std::to_string(still_steps)).c_str());
        require(session.viewer_status().plays==1 && session.viewer_status().refreezes==0,"the play ended in the game, no freeze");
        require(session.viewer_status().play_seconds>.6f && session.viewer_status().play_seconds<1.2f,"the play ran its length");
        in_the_game(vr_viewer::PlayEnd::settled);
        s.confirm=true;require(step(0,true)&XINPUT_GAMEPAD_A,"Right A is the game's dodge again");s.confirm=false;step(0,true);
        // Right A while playing freezes at once: Start, then X, as on entry.
        enter_again();play();
        step(0,true);s.confirm=true;require(step(0,true)&XINPUT_GAMEPAD_START,"Right A again: freeze now");s.confirm=false;
        require(session.viewer_status().play_end==vr_viewer::PlayEnd::cancelled && session.viewer_owns_camera(),"cancelled, the free camera stays");
        waited=0;while(!(step(0x908,true)&XINPUT_GAMEPAD_X)) require(++waited<30,"X");
        step(0x108,true);require(session.viewer_status().state==Seq::State::active && session.viewer_status().plays==2 && session.viewer_status().refreezes==1,"frozen after the cancel");
        // Played on from that freeze it ends in the game too: nothing moved, so after clip_seconds.
        // B landing on the pause menu is closed on the way in. Right A and the stick still held
        // at the end reach the game only after a release: no stray dodge, no walk.
        auto quick=vr_viewer::Play{};quick.values.values[vr_viewer::Play::CLIP_SECONDS]=.5f;quick.values.values[vr_viewer::Play::LIMIT_SECONDS]=1;session.set_viewer_play(quick);
        s.confirm=true;require(step(0x108,true)&XINPUT_GAMEPAD_B,"play on");
        waited=0;while(!(step(0x908,true)&XINPUT_GAMEPAD_START)) require(++waited<30,"Start closes the pause menu on the way");
        waited=0;while(session.viewer_status().state!=Seq::State::playing) {step(0,true);require(++waited<30,"then playing");}
        s.move_y=1;still_steps=0;
        while(session.viewer_status().state==Seq::State::playing) {require(!step(0,true),"neutral pad");require(++still_steps<35,"clip_seconds ends a play that never moves");}
        require(still_steps>=23,"not before clip_seconds");
        in_the_game(vr_viewer::PlayEnd::clip);
        {const auto pad=input.snapshot(base+milliseconds(frame*20)).pad.Gamepad;require(!pad.wButtons && !pad.sThumbLY,"held controls wait for their release");}
        s.confirm=false;s.move_y=0;step(0,true);
        // A Marine that never settles: limit_seconds hands the game back too.
        enter_again();play();still_steps=0;
        while(session.viewer_status().state==Seq::State::playing) {publish(.5f);require(!step(0,true),"neutral pad");require(++still_steps<60,"limit_seconds caps a play");}
        require(still_steps>=48,"not before limit_seconds");
        in_the_game(vr_viewer::PlayEnd::limit);
        // A Left B press under way as the play ends: its release in the game must not pause.
        enter_again();play();
        for(int i=0;i<5;++i) {publish(.5f);step(0,true);}
        for(int i=0;i<8;++i) {publish(0);step(0,true);}
        s.menu=true;still_steps=0;
        while(session.viewer_status().state==Seq::State::playing) {publish(0);step(0,true);require(++still_steps<30,"it settles with Left B down");}
        require(still_steps>=8,"Left B long enough to count as a tap");
        s.menu=false;
        in_the_game(vr_viewer::PlayEnd::settled);
        require(!session.viewer_engaged(),"and it does not re-enter the viewer");
        // Hold mode: letting go of Right A is the player's own freeze.
        enter_again();
        auto hold=vr_viewer::Play{};hold.values.values[vr_viewer::Play::MODE]=3;session.set_viewer_play(hold);
        s.confirm=true;step(0x108,true);
        waited=0;while(session.viewer_status().state!=Seq::State::playing) {step(0,true);require(++waited<30,"hold: playing");}
        step(0,true);s.confirm=false;require(step(0,true)&XINPUT_GAMEPAD_START,"hold: letting go freezes");
        require(session.viewer_status().play_end==vr_viewer::PlayEnd::released,"released");
        waited=0;while(!(step(0x908,true)&XINPUT_GAMEPAD_X)) require(++waited<30,"X");
        step(0x108,true);require(session.viewer_status().state==Seq::State::active,"frozen");
        // Switched off in the settings: Right A does nothing.
        auto off=session.viewer_play();off.values.values[vr_viewer::Play::MODE]=0;session.set_viewer_play(off);
        s.confirm=true;require(!step(0x108,true) && session.viewer_status().state==Seq::State::active,"mode off: no play");s.confirm=false;step(0x108,true);
        session.set_viewer_play(vr_viewer::Play{});
    }
    s.finisher=true;for(int i=0;i<40;++i)require(!(step(0x108,true)&XINPUT_GAMEPAD_X),"a held stick click in the viewer is never native X");
    s.finisher=false;step(0x108,true);
    s.menu=true;step(0x108,true);s.menu=false;
    require(step(0x108,true)&XINPUT_GAMEPAD_B,"a Left B tap in the viewer presses B to leave photo mode");
    require(!session.viewer_owns_camera(),"leaving hands the camera back at once");
    waited=0;while(!((buttons=step(0x908,false))&XINPUT_GAMEPAD_START))require(++waited<30,"Start closes the pause menu");
    require(!(buttons&XINPUT_GAMEPAD_B),"Start never overlaps B");
    for(int i=0;i<15;++i)step(0,false);
    require(session.viewer_status().state==Seq::State::off && !session.viewer_engaged(),"back in gameplay the viewer is off");
    step(0,true);require(!last->eyes[0].viewer_anchor && last->controllers,"first-person frames return with hands");
    {   // Left B while the game plays: out of the viewer at once, no Start (the game already runs).
        enter_again();
        s.confirm=true;step(0x108,true);s.confirm=false;
        waited=0;while(session.viewer_status().state!=Seq::State::playing) {step(0,true);require(++waited<30,"playing");}
        s.menu=true;require(!(step(0,true)&XINPUT_GAMEPAD_START),"Left B down");s.menu=false;
        require(!(step(0,true)&XINPUT_GAMEPAD_START),"a tap sends no Start");
        require(!session.viewer_engaged() && !session.viewer_owns_camera() && session.viewer_status().play_end==vr_viewer::PlayEnd::left,"the viewer ends; the game keeps running");
        step(0,true);require(!last->eyes[0].viewer_anchor && last->controllers,"first person returns at once");
    }
}
void menu_only_submission(Host& gpu) {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    require(session.set_render_path(VrSession::RenderPath::ordinary),"ordinary path for menu fixture");
    const auto now=std::chrono::steady_clock::now();session.poll(now,gpu.queue.Get());
    auto eyes=session.menu_eyes(now);
    require(eyes && (*eyes)[0].eye==0 && (*eyes)[1].eye==1 && (*eyes)[0].screen_menu && (*eyes)[1].tracked_stereo &&
        (*eyes)[0].runtime_eye_to_head[12]==fake->pose.eyes[0][12] && (*eyes)[1].projection[0]>0,"menu eyes use current tracking per eye");
    auto blue=gpu.color(0,0,1,32);std::array<EyeTextureCapture,2> copies;
    const auto capture=[&](uint64_t id,std::chrono::steady_clock::time_point when,bool menu){auto e=*session.menu_eyes(when);std::array<EyeTextureCapture::Image,2> out;
        for(int i=0;i<2;++i){e[i].render_id=id;e[i].pose_captured=when;e[i].screen_menu=menu;check(copies[i].capture(gpu.device.Get(),gpu.queue.Get(),blue.Get(),e[i]));out[i]=*copies[i].latest(i);wait(out[i].fence.Get(),out[i].fence_value);}
        return EyeTextureCapture::Pair{out[0],out[1]};};
    require(session.submit_menu(capture(1000,now,true),gpu.queue.Get(),now) && fake->submissions==1,"completed menu pair is submitted head-locked");
    require(!session.submit_menu(capture(1001,now,false),gpu.queue.Get(),now),"non-menu images cannot use the menu route");
    require(!session.submit_menu(capture(1002,now,true),gpu.queue.Get(),now+std::chrono::milliseconds(101)),"stale menu pose is refused");
    {   // While only the menu panel is live, menu buttons reach the game; gestures and turning do not.
        VrControllerInput input;input.enable(true);fake->controller_sample=VrControllerSample{};
        session.poll_controllers(input,true,now+std::chrono::milliseconds(10),0,true);
        auto& s=*fake->controller_sample;s.menu=s.confirm=true;s.grenade=1;s.swing_melee=true;s.guard=true;s.turn_x=1;
        // Gaze camera steering offered while the menu is up must not scroll or move the menu.
        session.poll_controllers(input,true,now+std::chrono::milliseconds(20),0,true,std::nullopt,true,std::nullopt,std::array<float,2>{1,1});
        const auto pad=input.snapshot(now+std::chrono::milliseconds(20)).pad.Gamepad;
        require(!(pad.wButtons&(XINPUT_GAMEPAD_START|XINPUT_GAMEPAD_Y)) && (pad.wButtons&XINPUT_GAMEPAD_A) && (pad.wButtons&XINPUT_GAMEPAD_B),"pause menu receives confirm and back; Left B waits for its release");
        require(!(pad.wButtons&(XINPUT_GAMEPAD_RIGHT_SHOULDER|XINPUT_GAMEPAD_LEFT_SHOULDER)),"no sword or guard while in the menu");
        require(pad.sThumbRX==32767 && !input.snapshot(now+std::chrono::milliseconds(20)).snaps,"the right stick is the native right stick on a menu (not the gaze look), and never snap turns");
        require(!pad.sThumbLX && !pad.sThumbLY,"Right A on the menu is a plain A: no neutral-dodge stick moves the cursor");
        s.menu=false;session.poll_controllers(input,true,now+std::chrono::milliseconds(40),0,true);
        const auto released=input.snapshot(now+std::chrono::milliseconds(40)).pad.Gamepad.wButtons;
        require((released&XINPUT_GAMEPAD_Y) && !(released&XINPUT_GAMEPAD_START),"a Left B tap on a menu is native Y on release, not Start (25 Oct: A/B/X/Y on the face buttons)");
        require(!(released&XINPUT_GAMEPAD_X),"no X without Left A");
        s.recenter=true;session.poll_controllers(input,true,now+std::chrono::milliseconds(60),0,true);
        require(input.snapshot(now+std::chrono::milliseconds(60)).pad.Gamepad.wButtons&XINPUT_GAMEPAD_X,"Left A held on a menu is native X");
        s.recenter=false;
        require(!input.snapshot(now+std::chrono::milliseconds(40)).pad.Gamepad.sThumbLY,"A still held on the menu: no delayed dodge direction");
        session.poll_controllers(input,true,now+std::chrono::milliseconds(400),0,true);
        require(!input.snapshot(now+std::chrono::milliseconds(400)).pad.Gamepad.wButtons,"menu input stops once menu frames stop");
        fake->controller_sample.reset();
    }
    const auto frame=session.ordinary_frame({17,23,0},now);require(frame.has_value(),"ordinary producer resumes");
    auto red=gpu.color(1,0,0);std::array<EyeTextureCapture,2> ordinary;
    for(int eye=0;eye<2;++eye){check(ordinary[eye].capture(gpu.device.Get(),gpu.queue.Get(),red.Get(),tracked_meta(frame->eyes[eye],17)));const auto image=*ordinary[eye].latest(eye);wait(image.fence.Get(),image.fence_value);}
    require(session.submit_ordinary({{*ordinary[0].latest(0),*ordinary[1].latest(1)},99},gpu.queue.Get(),now) && fake->submissions==2,"menu frames do not advance the ordinary sequence, so resumed play submits");
}
// A full-screen menu over a rendered world (barge Datavault, mission select) rides on the stereo
// pairs (VrScreenMenu.h): while the pairs carry the panel, controller input is menu input.
void screen_menu_over_stereo(Host& gpu) {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    require(session.set_render_path(VrSession::RenderPath::ordinary),"ordinary path for the screen menu fixture");
    const auto now=std::chrono::steady_clock::now();session.poll(now,gpu.queue.Get());
    VrControllerInput input;input.enable(true);fake->controller_sample=VrControllerSample{};
    auto red=gpu.color(1,0,0,32);std::array<EyeTextureCapture,2> eyes;
    const auto submit=[&](uint32_t token,std::chrono::steady_clock::time_point when,bool menu){
        const auto frame=session.ordinary_frame({token,token+100,0},when);require(frame.has_value(),"ordinary pose issued");
        for(int eye=0;eye<2;++eye){auto m=tracked_meta(frame->eyes[eye],token);m.screen_menu=menu;
            check(eyes[eye].capture(gpu.device.Get(),gpu.queue.Get(),red.Get(),m));const auto image=*eyes[eye].latest(eye);wait(image.fence.Get(),image.fence_value);}
        return session.submit_ordinary({{*eyes[0].latest(0),*eyes[1].latest(1)},token},gpu.queue.Get(),when);};
    const auto pad=[&](int ms){session.poll_controllers(input,true,now+std::chrono::milliseconds(ms),0,true);return input.snapshot(now+std::chrono::milliseconds(ms)).pad.Gamepad;};
    auto& s=*fake->controller_sample;
    require(submit(17,now,true),"a stereo pair carrying the menu panel is submitted");
    pad(5); // Input arms on neutral controls.
    s.confirm=true;s.interact=true;
    auto p=pad(10);
    require((p.wButtons&XINPUT_GAMEPAD_A) && (p.wButtons&XINPUT_GAMEPAD_B) && !(p.wButtons&XINPUT_GAMEPAD_X),"menu over stereo: A confirms and right B is native B (back), not interact");
    require(!p.sThumbLX && !p.sThumbLY,"A on the menu is a plain A: no neutral-dodge stick moves the cursor");
    s.confirm=s.interact=false;pad(20);
    s.turn_x=1;s.turn_y=-.5f;p=pad(30);
    require(p.sThumbRX==32767 && p.sThumbRY==-16384 && !input.snapshot(now+std::chrono::milliseconds(30)).snaps,"both right-stick axes reach the game on a menu; no snap turn");
    s.turn_x=0;s.turn_y=0;pad(35);
    require(submit(18,now+std::chrono::milliseconds(40),false),"a plain gameplay pair follows the menu");
    pad(45); // Leaving the menu re-arms on a neutral sample; a control held across it stays blocked.
    // In play the sample is the one the frame was issued with, so the press goes in before the pair.
    s.grenade=1;require(submit(19,now+std::chrono::milliseconds(50),false),"a gameplay pair issued with the trackpad pressed");
    p=pad(55);
    require((p.wButtons&XINPUT_GAMEPAD_X) && !(p.wButtons&XINPUT_GAMEPAD_B),"back in gameplay the right trackpad is native equipment (X), not back");
    s.grenade=0;
    check(gpu.queue->Signal(gpu.fence.Get(),++gpu.tick));wait(gpu.fence.Get(),gpu.tick);
}
// 29 Oct 08:19 and 30 Oct 20:45 (docs/RAVENER_ATTACK_2026-10-30.md): a pause menu opened mid-fight
// and "the buttons are always scrolling", but the trace had no input record while a menu was up,
// so what the menu was sent could not be read back. The menu branch is traced now, with raw Left A/B.
// Joe, 31 Oct: the button that opened the pause menu closes it. On the pause menu (0x908) a Left B tap
// is Start; on any other menu it stays native Y.
void pause_menu_left_b(Host& gpu) {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    require(session.set_render_path(VrSession::RenderPath::ordinary),"ordinary path for the pause menu fixture");
    const auto now=std::chrono::steady_clock::now();session.poll(now,gpu.queue.Get());
    VrControllerInput input;input.enable(true);fake->controller_sample=VrControllerSample{};
    auto red=gpu.color(1,0,0,32);std::array<EyeTextureCapture,2> eyes;
    const auto frame=session.ordinary_frame({41,141,0},now);require(frame.has_value(),"ordinary pose issued");
    for(int eye=0;eye<2;++eye){auto m=tracked_meta(frame->eyes[eye],41);m.screen_menu=true;
        check(eyes[eye].capture(gpu.device.Get(),gpu.queue.Get(),red.Get(),m));const auto image=*eyes[eye].latest(eye);wait(image.fence.Get(),image.fence_value);}
    require(session.submit_ordinary({{*eyes[0].latest(0),*eyes[1].latest(1)},41},gpu.queue.Get(),now),"a pair carrying the menu panel is submitted");
    const auto buttons=[&](int ms,std::optional<uint32_t> flags){const auto time=now+std::chrono::milliseconds(ms);session.poll_controllers(input,true,time,0,true,flags);return input.snapshot(time).pad.Gamepad.wButtons;};
    auto& s=*fake->controller_sample;
    buttons(5,0x908u);s.menu=true;
    require(!(buttons(20,0x908u)&(XINPUT_GAMEPAD_START|XINPUT_GAMEPAD_Y)),"Left B down on the pause menu waits for its release");
    s.menu=false;const auto closing=buttons(40,0x908u);
    require((closing&XINPUT_GAMEPAD_START) && !(closing&XINPUT_GAMEPAD_Y),"a Left B tap on the pause menu is Start, which closes it");
    const auto closed=buttons(50,0u);
    require((closed&XINPUT_GAMEPAD_START) && !(closed&XINPUT_GAMEPAD_Y),"the menu closing inside the pulse doesn't turn it into the class ability");
    // Other menus keep Y: menu_only_submission's "a Left B tap on a menu is native Y" (no pause flags).
    // 31 Oct 20:51:20: Edit Squad is Y on this menu and Left B closes it, so Y is left trackpad up.
    s.utility_press=1;s.utility_y=1;
    require(buttons(60,0x908u)&XINPUT_GAMEPAD_Y,"left trackpad up is native Y on the pause menu (Edit Squad)");
    s.utility_press=0;s.utility_y=0;
    fake->controller_sample.reset();
    check(gpu.queue->Signal(gpu.fence.Get(),++gpu.tick));wait(gpu.fence.Get(),gpu.tick);
}
void menu_input_trace(Host& gpu) {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    require(session.set_render_path(VrSession::RenderPath::ordinary),"ordinary path for the menu trace fixture");
    const auto now=std::chrono::steady_clock::now();session.poll(now,gpu.queue.Get());
    VrControllerInput input;input.enable(true);fake->controller_sample=VrControllerSample{};
    auto red=gpu.color(1,0,0,32);std::array<EyeTextureCapture,2> eyes;
    const auto frame=session.ordinary_frame({31,131,0},now);require(frame.has_value(),"ordinary pose issued");
    for(int eye=0;eye<2;++eye){auto m=tracked_meta(frame->eyes[eye],31);m.screen_menu=true;
        check(eyes[eye].capture(gpu.device.Get(),gpu.queue.Get(),red.Get(),m));const auto image=*eyes[eye].latest(eye);wait(image.fence.Get(),image.fence_value);}
    require(session.submit_ordinary({{*eyes[0].latest(0),*eyes[1].latest(1)},31},gpu.queue.Get(),now),"a pair carrying the menu panel is submitted");
    const auto trace_path=std::filesystem::temp_directory_path()/"sm2vr_menu_trace_test.jsonl";
    require(vr_trace::Recorder::get().open(trace_path,100,now),"trace opens");
    const auto poll=[&](int ms){const auto time=now+std::chrono::milliseconds(ms);session.poll_controllers(input,true,time,0,true);vr_trace::Recorder::get().tick(time);};
    auto& s=*fake->controller_sample;
    poll(5);s.menu=true;s.move_y=1;s.turn_y=-.75f;poll(20);poll(40);s.menu=false;poll(60);poll(80);
    vr_trace::Recorder::get().close();
    std::ifstream in{trace_path};std::string line,all;
    while(std::getline(in,line)){all+=line;all+='\n';}
    in.close();std::filesystem::remove(trace_path);
    require(all.find("\"menu\":true")!=std::string::npos,"input records are written while a menu is up, marked menu");
    require(all.find("\"left_b\":true")!=std::string::npos,"the raw Left B press is in the trace");
    require(all.find("\"name\":\"Y_ability\",\"down\":true,\"menu\":true")!=std::string::npos,"the native Y a Left B tap sends on a menu is a button event, marked menu");
    require(all.find("\"ly\":32767")!=std::string::npos,"the stick the menu was sent is in the trace");
    // 31 Oct 16:14:44 "Sticks don't do it": right stick down is detonate, and only left/right was traced.
    require(all.find("\"turn_y\":-0.750")!=std::string::npos,"the right stick's up and down is in the trace");
    fake->controller_sample.reset();
    check(gpu.queue->Signal(gpu.fence.Get(),++gpu.tick));wait(gpu.fence.Get(),gpu.tick);
}
// GENERIC_GUARDS piece 5. 25 Oct alone three menu controls were missing (the right stick stayed
// snap turn, left A/B were not X/Y, a grenade grab swallowed LB), each found by Joe in the headset.
// On a menu every native button and both sticks must be reachable from some Index control, or be
// listed below as a gap with its reason. A change that drops a route, or adds one, fails here until
// this list says so. Pause-panel input and menu-over-stereo input are the same branch of
// VrSession::poll_controllers_locked; this drives it through the menu-over-stereo route.
void menu_controls_coverage(Host& gpu) {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    require(session.set_render_path(VrSession::RenderPath::ordinary),"ordinary path for the coverage fixture");
    const auto start=std::chrono::steady_clock::now();session.poll(start,gpu.queue.Get());
    VrControllerInput input;input.enable(true);fake->controller_sample=VrControllerSample{};
    auto grey=gpu.color(.5f,.5f,.5f,32);std::array<EyeTextureCapture,2> eyes;uint32_t token=100;int ms=0;
    const auto menu_pair=[&]{const auto when=start+std::chrono::milliseconds(ms);++token;
        const auto frame=session.ordinary_frame({token,token+100,0},when);require(frame.has_value(),"ordinary pose issued");
        for(int eye=0;eye<2;++eye){auto m=tracked_meta(frame->eyes[eye],token);m.screen_menu=true;
            check(eyes[eye].capture(gpu.device.Get(),gpu.queue.Get(),grey.Get(),m));const auto image=*eyes[eye].latest(eye);wait(image.fence.Get(),image.fence_value);}
        require(session.submit_ordinary({{*eyes[0].latest(0),*eyes[1].latest(1)},token},gpu.queue.Get(),when),"menu pair submitted");};
    const auto pad=[&]{ms+=5;const auto when=start+std::chrono::milliseconds(ms);session.poll_controllers(input,true,when,0,true);return input.snapshot(when).pad.Gamepad;};
    struct Out {uint16_t buttons{};bool lt{},rt{},lx{},ly{},rx{},ry{};};Out reached;
    const auto add=[&](const XINPUT_GAMEPAD& p){reached.buttons|=p.wButtons;reached.lt|=p.bLeftTrigger>0;reached.rt|=p.bRightTrigger>0;
        reached.lx|=p.sThumbLX!=0;reached.ly|=p.sThumbLY!=0;reached.rx|=p.sThumbRX!=0;reached.ry|=p.sThumbRY!=0;};
    auto& s=*fake->controller_sample;
    // Every physical Index control VrControllerSample carries (the gesture fields are VrSession's own).
    const std::array<std::pair<const char*,std::function<void(VrControllerSample&)>>,16> controls{{
        {"right A",[](auto& v){v.confirm=true;}},{"right B",[](auto& v){v.interact=true;}},
        {"left A",[](auto& v){v.recenter=true;}},{"left B",[](auto& v){v.menu=true;}},
        {"left stick x",[](auto& v){v.move_x=1;}},{"left stick y",[](auto& v){v.move_y=1;}},{"left stick click",[](auto& v){v.sprint=true;}},
        {"right stick x",[](auto& v){v.turn_x=1;}},{"right stick y",[](auto& v){v.turn_y=1;}},
        {"right trigger",[](auto& v){v.fire=1;}},{"left trigger",[](auto& v){v.aim=1;}},
        {"right grip",[](auto& v){v.melee=1;}},{"left grip",[](auto& v){v.parry=1;}},{"right trackpad",[](auto& v){v.grenade=1;}},
        {"left trackpad up",[](auto& v){v.utility_press=1;v.utility_y=1;}},{"left trackpad down",[](auto& v){v.utility_press=1;v.utility_y=-1;}},
    }};
    const auto probe=[&](const std::function<void(VrControllerSample&)>& press){
        menu_pair();s=VrControllerSample{};pad();press(s);add(pad());s=VrControllerSample{};add(pad());}; // pressed, then released (taps fire on release)
    for(const auto& [name,press]:controls)probe(press);
    probe([](auto& v){v.utility_press=1;v.utility_x=1;});probe([](auto& v){v.utility_press=1;v.utility_x=-1;}); // left trackpad right, left
    // Gaps on a menu, each with its reason. Remove a line when a route is added.
    constexpr uint16_t GAPS=
        XINPUT_GAMEPAD_START|                                        // left B is native Y on menus since 25 Oct; right B (native B) backs out instead
        XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_LEFT|             // gesture-only in play (power/left push, salute); the trackpad gives down/right only
        XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB;        // sprint toggle and finisher: gameplay only
    constexpr uint16_t ALL=XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_X|XINPUT_GAMEPAD_Y|XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB|
        XINPUT_GAMEPAD_START|XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_LEFT_SHOULDER|XINPUT_GAMEPAD_RIGHT_SHOULDER|
        XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_DPAD_LEFT|XINPUT_GAMEPAD_DPAD_RIGHT;
    const auto missing=uint16_t(ALL&~reached.buttons&~GAPS),unexpected=uint16_t(reached.buttons&GAPS);
    require(!missing,("a native button is unreachable on a menu and not listed as a gap: wButtons "+std::to_string(missing)).c_str());
    require(!unexpected,("a listed gap is reachable on a menu now; remove it from GAPS: wButtons "+std::to_string(unexpected)).c_str());
    require(reached.lt && reached.rt,"both triggers reach menus (tabs)");
    require(reached.lx && reached.ly && reached.rx && reached.ry,"both sticks, both axes, reach menus (25 Oct: the right stick was snap turn)");
    // Joe, 26 Oct: the grips are the shoulder buttons on a menu (tabs).
    menu_pair();s=VrControllerSample{};pad();s.melee=1;auto p=pad();
    require((p.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER) && !(p.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER),"right grip is RB on a menu");
    s=VrControllerSample{};p=pad();require(!(p.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER),"RB ends with the grip");
    s.parry=1;p=pad();require((p.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER) && !(p.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER),"left grip is LB on a menu");
    s=VrControllerSample{};pad();
    check(gpu.queue->Signal(gpu.fence.Get(),++gpu.tick));wait(gpu.fence.Get(),gpu.tick);
}
void ordinary_submission(Host& gpu) {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    require(session.set_render_path(VrSession::RenderPath::ordinary),"ordinary path selected explicitly");
    const auto now=std::chrono::steady_clock::now();session.poll(now,gpu.queue.Get());
    const auto frame=session.ordinary_frame({17,23,0},now);require(frame.has_value(),"ordinary pose issued once");
    auto red=gpu.color(1,0,0),blue=gpu.color(0,0,1);std::array<EyeTextureCapture,2> capture;
    for(int eye=0;eye<2;++eye){check(capture[eye].capture(gpu.device.Get(),gpu.queue.Get(),eye?blue.Get():red.Get(),tracked_meta(frame->eyes[eye],17)));const auto image=*capture[eye].latest(eye);wait(image.fence.Get(),image.fence_value);}
    VrSession::OrdinaryImages pair{{*capture[0].latest(0),*capture[1].latest(1)},99};
    require(!session.submit(pair.images,gpu.queue.Get(),now),"ordinary pair cannot enter AFR submission");
    auto bad=pair;++bad.images.right.metadata.render_id;require(!session.submit_ordinary(bad,gpu.queue.Get(),now),"adjacent AFR IDs cannot enter ordinary submission");
    bad=pair;++bad.images.right.metadata.game_token;require(!session.submit_ordinary(bad,gpu.queue.Get(),now),"mixed native job tokens rejected");
    bad=pair;bad.images.right.metadata.runtime_eye_to_head[12]+=.01f;require(!session.submit_ordinary(bad,gpu.queue.Get(),now),"unissued eye offset rejected");
    bad=pair;bad.images.right=bad.images.left;bad.images.right.metadata=pair.images.right.metadata;require(!session.submit_ordinary(bad,gpu.queue.Get(),now),"aliased eye image rejected");
    require(!frame->valid_at(now+std::chrono::milliseconds(101)) && !session.ordinary_frame({17,23,0},now+std::chrono::milliseconds(101)),"image submission allowance cannot extend cached tracking or input freshness");
    require(!session.submit_ordinary(pair,gpu.queue.Get(),now+std::chrono::milliseconds(251)),"completed ordinary images beyond the eye-pose budget are rejected");
    require(!session.submit_ordinary(pair,gpu.queue.Get(),now-std::chrono::milliseconds(1)),"future ordinary images are rejected");
    require(!session.submit_ordinary(pair,gpu.queue.Get(),now+std::chrono::seconds(1)),"stale ordinary pose rejected");
    require(fake->submissions==0,"all invalid pairs refused before runtime transfer");
    const auto rejected=session.statistics();
    require(rejected.ordinary_refusals==std::array<uint64_t,5>{0,3,0,3,1} && rejected.ordinary_pose_age_ms==1000 && rejected.ordinary_generations==std::array<uint64_t,2>{pair.images.left.generation,pair.images.right.generation},"actual submission boundary identifies texture, metadata and stale-pose refusals independently");
    D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;ComPtr<ID3D12CommandQueue> foreign_queue;check(gpu.device->CreateCommandQueue(&desc,IID_PPV_ARGS(&foreign_queue)));
    bad=pair;bad.queue=foreign_queue;require(!session.submit_ordinary(bad,gpu.queue.Get(),now),"ordinary submission rejects a queue other than the bridge producer");
    pair.queue=gpu.queue;
    require(session.submit_ordinary(pair,gpu.queue.Get(),now+std::chrono::milliseconds(150)) && fake->submissions==1,"campaign GPU latency above 100ms still transfers the exact issued stereo pair on its producing queue");
    require(!session.submit_ordinary(pair,gpu.queue.Get(),now),"duplicate ordinary pair rejected");
    check(gpu.queue->Signal(gpu.fence.Get(),++gpu.tick));wait(gpu.fence.Get(),gpu.tick);
}
void physical_melee_session() {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    fake->controller_sample=VrControllerSample{};fake->pose.controllers[1]=vr_camera::IDENTITY;
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(20)};
    VrSession session{std::move(backend)};session.enable(true);session.poll(now);
    VrControllerInput input;input.enable(true);uint64_t frame=0;
    const auto step=[&](int ms,float x,float head_x=0) {
        fake->pose.head[12]=head_x;(*fake->pose.controllers[1])[12]=x;
        const auto time=now+std::chrono::milliseconds(ms);
        ++frame;
        require(session.eye({uint32_t(frame),frame,int(frame%2)},time).has_value(),"swing fixture must use real tracking acquisition");
        session.poll_controllers(input,true,time,0,true);
        XINPUT_STATE native{};uint32_t result=ERROR_DEVICE_NOT_CONNECTED;input.get_state(&result,0,&native,time);
        require(result==ERROR_SUCCESS,"swing fixture must read real cached native input");
        return (native.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER)!=0;
    };
    session.allow_swing_melee(42);
    for(int ms=0;ms<=100;ms+=20) require(!step(ms,ms*0.003f),"physical gestures are disabled by default");
    session.enable_swing_melee(true);
    std::vector<std::chrono::steady_clock::time_point> commits;session.set_swing_observer([&](auto time){commits.push_back(time);});
    require(!step(120,0) && !step(140,0) && !step(160,0.06f) && !step(180,0.12f) && step(200,0.2f),"tracked right-hand swing must reach actual session/XInput path");
    require(commits==std::vector{now+std::chrono::milliseconds(200)},"the swing observer hears the commit at its poll time (the melee sweep's path anchor)");
    const auto count=session.swing_statistics().swings;
    session.poll_controllers(input,true,now+std::chrono::milliseconds(201),0,true);
    require(session.swing_statistics().swings==count && input.snapshot(now+std::chrono::milliseconds(201)).pad.Gamepad.wButtons==XINPUT_GAMEPAD_RIGHT_SHOULDER,"cached tracking must hold one pulse without derivative/repeat");
    require(commits.size()==1,"a held pulse is not another commit");session.set_swing_observer({});
    session.allow_swing_melee(0);require(!step(220,0.3f),"lost exact player ownership must release swing input");
    fake->controller_sample->melee=1;require(step(240,0.3f),"manual grip remains available without motion ownership");
    fake->controller_sample->melee=0;require(!step(260,0.3f),"manual grip release remains independent");
    session.allow_swing_melee(99);
    for(int ms=280;ms<=400;ms+=20) require(!step(ms,(ms-280)*0.003f,(ms-280)*0.003f),"room-space walking must not request melee through real session");
    auto settings=session.swing_settings();settings.values[0]=4.5f;
    require(session.set_swing_settings(settings),"live threshold applies without restart");
    require(!step(420,0) && !step(440,0) && !step(460,0.06f) && !step(480,0.12f) && !step(500,0.2f),"real session consumes raised speed threshold");
    require(session.set_swing_settings({}),"reset restores candidate thresholds");
    step(520,0);step(540,0);step(560,0.06f);step(580,0.12f);require(step(600,0.2f),"reset live settings affects native output");
    fake->controller_sample->turn_x=1;require(!step(620,0.2f),"snap request releases physical attack in same poll");
    fake->controller_sample->turn_x=0;session.recenter();
    require(!step(640,0.4f),"recenter must discard prior motion pulse");
    fake->pose.controllers[1].reset();
    ++frame;
    require(session.eye({uint32_t(frame),frame,0},now+std::chrono::milliseconds(660)).has_value(),"head remains valid with sleeping controller");
    session.poll_controllers(input,true,now+std::chrono::milliseconds(660));
    require(!session.swing_statistics().pressed,"sleeping right controller releases motion attack");
    session.poll_controllers(input,false,now+std::chrono::milliseconds(670));
    require(!input.snapshot(now+std::chrono::milliseconds(670)).pad.Gamepad.wButtons,"focus loss remains neutral");
}
void trigger_gated_melee_and_holsters_session() {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    fake->controller_sample=VrControllerSample{};fake->pose.controllers[1]=vr_camera::IDENTITY;
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(40)};
    VrSession session{std::move(backend)};session.enable(true);session.poll(now);
    VrControllerInput input;input.enable(true);uint64_t frame=0;
    // Head at 1.7 m facing -Z; the hand position is room space, so x is to the right.
    std::optional<bool> secondary_equipped=true;
    const auto step=[&](int ms,float x,float y,float z=0) {
        auto& hand=*fake->pose.controllers[1];hand[12]=x;hand[13]=y;hand[14]=z;
        const auto time=now+std::chrono::milliseconds(ms);++frame;
        require(session.eye({uint32_t(frame),frame,int(frame%2)},time).has_value(),"fixture must use real tracking acquisition");
        session.poll_controllers(input,true,time,0,secondary_equipped);
        XINPUT_STATE native{};uint32_t result=ERROR_DEVICE_NOT_CONNECTED;input.get_state(&result,0,&native,time);
        require(result==ERROR_SUCCESS,"fixture must read real cached native input");
        return native.Gamepad.wButtons;
    };
    const auto swing=[&](int start,float y) {
        WORD buttons{};float x=0;
        // Gaps between swings exceed the 100 ms input staleness limit, so the first poll
        // re-arms neutral input; hold still for two more polls before moving.
        // Swing 40 cm in front of the body, clear of both holster zones.
        for(int ms=start;ms<=start+100;ms+=20) { if(ms>=start+60) x+=0.07f;buttons|=step(ms,x,y,-0.4f); }
        return (buttons&XINPUT_GAMEPAD_RIGHT_SHOULDER)!=0;
    };
    session.allow_swing_melee(42);session.enable_swing_melee(true);
    auto& sample=*fake->controller_sample;
    // Default gate (playtest 17 Oct): a finger resting on the trigger is the natural hold.
    sample.fire_touch_valid=sample.fire_touch=true;
    require(swing(0,1.3f),"default gate: a swing with the finger resting on the trigger reaches native melee");
    sample.fire=0.5f;
    // Recover with both triggers released before trying held-trigger motion.
    sample.fire=0;step(400,0,1.3f,-0.4f);sample.fire=0.5f;
    // Power needs the trigger pulled before the stroke starts (VrPowerTrigger::HOLD, 250 ms).
    { WORD buttons{};for(int ms=420;ms<=780;ms+=20) buttons|=step(ms,ms<740?0:(ms-720)*.0035f,1.3f,-.4f);
      require((buttons&XINPUT_GAMEPAD_DPAD_UP) && !(buttons&XINPUT_GAMEPAD_RIGHT_SHOULDER),"held right trigger plus sword swing selects only native power"); }
    sample.fire=0;step(1200,0,1.3f,-0.4f);sample.fire=0.5f;step(1220,0,1.3f,-0.4f);sample.fire=0;
    {WORD buttons{};float x=0;for(int ms=1240;ms<=1340;ms+=20) { if(ms>=1300) x+=0.07f;buttons|=step(ms,x,1.3f,-0.4f); }
     require(buttons&XINPUT_GAMEPAD_RIGHT_SHOULDER,"trigger release selects light without a firearm cooldown in sword mode");}
    require(swing(1700,1.3f),"released-trigger sword swings request light melee");
    // Opt-in touch gate: the finger must leave the trigger.
    session.require_trigger_lifted(true);
    require(!swing(2300,1.3f),"touch gate: a finger resting on the trigger blocks swings");
    sample.fire_touch=false;require(swing(2900,1.3f),"touch gate: lifting the finger allows swings");
    sample.fire_touch_valid=false;require(swing(3500,1.3f),"touch gate without a touch binding falls back to the pull gate");
    session.require_trigger_lifted(false);

    // Holsters are off by default: a hip squeeze is plain grip melee.
    step(4000,0.25f,1.0f);sample.melee=1;
    require(step(4020,0.25f,1.0f)==XINPUT_GAMEPAD_RIGHT_SHOULDER,"holsters disabled: hip squeeze is grip melee only");
    sample.melee=0;step(4040,0.25f,1.0f);
    session.enable_holsters(true);session.enable_swing_melee(false);secondary_equipped=false;
    step(4100,0.25f,1.0f);sample.melee=1;
    require(step(4120,0.25f,1.0f)==XINPUT_GAMEPAD_DPAD_DOWN,"hip selects sidearm through native cycle without emote or grip attack");
    require(step(4240,0.25f,1.0f)==0,"pulse ends while the squeeze is held in the zone");
    require(step(4260,0.15f,1.45f,-0.4f)==0,"drawing back with the selecting squeeze held never grip-attacks");
    sample.melee=0;step(4280,0.15f,1.45f,-0.4f);
    sample.melee=1;require(step(4300,0.15f,1.45f,-0.4f)==XINPUT_GAMEPAD_RIGHT_SHOULDER,"a fresh squeeze outside holsters is grip melee again");
    sample.melee=0;secondary_equipped=true;step(4500,0.25f,1.0f);step(4520,0.25f,1.0f);step(4530,0.25f,1.0f); // re-arm after the gap, hold still
    sample.melee=1;require(step(4540,0.25f,1.0f)==XINPUT_GAMEPAD_DPAD_DOWN,"with the sidearm out a hip squeeze swaps back to the primary (Joe, 31 Oct: the hip swaps both ways)");
    sample.melee=0;step(4560,0.25f,1.0f);secondary_equipped=true;
    sample.melee=0;step(4660,0.1f,1.6f,0.1f);step(4680,0.1f,1.6f,0.1f);
    sample.melee=1;require(step(4700,0.1f,1.6f,0.1f)==0,"a squeeze over the shoulder draws nothing (31 Oct) and is no grip melee either");
    require(session.holster_statistics().primary==1 && session.holster_statistics().secondary==1,"the hip swapped once each way");
    sample.melee=0;secondary_equipped=false;step(4720,0.1f,1.6f,0.1f);
    sample.melee=0;step(4940,0.25f,1.0f);step(4950,0.25f,1.0f);step(4960,0.25f,1.0f);
    sample.melee=1;step(4980,0.25f,1.0f);
    for(int ms=5000;ms<5400;ms+=20) step(ms,0.25f,1.0f);
    require(step(5400,0.25f,2.15f)==XINPUT_GAMEPAD_DPAD_LEFT,"raising a closed fist sends the native emote, never primary");
    require(step(5540,0.25f,2.15f)==0 && session.holster_statistics().salutes==1,"held overhead fist sends one pulse");
    sample.melee=0;step(5560,0.25f,1.3f,-0.4f);
    session.enable_swing_melee(true);
    require(!swing(5600,1.3f),"a fast draw right after a holster selection must not swing");
    secondary_equipped=true;
    require(swing(6100,1.3f),"sword swings return after the quiet window with confirmed sword ownership");

    // 30 Oct playtest (20:24): with the bolter out and the right hand hanging at the hip, pressing
    // right A to dodge squeezed the Index grip past the holster threshold (0.18 to 0.68 at 20:35:56)
    // and drew the pistol, six times, unasked. B, the stick click and the trackpad press do the same.
    session.enable_swing_melee(false);secondary_equipped=false;sample={};
    int ms=7000;
    const auto hip=[&](){ms+=20;return step(ms,0.25f,1.0f);};
    hip();hip();
    const auto draws=session.holster_statistics().secondary;
    for(int thumb=0;thumb<4;++thumb) {
        const auto press=[&](bool on){ sample.confirm=on && thumb==0;sample.interact=on && thumb==1;sample.finisher=on && thumb==2;sample.grenade=on && thumb==3?1.f:0.f; };
        press(true);sample.melee=0.2f;hip();
        sample.melee=0.68f;require(!(hip()&XINPUT_GAMEPAD_DPAD_DOWN),"a grip squeezed by the thumb on a right button never draws the pistol");
        sample.melee=0.78f;require(!(hip()&XINPUT_GAMEPAD_DPAD_DOWN),"the squeeze stays consumed while it rises");
        press(false);require(!(hip()&XINPUT_GAMEPAD_DPAD_DOWN),"lifting the thumb with the grip still held is no delayed draw");
        sample.melee=0;hip();hip();
    }
    require(session.holster_statistics().secondary==draws,"no thumb squeeze counted as a pistol draw");
    sample.melee=1;require(hip()&XINPUT_GAMEPAD_DPAD_DOWN,"a deliberate hip squeeze afterwards still draws the pistol");
    require(session.holster_statistics().secondary==draws+1,"the deliberate squeeze counts as one draw");
    sample.melee=0;hip();hip();sample.confirm=true;
    require(hip()&XINPUT_GAMEPAD_A,"right A still dodges while the hand rests in the hip zone");
}
// Left controller orientations (OpenVR, head facing -Z): pointing right (forearm across
// the face), pointing down (hanging at the side) and pointing forward (gun support).
vr_camera::Matrix left_hand(float x,float y,float z,int pointing) {
    vr_camera::Matrix m=pointing==0?vr_camera::Matrix{0,0,1,0, 0,1,0,0, -1,0,0,0, 0,0,0,1}:
        pointing==1?vr_camera::Matrix{-1,0,0,0, 0,0,1,0, 0,1,0,0, 0,0,0,1}:vr_camera::IDENTITY;
    m[12]=x;m[13]=y;m[14]=z;return m;
}
// Runs on both render paths. The single-frame (ordinary) path issues tracking per native
// camera job through ordinary_frame, and input polls can repeat that cached sample, so
// each step there polls twice without a new frame.
void left_hand_gestures_session(bool ordinary) {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    fake->controller_sample=VrControllerSample{};
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(60)};
    VrSession session{std::move(backend)};session.enable(true);
    if(ordinary) { session.set_stereo(true);require(session.set_render_path(VrSession::RenderPath::ordinary),"ordinary path selected"); }
    session.poll(now);
    VrControllerInput input;input.enable(true);uint64_t frame=0;int ms=0;
    const auto trace_path=std::filesystem::temp_directory_path()/"sm2vr_session_trace_test.jsonl";
    require(vr_trace::Recorder::get().open(trace_path,100,now),"trace opens");
    const auto read=[&](std::chrono::steady_clock::time_point time) {
        session.poll_controllers(input,true,time);vr_trace::Recorder::get().tick(time);
        XINPUT_STATE native{};uint32_t result=ERROR_DEVICE_NOT_CONNECTED;input.get_state(&result,0,&native,time);
        require(result==ERROR_SUCCESS,"fixture must read real cached native input");
        return native.Gamepad.wButtons;
    };
    const auto step=[&](vr_camera::Matrix hand) {
        fake->pose.controllers[0]=hand;ms+=20;
        const auto time=now+std::chrono::milliseconds(ms);++frame;
        if(ordinary) require(session.ordinary_frame({uint32_t(frame),frame,0},time).has_value(),"fixture must use real ordinary tracking acquisition");
        else require(session.eye({uint32_t(frame),frame,int(frame%2)},time).has_value(),"fixture must use real tracking acquisition");
        auto buttons=read(time);
        if(ordinary) buttons|=read(time+std::chrono::milliseconds(5)); // Repeated poll of the same cached frame.
        return buttons;
    };
    auto& sample=*fake->controller_sample;
    const auto side=left_hand(-0.3f,0.9f,0,1),guard=left_hand(-0.2f,1.6f,-0.25f,0),belt=left_hand(-0.25f,1.0f,0,1),support=left_hand(-0.15f,1.4f,-0.4f,2);
    step(side);step(side);
    require(step(guard)==0,"guard disabled by default: a raised forearm sends nothing");
    session.enable_guard(true);step(side);step(side);
    require(step(guard)==XINPUT_GAMEPAD_LEFT_SHOULDER,"raised left forearm holds native parry/block");
    require(step(guard)==XINPUT_GAMEPAD_LEFT_SHOULDER,"holding the guard keeps blocking");
    // Tracking stops arriving: the stale early return must release the held guard, and the
    // still-raised guard has to be lowered before it can block again.
    ms+=200;require(!(read(now+std::chrono::milliseconds(ms))&XINPUT_GAMEPAD_LEFT_SHOULDER),"stale tracking releases a held guard");
    require(!session.guard_statistics().held && !session.guard_statistics().pose,"stale poll clears the gesture itself before tracking recovers");
    require(!(step(guard)&XINPUT_GAMEPAD_LEFT_SHOULDER),"after stale tracking the raised guard must be lowered first");
    step(side);step(side);step(side);step(side);step(side);
    require(step(support)==0,"a forward-pointing support hand does not parry");
    require(step(left_hand(-0.2f,1.5f,-0.25f,2))==0,"nor does a forward-pointing hand close to the face (an angle rejection)");
    sample.aim=0.5f;require(!(step(guard)&XINPUT_GAMEPAD_LEFT_SHOULDER),"aiming down sights never parries");
    sample.aim=0;step(side);step(side);step(side);step(side);step(side);
    sample.parry=1;require(step(support)==XINPUT_GAMEPAD_LEFT_SHOULDER,"left grip parry still works anywhere");
    sample.parry=0;step(side);step(side);step(side);step(side);

    session.enable_grenade_gesture(true);
    step(belt);step(belt);
    sample.parry=1;require(step(belt)==0,"a squeeze at the belt takes a grenade and never parries");
    require(session.grenade_statistics().holding,"grenade held");
    WORD thrown{};
    for(int i=1;i<=10;++i) {
        const float f=i/10.0f;if(i==10) sample.parry=0;
        thrown|=step(left_hand(-0.25f+0.15f*f,1.0f+0.6f*f,-0.6f*f,1));
    }
    require(thrown&XINPUT_GAMEPAD_X,"a fast forward release sends native equipment (X)");
    auto anchor=vr_camera::IDENTITY;anchor[10]=-1;
    const auto launch=session.grenade_aim(anchor,42,session.epoch(),now+std::chrono::milliseconds(ms));
    require(launch && vr_camera::rigid(launch->world),"actual grenade poll publishes a native-world throw target");
    require(launch->world[8]*anchor[8]+launch->world[10]*anchor[10]>.5f && launch->world[9]>.3f,"forward/up physical release maps to native forward/up, not desktop camera heading");
    anchor[12]=20;
    const auto frozen=session.grenade_aim(anchor,42,session.epoch(),now+std::chrono::milliseconds(ms+1));
    require(frozen && frozen->world==launch->world,"movement after release does not rotate/translate the throw target");
    require(!session.grenade_aim(anchor,99,session.epoch(),now+std::chrono::milliseconds(ms+1)),"throw target cannot transfer to a new actor");
    require(!session.grenade_aim(anchor,42,session.epoch()+1,now+std::chrono::milliseconds(ms+1)),"throw target cannot cross a snap epoch");
    session.enable_grenade_gesture(false);
    require(!session.grenade_aim(anchor,42,session.epoch(),now+std::chrono::milliseconds(ms+1)),"disabling the grenade feature cancels the pending native projectile override immediately");
    session.enable_grenade_gesture(true);
    require(!(thrown&XINPUT_GAMEPAD_LEFT_SHOULDER),"the throw never parries or blocks on the way through the guard zone");
    WORD after{};for(int i=0;i<10;++i) after|=step(left_hand(-0.1f,1.6f,-0.6f,1));
    require(!(after&XINPUT_GAMEPAD_LEFT_SHOULDER),"the follow-through pose is latched until the hand leaves it");
    require(session.grenade_statistics().throws==1,"one throw counted");

    // Slow raised cancellation can cross the guard angle boundary after release;
    // merely latching the guard's initial pose is insufficient for that motion.
    step(belt);step(belt);sample.parry=1;step(belt);
    for(int i=1;i<=40;++i) {const float f=i/40.0f;step(left_hand(-.25f+.15f*f,1.0f+.6f*f,-.35f*f,2));}
    sample.parry=0;require(!(step(left_hand(-.1f,1.6f,-.35f,2))&XINPUT_GAMEPAD_X),"slow raised release does not spend equipment");
    require(session.grenade_statistics().last==VrGrenadeThrow::Outcome::cancelled && session.grenade_statistics().recovering,"actual poll retains grenade ownership after cancellation");
    for(int i=0;i<12;++i) {sample.parry=i>=6?1.0f:0.0f;const auto pad=step(guard);
        require(!(pad&(XINPUT_GAMEPAD_LEFT_SHOULDER|XINPUT_GAMEPAD_X)),"changing angle or squeezing during grenade follow-through never parries or throws (a raised slow release is not a put back)");}
    step(belt);sample.parry=0;step(belt);step(side);step(support);
    require(step(guard)&XINPUT_GAMEPAD_LEFT_SHOULDER,"normal guard resumes after grip release and lowering");
    step(side);step(side);step(side);step(side);step(side);

    // Playtest 26 Oct (docs/PARRY_TIMING_2026-10-26.md): the relaxed left hand rests in the belt zone
    // and a fast raise is already turned across on the first poll out of it. Until then the belt
    // latched the guard and that raise never pressed LB.
    sample.parry=0;step(belt);step(belt);
    require(step(guard)&XINPUT_GAMEPAD_LEFT_SHOULDER,"a raise straight out of the belt zone guards on its first poll");
    require(step(left_hand(-0.2f,1.25f,-0.25f,0))&XINPUT_GAMEPAD_LEFT_SHOULDER,"a raised guard sinking into the top of the belt zone keeps blocking (no LB flicker at the boundary)");
    step(belt);step(belt);step(belt); // Out of the 80 ms minimum hold.
    require(!(step(belt)&XINPUT_GAMEPAD_LEFT_SHOULDER),"lowered to the belt it lets go");
    step(belt);step(belt);
    session.set_guard_belt_latch(true);step(belt);step(belt);
    require(!(step(guard)&XINPUT_GAMEPAD_LEFT_SHOULDER),"SM2VR_GUARD_BELT_LATCH=1 restores the old rule: the belt latches the guard");
    session.set_guard_belt_latch(false);step(side);step(side);step(side);step(side);step(side);

    // Measured close-waist misses from the physical trace used to leak raw LB.
    for(float x:{.038f,.040f,.108f}) {
        const auto centre_belt=left_hand(x,1.05f,-.035f,1);sample.parry=0;step(centre_belt);sample.parry=1;
        require(step(centre_belt)==0 && session.grenade_statistics().holding,"near-centre waist squeeze belongs exclusively to grenade, never native grip parry");
        sample.parry=0;step(centre_belt);
    }

    step(belt);step(belt);sample.parry=1;step(belt);
    require(session.grenade_statistics().holding,"second grenade held before tracking loss");
    ms+=200;read(now+std::chrono::milliseconds(ms));
    require(!session.grenade_statistics().holding && session.grenade_statistics().last==VrGrenadeThrow::Outcome::lost,
        "stale poll cancels a held grenade before tracking recovers");

    vr_trace::Recorder::get().close();
    std::ifstream in{trace_path};std::string line,all;size_t samples=0;
    while(std::getline(in,line)) { all+=line;all+='\n';if(line.find("\"type\":\"sample\"")!=std::string::npos) ++samples; }
    in.close();std::filesystem::remove(trace_path);
    require(samples>=40,"trace samples written from the real input poll");
    for(const char* event:{"guard_start","guard_end","LB_parry_block","grenade_grab","\"outcome\":\"thrown\"","X_equipment_grenade","guard_rejected_angle"})
        require(all.find(event)!=std::string::npos,event);
    require(all.find("\"left\":[-0.2000,1.6000,-0.2500]")!=std::string::npos,"raw left-hand tracking is in the trace");
}
// Playtest 20 Oct (docs/validation/melee-trace-2026-10-20.md), through the real session:
// a left punch became a right light attack; a guard held through a sword swing re-pressed
// LB when the pulse ended (a new parry, which also aborts the attack); the pistol held out
// in the guard zone parried between shots; a follow-through into the hip zone cut the pulse.
void melee_hand_arbitration_session() {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    fake->controller_sample=VrControllerSample{};
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(80)};
    VrSession session{std::move(backend)};session.enable(true);session.poll(now);
    VrControllerInput input;input.enable(true);uint64_t frame=0;int ms=0;std::optional<bool> loadout=true;
    // Head at 1.7 m facing -Z; right hand 40 cm in front of the chest, clear of the holsters.
    struct Right { float x,y,z; };
    const auto step=[&](Right r,vr_camera::Matrix left) {
        auto right=vr_camera::IDENTITY;right[12]=r.x;right[13]=r.y;right[14]=r.z;
        fake->pose.controllers[1]=right;fake->pose.controllers[0]=left;ms+=20;
        const auto time=now+std::chrono::milliseconds(ms);++frame;
        require(session.eye({uint32_t(frame),frame,int(frame%2)},time).has_value(),"fixture must use real tracking acquisition");
        session.poll_controllers(input,true,time,0,loadout);
        XINPUT_STATE native{};uint32_t result=ERROR_DEVICE_NOT_CONNECTED;input.get_state(&result,0,&native,time);
        require(result==ERROR_SUCCESS,"fixture must read real cached native input");
        return native.Gamepad.wButtons;
    };
    const Right front{0,1.3f,-.4f};
    const auto side=left_hand(-.3f,.9f,0,1),guard=left_hand(-.2f,1.6f,-.25f,0),pistol_out=left_hand(-.35f,1.35f,-.25f,0);
    const auto rest=[&](int polls,vr_camera::Matrix left=vr_camera::Matrix{}) { WORD b{};for(int i=0;i<polls;++i) b|=step(front,left==vr_camera::Matrix{}?side:left);return b; };
    // Right swing: 7 cm per 20 ms across the chest (3.5 m/s).
    const auto swing=[&](vr_camera::Matrix left) { WORD b{};for(int i=1;i<=4;++i) b|=step({front.x+.07f*i,front.y,front.z},left);return b; };
    session.allow_swing_melee(42);session.enable_swing_melee(true);session.enable_guard(true);session.enable_holsters(true);
    rest(4);

    // A: left punch (10 cm per 20 ms, pointing forward so it is no guard) while the right
    // controller twists along with it (the same right motion as a real swing).
    WORD punch{};
    for(int i=1;i<=4;++i) punch|=step({front.x+.07f*i,front.y,front.z},left_hand(-.3f,1.4f,-.2f-.1f*i,2));
    require(!(punch&XINPUT_GAMEPAD_RIGHT_SHOULDER) && session.swing_statistics().vetoes==1 && session.swing_statistics().swings==0,
        "a left punch never sends the right hand's light attack");
    rest(30);
    require(swing(side)&XINPUT_GAMEPAD_RIGHT_SHOULDER,"the same right motion with the left hand still is a light attack");
    rest(30);

    // B: guard held through a sword swing.
    rest(2,guard);
    require(rest(3,guard)&XINPUT_GAMEPAD_LEFT_SHOULDER,"raised forearm blocks");
    const auto attack=swing(guard);
    require((attack&XINPUT_GAMEPAD_RIGHT_SHOULDER) && session.guard_statistics().yields==1,"a sword swing out of the guard attacks and the guard yields");
    WORD after{};for(int i=0;i<15;++i) { const auto b=step(front,guard);after|=b; }
    require(!(after&XINPUT_GAMEPAD_LEFT_SHOULDER),"the still-raised guard never re-presses LB after the attack pulse");
    rest(3);
    require(rest(2,guard)&XINPUT_GAMEPAD_LEFT_SHOULDER,"lowering and raising the guard blocks again");
    rest(30);

    // C: the left pistol held out in the guard zone, fired and released between shots.
    auto& sample=*fake->controller_sample;
    for(int shot=0;shot<3;++shot) {
        sample.aim=1;rest(3,pistol_out);
        require(input.snapshot(now+std::chrono::milliseconds(ms)).pad.Gamepad.bRightTrigger>200,"the left trigger fires the pistol (native RT)");
        sample.aim=0;const auto between=rest(5,pistol_out);
        require(!(between&XINPUT_GAMEPAD_LEFT_SHOULDER),"releasing the pistol trigger between shots never parries");
    }
    rest(3);
    require(rest(2,guard)&XINPUT_GAMEPAD_LEFT_SHOULDER,"after lowering the pistol the guard works again");
    rest(30);

    // D: a swing whose follow-through enters the hip holster zone keeps its pulse.
    for(int i=0;i<12;++i) step({-.15f,1.3f,-.4f},side);
    WORD first=step({-.05f,1.3f,-.4f},side);first|=step({.05f,1.3f,-.4f},side);first|=step({.12f,1.26f,-.35f},side);
    const auto commits=session.swing_statistics().swings;
    require(first&XINPUT_GAMEPAD_RIGHT_SHOULDER,"swing commits in front of the body");
    int held=0;for(int i=0;i<5;++i) held+=(step({.2f,1.2f,-.25f+i*.02f},side)&XINPUT_GAMEPAD_RIGHT_SHOULDER)!=0;
    require(session.holster_statistics().zone==VrHolster::Slot::secondary,"fixture follow-through ends in the hip zone");
    require(held>=4 && session.swing_statistics().swings==commits,"the native pulse runs its full length in the hip zone");
    rest(30);

    // E (Joe, 20 Oct): power is a swing with the trigger held; a resting finger is light.
    sample.fire=0.1f;rest(3);
    const auto resting=swing(side);
    require((resting&XINPUT_GAMEPAD_RIGHT_SHOULDER) && !(resting&XINPUT_GAMEPAD_DPAD_UP),"a finger resting on the trigger (0.1) swings light");
    // Joe, 21 Oct evening: the trigger clenched as the stroke starts is not power; held from
    // before the stroke (VrPowerTrigger::HOLD, 250 ms) it is.
    sample.fire=0;rest(30);sample.fire=0.5f;rest(3);
    const auto clenched=swing(side);
    require((clenched&XINPUT_GAMEPAD_RIGHT_SHOULDER) && !(clenched&XINPUT_GAMEPAD_DPAD_UP),"a trigger pulled 60 ms before the swing (a clench) swings light");
    sample.fire=0;rest(30);sample.fire=0.5f;rest(15);
    const auto held_trigger=swing(side);
    require((held_trigger&XINPUT_GAMEPAD_DPAD_UP) && !(held_trigger&XINPUT_GAMEPAD_RIGHT_SHOULDER),"a swing with the trigger held is power");
    sample.fire=0;rest(30);

    // F (Joe, 20 Oct): a wrist flick, the controller twisted 45 degrees about a still wrist.
    const auto before=session.swing_statistics();WORD flick{};
    for(int i=1;i<=6;++i) {
        const float yaw=std::min(i,4)*.2f;auto right=vr_camera::IDENTITY;
        right[0]=right[10]=std::cos(yaw);right[2]=-std::sin(yaw);right[8]=std::sin(yaw);right[12]=front.x;right[13]=front.y;right[14]=front.z;
        fake->pose.controllers[1]=right;fake->pose.controllers[0]=side;ms+=20;
        const auto time=now+std::chrono::milliseconds(ms);++frame;
        require(session.eye({uint32_t(frame),frame,int(frame%2)},time).has_value(),"fixture must use real tracking acquisition");
        session.poll_controllers(input,true,time,0,true);
        XINPUT_STATE native{};uint32_t result=ERROR_DEVICE_NOT_CONNECTED;input.get_state(&result,0,&native,time);flick|=native.Gamepad.wButtons;
    }
    require(!(flick&(XINPUT_GAMEPAD_RIGHT_SHOULDER|XINPUT_GAMEPAD_DPAD_UP)) && session.swing_statistics().swings==before.swings && session.swing_statistics().flicks==before.flicks+1,
        "a wrist flick never attacks and is counted as a flick");
    rest(30);

    // G (Joe, 21 Oct): both hands on the sword. The left hand is part of the grip: its pose
    // near the face is no guard, and moving with the right it does not veto the swing.
    // 27 cm from the right hand, in the guard zone and turned across: a guard if alone.
    const auto together=[&](float dx){return left_hand(front.x-.2f+dx,front.y+.1f,front.z+.15f,0);};
    WORD guard_pose{};for(int i=0;i<5;++i) guard_pose|=step(front,together(0));
    require(!(guard_pose&XINPUT_GAMEPAD_LEFT_SHOULDER),"the left hand holding the sword beside the right is not a guard");
    const auto vetoes=session.swing_statistics().vetoes;WORD both{};
    for(int i=1;i<=4;++i) both|=step({front.x+.07f*i,front.y,front.z},together(.09f*i));
    require((both&XINPUT_GAMEPAD_RIGHT_SHOULDER) && session.swing_statistics().vetoes==vetoes,"a two-handed swing attacks even though the left hand moves as fast");
    rest(30);

    // H (Joe, 21 Oct): the sword rests at the hip (the sidearm holster zone) and the swing
    // starts from there, forward and up out of the zone.
    for(int i=0;i<8;++i) step({.25f,1.0f,0},side);
    require(session.holster_statistics().zone==VrHolster::Slot::secondary,"fixture rests in the hip zone");
    WORD from_hip{};for(int i=1;i<=6;++i) from_hip|=step({.25f,1.0f+.04f*i,-.08f*i},side);
    require(from_hip&XINPUT_GAMEPAD_RIGHT_SHOULDER,"a swing starting from the hip rest attacks once it leaves the zone");
    rest(30);

    // I (Joe, 21 Oct): with the bolter out a hard stroke attacks; aiming speed, a shot just
    // fired or aiming down sights never do.
    loadout=false;rest(20);
    const auto stroke=[&](float per_step){WORD b{};for(int i=1;i<=4;++i) b|=step({front.x-.2f+per_step*i,front.y,front.z},side);rest(30);return b;};
    require(stroke(.13f)&XINPUT_GAMEPAD_RIGHT_SHOULDER,"a 6.5 m/s stroke with the bolter out is a light attack");
    require(!(stroke(.07f)&XINPUT_GAMEPAD_RIGHT_SHOULDER),"a 3.5 m/s motion with the bolter out is not");
    sample.fire=1;rest(3);sample.fire=0;rest(2);
    {WORD b{};for(int i=1;i<=4;++i) b|=step({front.x-.2f+.13f*i,front.y,front.z},side);
     require(!(b&XINPUT_GAMEPAD_RIGHT_SHOULDER),"a hard stroke within 300 ms of a shot is recoil, not a swing");}
    rest(30);sample.aim=.6f;
    {WORD b{};for(int i=1;i<=4;++i) b|=step({front.x-.2f+.13f*i,front.y,front.z},side);
     require(!(b&XINPUT_GAMEPAD_RIGHT_SHOULDER),"aiming down sights never swings");}
    sample.aim=0;rest(30);loadout=true;

    // J (Joe, 21 Oct evening): "knockback should just be a push with the left arm". An open
    // left hand thrust straight out (3 m/s, to 0.58 m) sends the native held strike (D-pad up).
    // The right hand rests at the side, clear of the left (not a two-handed grip).
    const auto thrust=[&]{WORD b{};for(int i=0;i<=8;++i) b|=step({.35f,1.1f,-.1f},left_hand(-.1f,1.25f,-.1f-.06f*i,2));rest(30);return b;};
    require(!(thrust()&XINPUT_GAMEPAD_DPAD_UP),"the push is off unless enabled (-Gestures)");
    fake->pose.fingers[0].reset();session.enable_left_push(true);rest(10); // No finger tracking: still pushes.
    {const auto b=thrust();require((b&XINPUT_GAMEPAD_DPAD_UP) && !(b&(XINPUT_GAMEPAD_RIGHT_SHOULDER|XINPUT_GAMEPAD_LEFT_SHOULDER)) && session.left_push_statistics().pushes==1,
        "a left-arm push sends only the native held strike (knockback)");}
    // Joe, 22 Oct: a closed-fist left punch is a push too (grip squeezed, fingers curled).
    fake->pose.fingers[0]=vr_finger::Curl{.6f,.95f,.95f,.95f,.95f};sample.parry=.9f;
    require((thrust()&XINPUT_GAMEPAD_DPAD_UP) && session.left_push_statistics().pushes==2,"a fist thrust with the grip squeezed sends the held strike");
    sample.parry=0;fake->pose.fingers[0].reset();rest(10);
    loadout=false;rest(20);require(!(thrust()&XINPUT_GAMEPAD_DPAD_UP) && session.left_push_statistics().pushes==2,"with the bolter out a push does nothing");
    loadout=true;session.enable_left_push(false);rest(30);
}
// Joe (20 Oct): grenades always flew along the spawn facing, whatever the head, snap turns or
// hand did. The native game aims along its own camera, which never turns with the headset.
// The override direction must follow the player in the native world: compare it with the
// submitted eye's world forward after snap turns and head yaw, through the real poll path.
void grenade_aim_follows_snap_and_head() {
    const auto yawed=[](float yaw,float x,float y,float z) { // Rotation about +Y (right turn = positive yaw), then position.
        const float s=std::sin(yaw),c=std::cos(yaw);return vr_camera::Matrix{c,0,s,0, 0,1,0,0, -s,0,c,0, x,y,z,1};
    };
    const auto horizontal_angle=[](const float* a,const float* b) { // Signed angle from b to a about +Y, degrees.
        return std::atan2(b[2]*a[0]-b[0]*a[2],a[0]*b[0]+a[2]*b[2])*57.29578f;
    };
    struct Case { float head_yaw; int snaps; float hand_weight; float hand_yaw; };
    for(const Case c:{Case{0,0,0,0},Case{0,1,0,0},Case{0,3,0,0},Case{0,-2,0,0},Case{.8f,0,0,0},Case{-.6f,2,0,0},Case{.5f,1,1,40},Case{0,-1,1,-30}}) {
        auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;fake->controller_sample=VrControllerSample{};
        const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(60)};
        VrSession session{std::move(backend)};session.enable(true);session.poll(now);
        session.enable_grenade_gesture(true);
        VrGrenadeAim::Settings aim;aim.values.values[VrGrenadeAim::HAND_YAW_WEIGHT]=c.hand_weight;aim.values.values[VrGrenadeAim::HAND_PITCH_WEIGHT]=0;
        aim.values.values[VrGrenadeAim::BASE_PITCH]=0;require(session.set_grenade_aim_settings(aim),"aim settings accepted");
        VrControllerInput input;input.enable(true);uint64_t frame=0;int ms=0;
        auto& sample=*fake->controller_sample;
        std::optional<EngineCameraProbe::VrEye> eye;
        const auto step=[&](float head_yaw,float lateral,float height,float forward) {
            const float sn=std::sin(head_yaw),cs=std::cos(head_yaw);
            fake->pose.head=yawed(head_yaw,0,1.7f,0);
            fake->pose.controllers[0]=yawed(head_yaw,lateral*cs+forward*sn,1.7f+height,lateral*sn-forward*cs);ms+=20;++frame;
            const auto time=now+std::chrono::milliseconds(ms);
            eye=session.eye({uint32_t(frame),frame,0},time);require(eye.has_value(),"fixture uses real tracking acquisition");
            session.poll_controllers(input,true,time);
            XINPUT_STATE native{};uint32_t result=ERROR_DEVICE_NOT_CONNECTED;input.get_state(&result,0,&native,time);
            return native.Gamepad.wButtons;
        };
        constexpr float BELT_LATERAL=-.25f,BELT_HEIGHT=-.7f;
        for(int i=0;i<3;++i) step(0,BELT_LATERAL,BELT_HEIGHT+.3f,.3f); // Reference at yaw 0; arm the pad.
        for(int i=0;i<std::abs(c.snaps);++i) {
            sample.turn_x=c.snaps>0?1.0f:-1.0f;step(0,BELT_LATERAL,BELT_HEIGHT+.3f,.3f);
            sample.turn_x=0;step(0,BELT_LATERAL,BELT_HEIGHT+.3f,.3f);
        }
        require(input.snapshot(now+std::chrono::milliseconds(ms)).snaps==uint64_t(std::abs(c.snaps)),"fixture snaps through the real input path");
        step(c.head_yaw,BELT_LATERAL,BELT_HEIGHT,0);step(c.head_yaw,BELT_LATERAL,BELT_HEIGHT,0);
        sample.parry=1;step(c.head_yaw,BELT_LATERAL,BELT_HEIGHT,0);
        require(session.grenade_statistics().holding,"grenade taken at the belt");
        const float t=std::tan(c.hand_yaw*0.01745329f);WORD thrown{};
        for(int i=1;i<=10;++i) { // 0.6 m forward (plus the case's sideways angle) and 0.3 m up in 200 ms.
            const float f=i/10.0f;if(i==10) sample.parry=0;
            thrown|=step(c.head_yaw,BELT_LATERAL+.6f*t*f,BELT_HEIGHT+.3f*f,.6f*f);
        }
        require((thrown&XINPUT_GAMEPAD_X) && session.grenade_statistics().throws==1,"throw sent");
        auto anchor=yawed(.6f,5,2,-3); // A turned native body anchor; the engine's frames are left-handed.
        for(int i=8;i<11;++i) anchor[i]=-anchor[i];
        const auto launch=session.grenade_aim(anchor,42,session.epoch(),now+std::chrono::milliseconds(ms));
        require(launch.has_value(),"throw publishes a launch direction");
        const auto released=session.grenade_statistics().last_aim;
        require(std::abs(launch->speed_scale-released.speed_scale)<1e-6f && std::abs(released.speed_scale-released.hand_speed/4)<.01f,
            "the published launch carries the hand's release speed as a fraction of the native throw");
        vr_camera::Matrix world{},view{};require(vr_camera::compose(anchor,eye->relative_eye,eye->units_per_metre,world,view),"eye composes");
        const float* direction=&launch->world[8];const float* gaze=&world[8];const float* right=&world[0];
        const float expected=c.hand_weight*c.hand_yaw;
        const float angle=horizontal_angle(direction,gaze);
        require(std::abs(std::abs(angle)-std::abs(expected))<1.5f,"launch yaw is the gaze yaw in the native world (plus the hand's share), after snaps and head yaw");
        require(expected==0 || (direction[0]*right[0]+direction[2]*right[2]>0)==(expected>0),"a throw to the right of the gaze lands right of it in the world");
        require(std::abs(direction[1])<.02f,"base pitch 0 and no hand pitch: level launch");
    }
}
void third_person_session() {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    fake->controller_sample=VrControllerSample{};fake->pose.controllers[1]=vr_camera::IDENTITY;
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    const auto now=std::chrono::steady_clock::now();session.poll(now);
    require(session.set_calibration({.9f,.05f,-.2f}),"life size calibration accepts existing head placement");
    const auto first=session.eye({1,1,0},now);require(first && close(first->relative_eye[14],.2f),"first person preserves calibrated head inset");
    VrViewSettings settings;settings.mode=VrViewMode::diorama;
    require(session.set_view_settings(settings),"live diorama activates through production session");
    const auto left=session.shared_eye({2,2,0},now),right=session.shared_eye({3,3,1},now);
    require(left && right && left->tracking_epoch!=first->tracking_epoch && left->reference_epoch!=first->reference_epoch,"mode changes invalidate old images and anchors");
    require(left->view_settings==settings && close(left->units_per_metre,9) && close(right->relative_eye[12]-left->relative_eye[12],.064f),"session applies scale once while preserving physical stereo metadata");
    require(close(left->relative_eye[13],0) && close(left->relative_eye[14],0) && !session.controller_poses(now),"third-person camera excludes first-person inset and tracked arm targets");
    VrControllerInput input;input.enable(true);session.poll_controllers(input,true,now);
    require(!input.snapshot(now).armed,"third person cannot arm VR walking or snap turns");
    const auto epoch=session.epoch();session.recenter();
    const auto centered=session.eye({4,4,0},now);
    require(centered && centered->tracking_epoch!=epoch,"third-person recenter starts fresh image epoch");
    settings.diorama_scale=20;require(session.set_view_settings(settings),"live miniature scale accepted");
    const auto smaller=session.eye({5,5,1},now);require(smaller && close(smaller->units_per_metre,18),"scale knob changes production eye output");
    require(!session.set_scale(6) && !session.set_calibration({6,0,0}),"combined life-size and miniature scale must stay inside camera contract");
    settings.mode=VrViewMode::shoulder;require(session.set_view_settings(settings),"shoulder mode accepted");
    const auto shoulder=session.eye({6,6,0},now);require(shoulder && close(shoulder->units_per_metre,.9f) && close(shoulder->relative_eye[14],0),"shoulder restores life-size scale without first-person offsets");
    settings.mode=VrViewMode::first_person;require(session.set_view_settings(settings),"first-person mode restored");
    const auto restored=session.eye({7,7,1},now);require(restored && close(restored->relative_eye[14],.2f) && session.controller_poses(now),"mode return restores placement and tracked controllers without restart");
    settings.diorama_scale=NAN;require(!session.set_view_settings(settings) && session.view_settings().mode==VrViewMode::first_person,"bad update preserves working mode");
}
void conversions() {
    vr::VRSkeletalSummaryData_t summary{};summary.flFingerCurl[0]=0.2f;summary.flFingerCurl[2]=1;
    const auto fingers=openvr_conversion::fingers(summary,2,2);
    require(fingers && (*fingers)[0]==0.2f && (*fingers)[1]==0 && (*fingers)[2]==1,"actual OpenVR summary converter preserves per-finger curl and pointing");
    require(!openvr_conversion::fingers(summary,3,2) && !openvr_conversion::fingers(summary,99,99),"skeletal action for another device or invalid role is excluded");
    summary.flFingerCurl[1]=NAN;require(!openvr_conversion::fingers(summary,2,2),"nonfinite runtime curl is excluded");
    const vr::HmdMatrix34_t input{{{0,0,1,3},{0,1,0,1.7f},{-1,0,0,4}}};
    const auto converted = openvr_conversion::from_matrix(input);
    require(vr_camera::rigid(converted) && converted[12] == 3 && converted[13] == 1.7f && converted[14] == 4,
        "OpenVR column-vector transform must transpose into row-vector camera storage");
    const auto output = openvr_conversion::to_matrix(converted);
    require(std::memcmp(&input,&output,sizeof(input)) == 0,"submitted tracking pose must round-trip exactly");
    const auto fov = openvr_conversion::tangents(-1.1f,0.9f,-0.8f,1.2f);
    require(fov.valid() && fov.bottom == -1.2f && fov.top == 0.8f,"OpenVR down-axis tangents must convert into up-axis game frustum");
    std::array<vr::TrackedDevicePose_t,4> poses{};
    for (int i:{1,3}) {
        poses[i].bDeviceIsConnected=poses[i].bPoseIsValid=true;
        poses[i].eTrackingResult=vr::TrackingResult_Running_OK;
        auto matrix=vr_camera::IDENTITY; matrix[12]=float(i);
        poses[i].mDeviceToAbsoluteTracking=openvr_conversion::to_matrix(matrix);
    }
    auto hands=openvr_conversion::controllers(poses,{3,1});
    require(hands[0] && hands[1] && (*hands[0])[12]==3 && (*hands[1])[12]==1,"left/right must follow current runtime roles rather than device order");
    poses[3].bDeviceIsConnected=false; hands=openvr_conversion::controllers(poses,{3,1});
    require(!hands[0] && hands[1],"sleeping hand must release only its own pose");
    poses[3].bDeviceIsConnected=true; hands=openvr_conversion::controllers(poses,{1,3});
    require(hands[0] && hands[1] && (*hands[0])[12]==1,"wake and role reassignment must be discovered on the next sample");
    poses[1].bPoseIsValid=false; require(!openvr_conversion::controllers(poses,{1,3})[0],"invalid pose must not reach arm target cache");
    poses[1].bPoseIsValid=true; poses[1].eTrackingResult=vr::TrackingResult_Running_OutOfRange;
    require(!openvr_conversion::controllers(poses,{1,3})[0],"unreliable tracking must release arm target");
    poses[1].eTrackingResult=vr::TrackingResult_Running_OK; poses[1].mDeviceToAbsoluteTracking.m[0][0]=NAN;
    require(!openvr_conversion::controllers(poses,{1,3})[0],"non-finite runtime matrix must be rejected");
    require(!openvr_conversion::controllers(poses,{vr::k_unTrackedDeviceIndexInvalid,3})[0],"missing runtime role must be bounds checked");
    hands=openvr_conversion::controllers(poses,{3,3});
    require(!hands[0] && !hands[1],"one device cannot become both hands");
}
void controller_poses() {
    auto backend=std::make_unique<Backend>(); auto* fake=backend.get(); fake->present=true;
    fake->pose.head[12]=1; fake->pose.head[14]=2;
    for (size_t hand=0;hand<2;++hand) {
        auto pose=vr_camera::IDENTITY; pose[12]=hand==0 ? 0.7f : 1.3f; pose[13]=1.2f; pose[14]=1.5f;
        fake->pose.controllers[hand]=pose;
        fake->pose.fingers[hand]=vr_finger::Curl{0.2f,0,1,0.8f,0.9f};
    }
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(100)};
    VrSession session{std::move(backend)}; session.enable(true); session.poll(now);
    require(!session.controller_poses(now),"hands require a valid headset reference");
    require(session.eye({1,1,0},now).has_value(),"hand fixture must sample the production eye provider");
    auto hands=session.controller_poses(now);
    require(hands && hands->fingers[0]==fake->pose.fingers[0] && hands->fingers[1]==fake->pose.fingers[1],"individual finger curls use same fresh controller/head sample");
    require(hands && hands->relative[0] && hands->relative[1] && std::abs((*hands->relative[0])[12]+0.3f)<1e-6f &&
        std::abs((*hands->relative[0])[13]+0.5f)<1e-6f && std::abs((*hands->relative[0])[14]+0.5f)<1e-6f,
        "controllers must use the headset's level reference, not absolute room-floor height or IPD");
    require(!session.controller_poses(now-std::chrono::milliseconds(1)) && !session.controller_poses(now+std::chrono::milliseconds(101)),"future and stale poses must not reach animation consumers");
    VrControllerInput input; input.enable(true); fake->controller_sample=VrControllerSample{};
    fake->pose.head[12]+=0.2f;
    for (auto& pose:fake->pose.controllers) { (*pose)[12]+=0.2f; }
    session.eye({2,2,1},now); session.poll_controllers(input,true,now);
    fake->controller_sample->turn_x=1; session.poll_controllers(input,true,now);
    const auto turned=session.controller_poses(now);
    require(turned && turned->fingers[0]==fake->pose.fingers[0],"snap preserves scalar finger curls without rotating or scaling them");
    const float c=std::cos(0.5235987756f),s=std::sin(0.5235987756f);
    require(turned && hands && turned->reference_epoch==hands->reference_epoch && turned->tracking_epoch>hands->tracking_epoch &&
        std::abs((*turned->relative[0])[12]-(0.2f-0.3f*c+0.5f*s))<1e-6f &&
        std::abs((*turned->relative[0])[14]-(-0.3f*s-0.5f*c))<1e-6f &&
        std::abs((*turned->relative[0])[2]-s)<1e-6f,
        "snap must turn cached hand position and orientation around the leaned head using the current rig");
    session.recenter(); require(!session.controller_poses(now),"recenter must discard old targets before a fresh tracking sample");
    session.eye({3,3,0},now); hands=session.controller_poses(now);
    require(hands && turned && hands->reference_epoch>turned->reference_epoch && std::abs((*hands->relative[0])[12]+0.3f)<1e-6f,"recenter must preserve wrist-to-head displacement without old snap rotation");
    require(session.set_calibration({0.9f,-0.3f}),"hand calibration fixture must be valid");
    session.eye({4,4,1},now); hands=session.controller_poses(now);
    require(hands && hands->units_per_metre==0.9f && std::abs((*hands->relative[0])[13]+0.8f)<1e-6f &&
        std::abs((*hands->relative[0])[12]+0.3f)<1e-6f,"hands must share camera height calibration and report scale without scaling raw metre coordinates twice");
    fake->pose.controllers[0].reset(); session.eye({5,5,0},now); hands=session.controller_poses(now);
    require(hands && !hands->fingers[0] && hands->fingers[1],"lost wrist releases only its finger data");
    require(hands && !hands->relative[0] && hands->relative[1],"lost controller must not leave its previous pose cached");
    auto bad=vr_camera::IDENTITY; bad[0]=NAN; fake->pose.controllers[0]=bad;
    session.eye({6,6,1},now); require(!session.controller_poses(now)->relative[0],"session must independently reject malformed backend poses");
    fake->valid=false; session.eye({7,7,0},now);
    require(!session.controller_poses(now),"head tracking loss must release both hand targets");
    fake->valid=true; fake->pose.controllers[0]=vr_camera::IDENTITY; session.eye({8,8,1},now);
    require(session.controller_poses(now)->relative[0].has_value(),"valid tracking recovery must republish targets");
    (*fake->pose.fingers[0])[1]=NAN;fake->pose.fingers[1].reset();session.eye({9,9,0},now);
    require(!session.controller_poses(now)->fingers[0] && !session.controller_poses(now)->fingers[1] && session.controller_poses(now)->relative[0],"bad/unbound skeletal action releases fingers without losing valid wrist tracking");
    session.enable(false); require(!session.controller_poses(now),"disabled runtime must release targets");
}
// docs/SPRINT_CAMERA.md: the real session path from the emitted sprint pad and native body
// speed to the composed eyes and controller origins.
void sprint_camera_session() {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;fake->controller_sample=VrControllerSample{};
    for(size_t hand=0;hand<2;++hand){auto pose=vr_camera::IDENTITY;pose[12]=hand?.2f:-.2f;pose[13]=1.3f;pose[14]=-.4f;fake->pose.controllers[hand]=pose;}
    VrSession session{std::move(backend)};session.enable(true);
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(80)};session.poll(now);
    require(session.set_calibration({0.9f,0.05f,-0.2f}),"accepted placement with default sprint fields");
    VrControllerInput input;input.enable(true);uint64_t frame{};
    // Native speeds in world units/second at 0.9 units per metre: 5.8 m/s run, 8.6 m/s sprint.
    constexpr float RUN=5.8f*.9f,SPRINT=8.6f*.9f;
    struct Step{float eye_z,hand_z;uint64_t epoch;uint16_t buttons;};
    const auto step=[&](int ms,std::optional<float> speed){
        ++frame;const auto time=now+std::chrono::milliseconds(ms);
        const auto eye=session.eye({uint32_t(frame),frame,int(frame%2)},time);
        require(eye.has_value(),"sprint fixture frame issued");
        session.poll_controllers(input,true,time,0,false,std::nullopt,true,speed);
        const auto hands=session.controller_poses(time);
        require(hands && hands->relative[0],"sprint fixture hands");
        return Step{eye->relative_eye[14],(*hands->relative[0])[14],session.epoch(),input.snapshot(time).pad.Gamepad.wButtons};
    };
    int ms=0;Step s{};
    for(;ms<100;ms+=10) s=step(ms,0.f);
    require(close(s.eye_z,0.2f) && close(s.hand_z,-0.2f),"standing: accepted -0.2 m placement, no sprint offset");
    const auto epoch=s.epoch;
    auto& c=*fake->controller_sample;c.move_y=1;
    for(;ms<400;ms+=10) s=step(ms,RUN);
    require(!(s.buttons&XINPUT_GAMEPAD_LEFT_THUMB) && close(s.eye_z,0.2f),"running forward without the sprint click: no offset");
    c.sprint=true;s=step(ms,RUN);ms+=10;c.sprint=false;
    require(s.buttons&XINPUT_GAMEPAD_LEFT_THUMB,"click-to-sprint reaches the native pad");
    float previous=s.eye_z;
    for(;ms<1000;ms+=10) {
        s=step(ms,ms<550?RUN:SPRINT);
        require(s.eye_z<=previous+1e-6f && previous-s.eye_z<=0.0121f,"eye moves forward smoothly (<= 12 mm per 10 ms) and never back while sprinting");
        require(close(s.eye_z-s.hand_z,0.4f),"controller origins move with the eyes: hands stay registered");
        previous=s.eye_z;
    }
    require(std::abs(s.eye_z-0.0f)<1e-5f,"sprinting: -0.2 m + 0.20 m sprint offset");
    require(s.epoch==epoch,"the sprint offset never recenters or discards frames");
    require(session.sprint_camera(now+std::chrono::milliseconds(ms)).confirmed,"native sprint speed confirmed");
    // Live calibration: sprint fields apply without a recenter, placement fields still recenter.
    auto calibration=session.calibration();calibration.sprint_forward_offset_metres=0.3f;
    require(session.set_calibration(calibration),"sprint offset hot reload");
    for(int n=0;n<2;++n,ms+=10) s=step(ms,SPRINT); // The next poll hands the new value to the blend.
    require(s.epoch==epoch && std::abs(s.eye_z+0.1f)<1e-5f,"new sprint offset applies live without invalidating");
    calibration.sprint_forward_offset_metres=0.2f;require(session.set_calibration(calibration),"restore sprint offset");
    for(int n=0;n<2;++n,ms+=10) s=step(ms,SPRINT);
    // Stop: the stick release cancels sprint; the offset holds through the stop lean, then ramps out.
    c.move_y=0;const int stop=ms;
    for(;ms<stop+700;ms+=10) { s=step(ms,ms<stop+600?SPRINT*(1-float(ms-stop)/600):0.f);
        require(!(s.buttons&XINPUT_GAMEPAD_LEFT_THUMB) && std::abs(s.eye_z-0.0f)<1e-5f,"held through the native stop animation"); }
    previous=s.eye_z;
    for(;ms<stop+1100;ms+=10) { s=step(ms,0.f);require(s.eye_z>=previous-1e-6f && s.eye_z-previous<=0.0121f,"ramps back smoothly");previous=s.eye_z; }
    require(close(s.eye_z,0.2f) && s.epoch==epoch,"back to the accepted placement after the stop");
    calibration.head_forward_offset_metres=-0.1f;require(session.set_calibration(calibration),"placement change");
    require(session.epoch()>epoch,"changing the accepted head placement still invalidates the reference");
    // Refused: the mod requests sprint but the body never reaches sprint speed.
    for(int n=0;n<5;++n,ms+=10) s=step(ms,0.f);
    c.move_y=1;c.sprint=true;s=step(ms,RUN);ms+=10;c.sprint=false;
    for(const int start=ms;ms<start+1200;ms+=10) s=step(ms,RUN);
    require((s.buttons&XINPUT_GAMEPAD_LEFT_THUMB) && close(s.eye_z,0.1f),"no native confirmation: the offset is withdrawn although the pad still sprints");
    require(session.sprint_camera(now+std::chrono::milliseconds(ms)).refusals==1,"refusal counted");
}
void calibration_files() {
    std::istringstream input{"# Physical Index feedback\r\n\r\nunits_per_metre=0.9\r\nheight_offset_metres=-0.3\r\n"};
    const auto parsed = VrCalibration::read(input);
    require(parsed && parsed->units_per_metre == 0.9f && parsed->height_offset_metres == -0.3f,"calibration must parse both named values and Windows newlines");
    std::ostringstream saved;
    require(parsed->write(saved),"valid calibration must serialize");
    std::istringstream roundtrip{saved.str()};
    require(VrCalibration::read(roundtrip) == parsed,"serialized calibration must round-trip exactly");
    require(parsed->head_forward_offset_metres==0,"existing two-field presets must retain zero forward correction");
    std::istringstream extended{"units_per_metre=0.9\nheight_offset_metres=0.05\nhead_forward_offset_metres=-0.2\n"};
    const auto placement=VrCalibration::read(extended);
    require(placement && *placement==VrCalibration{0.9f,0.05f,-0.2f},"placement preset must parse physical height and forward corrections");
    for (const auto& text:{"head_forward_offset_metres=0.6\n","head_forward_offset_metres=nan\n","head_forward_offset_metres=0\nhead_forward_offset_metres=0\n"}) {
        std::istringstream broken_forward{std::string{"units_per_metre=0.9\nheight_offset_metres=0\n"}+text};
        require(!VrCalibration::read(broken_forward),"invalid/out-of-range/duplicate forward placement must reject the entire preset");
    }
    require(placement->sprint_forward_offset_metres==0.2f && placement->sprint_blend_seconds==0.25f,"presets without sprint fields get the 0.20 m / 0.25 s default");
    std::istringstream sprint{"units_per_metre=0.9\r\nheight_offset_metres=0.05\r\nhead_forward_offset_metres=-0.2\r\nsprint_forward_offset_metres=0.1\r\nsprint_blend_seconds=0.4\r\n"};
    const auto sprinting=VrCalibration::read(sprint);
    require(sprinting && sprinting->sprint_forward_offset_metres==0.1f && sprinting->sprint_blend_seconds==0.4f && sprinting->same_placement(*placement) && *sprinting!=*placement,
        "sprint fields parse; they are not part of the head placement");
    std::ostringstream sprint_saved;require(sprinting->write(sprint_saved),"sprint calibration serializes");
    std::istringstream sprint_roundtrip{sprint_saved.str()};
    require(VrCalibration::read(sprint_roundtrip)==sprinting,"sprint fields round-trip");
    for (const auto& text:{"sprint_forward_offset_metres=-0.01\n","sprint_forward_offset_metres=0.41\n","sprint_forward_offset_metres=nan\n","sprint_blend_seconds=0\n",
        "sprint_blend_seconds=2\n","sprint_forward_offset_metres=0.1\nsprint_forward_offset_metres=0.1\n"}) {
        std::istringstream broken_sprint{std::string{"units_per_metre=0.9\nheight_offset_metres=0\n"}+text};
        require(!VrCalibration::read(broken_sprint),"invalid/out-of-range/duplicate sprint fields reject the entire preset");
    }
    {
        const auto path=std::filesystem::path{__FILE__}.parent_path().parent_path()/"config"/"index.calibration";
        const auto shipped=VrCalibration::load(path);
        require(shipped && shipped->same_placement(VrCalibration{0.9f,0.10f,-0.2f,0.2f,0.25f,1.644f,1.45f}) && shipped->sprint_forward_offset_metres==0.2f && shipped->sprint_blend_seconds==0.25f,
            "shipped config/index.calibration keeps the accepted placement (eyes 0.10 m up), the 0.20 m sprint default, Joe's 1.644 m eye height and the 1.45 first-person scale");
    }
    wchar_t folder[MAX_PATH]{}, filename[MAX_PATH]{};
    require(GetTempPathW(MAX_PATH,folder) && GetTempFileNameW(folder,L"svr",0,filename),"calibration fixture needs a unique temporary file");
    const auto fixture = std::filesystem::path{filename};
    const bool saved_file = parsed->save(fixture);
    const auto loaded_file = VrCalibration::load(fixture);
    DeleteFileW(filename);
    require(saved_file && loaded_file == parsed,"live calibration file and saved presets must use the same validated production format");
    for (const auto invalid : {"units_per_metre=0.9\n", "units_per_metre=0.9\nheight_offset_metres=2\n",
        "units_per_metre=0\nheight_offset_metres=0\n", "units_per_metre=0.9x\nheight_offset_metres=0\n",
        "units_per_metre=nan\nheight_offset_metres=0\n", "units_per_metre=1\nunits_per_metre=2\nheight_offset_metres=0\n"}) {
        std::istringstream broken{invalid};
        require(!VrCalibration::read(broken),"partial, duplicate, non-finite and out-of-range calibration must fail without partial application");
    }
}
}
// Guard guide through the production poll: shown only with the guard on and the sword out,
// coloured by what the guard does, hidden when tracking stops arriving (VrGuardGuide.h).
void guard_guide_session(bool ordinary) {
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    fake->controller_sample=VrControllerSample{};
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(60)};
    VrSession session{std::move(backend)};session.enable(true);
    if(ordinary) { session.set_stereo(true);require(session.set_render_path(VrSession::RenderPath::ordinary),"ordinary path selected"); }
    session.poll(now);
    VrControllerInput input;input.enable(true);uint64_t frame=0;int ms=0;std::optional<bool> secondary=true;
    const auto step=[&](vr_camera::Matrix left,vr_camera::Matrix right) {
        fake->pose.controllers[0]=left;fake->pose.controllers[1]=right;ms+=20;
        const auto time=now+std::chrono::milliseconds(ms);++frame;
        if(ordinary) require(session.ordinary_frame({uint32_t(frame),frame,0},time).has_value(),"fixture must use real ordinary tracking acquisition");
        else require(session.eye({uint32_t(frame),frame,int(frame%2)},time).has_value(),"fixture must use real tracking acquisition");
        session.poll_controllers(input,true,time,0,secondary);
        XINPUT_STATE native{};uint32_t result=ERROR_DEVICE_NOT_CONNECTED;input.get_state(&result,0,&native,time);
        require(result==ERROR_SUCCESS,"fixture must read real cached native input");
        const auto guide=session.guard_guide(time);
        return std::pair{guide?guide->state:VrGuardGuide::State::hidden,bool(native.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER)};
    };
    using S=VrGuardGuide::State;
    const auto t=VrGuardGuide::target(session.guard_settings());
    // Head at (0,1.7,0) facing -Z: the target in standing space, pointing right (across).
    const auto across=left_hand(t.lateral,1.7f+t.height,-t.forward,0),forward=left_hand(t.lateral,1.7f+t.height,-t.forward,2);
    const auto side=left_hand(-.3f,.9f,0,1);
    auto rest=vr_camera::IDENTITY;rest[12]=.3f;rest[13]=1.0f;rest[14]=-.2f; // Right hand (sword) at the hip, away from the left.
    step(side,rest);
    require(step(across,rest).first==S::hidden,"guide off by default");
    session.enable_guard_guide(true);
    require(step(across,rest).first==S::hidden,"guide on but guard off: nothing to teach");
    session.enable_guard(true);step(side,rest);step(side,rest);
    require(step(side,rest).first==S::idle,"sword out, guard on: idle guide");
    secondary=false;
    require(step(side,rest).first==S::idle,"the sword gate is debounced like the rev (a dropped reading keeps it)");
    for(int i=0;i<15;++i) step(side,rest); // Past VrCombatRouting::REV_HOLDOVER.
    require(step(side,rest).first==S::hidden,"bolter out: no guide");
    secondary=true;step(side,rest);step(side,rest);
    require(step(forward,rest).first==S::turn,"right place, pointing forward: turn");
    step(side,rest);
    const auto [state,lb]=step(across,rest);
    require(state==S::active && lb,"at the target, pointing across: the guard holds LB and the guide is active");
    // Right hand brought next to the left (both hands on the sword): the guard lets go.
    auto together=across;together[12]+=.1f;together[14]-=.02f;
    const auto [two,two_lb]=step(across,together);
    require(two==S::blocked && !two_lb,"two-handed hold in the pose: no guard, blocked guide");
    step(side,rest);step(side,rest);
    ms+=200;require(!session.guard_guide(now+std::chrono::milliseconds(ms)),"no fresh poll: the guide disappears");
    auto look=session.guard_guide_settings();look.values.values[VrGuardGuide::ENABLED]=0;require(session.set_guard_guide_settings(look),"config switch");
    require(step(side,rest).first==S::hidden,"config enabled=0 hides it");
    require(session.guard_guide_status().changes>=5,"status counts state changes");
}
void lease_follows_completion_queue(Host& gpu) {
    // A backend that copies on its own queue: the pair stays leased until THAT queue is done.
    D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> own;check(gpu.device->CreateCommandQueue(&desc,IID_PPV_ARGS(&own)));
    ComPtr<ID3D12Fence> gate;check(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
    check(own->Wait(gate.Get(),1)); // The backend's queue is still busy reading.
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;fake->completion=own.Get();
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    const auto now=std::chrono::steady_clock::now();session.poll(now,gpu.queue.Get());
    auto red=gpu.color(1,0,0),blue=gpu.color(0,0,1);EyeTextureCapture capture;
    const auto submit=[&](uint64_t first){
        const auto left=session.eye({uint32_t(first),first,0},now),right=session.eye({uint32_t(first+1),first+1,1},now);
        require(left && right,"tracking stays active");
        check(capture.capture(gpu.device.Get(),gpu.queue.Get(),red.Get(),tracked_meta(*left,uint32_t(first))));
        check(capture.capture(gpu.device.Get(),gpu.queue.Get(),blue.Get(),tracked_meta(*right,uint32_t(first+1))));
        const auto image=capture.latest(1);wait(image->fence.Get(),image->fence_value);
        return session.submit(*capture.pair(),gpu.queue.Get(),now);};
    require(submit(1) && submit(3),"two pairs in flight");
    check(gpu.queue->Signal(gpu.fence.Get(),++gpu.tick));wait(gpu.fence.Get(),gpu.tick); // The game queue is idle.
    {
        const auto left=session.eye({5,5,0},now);require(left.has_value(),"tracking stays active");
        require(capture.capture(gpu.device.Get(),gpu.queue.Get(),red.Get(),tracked_meta(*left,5))==E_PENDING,
            "the eye images stay leased while the backend's own queue has not finished, though the game queue is idle");
    }
    check(gate->Signal(1));check(own->Signal(gpu.fence.Get(),++gpu.tick));wait(gpu.fence.Get(),gpu.tick);
    session.poll(now,gpu.queue.Get()); // Retires completed transfers.
    require(submit(7) && session.statistics().failed_transfers==0,"its completion releases them");
}
// docs/OPENXR_SUBMIT_THREAD.md: with the submit thread, a runtime whose frame calls block holds that
// thread, never the caller (the game's render/Present thread).
void submit_thread_session(Host& gpu) {
    using namespace std::chrono;
    // Backends that do not allow it keep the synchronous path.
    {
        auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
        VrSession session{std::move(backend)};session.set_submit_thread(true);session.enable(true);session.set_stereo(true);
        session.set_render_path(VrSession::RenderPath::ordinary);session.poll(steady_clock::now(),gpu.queue.Get());
        require(!session.statistics().submit_thread && fake->ticks==1,"a backend without separate_frame_thread ticks on the caller");
    }
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;fake->frame_thread=true;
    std::atomic<unsigned> wrapped{};
    VrSession session{std::move(backend)};
    session.set_submit_thread(true,[&](const std::function<void()>& step){++wrapped;step();});
    session.enable(true);session.set_stereo(true);session.set_render_path(VrSession::RenderPath::ordinary);
    const auto start=steady_clock::now();session.poll(start,gpu.queue.Get());
    require(session.statistics().submit_thread,"the submit thread runs once the backend starts");
    auto red=gpu.color(1,0,0),blue=gpu.color(0,0,1);
    std::deque<std::array<EyeTextureCapture,2>> captures; // One pool per pair, so leased pairs never block a capture.
    uint64_t frame_id=40;
    const auto pair=[&](steady_clock::time_point now){
        ++frame_id;const auto frame=session.ordinary_frame({uint32_t(frame_id),frame_id,0},now);require(frame.has_value(),"ordinary pose issued");
        auto& capture=captures.emplace_back();
        for(int eye=0;eye<2;++eye){check(capture[eye].capture(gpu.device.Get(),gpu.queue.Get(),eye?blue.Get():red.Get(),tracked_meta(frame->eyes[eye],uint32_t(frame_id))));
            const auto image=*capture[eye].latest(eye);wait(image.fence.Get(),image.fence_value);}
        VrSession::OrdinaryImages out{{*capture[0].latest(0),*capture[1].latest(1)},frame_id};out.queue=gpu.queue;return out;};
    // Every call below must return while the runtime is blocked; a caller that waited would hang here.
    const auto returns=[](auto call){auto done=std::async(std::launch::async,call);require(done.wait_for(seconds(5))==std::future_status::ready,"the caller never waits for the runtime");return done.get();};
    fake->set_hold(true);
    auto now=start+milliseconds(10);
    require(returns([&]{return session.submit_ordinary(pair(now),gpu.queue.Get(),now);}),"the first pair is handed over");
    require(fake->wait_entered(1),"the submit thread takes it into the runtime");
    require(fake->transfer_thread!=std::this_thread::get_id(),"the runtime transfer runs on the submit thread");
    now+=milliseconds(10);require(returns([&]{return session.submit_ordinary(pair(now),gpu.queue.Get(),now);}),"a second pair waits while the runtime is blocked");
    now+=milliseconds(10);require(returns([&]{return session.submit_ordinary(pair(now),gpu.queue.Get(),now);}),"a third pair replaces the waiting one");
    returns([&]{session.poll(now,gpu.queue.Get());return session.status();});
    require(session.statistics().submit_replaced==1 && session.statistics().submitted_pairs==0,"the replaced pair is counted, nothing is accepted yet");
    fake->set_hold(false);
    const auto settled=[&](uint64_t pairs){for(int n=0;n<500;++n){if(session.statistics().submitted_pairs>=pairs)return true;std::this_thread::sleep_for(milliseconds(10));}return false;};
    require(settled(2) && fake->submissions==2,"the first and the newest pair are transferred, the replaced one never");
    const auto accepted=session.statistics();
    require(accepted.last_render_id==frame_id && accepted.failed_transfers==0 && wrapped>0 && fake->ticks>0,"counts, the step wrapper and runtime ticks come from the submit thread");
    // Leases: the transferred pairs stay leased until their fence completes, then retire.
    check(gpu.queue->Signal(gpu.fence.Get(),++gpu.tick));wait(gpu.fence.Get(),gpu.tick);
    now+=milliseconds(10);session.poll(now,gpu.queue.Get());
    require(session.submit_ordinary(pair(now),gpu.queue.Get(),now) && settled(3),"completed leases retire, so the session keeps accepting pairs");
    // A failed transfer stops the session at the caller's next poll, as on the synchronous path.
    fake->submit_ok=false;now+=milliseconds(10);
    require(session.submit_ordinary(pair(now),gpu.queue.Get(),now),"handed over before the runtime fails");
    for(int n=0;n<500 && session.statistics().failed_transfers==0;++n)std::this_thread::sleep_for(milliseconds(10));
    session.poll(now,gpu.queue.Get());
    const auto failed=session.statistics();
    require(failed.failed_transfers==1 && !failed.submit_thread && fake->stops==1 && failed.submitted_pairs==3,"the failure stops the backend once and keeps the counts");
    now+=milliseconds(10);require(!session.ordinary_frame({uint32_t(frame_id+1),frame_id+1,0},now),"a failed session issues no poses");
    check(gpu.queue->Signal(gpu.fence.Get(),++gpu.tick));wait(gpu.fence.Get(),gpu.tick);
}
// docs/BODY_SCALE.md: a player measured at a player-initiated recenter gets the first-person
// scale multiplied by reference / their eye height. Players of any height, hands placed in
// proportion to Joe's, must land on the same game-unit spots, with the eye and the floor
// where Joe's are.
void body_scale_session() {
    using namespace std::chrono;
    constexpr float JOE=1.7f;
    const VrCalibration joe_calibration{0.9f,0.05f,-0.2f,0.2f,0.25f,JOE};
    const float joe_hand[3]{-0.3f,-0.5f,-0.5f}; // Joe's left hand from his head, metres.
    struct Placed { float units,eye_y,eye_z,hand[3]; uint64_t epoch; };
    const auto place=[&](VrSession& session,Backend& fake,float eye_height,uint64_t& frame,time_point<steady_clock> now) {
        const float body=eye_height/JOE;
        fake.pose.head[12]=0;fake.pose.head[13]=eye_height;fake.pose.head[14]=0;
        auto hand=vr_camera::IDENTITY;for(int j=0;j<3;++j) hand[12+j]=fake.pose.head[12+j]+joe_hand[j]*body;
        fake.pose.controllers={hand,std::nullopt};
        ++frame;const auto eye=session.eye({uint32_t(frame),frame,int(frame%2)},now);
        const auto hands=session.controller_poses(now);
        require(eye && hands && hands->relative[0],"body scale fixture frame issued");
        Placed p{eye->units_per_metre,eye->relative_eye[13],eye->relative_eye[14],{},session.epoch()};
        for(int j=0;j<3;++j) p.hand[j]=(*hands->relative[0])[12+j];
        require(hands->units_per_metre==eye->units_per_metre,"hands and eyes share the body-scaled units");
        return p;
    };
    const auto session_for=[&](Backend*& fake) {
        auto backend=std::make_unique<Backend>();fake=backend.get();fake->present=true;
        auto session=std::make_unique<VrSession>(std::move(backend));session->enable(true);
        require(session->set_calibration(joe_calibration),"Joe's calibration with a reference eye height");
        return session;
    };
    const auto now=time_point_cast<steady_clock::duration>(steady_clock::time_point{seconds(300)});
    // Joe today: 0.9 units/m, +0.05 m, -0.2 m. Game-unit expectations every player must meet.
    const float eye_y=0.05f*0.9f,eye_z=0.2f*0.9f;
    for(const float height:{1.45f,JOE,1.9f}) {
        Backend* fake{};auto session=session_for(fake);session->poll(now);uint64_t frame{};
        auto p=place(*session,*fake,height,frame,now);
        require(close(p.units,0.9f) && !session->body_scale().player_eye_height,"before any explicit recenter: factor 1, Joe's scale");
        session->recenter();p=place(*session,*fake,height,frame,now);
        const float factor=JOE/height;
        require(session->body_scale().player_eye_height==height && close(session->body_scale().factor,factor),"an explicit recenter measures the eye height");
        require(close(p.units,0.9f*factor),"first-person scale is Joe's times reference / player");
        require(close(p.eye_y*p.units,eye_y) && close(p.eye_z*p.units,eye_z),"the eye keeps Joe's game-unit place in the helmet");
        for(int j=0;j<3;++j) require(close(p.hand[j]*p.units,(joe_hand[j]+(j==1?0.05f:0)+(j==2?0.2f:0))*0.9f),
            "a proportional hand lands where Joe's does on the Marine");
        require(close(height*p.units,JOE*0.9f),"the floor sits at Joe's game-unit depth below the eyes");
        if(height==JOE) require(close(p.units,0.9f) && close(p.eye_y,0.05f) && close(p.eye_z,0.2f),"at the reference height nothing changes");
    }
    // Readings within one launch: crouch rejected, small slouch accepted, automatic resets ignored.
    Backend* fake{};auto session=session_for(fake);session->poll(now);uint64_t frame{};
    session->recenter();place(*session,*fake,1.45f,frame,now);
    session->recenter();auto p=place(*session,*fake,1.25f,frame,now);
    auto body=session->body_scale();
    require(body.player_eye_height==1.45f && body.last==VrBodyScale::Reading::crouched && body.rejected==1 && close(p.units,0.9f*JOE/1.45f),
        "a recenter 0.2 m below the launch's highest keeps the height and is reported");
    session->recenter();place(*session,*fake,1.38f,frame,now);
    require(session->body_scale().player_eye_height==1.38f,"a slouch within 0.15 m is accepted, down as well as up");
    session->recenter();place(*session,*fake,1.60f,frame,now);
    require(session->body_scale().player_eye_height==1.60f,"a taller reading is accepted");
    session->recenter();place(*session,*fake,1.42f,frame,now);
    require(session->body_scale().player_eye_height==1.60f && session->body_scale().last==VrBodyScale::Reading::crouched,"the highest reading sets the crouch limit");
    session->recenter();place(*session,*fake,0.8f,frame,now);
    require(session->body_scale().player_eye_height==1.60f && session->body_scale().last==VrBodyScale::Reading::implausible,"sitting on the floor is not a height");
    const auto readings=session->body_scale().readings;
    session->reset_reference();place(*session,*fake,1.70f,frame,now);
    fake->valid=false;session->eye({uint32_t(++frame),frame,0},now);fake->valid=true;place(*session,*fake,1.70f,frame,now);
    require(session->body_scale().player_eye_height==1.60f && session->body_scale().readings==readings,"automatic resets and tracking loss never measure");
    // Left A held for a second is a player-initiated recenter.
    fake->controller_sample=VrControllerSample{};VrControllerInput input;input.enable(true);
    for(int ms=0;ms<=1100;ms+=20) {
        // Released for the first frames: a button already held when input arms must be released first.
        const auto time=now+milliseconds(ms);fake->controller_sample->recenter=ms>=60;fake->pose.head[13]=1.65f;
        session->eye({uint32_t(++frame),frame,int(frame%2)},time);session->poll_controllers(input,true,time,0,true);
    }
    fake->controller_sample->recenter=false;place(*session,*fake,1.65f,frame,now+milliseconds(1120));
    require(session->body_scale().player_eye_height==1.65f && session->body_scale().readings==readings+1,"Left A hold measures");
    // Third person keeps its own scale.
    auto view=session->view_settings();view.mode=VrViewMode::shoulder;require(session->set_view_settings(view),"over-shoulder view");
    ++frame;const auto third=session->eye({uint32_t(frame),frame,int(frame%2)},now+milliseconds(1120));
    require(third && close(third->units_per_metre,0.9f) && close(session->body_scale().factor,1),"body scale is first person only");
    {   // Third person, seated: a recenter references the head where it is and measures nothing.
        const auto before=session->body_scale();
        session->recenter();fake->pose.head[13]=1.1f;++frame;
        const auto seated=session->eye({uint32_t(frame),frame,int(frame%2)},now+milliseconds(1120));
        require(seated && close(seated->relative_eye[13],0),"a third-person recenter puts the eye on the camera at the seated head");
        require(session->body_scale().readings==before.readings && session->body_scale().player_eye_height==before.player_eye_height,"a third-person recenter takes no eye-height reading");
        // A world scale step is an automatic reference: it stays at the head too, not at standing height.
        auto scaled=view;scaled.mode=VrViewMode::diorama;require(session->set_view_settings(scaled),"diorama");
        ++frame;const auto step=session->eye({uint32_t(frame),frame,int(frame%2)},now+milliseconds(1120));
        require(step && close(step->relative_eye[13],0) && close(step->units_per_metre,9),"an automatic third-person reference stays at the head");
        require(session->body_scale().readings==before.readings,"nor does it measure");
        require(session->set_view_settings(view),"back to the shoulder view");
    }
    // No reference eye height in the calibration: no body scale at all.
    auto plain=joe_calibration;plain.reference_eye_height_metres.reset();view.mode=VrViewMode::first_person;
    require(session->set_view_settings(view) && session->set_calibration(plain),"calibration without a reference");
    p=place(*session,*fake,1.65f,frame,now+milliseconds(1120));
    require(close(p.units,0.9f) && close(p.eye_y,0.05f),"absent reference: factor 1");
}
// The launch's first reference is taken wherever the headset is (a desk, the player's hands):
// an automatic reference stands at the eye height, and putting the headset on references again.
void standing_reference_session() {
    using namespace std::chrono;
    constexpr float JOE=1.644f,DESK=0.9f;
    const VrCalibration joe_calibration{0.9f,0.05f,-0.2f,0.2f,0.25f,JOE};
    const auto now=time_point_cast<steady_clock::duration>(steady_clock::time_point{seconds(300)});
    uint64_t frame{};
    const auto eye_at=[&](VrSession& session,Backend& fake,float x,float y) {
        fake.pose.head[12]=x;fake.pose.head[13]=y;fake.pose.head[14]=0;
        ++frame;const auto eye=session.eye({uint32_t(frame),frame,int(frame%2)},now);
        require(eye.has_value(),"standing reference fixture frame issued");
        return *eye;
    };
    const auto session_for=[&](Backend*& fake,const VrCalibration& calibration) {
        auto backend=std::make_unique<Backend>();fake=backend.get();fake->present=true;
        auto session=std::make_unique<VrSession>(std::move(backend));session->enable(true);
        require(session->set_calibration(calibration),"standing reference calibration");session->poll(now);
        return session;
    };
    {   // Loaded with the headset on the desk, then put on standing: no reset needed.
        Backend* fake{};auto session=session_for(fake,joe_calibration);
        eye_at(*session,*fake,0,DESK);
        const auto standing=eye_at(*session,*fake,0,JOE);
        require(close(standing.relative_eye[13],0.05f),"a reference taken at desk height still puts the standing eye at the native height");
        require(close(eye_at(*session,*fake,0,JOE-0.5f).relative_eye[13],0.05f-0.5f),"crouching after load lowers the eye");
        require(session->body_scale().readings==0 && !session->body_scale().player_eye_height,"the automatic reference measures nothing");
        // A measured player: automatic resets stand at their measured height.
        session->recenter();eye_at(*session,*fake,0,1.5f);
        const auto measured=eye_at(*session,*fake,0,1.5f).relative_eye[13];
        session->reset_reference();eye_at(*session,*fake,0,DESK);
        require(close(eye_at(*session,*fake,0,1.5f).relative_eye[13],measured) && session->body_scale().player_eye_height==1.5f,
            "an automatic reset at desk height stands at the measured height");
        // A player-initiated recenter still references where the head is, even when rejected.
        session->recenter();eye_at(*session,*fake,0,1.1f);
        require(session->body_scale().last==VrBodyScale::Reading::crouched && close(eye_at(*session,*fake,0,1.1f).relative_eye[13],measured),
            "a crouched recenter references the crouched head");
    }
    {   // No reference eye height in the calibration: the reference is the head, as before.
        auto plain=joe_calibration;plain.reference_eye_height_metres.reset();
        Backend* fake{};auto session=session_for(fake,plain);
        eye_at(*session,*fake,0,DESK);
        require(close(eye_at(*session,*fake,0,JOE).relative_eye[13],0.05f+JOE-DESK),"without a known eye height the headset's own height is the reference");
    }
    {   // Putting the headset on references again, without measuring.
        Backend* fake{};auto session=session_for(fake,joe_calibration);
        fake->pose.worn=false;eye_at(*session,*fake,0.5f,DESK);
        const auto off=session->epoch();
        require(close(eye_at(*session,*fake,0,JOE).relative_eye[12],-0.5f),"while off, the reference stays where it was taken");
        fake->pose.worn=true;const auto on=eye_at(*session,*fake,0,JOE);
        require(session->epoch()>off && session->statistics().worn_references==1 && close(on.relative_eye[12],0) && close(on.relative_eye[13],0.05f),
            "putting the headset on references where it is worn");
        require(session->body_scale().readings==0,"putting the headset on does not measure");
        eye_at(*session,*fake,0.3f,JOE);fake->pose.worn.reset();eye_at(*session,*fake,0.3f,JOE);fake->pose.worn=true;
        require(close(eye_at(*session,*fake,0.3f,JOE).relative_eye[12],0.3f) && session->statistics().worn_references==1,
            "staying on, or a runtime that cannot tell (the dashboard), keeps the reference");
        fake->pose.worn=false;eye_at(*session,*fake,0.3f,JOE);fake->pose.worn=true;
        require(close(eye_at(*session,*fake,0.3f,JOE).relative_eye[12],0) && session->statistics().worn_references==2,"off and on again references again");
    }
    {   // Already worn at the first frame: nothing to take again.
        Backend* fake{};auto session=session_for(fake,joe_calibration);
        fake->pose.worn=true;eye_at(*session,*fake,0,JOE);eye_at(*session,*fake,0,JOE);
        require(session->statistics().worn_references==0,"a headset worn from the start is not referenced twice");
    }
    require(openvr_conversion::worn(vr::k_EDeviceActivityLevel_UserInteraction)==true && openvr_conversion::worn(vr::k_EDeviceActivityLevel_Standby)==false &&
        !openvr_conversion::worn(vr::k_EDeviceActivityLevel_Idle) && !openvr_conversion::worn(vr::k_EDeviceActivityLevel_UserInteraction_Timeout) &&
        !openvr_conversion::worn(vr::k_EDeviceActivityLevel_Unknown),"OpenVR wear: interaction on, standby off, the rest unknown");
}
// Joe, 26 October 2026: the eyes sit 5 cm higher in the armour (height offset 0.05 -> 0.10 m).
// The shipped preset puts a standing player's eye 0.10 m above the native camera and raises
// the hands by the same amount, so they stay registered to the eyes.
void shipped_eye_height() {
    using namespace std::chrono;
    const auto shipped=VrCalibration::load(std::filesystem::path{__FILE__}.parent_path().parent_path()/"config"/"index.calibration");
    require(shipped && shipped->reference_eye_height_metres,"shipped preset loads with a reference eye height");
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
    const float standing=*shipped->reference_eye_height_metres;
    fake->pose.head[13]=standing;
    auto hand=vr_camera::IDENTITY;hand[12]=-0.3f;hand[13]=standing-0.5f;hand[14]=-0.5f;
    fake->pose.controllers={hand,std::nullopt};
    const auto now=time_point_cast<steady_clock::duration>(steady_clock::time_point{seconds(100)});
    VrSession session{std::move(backend)};session.enable(true);
    require(session.set_calibration(*shipped),"shipped preset applies");session.poll(now);
    const auto eye=session.eye({1,1,0},now);const auto hands=session.controller_poses(now);
    require(eye && hands && hands->relative[0],"shipped preset fixture frame issued");
    require(close(eye->relative_eye[13]*eye->units_per_metre,0.09f),"the standing eye sits 0.09 game units above the native camera");
    require(close((*hands->relative[0])[13]-eye->relative_eye[13],-0.5f),"the hands rise with the eye");
    // Measured on the 31 Oct 18:39 run (docs/BODY_SCALE.md): the native camera 2.30 units over the
    // Marine's root, his wrists 2.358 units apart at full stretch, Joe's controllers 1.42 m.
    constexpr float NATIVE_CAMERA=2.30f,MARINE_SPAN=2.358f,JOE_SPAN=1.42f;
    hand[13]=0;fake->pose.controllers={hand,std::nullopt};
    const auto floor_eye=session.eye({2,2,1},now);const auto floor_hands=session.controller_poses(now);
    require(floor_eye && floor_hands && floor_hands->relative[0],"shipped preset floor frame issued");
    const float landed=NATIVE_CAMERA+(*floor_hands->relative[0])[13]*floor_hands->units_per_metre;
    require(std::abs(landed)<0.03f,"a hand on the real floor lands on the game's ground, not at the Marine's knee");
    require(JOE_SPAN*floor_eye->units_per_metre>=.85f*MARINE_SPAN,"Joe's arms out reach most of the Marine's span");
}
void first_person_scale_session() {
    using namespace std::chrono;
    constexpr float JOE=1.644f,UNITS=0.9f,FIRST=1.45f;
    VrCalibration calibration{UNITS,0.1f,-0.2f,0.2f,0.25f,JOE,FIRST};
    auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;fake->pose.head[13]=JOE;
    for(size_t side=0;side<2;++side) {auto hand=vr_camera::IDENTITY;hand[12]=side?.3f:-.3f;hand[13]=JOE-.5f;hand[14]=-.5f;fake->pose.controllers[side]=hand;}
    const auto now=time_point_cast<steady_clock::duration>(steady_clock::time_point{seconds(400)});
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    require(session.set_calibration(calibration),"first-person scale calibration");session.poll(now);
    uint64_t frame{};
    const auto frame_at=[&](int slot) {++frame;const auto eye=session.eye({uint32_t(frame),frame,slot},now);require(eye.has_value(),"first-person scale fixture frame issued");return *eye;};
    auto left=frame_at(0);const auto right=frame_at(1);auto hands=session.controller_poses(now);
    require(hands && hands->relative[0],"first-person scale fixture hands");
    require(close(left.units_per_metre,FIRST) && close(left.fit_factor,FIRST/UNITS) && close(left.marine_units_per_metre(),UNITS),"the eyes take the first-person scale, the Marine keeps his");
    require(close(left.relative_eye[13]*left.units_per_metre,0.1f*UNITS) && close(left.relative_eye[14]*left.units_per_metre,0.2f*UNITS),"the eye keeps its game-unit place in the helmet");
    require(close((right.relative_eye[12]-left.relative_eye[12])*left.units_per_metre,0.064f*FIRST),"stereo separation scales with the eyes");
    require(hands->units_per_metre==left.units_per_metre && hands->fit_factor==left.fit_factor,"hands and eyes share one scale");
    require(close((*hands->relative[0])[13]-left.relative_eye[13],-.5f) && close((*hands->relative[0])[12],-.3f),"the hands stay registered to the eyes");
    const auto body=session.body_scale();
    require(close(body.factor,1) && close(body.units_per_metre,FIRST),"zones stay in Joe's metres: the first-person scale is not a body factor");
    // A shorter player keeps the floor and the helmet place: the factors multiply.
    fake->pose.head[13]=1.45f;session.recenter();left=frame_at(0);
    require(close(left.units_per_metre,FIRST*JOE/1.45f) && close(left.marine_units_per_metre(),UNITS),"body scale multiplies the first-person scale");
    require(close(1.45f*left.units_per_metre,JOE*FIRST),"the floor sits at Joe's game-unit depth below the eyes");
    require(close(left.relative_eye[13]*left.units_per_metre,0.1f*UNITS),"the helmet place survives both factors");
    // Saved live: a new reference at the new scale. Without the field, the old behaviour.
    const auto epoch=session.epoch();calibration.first_person_units_per_metre=1.3f;
    require(session.set_calibration(calibration) && session.epoch()>epoch,"a new first-person scale takes a new reference");
    left=frame_at(0);require(close(left.units_per_metre,1.3f*JOE/1.45f),"the new scale applies at once");
    calibration.first_person_units_per_metre.reset();require(session.set_calibration(calibration),"no first-person scale");
    left=frame_at(0);require(close(left.units_per_metre,UNITS*JOE/1.45f) && close(left.fit_factor,JOE/1.45f),"absent: the calibration's scale and the body factor only");
    // Third person never sees it.
    calibration.first_person_units_per_metre=FIRST;require(session.set_calibration(calibration),"first-person scale again");
    auto view=session.view_settings();view.mode=VrViewMode::shoulder;require(session.set_view_settings(view),"over-shoulder view");
    left=frame_at(0);require(close(left.units_per_metre,UNITS) && left.fit_factor==1,"the shoulder view keeps the Marine's scale");
    view.mode=VrViewMode::diorama;require(session.set_view_settings(view),"diorama");
    left=frame_at(0);require(close(left.units_per_metre,UNITS*10) && left.fit_factor==1,"the diorama keeps its scale");
    view.mode=VrViewMode::first_person;require(session.set_view_settings(view),"first person again");
    // Native sprint speed is the game's: the eyes' scale must not hide a confirmed sprint.
    constexpr float RUN=5.8f*UNITS,SPRINT=8.6f*UNITS;
    fake->pose.head[13]=JOE;fake->controller_sample=VrControllerSample{};auto& c=*fake->controller_sample;VrControllerInput input;input.enable(true);
    const auto step=[&](int ms,float speed){++frame;const auto time=now+milliseconds(ms);session.eye({uint32_t(frame),frame,int(frame%2)},time);session.poll_controllers(input,true,time,0,false,std::nullopt,true,speed);};
    int ms=0;for(;ms<100;ms+=10) step(ms,0);
    c.move_y=1;for(;ms<400;ms+=10) step(ms,RUN);
    c.sprint=true;step(ms,RUN);ms+=10;c.sprint=false;
    for(;ms<900;ms+=10) step(ms,SPRINT);
    require(session.sprint_camera(now+milliseconds(ms)).confirmed,"native sprint confirms at the Marine's scale");
}
int main() {
    try {
        {
            auto backend=std::make_unique<Backend>();auto* fake=backend.get();fake->present=true;
            VrSession shared{std::move(backend)};
            const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(100)};
            shared.enable(true);shared.poll(now);shared.set_stereo(true);
            const auto ui=shared.shared_eye({1,1,0},now);
            fake->pose.head[12]=0.5f;
            const auto scene=shared.shared_eye({1,1,0},now+std::chrono::milliseconds(1));
            require(ui && scene && ui->relative_eye==scene->relative_eye && ui->captured==scene->captured && fake->trackings==1,
                "marker and scene consumers must share exactly one runtime pose per frame");
            require(!shared.eye({1,1,0},now),"legacy one-shot eye interface must retain strict duplicate refusal");
            require(!shared.shared_eye({1,1,1},now) && !shared.shared_eye({1,1,0,0,true},now),"shared provider must refuse wrong eye and invalid frame");
            require(!shared.shared_eye({1,1,0},now-std::chrono::milliseconds(1)) && !shared.shared_eye({1,1,0},now+std::chrono::milliseconds(251)),"future/stale shared poses must not be reused");
            require(shared.shared_eye({2,2,1},now).has_value() && fake->trackings==2,"next frame must sample fresh tracking");
            require(shared.shared_eye({1,1,0},now).has_value() && fake->trackings==2,"overlapping render/UI consumers may retrieve their exact retained older frame");
            shared.recenter();require(!shared.shared_eye({1,1,0},now),"recenter must discard previously shared frames");
            require(shared.shared_eye({3,3,0},now).has_value(),"post-recenter frame must receive a fresh reference");
            shared.set_scale(0.9f);require(!shared.shared_eye({3,3,0},now),"calibration must invalidate shared poses");
            for(uint64_t id=4;id<=13;++id) require(shared.shared_eye({uint32_t(id),id,int(id%2)},now).has_value(),"bounded cache fixture frame must sample");
            require(!shared.shared_eye({4,4,0},now),"retired frame must not be resampled under a reused identity");
            shared.enable(false);require(!shared.shared_eye({13,13,1},now),"disabled session must not supply cached poses");
        }
        conversions();third_person_session();left_a_button();touch_x_button();rumble_session();viewer_mode_session();eye_source_diagnostic();
        body_scale_session();
        shipped_eye_height();
        first_person_scale_session();
        standing_reference_session();
        physical_melee_session();
        trigger_gated_melee_and_holsters_session();
        weapon_controls_session(false);weapon_controls_session(true);stereo_pause_keeps_controls(false);stereo_pause_keeps_controls(true);
        primary_support_belt_ownership_session(false);primary_support_belt_ownership_session(true);
        sword_attack_modes_session(false);sword_attack_modes_session(true);
        left_hand_gestures_session(false);left_hand_gestures_session(true);guard_guide_session(false);guard_guide_session(true);grenade_aim_follows_snap_and_head();melee_hand_arbitration_session();
        controller_poses();
        calibration_files();
        sprint_camera_session();
        {
            auto placement_backend=std::make_unique<Backend>(); auto* placement_fake=placement_backend.get(); placement_fake->present=true;
            for (size_t hand=0;hand<2;++hand) {
                auto pose=vr_camera::IDENTITY; pose[12]=hand==0 ? -0.3f : 0.3f; pose[13]=1.2f; pose[14]=-0.4f;
                placement_fake->pose.controllers[hand]=pose;
            }
            VrSession placement{std::move(placement_backend)}; placement.enable(true); placement.poll(); placement.set_stereo(true);
            require(placement.set_calibration({0.9f,0.05f,-0.2f}),"new head placement must apply through the production session");
            auto left=placement.eye({1,1,0}); auto right=placement.eye({2,2,1}); auto hands=placement.controller_poses();
            require(left && right && close(left->relative_eye[14],0.2f) && close(right->relative_eye[14],0.2f) &&
                close(left->relative_eye[13],0.05f) && close(right->relative_eye[12]-left->relative_eye[12],0.064f),
                "placement must retract both eyes equally, raise height and preserve native IPD");
            require(hands && hands->relative[0] && close((*hands->relative[0])[14],-0.2f) && close((*hands->relative[0])[13],-0.45f),
                "controller origins must share eye placement correction to preserve hand-to-head alignment");
            placement_fake->pose.head[0]=-1; placement_fake->pose.head[10]=-1;
            for (auto& pose:placement_fake->pose.controllers) { (*pose)[12]*=-1; (*pose)[14]*=-1; }
            left=placement.eye({3,3,0}); hands=placement.controller_poses();
            require(left && close(left->relative_eye[14],-0.2f) && hands && close((*hands->relative[0])[14],0.2f),
                "half-turn placement must follow tracked yaw for eyes and hands instead of frozen recenter axes");
            placement_fake->pose.head={1,0,0,0, 0,0,1,0, 0,-1,0,0, 0,1.7f,0,1};
            left=placement.eye({4,4,0});
            require(left && close(left->relative_eye[14],0.2f) && close(left->relative_eye[13],0.05f),
                "vertical gaze must retain a level forward correction without moving viewpoint up/down along gaze");
            const auto previous=placement.calibration();
            require(!placement.set_calibration({0.9f,0.05f,0.6f}) && placement.calibration()==previous,
                "invalid live forward placement must preserve all active calibration fields");
        }
        {
            auto horizon_backend = std::make_unique<Backend>(); auto* horizon_fake = horizon_backend.get();
            horizon_fake->present = true;
            const float c = std::cos(0.174532925f),s = std::sin(0.174532925f);
            const vr_camera::Matrix roll{c,s,0,0, -s,c,0,0, 0,0,1,0, 0,0,0,1};
            const vr_camera::Matrix yaw{0,0,-1,0, 0,1,0,0, 1,0,0,0, 0,0,0,1};
            horizon_fake->pose.head = vr_camera::multiply(roll,yaw);
            horizon_fake->pose.head[12] = 3; horizon_fake->pose.head[13] = 1.7f; horizon_fake->pose.head[14] = 4;
            const auto raw = horizon_fake->pose.head;
            VrSession horizon{std::move(horizon_backend)}; horizon.enable(true); horizon.poll();
            const auto tilted = horizon.eye({1,1,0});
            require(tilted && tilted->runtime_head == raw && tilted->relative_eye[1] > 0.17f,
                "recenter must retain current physical head roll and unchanged raw compositor pose");
            horizon_fake->pose.head = yaw;
            horizon_fake->pose.head[12] = 3; horizon_fake->pose.head[13] = 1.7f; horizon_fake->pose.head[14] = 4;
            const auto upright = horizon.eye({2,2,1});
            vr_camera::Matrix world{},view{},game = roll; game[10] = -1;
            require(upright && vr_camera::compose(game,upright->relative_eye,0.9f,world,view) &&
                std::abs(world[4]) < 1e-6f && std::abs(world[5]-1) < 1e-6f && std::abs(world[6]) < 1e-6f,
                "returning upright after tilted recenter must level floor even with rolled game camera");
            horizon.recenter();
            require(horizon.eye({3,3,0})->relative_eye == vr_camera::IDENTITY,
                "upright recenter must remove yaw and translation without residual pitch/roll");
        }
        {
            auto mono_backend = std::make_unique<Backend>(); mono_backend->present = true;
            VrSession mono{std::move(mono_backend)}; mono.enable(true); mono.poll();
            const auto central = mono.eye({10,1,0});
            require(central && central->relative_eye[12] == 0 && central->relative_eye[13] == 0,
                "default runtime mode must use central recentered head pose before stereo validation");
            const auto epoch = mono.epoch();
            require(mono.set_scale(2) && mono.epoch() > epoch && !mono.set_scale(0),"scale must validate and invalidate previous calibration");
            const auto scaled = mono.eye({11,2,1});
            require(scaled && scaled->units_per_metre == 2,"pose provider must publish selected game units per metre");
            const auto prior_epoch = mono.epoch();
            require(mono.set_calibration({0.9f,-0.3f}) && mono.epoch() > prior_epoch,"height and scale changes must invalidate old images atomically");
            const auto calibrated = mono.eye({12,3,0});
            vr_camera::Matrix world{},view{};
            require(calibrated && vr_camera::compose(vr_camera::IDENTITY,calibrated->relative_eye,calibrated->units_per_metre,world,view) &&
                std::abs(world[13]+0.27f) < 1e-6f && calibrated->runtime_head[13] == 1.7f,
                "calibration must lower actual game camera by physical height times scale without changing submitted absolute HMD pose");
            const auto calibrated_epoch = mono.epoch();
            require(mono.set_calibration(mono.calibration()) && mono.epoch() == calibrated_epoch && !mono.set_calibration({1,2}) &&
                mono.calibration() == VrCalibration{0.9f,-0.3f},"unchanged hot reload must not recenter; invalid input must preserve active values");
            require(mono.set_calibration({}) && mono.eye({13,4,1})->relative_eye[13] == 0,"reset must restore original height through the real pose provider");
        }
        {
            auto rig_backend = std::make_unique<Backend>(); auto* rig_fake = rig_backend.get(); rig_fake->present = true;
            VrSession rig{std::move(rig_backend)}; rig.enable(true); rig.poll();
            VrControllerInput input; input.enable(true);
            const auto moment = std::chrono::steady_clock::now();
            require(rig.eye({1,1,0},moment).has_value(),"rig must establish head reference");
            rig_fake->pose.head[12] = 0.2f;
            const auto leaned = rig.eye({2,2,1},moment);
            require(leaned && leaned->relative_eye[12] == 0.2f,"lean must remain physical before turning");
            rig_fake->controller_sample = VrControllerSample{};
            rig.poll_controllers(input,true,moment);
            rig_fake->controller_sample->turn_x = 1;
            const auto epoch = rig.epoch();
            rig.poll_controllers(input,true,moment);
            const auto turned = rig.eye({3,3,0},moment);
            require(turned && leaned && turned->reference_epoch==leaned->reference_epoch &&
                turned->relative_head==turned->relative_eye,"snap must keep the body/reference origin while publishing central head rotation");
            require(turned && rig.epoch() > epoch && std::abs(turned->relative_eye[12]-0.2f) < 1e-6f &&
                std::abs(turned->relative_eye[14]) < 1e-6f && turned->relative_eye[8] < -0.49f,
                "snap must rotate actual eye orientation around leaned head and invalidate preceding images");
            rig_fake->controller_sample->move_y = 1;
            rig.poll_controllers(input,true,moment);
            require(std::abs(input.snapshot(moment).pad.Gamepad.sThumbLX-16384) <= 1,
                "forward walking after snap must follow the camera's new heading");
            rig.poll_controllers(input,true,moment,-30*.01745329252f);
            require(std::abs(input.snapshot(moment).pad.Gamepad.sThumbLX)<=1 && input.snapshot(moment).pad.Gamepad.sThumbLY==32767,
                "native camera yaw correction combines with current snap yaw before publishing the real pad");
            rig_fake->valid = false; rig.eye({4,4,1},moment); rig.poll_controllers(input,true,moment);
            require(input.snapshot(moment).pad.Gamepad.sThumbLY == 0 && !input.snapshot(moment).armed,
                "session tracking loss must release locomotion through the production input poll");
            rig_fake->valid = true; rig.eye({5,5,0},moment);
            rig_fake->controller_sample = VrControllerSample{}; rig.poll_controllers(input,true,moment);
            rig_fake->pose.head[12] += 0.3f; rig.eye({6,6,1},moment);
            rig_fake->controller_sample->recenter = true; const auto pre_recenter = rig.epoch();
            rig.poll_controllers(input,true,moment);
            require(rig.epoch()==pre_recenter,"pressing Left A does not recenter until it has been held");
            // Left A held for one second recenters (a tap would use the ability instead).
            const auto held = moment+std::chrono::milliseconds(1000);
            uint64_t id = 7;
            for(auto t = moment+std::chrono::milliseconds(50); t <= held; t += std::chrono::milliseconds(50),++id) {
                rig.eye({uint32_t(id),id,int(id%2)},t); rig.poll_controllers(input,true,t);
                if(t < held) require(rig.epoch()==pre_recenter,"holding Left A under a second keeps the current reference");
            }
            const auto centered = rig.eye({uint32_t(id),id,int(id%2)},held); ++id;
            require(centered && turned && centered->reference_epoch>turned->reference_epoch,
                "recenter/loss must advance body reference identity separately from snap image epochs");
            require(centered && std::abs(centered->relative_eye[12]) < 1e-6f && rig.epoch() > pre_recenter &&
                input.snapshot(held).pad.Gamepad.wButtons == 0,"controller recenter must establish current head reference without emitting a game action");
            const auto held_epoch = rig.epoch(); rig.eye({uint32_t(id),id,int(id%2)},held+std::chrono::milliseconds(50)); rig.poll_controllers(input,true,held+std::chrono::milliseconds(50));
            require(rig.epoch() == held_epoch,"held recenter button must not repeatedly reset camera epochs");
        }
        Host gpu;
        ordinary_submission(gpu);menu_only_submission(gpu);screen_menu_over_stereo(gpu);menu_input_trace(gpu);pause_menu_left_b(gpu);menu_controls_coverage(gpu);
        lease_follows_completion_queue(gpu);submit_thread_session(gpu);
        skipped_transfers_recover(gpu);
        auto backend = std::make_unique<Backend>(); auto* fake = backend.get(); fake->gpu = &gpu;
        VrSession session{std::move(backend)};
        session.set_stereo(true);
        const auto base = std::chrono::steady_clock::now();
        session.enable(true); session.poll(base);
        require(fake->starts == 0 && !session.eye({10,1,0},base),"missing headset must never initialize runtime or alter camera");
        fake->present = true; const auto now = base+std::chrono::seconds(4); session.poll(now);
        require(fake->starts == 1,"available headset must start once");
        const auto left_eye = session.eye({10,1,0},now);
        require(left_eye && std::abs(left_eye->relative_eye[12]+0.032f) < 1e-6f && left_eye->relative_eye[13] == 0,
            "first tracking sample must recenter head height while preserving left IPD");
        fake->pose.head[12] = 0.1f;
        const auto right_eye = session.eye({11,2,1},now);
        require(right_eye && std::abs(right_eye->relative_eye[12]-0.132f) < 1e-6f && right_eye->fov->left == -0.9f,
            "right sample must carry movement, its eye transform and asymmetric frustum");
        require(!session.eye({11,2,1},now),"duplicate camera IDs must not fetch another tracking pose");
        auto red = gpu.color(1,0,0),blue = gpu.color(0,0,1);
        EyeTextureCapture capture;
        const auto lm = tracked_meta(*left_eye,11), rm = tracked_meta(*right_eye,12);
        check(capture.capture(gpu.device.Get(),gpu.queue.Get(),red.Get(),lm));
        check(capture.capture(gpu.device.Get(),gpu.queue.Get(),blue.Get(),rm));
        const auto right = capture.latest(1); wait(right->fence.Get(),right->fence_value);
        auto pair = *capture.pair();
        auto diagnostic = pair; diagnostic.left.metadata.tracked_stereo = false;
        require(!session.submit(diagnostic,gpu.queue.Get(),now) && fake->submissions == 0,"diagnostic images must never reach runtime");
        diagnostic = pair; diagnostic.left.metadata.runtime_head[12] += 1;
        require(!session.submit(diagnostic,gpu.queue.Get(),now),"image flags cannot authorize a pose the provider did not issue");
        diagnostic = pair; diagnostic.left.metadata.runtime_eye_to_head[12] += 1;
        require(!session.submit(diagnostic,gpu.queue.Get(),now),"a forged source eye offset must never reach the compositor");
        diagnostic = {};
        require(!session.submit(pair,gpu.queue.Get(),now+std::chrono::seconds(1)),"old image poses must never reach runtime");
        require(session.statistics().submitted_pairs == 0 && session.statistics().failed_transfers == 0,
            "rejected diagnostic, forged and stale pairs must not count as runtime transfers");
        check(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fake->transfer_gate)));
        require(session.submit(pair,gpu.queue.Get(),now+std::chrono::milliseconds(25)) && fake->submissions == 1,"ready tracked pair must submit with exact image metadata");
        const auto delivered = session.statistics();
        require(delivered.submitted_pairs == 1 && delivered.last_render_id == 2 && delivered.oldest_pose_age_ms == 25,
            "delivery statistics must count only accepted pairs and measure oldest source pose age at submission entry");
        require(!session.submit(pair,gpu.queue.Get(),now),"same pair must not submit twice");
        require(session.statistics().submitted_pairs == 1,"duplicate submissions must not inflate per-eye delivery rate");
        auto next_left = tracked_meta(*session.eye({12,3,0},now),13);
        auto next_right = tracked_meta(*session.eye({13,4,1},now),14);
        check(capture.capture(gpu.device.Get(),gpu.queue.Get(),red.Get(),next_left));
        check(capture.capture(gpu.device.Get(),gpu.queue.Get(),blue.Get(),next_right));
        pair = {}; // Only session now retains the runtime's first pair.
        auto overflow = lm; overflow.render_id = 5; overflow.game_token = 15;
        require(capture.capture(gpu.device.Get(),gpu.queue.Get(),red.Get(),overflow) == E_PENDING,
            "runtime transfer lease must prevent reuse after copy fence completion");
        check(fake->transfer_gate->Signal(1));
        const auto next_image = capture.latest(1); wait(next_image->fence.Get(),next_image->fence_value);
        session.poll(now); fake->transfer_gate.Reset();
        require(capture.capture(gpu.device.Get(),gpu.queue.Get(),red.Get(),overflow) == S_OK,"runtime completion must retire leases and permit pool reuse");
        auto retained = capture.pair();
        require(retained.has_value(),"completed pair must survive next left capture");
        session.recenter();
        require(!session.submit(*retained,gpu.queue.Get(),now),"recenter must invalidate pairs from prior tracking epoch");
        const auto recentered = session.eye({15,5,0},now);
        require(recentered && std::abs(recentered->relative_eye[12]+0.032f)<1e-6f,"explicit recenter must use current head as reference");
        fake->valid = false;
        require(!session.eye({16,6,1},now),"tracking loss must make camera passive");
        fake->valid = true;
        const auto restored = session.eye({17,7,0},now);
        require(restored && restored->tracking_epoch != recentered->tracking_epoch,"tracking recovery must invalidate old images");
        session.enable(false);
        require(fake->stops == 1 && !session.eye({18,8,1},now),"shutdown must stop runtime and suppress provider");
        session.enable(true); session.poll(now);
        EyeTextureCapture failing_capture;
        const auto failed_l = tracked_meta(*session.eye({19,9,0},now),20);
        const auto failed_r = tracked_meta(*session.eye({20,10,1},now),21);
        check(failing_capture.capture(gpu.device.Get(),gpu.queue.Get(),red.Get(),failed_l));
        check(failing_capture.capture(gpu.device.Get(),gpu.queue.Get(),blue.Get(),failed_r));
        const auto failed_image = failing_capture.latest(1); wait(failed_image->fence.Get(),failed_image->fence_value);
        fake->submit_ok = false;
        require(!session.submit(*failing_capture.pair(),gpu.queue.Get(),now) && fake->stops == 2 &&
            !session.eye({21,11,0},now),"partial runtime submission failure must stop tracking and invalidate pending poses");
        session.poll(now+std::chrono::seconds(10));
        require(fake->starts == 2,"failed submission must not automatically restart runtime");
        require(session.statistics().submitted_pairs == 1 && session.statistics().failed_transfers == 1 &&
            session.status().find("mock active") != std::string::npos,
            "failed transfers must preserve prior delivery count and expose backend failure detail");
        check(gpu.queue->Signal(gpu.fence.Get(),++gpu.tick)); wait(gpu.fence.Get(),gpu.tick);
        std::cout << "mock runtime with real WARP image transfer, tracking, recenter and lifetime checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
