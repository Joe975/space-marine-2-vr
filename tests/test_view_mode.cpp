#include "VrViewMode.h"
#include "VrControllerInput.h"
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <limits>
#include <cstring>

namespace {
void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
bool close(float a,float b) {return std::abs(a-b)<0.0001f;}
void cameras() {
    auto game=vr_camera::IDENTITY;game[10]=-1;game[12]=10;game[13]=3;game[14]=20;
    const VrViewCamera::Focus player{1,{10,0,20}};
    VrViewSettings settings;settings.mode=VrViewMode::diorama;
    VrViewCamera camera;
    const auto anchor=camera.camera(game,settings,1,9,player,false);
    require(close(anchor[12],10) && close(anchor[13],8.4f) && close(anchor[14],24.5f),"diorama starts 0.5 m behind and 0.6 m above the character at miniature scale");
    // 27 Oct headset: a pitched anchor tilts the whole table against the real horizon; the default is level
    // and the player looks down with their own head.
    require(close(anchor[9],0) && close(anchor[5],1) && vr_camera::rigid(anchor,false),"default tabletop is level with a rigid basis");
    auto tilted_settings=settings;tilted_settings.diorama_pitch=30;
    const auto tilted=VrViewCamera{}.camera(game,tilted_settings,1,9,player,false);
    require(close(tilted[9],-.5f) && vr_camera::rigid(tilted,false),"diorama_pitch still tilts the table when asked");
    auto moved=game;moved[12]+=100;moved[0]=-1;moved[10]=1;
    require(camera.camera(moved,settings,1,9,player,false)==anchor,"native walking and turning must not drag or rotate frozen observer");
    const auto teleported=camera.camera(moved,settings,1,9,VrViewCamera::Focus{1,{100,0,0}},false);
    require(teleported!=anchor,"large same-owner scene discontinuity must release previous level anchor");
    camera.reset();camera.camera(game,settings,1,9,player,false);
    auto eye=vr_camera::IDENTITY;eye[12]=.1f;eye[13]=.2f;eye[14]=-.3f;
    vr_camera::Matrix world{},view{};
    require(vr_camera::compose(anchor,eye,9,world,view,false),"head lean composes into frozen tabletop basis");
    require(world!=anchor && close(world[12],anchor[12]+.9f),"physical head movement stays active at diorama scale");
    require(vr_camera::compose(tilted,eye,9,world,view,false) && close(world[9],-.5f),"diorama tilt survives production composition instead of being silently leveled away");
    auto ipd_l=vr_camera::IDENTITY,ipd_r=vr_camera::IDENTITY;ipd_l[12]=-.032f;ipd_r[12]=.032f;
    vr_camera::Matrix left{},right{};
    require(vr_camera::compose(anchor,ipd_l,9,left,view,false) && vr_camera::compose(anchor,ipd_r,9,right,view,false) && close(right[12]-left[12],.576f),"miniature stereo preserves scaled physical IPD");
    const auto captures=camera.snapshot().captures;
    require(camera.camera(moved,settings,2,9,player,false)!=anchor && camera.snapshot().captures==captures+1,"recenter reanchors on current native view exactly once");
    const auto recentered=camera.snapshot().anchor;
    require(camera.camera(moved,settings,2,9,player,false)==recentered && camera.snapshot().captures==captures+1,"both eyes share one recenter anchor");
    settings.diorama_distance=2;require(camera.camera(game,settings,2,9,player,false)!=recentered,"live distance knob must change actual camera");
    const auto distance_anchor=camera.snapshot().anchor;
    settings.diorama_height=1;require(camera.camera(game,settings,2,9,player,false)[13]!=distance_anchor[13],"live height must change output");
    settings.diorama_pitch=45;require(!close(camera.camera(game,settings,2,9,player,false)[9],0),"live pitch must change output basis");
    require(camera.camera(game,settings,2,9,player,true)==game && !camera.snapshot().anchored,"scripted camera releases frozen anchor");
    require(camera.camera(moved,settings,2,9,player,false)!=distance_anchor && camera.snapshot().anchored,"gameplay return reacquires without explicit recenter");
    require(camera.camera(game,settings,2,9,std::nullopt,false)==game && !camera.snapshot().anchored,"missing exact actor fails closed and releases old level anchor");
    camera.camera(game,settings,2,9,player,false);
    require(camera.camera(moved,settings,2,9,VrViewCamera::Focus{2,{0,0,0}},false)!=camera.camera(game,settings,2,9,player,false),"changed actor cannot inherit another actor's anchor");
    settings.mode=VrViewMode::shoulder;
    const auto shoulder=camera.camera(game,settings,3,.9f,player,false);
    require(close(shoulder[12],10.36f) && close(shoulder[13],3.225f) && close(shoulder[14],21.8f),"shoulder has life-size placement offsets");
    const auto turned=camera.camera(moved,settings,3,.9f,player,false);
    require(close(turned[12],109.64f) && close(turned[14],18.2f) && turned[0]==-1,"shoulder follows native translation and yaw");
    settings.shoulder_side=-.4f;require(!close(camera.camera(moved,settings,3,.9f,player,false)[12],turned[12]),"shoulder side tunable reaches real path");
    settings.shoulder_distance=3;require(!close(camera.camera(moved,settings,3,.9f,player,false)[14],turned[14]),"shoulder distance is live");
    settings.shoulder_height=1;require(!close(camera.camera(moved,settings,3,.9f,player,false)[13],turned[13]),"shoulder height is live");
    settings.mode=VrViewMode::first_person;require(camera.camera(game,settings,4,.9f,player,false)==game,"default does not reposition existing first-person path");
}
void shoulder_zoom() {
    auto game=vr_camera::IDENTITY;game[10]=-1;game[12]=10;game[13]=3;game[14]=20;
    const VrViewCamera::Focus player{1,{10,0,20}};
    VrViewSettings settings;settings.mode=VrViewMode::shoulder;
    VrViewCamera camera;
    const auto life=camera.camera(game,settings,1,settings.scale(.9f),player,false);
    settings.shoulder_scale=2;
    const auto out=camera.camera(game,settings,1,settings.scale(.9f),player,false);
    require(close(out[12]-10,2*(life[12]-10)) && close(out[13]-3,2*(life[13]-3)) && close(out[14]-20,2*(life[14]-20)),"shoulder zoom moves the camera out with the world");
    // 12 game units are a jump at any zoom: the limit stays in the game world, not in scaled metres.
    const auto captures=camera.snapshot().captures;
    camera.camera(game,settings,1,settings.scale(.9f),VrViewCamera::Focus{1,{22,0,20}},false);
    require(camera.snapshot().captures==captures+1,"a zoomed shoulder view still re-anchors on a jump");
}
void chase() {
    // Stock chase camera (-ChaseCam pack): the native camera is the observer, leveled, every frame.
    auto game=vr_camera::IDENTITY;game[10]=-1;game[12]=10;game[13]=3;game[14]=20;
    VrViewSettings settings;settings.mode=VrViewMode::chase;
    require(settings.valid() && std::strcmp(VrViewSettings::name(settings.mode),"chase")==0,"chase is a valid named mode");
    VrViewCamera camera;
    const auto anchor=camera.camera(game,settings,1,.9f,std::nullopt,false);
    require(camera.snapshot().anchored && close(anchor[12],10) && close(anchor[13],3) && close(anchor[14],20),"chase sits on the native camera with no offset and needs no body focus");
    auto moved=game;moved[12]=50;moved[0]=-1;moved[10]=1;
    const auto followed=camera.camera(moved,settings,1,.9f,std::nullopt,false);
    require(close(followed[12],50) && followed[0]==-1 && camera.snapshot().captures==1,"chase follows native translation and yaw without re-anchoring");
    auto pitched=game;const float c=std::cos(.5f),s=std::sin(.5f);pitched[5]=c;pitched[6]=s;pitched[9]=s;pitched[10]=-c;
    const auto level=camera.camera(pitched,settings,1,.9f,std::nullopt,false);
    require(close(level[9],0) && close(level[13],3),"native chase pitch is leveled so the headset supplies pitch");
    require(camera.camera(game,settings,2,.9f,std::nullopt,false)==anchor && camera.snapshot().captures==2,"recenter counts one new capture");
    require(camera.camera(game,settings,2,.9f,std::nullopt,true)==game && !camera.snapshot().anchored,"scripted camera releases chase");
    require(settings.scale(.9f)==.9f,"chase is life-size");
    // The camera files are chosen at launch: chase only on a -ChaseCam launch, never elsewhere.
    for(auto mode:{VrViewMode::first_person,VrViewMode::diorama,VrViewMode::shoulder}) {
        require(VrViewSettings::allowed(mode,false) && !VrViewSettings::allowed(mode,true),"head-camera modes refused on a chase launch");
        require(VrViewSettings::next(mode)!=VrViewMode::chase,"F8 never cycles into chase");
    }
    require(VrViewSettings::allowed(VrViewMode::chase,true) && !VrViewSettings::allowed(VrViewMode::chase,false),"chase only on a chase launch");
    const std::string knobs="diorama_scale=10\ndiorama_distance=1\ndiorama_height=0.6\ndiorama_pitch=30\ndiorama_follow=1.5\ndiorama_recenter=2\nshoulder_distance=2\nshoulder_height=0.25\nshoulder_side=0.4\nchase_scale=3\n";
    std::istringstream file{"mode=chase\n"+knobs};
    require(!VrViewSettings::read(file),"a settings file cannot select chase; only the launch can");
    // A chase launch uses the file's knobs in chase mode and writes back the file's own mode.
    std::istringstream diorama_file{"mode=diorama\n"+knobs};
    const auto stored=VrViewSettings::read(diorama_file);
    require(stored && stored->mode==VrViewMode::diorama,"complete file read");
    require(stored->shoulder_scale==1 && stored->diorama_distance==1,"a file saved before shoulder_scale keeps its knobs");
    std::istringstream cut_file{"mode=diorama\ndiorama_scale=10\n"};
    require(!VrViewSettings::read(cut_file),"any other missing knob still fails the file");
    auto zoomed=*stored;zoomed.shoulder_scale=2.83f;
    std::stringstream zoomed_file;zoomed.write(zoomed_file);
    require(VrViewSettings::read(zoomed_file)==zoomed,"shoulder zoom survives a save");
    const auto launched=stored->for_launch(true);
    require(launched.mode==VrViewMode::chase && close(launched.scale(.9f),2.7f),"chase launch takes chase_scale from the file");
    require(stored->for_launch(false)==*stored,"other launches use the file as it is");
    {
        VrViewFile file;
        require(file.start(stored,true)==launched,"a chase launch starts from the file's knobs in chase");
        require(file.to_save(launched)==*stored,"saving from a chase launch keeps the file's mode, never chase");
    }
    {
        // 27 Oct: F8 saved the live mode, so the next launch started in diorama.
        VrViewSettings on_disk;VrViewFile file;
        require(file.start(on_disk,false).mode==VrViewMode::first_person,"a launch starts in the file's mode");
        auto live=on_disk;live.mode=VrViewMode::diorama;live.diorama_scale=20;
        const auto saved=file.to_save(live);
        require(saved.mode==VrViewMode::first_person && saved.diorama_scale==20,"a save keeps the live knobs but never the live mode");
        require(!file.changed(saved,false),"the mod's own save is not applied back by the poll (it would undo F8)");
        auto edited=saved;edited.mode=VrViewMode::shoulder;
        const auto applied=file.changed(edited,false);
        require(applied && applied->mode==VrViewMode::shoulder,"a hand edit on disk, mode included, applies live");
        require(!file.changed(edited,false),"and only once");
        require(!file.changed(std::nullopt,false),"a missing or unreadable file changes nothing");
        VrViewFile none;
        require(none.start(std::nullopt,false)==VrViewSettings{} && none.to_save(live).mode==VrViewMode::first_person,"no file: defaults, saves start in first person");
    }
    VrViewCamera scaled;scaled.camera(game,launched,1,2.7f,std::nullopt,false);
    require(close(scaled.camera(moved,launched,1,2.7f,std::nullopt,false)[12],50),"a scaled chase camera still sits on the native camera");
}
void world_scale() {
    // Gamepad Back + D-pad: up makes the world bigger (smaller multiplier), down smaller.
    VrViewSettings settings;
    require(!settings.resize_world(1),"first person has no third-person scale");
    settings.mode=VrViewMode::shoulder;require(settings.scale(.9f)==.9f,"shoulder starts life-size");
    require(settings.resize_world(-1) && close(settings.shoulder_scale,1.41f) && close(settings.scale(.9f),1.269f),"shoulder scale reaches the issued units");
    require(settings.resize_world(-20) && settings.shoulder_scale==4 && !settings.resize_world(-1),"shoulder stops at 1/4, well short of the diorama");
    require(settings.resize_world(20) && settings.shoulder_scale==.5f && !settings.resize_world(1),"shoulder comes in to 2x");
    require(settings.diorama_scale==10 && settings.chase_scale==1 && settings.valid(),"shoulder steps leave the other modes' scales alone");
    settings=VrViewSettings{};
    settings.mode=VrViewMode::diorama;
    require(settings.resize_world(1) && close(settings.diorama_scale,7.07f),"up: world bigger by one step");
    require(settings.resize_world(-2) && close(settings.diorama_scale,14.14f),"down twice: smaller, two steps");
    require(settings.resize_world(-10) && settings.diorama_scale==40 && !settings.resize_world(-1),"clamped at the smallest world");
    require(settings.resize_world(20) && settings.diorama_scale==2 && !settings.resize_world(1),"clamped at the biggest diorama world");
    require(settings.valid(),"stepped settings stay valid");
    settings.mode=VrViewMode::chase;
    require(settings.resize_world(-1) && close(settings.chase_scale,1.41f) && close(settings.scale(.9f),1.269f),"chase scale reaches the issued units");
    require(settings.resize_world(3) && settings.chase_scale==.5f && !settings.resize_world(1),"chase can make the world bigger than life, to 2x");
    require(settings.resize_world(-20) && settings.chase_scale==20,"chase world down to 1/20");
    require(!settings.resize_world(0),"no steps, no change");
}
void diorama_follow() {
    // 27 Oct playtest: the table sliding by itself was confusing; it stays put until F9 unless asked.
    require(VrViewSettings{}.diorama_follow==0,"diorama follow is off by default");
    // 9 units/m: 1.5 physical metres is 13.5 game units of walking.
    auto game=vr_camera::IDENTITY;game[10]=-1;game[12]=10;game[13]=3;game[14]=20;
    VrViewSettings settings;settings.mode=VrViewMode::diorama;settings.diorama_follow=1.5f;
    VrViewCamera camera;
    const auto start=camera.camera(game,settings,1,9,VrViewCamera::Focus{1,{10,0,20}},false);
    // Walk in 5-unit frames (below the 9-unit teleport release), turning the native camera too.
    auto turned=game;turned[0]=-1;turned[10]=1;
    require(camera.camera(turned,settings,1,9,VrViewCamera::Focus{1,{15,0,20}},false)==start && camera.camera(turned,settings,1,9,VrViewCamera::Focus{1,{20,0,20}},false)==start,
        "within the follow distance the table stays put");
    const auto followed=camera.camera(turned,settings,1,9,VrViewCamera::Focus{1,{24,1,20}},false);
    require(close(followed[12],start[12]+14) && close(followed[13],start[13]+1) && close(followed[14],start[14]),"past it the table slides by the whole walk, height too");
    for(int i=0;i<12;++i) require(followed[i]==start[i],"following never turns or tilts the table");
    require(camera.snapshot().follows==1 && camera.snapshot().captures==1,"a follow is not a recenter");
    require(camera.camera(turned,settings,1,9,VrViewCamera::Focus{1,{28,1,20}},false)==followed,"the next follow counts from where the table moved");
    settings.diorama_follow=0;
    VrViewCamera fixed;const auto frozen=fixed.camera(game,settings,1,9,VrViewCamera::Focus{1,{10,0,20}},false);
    for(float x=15;x<=60;x+=5) fixed.camera(game,settings,1,9,VrViewCamera::Focus{1,{x,0,20}},false);
    require(fixed.camera(game,settings,1,9,VrViewCamera::Focus{1,{60,0,20}},false)==frozen && fixed.snapshot().follows==0,"follow 0 keeps the table frozen");
}
void diorama_recenter() {
    // Joe, 31 Oct: when the Marine gets too far away, recenter as the button would.
    require(VrViewSettings{}.diorama_recenter==1.5f,"the automatic recenter is on by default");
    // 9 units/m: 1.5 physical metres is 13.5 game units of walking. Frames stay below the 9-unit teleport release.
    auto game=vr_camera::IDENTITY;game[10]=-1;game[12]=10;game[13]=3;game[14]=20;
    VrViewSettings settings;settings.mode=VrViewMode::diorama;
    {
        VrViewCamera shipped;shipped.camera(game,settings,1,9,VrViewCamera::Focus{1,{10,0,20}},false);
        shipped.camera(game,settings,1,9,VrViewCamera::Focus{1,{15,0,20}},false);
        shipped.camera(game,settings,1,9,VrViewCamera::Focus{1,{23.4f,0,20}},false);
        require(!shipped.take_recenter(),"as shipped, up to 13.5 game units nothing happens");
        shipped.camera(game,settings,1,9,VrViewCamera::Focus{1,{23.6f,0,20}},false);
        require(shipped.take_recenter(),"as shipped, past 13.5 game units it recenters");
    }
    settings.diorama_recenter=1; // Round numbers below: 1 physical metre is 9 game units.
    VrViewCamera camera;
    const auto start=camera.camera(game,settings,1,9,VrViewCamera::Focus{1,{10,0,20}},false);
    camera.camera(game,settings,1,9,VrViewCamera::Focus{1,{14,0,20}},false);
    require(camera.camera(game,settings,1,9,VrViewCamera::Focus{1,{19,0,20}},false)==start && !camera.take_recenter(),"up to a metre nothing happens");
    require(camera.camera(game,settings,1,9,VrViewCamera::Focus{1,{18,0,25}},false)==start,"the camera never moves the table itself: the recenter does");
    require(camera.take_recenter() && !camera.take_recenter() && camera.snapshot().recenters==1,"past a metre on the diagonal, one request");
    camera.camera(game,settings,1,9,VrViewCamera::Focus{1,{22,0,25}},false);
    require(!camera.take_recenter() && camera.snapshot().recenters==1 && camera.snapshot().captures==1,"walking on before the recenter lands asks nothing more");
    // The recenter lands as a new reference, as for F9 or Back.
    auto turned=game;turned[0]=-1;turned[10]=1;turned[12]=22;turned[14]=25;
    const auto recentered=camera.camera(turned,settings,2,9,VrViewCamera::Focus{1,{22,0,25}},false);
    require(close(recentered[12],22) && close(recentered[14],20.5f) && recentered[0]==-1 && camera.snapshot().captures==2,"it anchors at the Marine, turned to his heading");
    require(!camera.take_recenter(),"the new anchor asks nothing");
    camera.camera(turned,settings,2,9,VrViewCamera::Focus{1,{27,0,25}},false);
    require(camera.camera(turned,settings,2,9,VrViewCamera::Focus{1,{31,0,25}},false)==recentered && !camera.take_recenter(),"the walk counts from the new anchor");
    camera.camera(turned,settings,2,9,VrViewCamera::Focus{1,{32,0,25}},false);
    require(camera.take_recenter() && camera.snapshot().recenters==2,"and the next metre asks again");
    {   // Horizontal only.
        VrViewCamera lift;lift.camera(game,settings,1,9,VrViewCamera::Focus{1,{10,0,20}},false);
        for(float y=5;y<=40;y+=5) lift.camera(game,settings,1,9,VrViewCamera::Focus{1,{10,y,20}},false);
        require(!lift.take_recenter(),"height does not count");
    }
    // The same walk in the game world at any scale: 1 m at the default, 1.41 m at 7.07, 0.5 m at 20.
    for(float scale:{2.f,7.07f,10.f,20.f,40.f}) {
        auto sized=settings;sized.diorama_scale=scale;const float eye=scale*.9f;
        VrViewCamera world;world.camera(game,sized,1,eye,VrViewCamera::Focus{1,{10,0,20}},false);
        world.camera(game,sized,1,eye,VrViewCamera::Focus{1,{18.9f,0,20}},false);
        require(!world.take_recenter(),"9 game units are the limit at any scale");
        world.camera(game,sized,1,eye,VrViewCamera::Focus{1,{19.1f,0,20}},false);
        require(world.take_recenter(),"past 9 game units at any scale");
    }
    auto twice=settings;twice.diorama_recenter=2;
    VrViewCamera longer;longer.camera(game,twice,1,9,VrViewCamera::Focus{1,{10,0,20}},false);
    for(float x=15;x<=25;x+=5) longer.camera(game,twice,1,9,VrViewCamera::Focus{1,{x,0,20}},false);
    require(!longer.take_recenter(),"the knob sets the distance");
    longer.camera(game,twice,1,9,VrViewCamera::Focus{1,{30,0,20}},false);
    require(longer.take_recenter(),"past twice the distance");
    auto off=settings;off.diorama_recenter=0;
    VrViewCamera fixed;fixed.camera(game,off,1,9,VrViewCamera::Focus{1,{10,0,20}},false);
    for(float x=15;x<=60;x+=5) fixed.camera(game,off,1,9,VrViewCamera::Focus{1,{x,0,20}},false);
    require(!fixed.take_recenter() && fixed.snapshot().recenters==0,"0 keeps the table frozen until F9");
    auto shoulder=settings;shoulder.mode=VrViewMode::shoulder;
    VrViewCamera life_size;life_size.camera(game,shoulder,1,.9f,VrViewCamera::Focus{1,{10,0,20}},false);
    for(float x=10.5f;x<=30;x+=.5f) life_size.camera(game,shoulder,1,.9f,VrViewCamera::Focus{1,{x,0,20}},false);
    require(!life_size.take_recenter(),"shoulder view follows by itself and never asks");
    // F8 to first person with a request waiting: a recenter there would take a body-scale reading.
    VrViewCamera pending;pending.camera(game,settings,1,9,VrViewCamera::Focus{1,{10,0,20}},false);
    pending.camera(game,settings,1,9,VrViewCamera::Focus{1,{15,0,20}},false);
    pending.camera(game,settings,1,9,VrViewCamera::Focus{1,{20,0,20}},false);
    pending.reset();
    require(!pending.take_recenter(),"a mode change drops a waiting request");
    // A cutscene moves the Marine: gameplay reacquires where he stands, so nothing is owed.
    VrViewCamera shot;shot.camera(game,settings,1,9,VrViewCamera::Focus{1,{10,0,20}},false);
    shot.camera(game,settings,1,9,VrViewCamera::Focus{1,{40,0,20}},true);
    shot.camera(game,settings,1,9,VrViewCamera::Focus{1,{40,0,20}},false);
    shot.camera(game,settings,1,9,VrViewCamera::Focus{1,{41,0,20}},false);
    require(!shot.take_recenter(),"a scripted shot starts the walk again");
}
void settings_and_keys() {
    VrViewSettings settings;require(settings.valid() && settings.scale(.9f)==.9f,"first person remains default life size");
    settings.mode=VrViewMode::diorama;require(close(settings.scale(.9f),9),"scale knob changes real issued units");
    settings.diorama_scale=20;require(close(settings.scale(.9f),18),"changing scale has observable output");
    const auto path=std::filesystem::temp_directory_path()/"sm2vr-view-mode-fixture.calibration";
    require(settings.save(path) && VrViewSettings::load(path)==settings,"presets must round trip every active knob and mode");std::filesystem::remove(path);
    {
        // 27 Oct: F8 in a scripted run rewrote the repo's file as diorama_height=0.600000024 and dropped its header.
        std::ostringstream text;VrViewSettings{}.write(text);
        std::ifstream repo{std::filesystem::path{__FILE__}.parent_path().parent_path()/"config"/"index_camera_modes.calibration"};
        const std::string shipped{std::istreambuf_iterator<char>{repo},std::istreambuf_iterator<char>{}};
        std::istringstream back{text.str()};
        require(text.str().find("diorama_height=0.6\n")!=std::string::npos && text.str().starts_with("# F8 cycles modes"),"saved knobs are short and keep the header");
        require(VrViewSettings::read(back)==VrViewSettings{},"the short form reads back exactly");
        auto lf=shipped;std::erase(lf,'\r');
        require(lf==text.str(),"saving the defaults reproduces the shipped calibration byte for byte");
    }
    std::istringstream duplicate{"mode=diorama\nmode=shoulder\n"},incomplete{"mode=diorama\n"},unknown{"mode=unknown\n"};
    require(!VrViewSettings::read(duplicate) && !VrViewSettings::read(incomplete) && !VrViewSettings::read(unknown),"partial/duplicate/unknown config must not partially apply");
    settings.shoulder_height=NAN;require(!settings.valid(),"nonfinite config rejected");
    VrViewHotkeys keys;
    keys.press(true,true,false);keys.press(true,false,false);
    const auto taps=keys.take(true);require(taps.cycle && taps.recenter,"short taps between frame polls must remain queued");
    require(!keys.take(true).cycle,"consuming hotkeys must clear requests");
    keys.press(true,true,true);keys.press(false,false,false);
    const auto repeats=keys.take(true);require(!repeats.cycle && !repeats.recenter,"auto-repeat and background messages cannot act");
    keys.press(true,true,false);require(!keys.take(false).cycle && !keys.take(true).cycle,"focus loss drops queued actions without replay");
    require(keys.message(true,WM_KEYDOWN,VK_F8,0) && keys.message(true,WM_KEYUP,VK_F8,0),"real Windows F8 down/up must be consumed");
    require(keys.take(true).cycle && !keys.take(true).cycle,"short Windows key messages survive until next frame exactly once");
    keys.message(true,WM_KEYDOWN,VK_F9,0);require(keys.take(true).recenter,"real F9 maps to recenter");
    keys.message(true,WM_KEYDOWN,VK_F8,1LL<<30);require(!keys.take(true).cycle,"Windows repeat flag suppresses repeated mode changes");
    keys.message(true,WM_KEYDOWN,VK_F8,0);keys.message(false,WM_KILLFOCUS,0,0);require(!keys.take(true).cycle,"WM_KILLFOCUS discards pending input");
    require(!keys.message(true,WM_KEYDOWN,VK_F11,0),"unrelated framework/menu keys retain normal behavior");
}
void native_controls() {
    VrControllerInput input;input.enable(true);input.native_passthrough(true);
    require(!input.snapshot().enabled && input.needs_dispatch() && input.motion_preference(),"third person suspends motion bridge but retains native dispatch and saved preference");
    XINPUT_STATE original{};original.dwPacketNumber=72;original.Gamepad.sThumbLX=1234;original.Gamepad.sThumbRX=-5678;original.Gamepad.bRightTrigger=217;original.Gamepad.wButtons=XINPUT_GAMEPAD_A;
    auto state=original;uint32_t result=ERROR_SUCCESS;input.get_state(&result,0,&state);input.native_actions(result,0,&state);
    require(result==ERROR_SUCCESS && std::memcmp(&state,&original,sizeof(state))==0,"native movement, camera stick, trigger, button and packet remain byte-identical");
    state=original;state.Gamepad.wButtons|=XINPUT_GAMEPAD_BACK;input.get_state(&result,0,&state);input.native_actions(result,0,&state);
    require(input.take_native_recenter() && state.Gamepad.wButtons==XINPUT_GAMEPAD_A,"Back is consumed as a single third-person recenter request");
    state.Gamepad.wButtons|=XINPUT_GAMEPAD_BACK;input.get_state(&result,0,&state);input.native_actions(result,0,&state);require(!input.take_native_recenter(),"holding Back never repeats recenter");
    result=ERROR_DEVICE_NOT_CONNECTED;state=original;input.get_state(&result,0,&state);input.native_actions(result,0,&state);require(result==ERROR_DEVICE_NOT_CONNECTED && std::memcmp(&state,&original,sizeof(state))==0,"disconnected physical controller is not replaced with virtual neutral success");
    result=ERROR_SUCCESS;state.Gamepad.wButtons|=XINPUT_GAMEPAD_BACK;input.get_state(&result,0,&state);input.native_actions(result,0,&state);require(input.take_native_recenter(),"native reconnect rearms recenter");
    result=ERROR_DEVICE_NOT_CONNECTED;XINPUT_CAPABILITIES caps{};input.get_capabilities(&result,0,&caps);require(result==ERROR_DEVICE_NOT_CONNECTED,"third person must not advertise a virtual controller");
    // Back held + D-pad: world scale steps the game never sees.
    const auto send=[&](WORD buttons){state=original;state.Gamepad.wButtons=buttons;input.get_state(&result,0,&state);input.native_actions(result,0,&state);return state.Gamepad.wButtons;};
    result=ERROR_SUCCESS;send(0);input.take_native_recenter();input.take_native_scale_steps();
    require(send(XINPUT_GAMEPAD_DPAD_UP)==XINPUT_GAMEPAD_DPAD_UP && !input.take_native_scale_steps(),"D-pad alone stays native");
    require(send(XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_BACK)==XINPUT_GAMEPAD_DPAD_UP && !input.take_native_scale_steps() && input.take_native_recenter(),
        "a direction already held when Back goes down stays the game's and is not a step");
    send(XINPUT_GAMEPAD_BACK);
    require(send(XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_DPAD_UP)==0 && send(XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_DPAD_UP)==0,"a direction pressed under Back is hidden while held");
    send(XINPUT_GAMEPAD_BACK);send(XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_DPAD_UP);send(XINPUT_GAMEPAD_BACK);send(XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_DPAD_DOWN);
    require(input.take_native_scale_steps()==1 && !input.take_native_scale_steps() && !input.take_native_recenter(),"two presses up, one down; a held Back never recenters again");
    require(send(XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_A)==XINPUT_GAMEPAD_A,"letting Back go first keeps the scaling direction hidden until it is released");
    require(send(0)==0 && send(XINPUT_GAMEPAD_DPAD_DOWN)==XINPUT_GAMEPAD_DPAD_DOWN && !input.take_native_scale_steps(),"released, the direction is the game's again");
    send(XINPUT_GAMEPAD_BACK);send(XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_DPAD_UP);input.native_passthrough(false);input.native_passthrough(true);
    require(!input.take_native_scale_steps() && !input.take_native_recenter(),"a mode change drops pending steps");
    input.native_passthrough(false);require(input.snapshot().enabled && !input.snapshot().armed,"returning first person restores preference but awaits neutral tracking");
    input.enable(false);input.native_passthrough(true);input.native_passthrough(false);require(!input.snapshot().enabled,"mode switch must not enable a deliberately disabled bridge");
}
}
int main() {try {cameras();shoulder_zoom();chase();world_scale();diorama_follow();diorama_recenter();settings_and_keys();native_controls();std::cout<<"third-person anchors, scale, presets, hotkeys and native controls passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
