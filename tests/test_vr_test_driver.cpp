#include "VrTestBackend.h"
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
using vr_test::Clock;
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
std::string program(const std::string& id,unsigned buttons=1,bool head=false){
    std::ostringstream out;out<<"SM2VR_TEST 1 "<<id<<(head?" scripted_head ":" live_head ")<<"1200 2\n";
    for(unsigned ms:{0u,1200u})out<<ms<<' '<<buttons<<" -1 0 0 0 0 0 0 0 0 0 -.25 -.35 -.45 0 0 0 .3 -.65 0 0 0 0 0 0 0 "<<(head?90:0)<<" 0 0\n";
    return out.str();
}
struct Fake final:VrBackend {
    Tracking pose;bool focus{true},valid{true},fallback{true},seed_available{};unsigned reads{};
    Fake(){pose.head[13]=1.7f;pose.eyes[0][12]=-.032f;pose.eyes[1][12]=.032f;
        pose.fov={vr_projection::Tangents{-1.1f,.9f,-.9f,1.1f},vr_projection::Tangents{-.9f,1.1f,-1.1f,.9f}};}
    bool available()override{return true;}bool start()override{return true;}void stop()override{}
    bool test_input_available()const override{return focus;}
    bool test_tracking_fallback_available()const override{return fallback;}
    std::optional<Tracking> test_tracking_seed()const override{return seed_available?std::optional{pose}:std::nullopt;}
    std::optional<Tracking> tracking()override{++reads;return valid?std::optional{pose}:std::nullopt;}
    bool submit(const EyeTextureCapture::Pair&,ID3D12CommandQueue*)override{return true;}
    std::string status()const override{return "fixture";}
    ID3D12CommandQueue* own{};bool frame_thread{};
    ID3D12CommandQueue* completion_queue(ID3D12CommandQueue* caller)override{return own?own:caller;}
    bool separate_frame_thread()const override{return frame_thread;}
};
int main(int argc,char** argv){try{
    require(argc==2,"fixture directory required");const std::filesystem::path dir{argv[1]};std::filesystem::create_directories(dir);
    auto now=Clock::now();auto backend=std::make_unique<Fake>();auto* real=backend.get();bool foreground=true;
    auto driver=std::make_unique<VrTestBackend>(std::move(backend),dir,false,[&]{return foreground;},[&]{return now;});auto* control=driver.get();
    {   // The decorator must not hide where the real backend's copies finish, nor whether it allows a submit thread.
        auto* game=reinterpret_cast<ID3D12CommandQueue*>(uintptr_t{0x1000}),*own=reinterpret_cast<ID3D12CommandQueue*>(uintptr_t{0x2000});
        require(control->completion_queue(game)==game && !control->separate_frame_thread(),"defaults follow the real backend");
        real->own=own;real->frame_thread=true;
        require(control->completion_queue(game)==own && control->separate_frame_thread(),"own-queue leases end on the real backend's queue under the test driver");
        real->own=nullptr;real->frame_thread=false;
    }
    require(!vr_test::parse(program("head",1,true),false),"live-head launch rejects synthetic head request");
    require(vr_test::parse(program("head",1,true),true).has_value(),"explicit unattended launch accepts head path");
    require(!vr_test::parse(program("bad")+"extra",false),"trailing tokens rejected");
    auto bad=program("bad");bad.replace(bad.find("1200 2"),6,"120001 2");require(!vr_test::parse(bad,false),"duration bounded");
    bad=program("bad");bad.replace(bad.find(".3 -.65"),2,"nan");require(!vr_test::parse(bad,false),"nonfinite pose rejected");
    auto parsed=*vr_test::parse(program("core"),false);vr_test::Timeline timeline;
    timeline.start(parsed,now,real->pose.head);auto out=timeline.sample(real->pose,now,true);
    require(!out.input.confirm && out.tracking.head==real->pose.head && out.tracking.eyes==real->pose.eyes,"preroll neutral and real head/optics unchanged");
    require(std::abs((*out.tracking.controllers[1])[13]-1.05f)<.00001f,"head-relative wrist pose reaches actual standing-space transform");
    for(unsigned t=100;t<=500;t+=100)out=timeline.sample(real->pose,now+std::chrono::milliseconds(t),true);
    require(out.input.confirm && out.input.move_x==-1,"scheduled actions start after neutral preroll");
    out=timeline.sample(real->pose,now+800ms,true);require(!out.input.confirm && timeline.state()=="clock_gap","stalled clock cancels rather than replaying stale actions");
    auto a=vr_test::Pose{},b=a;a[3]=170;b[3]=-170;auto blend=vr_test::interpolate(a,b,.5f);
    require(std::abs(blend[3]-180)<.001f && vr_camera::rigid(vr_test::matrix(blend)),"angle interpolation takes short arc and remains rigid");
    auto moved=real->pose;moved.head=vr_test::matrix({1,1.7f,2,90,0,0});timeline.start(parsed,now,moved.head);
    out=timeline.sample(moved,now,true);const auto hand=vr_camera::multiply(*out.tracking.controllers[1],vr_camera::inverse(moved.head));
    require(std::abs(hand[12]-.3f)<.00001f && std::abs(hand[13]+.65f)<.00001f,"hand offsets follow current head yaw without changing head pose");
    auto scripted=*vr_test::parse(program("head_path",0,true),true);scripted.frames[0].head[3]=0;
    timeline.start(scripted,now,real->pose.head);
    for(unsigned t=0;t<=1100;t+=100)out=timeline.sample(real->pose,now+std::chrono::milliseconds(t),true);
    require(out.tracking.head!=real->pose.head && std::abs(out.tracking.head[0]-.70710678f)<.001f && out.tracking.eyes==real->pose.eyes,"unattended head path interpolates 45 degrees with native optics intact");
    const auto virtual_head=out.tracking.head;timeline.cancel("stopped");out=timeline.sample(real->pose,now+1200ms,true);
    require(out.tracking.head==virtual_head && !out.input.confirm,"stopping scripted head holds its last pose rather than snapping back");
    // Real production session and mapper, driven through the real mailbox/backend.
    VrSession session{std::move(driver)};session.enable(true);session.set_stereo(true);session.set_render_path(VrSession::RenderPath::ordinary);session.enable_holsters(true);
    session.poll(now);VrControllerInput input;input.enable(true);uint64_t frame{};unsigned writes{};
    const auto write=[&](const std::string& text){std::ofstream file{dir/"request.txt"};file<<text;file.close();std::filesystem::last_write_time(dir/"request.txt",std::filesystem::file_time_type::clock::now()+std::chrono::seconds(++writes));};
    const auto tick=[&](unsigned ms){now+=std::chrono::milliseconds(ms);auto f=session.ordinary_frame({uint32_t(++frame),frame,0},now);session.poll_controllers(input,foreground,now,0,false);return f;};
    write(program("first"));
    for(int n=0;n<7;++n)require(tick(100).has_value(),"fresh same-frame snapshot remains valid");
    auto pad=input.snapshot(now);require(pad.armed && pad.pad.Gamepad.sThumbLX==-32767 && (pad.pad.Gamepad.wButtons&XINPUT_GAMEPAD_A),"file command reaches real session and native pad mapper");
    const auto reads=real->reads;control->controllers();control->controllers();require(real->reads==reads,"input reads reuse frozen tracking boundary");
    write("STOP stop1\n");tick(100);pad=input.snapshot(now);require(!pad.armed && !pad.pad.Gamepad.wButtons && !pad.pad.Gamepad.sThumbLX,"stop resets gestures and native actions on next frame");
    write(program("first"));tick(100);require(!input.snapshot(now).armed,"same id cannot silently restart");
    write(program("holster",0));for(int n=0;n<6;++n)tick(100);
    // New complete frame sequence squeezes the virtual right grip in the hip zone.
    write("STOP stop2\n");tick(100);auto holster=program("holster2",0);
    const auto needle=std::string{"0 -1 0 0 0 0 0 0 0 0 0"};const auto pos=holster.find(needle);require(pos!=std::string::npos,"fixture layout");
    holster.replace(pos,needle.size(),"0 0 0 0 0 0 1 0 0 0 0");write(holster);
    bool selected=false;for(int n=0;n<8;++n){tick(100);selected|=bool(input.snapshot(now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_DPAD_DOWN);require(!(input.snapshot(now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_DPAD_LEFT),"hip must never emote");}
    require(selected && session.holster_statistics().secondary==1,"virtual hip squeeze selects sidearm via native cycle");
    write("STOP fist_stop\n");tick(100);
    auto salute=program("raised_fist",0);
    salute.replace(salute.find(needle),needle.size(),"0 0 0 0 0 0 1 0 0 0 0");
    const auto last=salute.rfind(".3 -.65 0");salute.replace(last,9,".3 .6 0");write(salute); // Joe raised 0.49-0.72 m; the emote fires at 0.35
    selected=false;for(int n=0;n<17;++n){tick(100);selected|=bool(input.snapshot(now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_DPAD_LEFT);}
    require(selected && session.holster_statistics().salutes==1,"virtual closed fist raise reaches real gesture/session/native emote mapper");
    real->focus=false;tick(100);require(!input.snapshot(now).armed && !input.snapshot(now).pad.Gamepad.wButtons,"dashboard focus cancels and neutralizes");
    real->focus=true;tick(100);require(!input.snapshot(now).armed,"focus return never restarts old script");
    write(program("end"));for(int n=0;n<20;++n)tick(100);require(!input.snapshot(now).armed,"completion releases held action without extra neutral keyframe");
    write(program("desktop"));for(int n=0;n<7;++n)tick(100);foreground=false;tick(100);foreground=true;tick(100);
    require(!input.snapshot(now).armed,"desktop focus loss cancels script persistently");
    write(program("tracking"));for(int n=0;n<7;++n)tick(100);real->valid=false;tick(100);real->valid=true;tick(100);
    require(!input.snapshot(now).armed,"lost HMD cancels script persistently");
    write(program("malformed_stop"));for(int n=0;n<7;++n)tick(100);
    require(input.snapshot(now).armed,"malformed stop fixture begins with active controls");
    write("STOP\n");tick(100);
    require(!input.snapshot(now).armed && !input.snapshot(now).pad.Gamepad.wButtons,"malformed replacement stop still releases active controls");
    // The explicit unattended mode can run without a tracked HMD after native
    // optics were observed. Live-head mode and shutdown/origin reset still fail closed.
    std::filesystem::remove(dir/"request.txt");
    auto unattended_real=std::make_unique<Fake>();auto* fake=unattended_real.get();
    VrTestBackend unattended{std::move(unattended_real),dir,true,[&]{return foreground;},[&]{return now;}};
    fake->valid=false;write(program("no_calibration",1,true));now+=100ms;
    require(!unattended.tracking() && !unattended.controllers(),"untracked startup cannot fabricate native optics");
    std::filesystem::remove(dir/"request.txt");
    fake->valid=true;unattended.tracking();fake->valid=false;
    write(program("cached_script",1,true));
    for(int n=0;n<7;++n){now+=100ms;require(unattended.tracking().has_value(),"scripted head survives tracking loss with observed optics");}
    require(unattended.controllers() && unattended.controllers()->confirm && unattended.status().find("cached native optics")!=std::string::npos,"cached mode advances actions and is explicitly labelled");
    write("STOP cached_stop\n");now+=100ms;auto held=unattended.tracking();
    require(held && held->eyes==fake->pose.eyes && !unattended.controllers(),"stop releases actions while retaining scripted view and native optics");
    write(program("cached_live_head"));now+=100ms;unattended.tracking();
    require(!unattended.controllers(),"live-head request cannot arm using cached optics");
    write(program("cached_again",1,true));for(int n=0;n<7;++n){now+=100ms;unattended.tracking();}
    fake->focus=false;now+=100ms;require(!unattended.tracking() && !unattended.controllers(),"focus loss cancels even scripted cached mode");
    fake->focus=true;now+=100ms;require(!unattended.tracking(),"focus return cannot reuse invalidated calibration");
    fake->valid=true;now+=100ms;unattended.tracking();fake->valid=false;fake->fallback=false;
    now+=100ms;require(!unattended.tracking() && !unattended.controllers(),"runtime shutdown or origin change invalidates cached optics");
    fake->fallback=true;fake->seed_available=true;
    write(program("runtime_optics_seed",1,true));
    for(int n=0;n<7;++n){now+=100ms;require(unattended.tracking().has_value(),"explicit scripted mode accepts native optics with a synthetic standing origin");}
    require(unattended.controllers() && unattended.controllers()->confirm,"runtime optics seed advances real mailbox actions without physical tracking");
    write(program("seed_busy_live"));now+=100ms;unattended.tracking();
    require(!unattended.controllers(),"seeded mode still rejects a live-head request");
    std::filesystem::remove(dir/"request.txt");std::filesystem::remove(dir/"status.json");
    std::cout<<"live test driver parser, clock, real session, holsters and release gates PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
