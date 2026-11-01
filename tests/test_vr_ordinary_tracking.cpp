#include "VrSession.h"
#include "OrdinaryTrackedCamera.h"
#include "OrdinaryCameraCulling.h"
#include "fixtures/native_compact_frustum.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
bool close(float a,float b){return std::abs(a-b)<1e-5f;}
struct Fake final:VrBackend {
    unsigned starts{},stops{},trackings{},inputs{},submissions{};bool valid{true},throws{};
    Tracking pose;std::optional<VrControllerSample> input{VrControllerSample{}};
    Fake(){pose.head[13]=1.7f;pose.eyes[0][12]=-.032f;pose.eyes[1][12]=.032f;
        pose.fov={vr_projection::Tangents{-1.1f,.9f,-.9f,1.1f},vr_projection::Tangents{-.9f,1.1f,-1.1f,.9f}};
        for(size_t hand=0;hand<2;++hand){auto p=vr_camera::IDENTITY;p[12]=hand?.3f:-.3f;p[13]=1.2f;p[14]=-.5f;pose.controllers[hand]=p;pose.fingers[hand]=vr_finger::Curl{.2f,0,1,.8f,.9f};}}
    bool available()override{return true;}bool start()override{++starts;return true;}void stop()override{++stops;}
    std::optional<Tracking> tracking()override{++trackings;if(throws)throw std::runtime_error("tracking");return valid?std::optional{pose}:std::nullopt;}
    std::optional<VrControllerSample> controllers()override{++inputs;return input;}
    bool submit(const EyeTextureCapture::Pair&,ID3D12CommandQueue*)override{++submissions;return false;}
    std::string status()const override{return "synthetic ordinary tracking backend";}
};
using Clock=std::chrono::steady_clock;
const auto NOW=Clock::time_point{std::chrono::seconds(100)};
FrameSyncClock::Frame frame(uint64_t id,int parity=0){return {uint32_t(id+100),id,parity,0,false};}
struct Fixture {
    Fake* fake{};VrSession session;
    Fixture():session{[&]{auto b=std::make_unique<Fake>();fake=b.get();return b;}()}{session.enable(true);session.set_stereo(true);session.set_render_path(VrSession::RenderPath::ordinary);session.poll(NOW);}
};
void passive_until_explicit_start(){
    auto backend=std::make_unique<Fake>();auto* fake=backend.get();VrSession session{std::move(backend)};
    require(session.render_path()==VrSession::RenderPath::afr,"AFR remains the default render path");
    session.set_stereo(true);session.set_render_path(VrSession::RenderPath::ordinary);
    require(!session.ordinary_frame(frame(1),NOW) && !fake->starts && !fake->trackings,"ordinary pose request alone never enables or starts a runtime");
    session.enable(true);require(!session.ordinary_frame(frame(1),NOW) && !fake->starts,"enabled but unstarted session remains passive until explicit polling");
}
void one_frozen_sample(){
    Fixture f;const auto value=f.session.ordinary_frame(frame(1,1),NOW);require(value && value->valid_at(NOW),"true ordinary frame issues both eyes from one valid tracking sample");
    require(f.fake->starts==1 && f.fake->trackings==1 && f.fake->inputs==1 && f.fake->submissions==0,"fake backend sampled once without any runtime image submission");
    require(value->render_id==1 && value->game_token==101 && value->eyes[0].eye==0 && value->eyes[1].eye==1 && value->eyes[0].render_token==value->eyes[1].render_token,"both explicit slots carry the SAME native camera-job identity regardless of AFR parity");
    require(close(value->eyes[1].relative_eye[12]-value->eyes[0].relative_eye[12],.064f) && value->eyes[0].fov->left==-1.1f && value->eyes[1].fov->left==-.9f,"ordinary pair preserves real per-eye transform and asymmetric lens");
    require(value->controllers && value->controllers->captured==value->eyes[0].captured && value->controllers->tracking_epoch==value->eyes[1].tracking_epoch && value->controllers->relative[0] && close((*value->controllers->relative[0])[13],-.5f),"head, eyes, hands and fingers share one level reference, timestamp and epoch");
    f.fake->pose.head[12]=4;(*f.fake->pose.controllers[0])[12]=8;f.fake->input->fire=1;
    const auto cached=f.session.ordinary_frame(frame(1,-1),NOW+std::chrono::milliseconds(10));
    require(cached && cached->eyes[0].relative_eye==value->eyes[0].relative_eye && cached->input->fire==0 && f.fake->trackings==1 && f.fake->inputs==1,"same job reuses its immutable snapshot; neither eye nor input silently resamples later backend state");
    auto wrong=frame(1);wrong.token++;
    require(!f.session.ordinary_frame(wrong,NOW) && !f.session.eye(frame(2),NOW) && !f.session.shared_eye(frame(2),NOW),"different token and AFR APIs cannot relabel an ordinary snapshot");
    require(value->valid_at(NOW+std::chrono::milliseconds(100)) && !value->valid_at(NOW+std::chrono::milliseconds(101)) && !value->valid_at(NOW-std::chrono::milliseconds(1)),"ordinary camera/controller snapshot has one bounded freshness interval");
    require(!f.session.ordinary_frame(frame(1),NOW+std::chrono::milliseconds(101)) && f.fake->trackings==1,"expired job never resamples fresh tracking under an old native identity");
    const auto next=f.session.ordinary_frame(frame(2),NOW+std::chrono::milliseconds(102));require(next && next->eyes[0].runtime_head[12]==4 && next->input->fire==1 && f.fake->trackings==2,"only a new native job obtains the next full tracking/action snapshot");
}
void controllers_use_issued_frame(){
    Fixture f;VrControllerInput input;input.enable(true);
    require(bool(f.session.ordinary_frame(frame(1),NOW)),"neutral first job required");f.session.poll_controllers(input,true,NOW,0,false);
    require(input.snapshot(NOW).armed,"ordinary controls arm from a neutral frozen snapshot");
    f.fake->input->fire=1;f.session.poll_controllers(input,true,NOW+std::chrono::milliseconds(1),0,false);
    require(input.snapshot(NOW+std::chrono::milliseconds(1)).pad.Gamepad.bRightTrigger==0 && f.fake->inputs==1,"controller poll cannot mix later action state into an existing stereo job");
    require(bool(f.session.ordinary_frame(frame(2),NOW+std::chrono::milliseconds(2))),"new fire job issued");f.session.poll_controllers(input,true,NOW+std::chrono::milliseconds(2),0,false);
    require(input.snapshot(NOW+std::chrono::milliseconds(2)).pad.Gamepad.bRightTrigger==255 && f.fake->inputs==2,"native motion-control path consumes the new exact-job action snapshot");
    const auto hands=f.session.controller_poses(NOW+std::chrono::milliseconds(2));require(hands && hands->captured==NOW+std::chrono::milliseconds(2),"arm consumers read the same issued controller frame");
    f.session.poll_controllers(input,false,NOW+std::chrono::milliseconds(3));require(!input.snapshot(NOW+std::chrono::milliseconds(3)).armed && input.snapshot(NOW+std::chrono::milliseconds(3)).pad.Gamepad.bRightTrigger==0,"focus loss releases controls without resampling or backend calls");
    f.session.poll_controllers(input,true,NOW+std::chrono::milliseconds(103));require(!input.snapshot(NOW+std::chrono::milliseconds(103)).armed && !f.session.controller_poses(NOW+std::chrono::milliseconds(103)),"stale stereo snapshot releases input and both arm targets");
}
void invalidations_and_modes(){
    Fixture f;const auto first=*f.session.ordinary_frame(frame(1),NOW);const auto epoch=f.session.epoch();f.session.recenter();
    require(f.session.epoch()>epoch && !f.session.controller_poses(NOW) && !f.session.ordinary_frame(frame(1),NOW),"recenter clears both ordinary eyes/controllers and cannot reissue old IDs");
    const auto next=f.session.ordinary_frame(frame(2),NOW);require(next && next->eyes[0].reference_epoch>first.eyes[0].reference_epoch,"fresh ordinary job carries new recenter reference");
    require(f.session.set_calibration({.9f,.05f,-.2f}),"physical placement accepted");
    const auto placed=f.session.ordinary_frame(frame(3),NOW);require(placed && close(placed->eyes[0].relative_eye[13],.05f) && close(placed->eyes[0].relative_eye[14],.2f) && close((*placed->controllers->relative[0])[14],-.3f) && close(placed->controllers->units_per_metre,.9f),"both eyes/hands share placement once with raw metre coordinates and separate game scale");
    VrViewSettings settings;settings.mode=VrViewMode::diorama;require(f.session.set_view_settings(settings),"diorama mode accepted");
    const auto diorama=f.session.ordinary_frame(frame(4),NOW);require(diorama && !diorama->controllers && !diorama->input && close(diorama->eyes[0].units_per_metre,9),"third-person ordinary rendering excludes first-person controller actions and placement");
    require(!f.session.set_render_path(static_cast<VrSession::RenderPath>(99)) && f.session.render_path()==VrSession::RenderPath::ordinary,"invalid path preserves current working configuration");
    require(f.session.set_render_path(VrSession::RenderPath::afr) && !f.session.ordinary_frame(frame(5),NOW) && f.session.eye(frame(5),NOW),"explicit AFR return restores original one-eye path without casting ordinary frames");
    f.session.set_render_path(VrSession::RenderPath::ordinary);f.session.set_stereo(false);require(!f.session.ordinary_frame(frame(6),NOW),"mono diagnostics cannot satisfy true two-view tracking");
    f.session.set_stereo(true);require(bool(f.session.ordinary_frame(frame(6),NOW)),"explicit stereo reenable accepts a fresh native identity");
    f.session.enable(false);require(!f.session.controller_poses(NOW) && !f.session.ordinary_frame(frame(7),NOW),"shutdown clears ordinary image/hand/input eligibility");
}
void snap_and_recenter(){
    Fixture f;VrControllerInput input;input.enable(true);
    const auto first=*f.session.ordinary_frame(frame(1),NOW);f.session.poll_controllers(input,true,NOW);
    f.fake->input->turn_x=1;const auto before=*f.session.ordinary_frame(frame(2),NOW+std::chrono::milliseconds(20));
    f.session.poll_controllers(input,true,NOW+std::chrono::milliseconds(20));
    require(f.session.epoch()>before.eyes[0].tracking_epoch && !f.session.controller_poses(NOW+std::chrono::milliseconds(20)) &&
        !f.session.ordinary_frame(frame(2),NOW+std::chrono::milliseconds(20)),"snap discards both old ordinary eyes and hand/input cache immediately");
    f.fake->input->turn_x=0;const auto turned=*f.session.ordinary_frame(frame(3),NOW+std::chrono::milliseconds(40));
    f.session.poll_controllers(input,true,NOW+std::chrono::milliseconds(40));
    require(turned.eyes[0].reference_epoch==first.eyes[0].reference_epoch && turned.eyes[0].tracking_epoch>first.eyes[0].tracking_epoch &&
        close(turned.eyes[0].relative_head[2],.5f) && close((*turned.controllers->relative[0])[2],.5f),"new ordinary frame rotates both head and controller rig together while retaining physical reference");
    f.fake->input->recenter=true;const auto prior_epoch=f.session.epoch();require(bool(f.session.ordinary_frame(frame(4),NOW+std::chrono::milliseconds(60))),"recenter action captured with its job");
    f.session.poll_controllers(input,true,NOW+std::chrono::milliseconds(60));
    require(f.session.epoch()==prior_epoch,"pressing Left A alone does not recenter (a tap is the class ability)");
    // Left A held for one second recenters; each 20 ms job carries the held action.
    auto held=NOW+std::chrono::milliseconds(60);uint64_t job=4;
    while(held<NOW+std::chrono::milliseconds(1060)){held+=std::chrono::milliseconds(20);++job;
        require(bool(f.session.ordinary_frame(frame(job),held)),"held recenter action captured with its job");f.session.poll_controllers(input,true,held);}
    require(f.session.epoch()>prior_epoch && !f.session.controller_poses(held) && !input.snapshot(held).armed,"recenter action invalidates pair/controllers and releases native input in the same poll");
    const auto after=held+std::chrono::milliseconds(20);
    const auto fresh=*f.session.ordinary_frame(frame(job+1),after);const auto epoch=f.session.epoch();f.session.poll_controllers(input,true,after);
    require(f.session.epoch()==epoch && fresh.eyes[0].reference_epoch>turned.eyes[0].reference_epoch && close(fresh.eyes[0].relative_head[2],0),"held recenter does not repeat and the next ordinary pair uses a fresh shared reference");
}
void paired_owned_camera_sources(){
    Fixture f;const auto tracking=*f.session.ordinary_frame(frame(1),NOW);
    ordinary_tracked_camera::Source source;source.fill(0x5a);
    auto game=vr_camera::IDENTITY;game[0]=0;game[2]=-1;game[8]=1;game[10]=0;game[12]=30;game[13]=7;game[14]=9;
    const auto view=vr_camera::inverse(game);const float near_z=.1f,far_z=1500;
    std::memcpy(source.data(),game.data(),64);std::memcpy(source.data()+0x40,view.data(),64);
    std::memcpy(source.data()+0x80,&near_z,4);std::memcpy(source.data()+0x84,&far_z,4);
    const auto original=source;
    const auto pair=ordinary_tracked_camera::prepare(source,game,frame(1),tracking,NOW);
    require(pair && source==original,"paired source preparation never mutates borrowed game camera bytes");
    require(pair->render_id==1 && pair->game_token==101 && pair->eyes[0].tracking.eye==0 && pair->eyes[1].tracking.eye==1,"one native job has explicit eye slots instead of AFR parity");
    require(pair->eyes[0].projection!=pair->eyes[1].projection && close(pair->eyes[0].projection[8],.1f) && close(pair->eyes[1].projection[8],-.1f),"asymmetric left and right lenses remain distinct");
    require(close(pair->eyes[0].world[12],30) && close(pair->eyes[1].world[12],30) &&
        close(pair->eyes[0].world[14],9.032f) && close(pair->eyes[1].world[14],8.968f),"both eyes compose independently onto the one rotated native centre anchor");
    require(close(pair->eyes[0].rectangle[0],-.11f) && close(pair->eyes[1].rectangle[0],-.09f),"native culling rectangles use each actual eye lens");
    for(const auto& eye:pair->eyes){
        vr_camera::Matrix written_world{},written_view{};std::array<float,4> rectangle{};std::array<float,3> lens{};
        std::memcpy(written_world.data(),eye.source.data(),64);std::memcpy(written_view.data(),eye.source.data()+0x40,64);
        std::memcpy(rectangle.data(),eye.source.data()+0x12c,sizeof(rectangle));std::memcpy(lens.data(),eye.source.data()+0x2d0,sizeof(lens));
        require(written_world==eye.world && written_view==eye.view && rectangle==eye.rectangle && lens==eye.lens,"owned native source bytes match prepared pose and culling/lens metadata");
        uint32_t count{};std::array<float,8> native_corners{};
        std::memcpy(&count,eye.source.data()+0x88,4);
        std::memcpy(native_corners.data(),eye.source.data()+0x8c,sizeof(native_corners));
        const std::array<float,8> expected_corners{rectangle[0],rectangle[1],rectangle[0]+rectangle[2],rectangle[1],
            rectangle[0]+rectangle[2],rectangle[1]+rectangle[3],rectangle[0],rectangle[1]+rectangle[3]};
        require(count==4 && native_corners==expected_corners,"native visibility consumes four per-eye corners, not the copied desktop polygon");
        auto native=eye.source;native_compact_frustum(native.data());
        const auto corrected=ordinary_camera_culling::correct(native,eye);
        require(bool(corrected),"authenticated native compact frustum can be corrected");
        const auto radius=ordinary_camera_culling::field<float>(*corrected,0x2f8);
        const auto cap=ordinary_camera_culling::field<float>(*corrected,0x30c);
        require(close(cap,-near_z),"asymmetric near cap remains at the physical near plane");
        for(size_t c=0;c<4;++c)require(radius>=std::sqrt(native_corners[2*c]*native_corners[2*c]+native_corners[2*c+1]*native_corners[2*c+1]+near_z*near_z)-1e-6f,"compact radius encloses every asymmetric corner");
        for(size_t i=0;i<native.size();++i)if(!(i>=0x2f8&&i<0x2fc) && !(i>=0x30c&&i<0x310))require((*corrected)[i]==native[i],"compact correction preserves all other native bytes");
        auto corrupt=native;const float bad=99;std::memcpy(corrupt.data()+0x340,&bad,4);
        require(!ordinary_camera_culling::correct(corrupt,eye),"wrong native side cross refuses compact correction");
        corrupt=native;std::memcpy(corrupt.data()+0x80,&bad,4);
        require(!ordinary_camera_culling::correct(corrupt,eye),"changed native near plane refuses correction");
        for(size_t corner=0;corner<4;++corner){
            const float x=rectangle[0]+float(corner&1)*rectangle[2];
            const float y=rectangle[1]+float((corner>>1)&1)*rectangle[3];
            require(close((x*eye.projection[0]+near_z*eye.projection[8])/near_z,(corner&1)?1.f:-1.f) &&
                close((y*eye.projection[5]+near_z*eye.projection[9])/near_z,(corner&2)?1.f:-1.f),"each native culling rectangle projects to its own asymmetric raster edges");
        }
        const auto identity=vr_camera::multiply(eye.world,eye.view);for(size_t i=0;i<16;++i)require(close(identity[i],vr_camera::IDENTITY[i]),"per-eye world/view must be inverses");
        for(size_t i=0;i<source.size();++i)if(!(i<0x80 || (i>=0x88&&i<0xac) || (i>=0x12c&&i<0x13c) || (i>=0x2d0&&i<0x2dc)))require(eye.source[i]==original[i],"opaque source fields and native near/far planes stay byte-exact");
    }
    const auto alternate=ordinary_tracked_camera::prepare(source,game,frame(1,1),tracking,NOW);
    require(alternate && alternate->eyes[0].source==pair->eyes[0].source && alternate->eyes[1].source==pair->eyes[1].source,"native Frame.eye parity cannot change ordinary pair preparation");
    auto moved_anchor=game;moved_anchor[12]+=3;
    const auto moved=ordinary_tracked_camera::prepare(source,moved_anchor,frame(1),tracking,NOW);
    require(moved && close(moved->eyes[0].world[12],pair->eyes[0].world[12]+3) && close(moved->eyes[1].world[12],pair->eyes[1].world[12]+3),"one caller-resolved body anchor moves both eyes exactly once");
    f.fake->pose.head[12]=.3f;f.fake->pose.head[14]=-.2f;
    const auto translated=*f.session.ordinary_frame(frame(2),NOW);
    const auto translated_pair=ordinary_tracked_camera::prepare(source,game,frame(2),translated,NOW);
    require(translated_pair && close(translated_pair->eyes[0].world[12],30.2f) && close(translated_pair->eyes[1].world[12],30.2f) &&
        close(translated_pair->eyes[0].world[14],8.732f) && close(translated_pair->eyes[1].world[14],8.668f),"right eye includes the same tracked head translation as left instead of shifting the untouched game centre");
    f.fake->pose.head[0]=0;f.fake->pose.head[2]=-1;f.fake->pose.head[8]=1;f.fake->pose.head[10]=0;
    const auto rotated=*f.session.ordinary_frame(frame(3),NOW);
    const auto rotated_pair=ordinary_tracked_camera::prepare(source,game,frame(3),rotated,NOW);
    require(rotated_pair && close(rotated_pair->eyes[0].world[0],1) && close(rotated_pair->eyes[1].world[0],1) &&
        close(rotated_pair->eyes[1].world[12]-rotated_pair->eyes[0].world[12],.064f),"head rotation moves the physical eye baseline and both view orientations together");
    auto& canted_eye=f.fake->pose.eyes[1];canted_eye[0]=canted_eye[10]=std::cos(.1f);canted_eye[2]=-std::sin(.1f);canted_eye[8]=std::sin(.1f);
    const auto canted=*f.session.ordinary_frame(frame(4),NOW);
    const auto canted_pair=ordinary_tracked_camera::prepare(source,game,frame(4),canted,NOW);
    require(canted_pair && canted_pair->eyes[0].world[2]!=canted_pair->eyes[1].world[2] &&
        close(canted_pair->eyes[1].world[2],std::sin(.1f)),"canted physical eye rotation survives independent source composition rather than copying the left orientation");
    require(!ordinary_tracked_camera::prepare(source,game,frame(2),tracking,NOW),"different native job cannot reuse tracked cameras");
    auto wrong_token=frame(1);++wrong_token.token;require(!ordinary_tracked_camera::prepare(source,game,wrong_token,tracking,NOW),"game token mismatch refuses pair sources");
    require(!ordinary_tracked_camera::prepare(source,game,frame(1),tracking,NOW+std::chrono::milliseconds(101)),"camera preparation rejects stale frozen head and hands");
    require(!ordinary_tracked_camera::prepare(std::span{source}.first(source.size()-1),game,frame(1),tracking,NOW),"truncated native source refuses before reads");
    auto broken=source;const float zero=0;std::memcpy(broken.data()+0x80,&zero,4);
    require(!ordinary_tracked_camera::prepare(broken,game,frame(1),tracking,NOW),"invalid native depth planes refuse both eye sources");
    broken=source;const float noninverse=10;std::memcpy(broken.data()+0x70,&noninverse,4);
    require(!ordinary_tracked_camera::prepare(broken,game,frame(1),tracking,NOW),"contradictory native world/view input cannot become tracked stereo");
    auto invalid=tracking;invalid.eyes[1].runtime_eye_to_head=invalid.eyes[0].runtime_eye_to_head;
    require(!ordinary_tracked_camera::prepare(source,game,frame(1),invalid,NOW),"mono/corrupt snapshot cannot satisfy true two-view geometry");
}
void loss_and_provenance(){
    Fixture f;auto valid=*f.session.ordinary_frame(frame(1),NOW);
    const auto rejects=[&](const auto& mutate){auto value=valid;mutate(value);require(!value.valid_at(NOW),"cross-eye/controller metadata corruption must reject ordinary frame");};
    rejects([](auto& v){v.eyes[1].captured+=std::chrono::milliseconds(1);});rejects([](auto& v){v.eyes[1].runtime_head[12]=1;});
    rejects([](auto& v){v.eyes[1].render_token++;});rejects([](auto& v){v.eyes[1].reference_epoch++;});rejects([](auto& v){v.eyes[1].synthetic=true;});
    rejects([](auto& v){v.eyes[1].fov.reset();});rejects([](auto& v){v.controllers->tracking_epoch++;});rejects([](auto& v){v.controllers->captured+=std::chrono::milliseconds(1);});
    rejects([](auto& v){v.eyes[1].runtime_eye_to_head=v.eyes[0].runtime_eye_to_head;});
    f.fake->pose.controllers[0].reset();const auto partial=f.session.ordinary_frame(frame(2),NOW);require(partial && partial->controllers && !partial->controllers->relative[0] && !partial->controllers->fingers[0] && partial->controllers->relative[1],"lost hand releases only that wrist/fingers without losing valid two-eye tracking");
    f.fake->pose.fov[1].right=NAN;const auto epoch=f.session.epoch();require(!f.session.ordinary_frame(frame(3),NOW) && f.session.epoch()>epoch && !f.session.controller_poses(NOW),"bad RIGHT lens releases whole ordinary pair and controllers even when left eye is valid");
    f.fake->pose.fov[1].right=1.1f;require(bool(f.session.ordinary_frame(frame(4),NOW)),"valid tracking recovers under a fresh native identity");
    f.fake->throws=true;require(!f.session.ordinary_frame(frame(5),NOW) && !f.session.controller_poses(NOW),"backend exception cannot leave prior controller or stereo snapshot active");f.fake->throws=false;
    for(uint64_t id=6;id<=15;++id)require(bool(f.session.ordinary_frame(frame(id),NOW)),"fresh ordinary job fills bounded snapshot cache");
    const auto reads=f.fake->trackings;require(!f.session.ordinary_frame(frame(6),NOW) && f.fake->trackings==reads,"evicted old native job cannot acquire a fresh pose or nearest-frame fallback");
    auto invalid=frame(16);invalid.invalid=true;require(!f.session.ordinary_frame(invalid,NOW) && !f.session.controller_poses(NOW),"invalid native frame clears ordinary eligibility");
    require(!f.session.ordinary_frame(frame(16),NOW),"an invalid consumed native job cannot be reissued with its invalid flag removed");
}
}
int main(){try{passive_until_explicit_start();one_frozen_sample();controllers_use_issued_frame();invalidations_and_modes();snap_and_recenter();paired_owned_camera_sources();loss_and_provenance();std::cout<<"ordinary frozen tracking, owned paired camera sources and AFR isolation PASS; fake backend only\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
