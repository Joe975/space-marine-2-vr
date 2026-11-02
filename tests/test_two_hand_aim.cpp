#include "VrTwoHandAim.h"
#include <iostream>
#include <stdexcept>
using namespace vr_two_hand;
using namespace std::chrono_literals;
void require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
Matrix at(float x,float y,float z){auto m=vr_camera::IDENTITY;vr_arm::position(m,{x,y,z});return m;}
bool equal(const Matrix& a,const Matrix& b){for(size_t i=0;i<16;++i)if(std::abs(a[i]-b[i])>1e-4f)return false;return true;}
int main(){try {
    const auto now=Clock::time_point{10s};
    for(size_t primary:{size_t{0},size_t{1}})for(float units:{.9f,1.45f}) {
        Model m;m.actor=42;m.epoch=7;m.generation=9;m.captured=now;m.dominant=primary;m.units=units;
        m.gun_to_primary=at(0,0,-.3f*units);m.drawn_to_primary=m.gun_to_primary;m.support_to_gun=at(0,0,-.3f*units);
        std::array<std::optional<Matrix>,2> wrists;wrists[primary]=at(0,1*units,-.2f*units);wrists[1-primary]=at(0,1*units,-.8f*units);
        const auto neutral=solve(m,wrists);require(neutral.has_value(),"neutral supported pose solves");
        (*wrists[1-primary])[12]+=.1f*units;
        const auto moved=solve(m,wrists);require(moved && !equal(moved->gun,neutral->gun),"moving only support steers barrel; old single-wrist rule fails this");
        require(vr_arm::length(vr_arm::sub(vr_arm::position(moved->wrists[primary]),vr_arm::position(*wrists[primary])))<1e-5f,"dominant pivot stays put");
        require(equal(vr_camera::multiply(m.gun_to_primary,moved->wrists[primary]),moved->gun) && equal(vr_camera::multiply(m.support_to_gun,moved->gun),moved->wrists[1-primary]),"one rigid weapon determines both virtual grips");
        VrWeaponAim aim;aim.enable(true);aim.allow(true,7);aim.publish({42,7,1,now,moved->gun,1});
        require(aim.ray_pose(42,now) && equal(aim.ray_pose(42,now)->world,moved->gun),"native ray consumes the same constrained gun");
        auto missing=wrists;missing[1-primary].reset();require(!solve(m,missing),"lost support cannot solve from cached wrist");
        missing=wrists;missing[1-primary]=missing[primary];require(!solve(m,missing),"coincident controllers refuse a direction");
        missing=wrists;missing[1-primary]=at(0,units,.8f*units);require(!solve(m,missing),"crossed/reversed hands refuse");
        missing=wrists;(*missing[1-primary])[14]-=units;require(!solve(m,missing),"hands parted beyond rigid reach refuse");
        missing=wrists;(*missing[1-primary])[0]=NAN;require(!solve(m,missing),"invalid tracking refuses");
        // The same global turn/translation must commute with the entire constraint.
        auto turn=vr_arm::turn({0,0,1},{1,0,0});vr_arm::position(turn,{3,4,5});
        auto turned=wrists;for(auto& w:turned)*w=vr_camera::multiply(*w,turn);
        const auto snap=solve(m,turned);require(snap && equal(snap->gun,vr_camera::multiply(moved->gun,turn)),"snap/head anchor does not change support geometry");
    }
    // Real playtest scales: Marine geometry is .9 units/m, physical tracking is
    // 1.45 units/m. The old .18 Marine-metre tolerance rejected a still-held grab.
    for(size_t primary:{size_t{0},size_t{1}}) {
        Model m;m.dominant=primary;m.units=.9f;m.tracking_units=1.45f;
        m.gun_to_primary=at(0,0,-.3f);m.support_to_gun=at(0,0,-.348f);
        std::array<std::optional<Matrix>,2> wrists;wrists[primary]=at(0,1.4f,0);
        for(float offset:{-.17f,.19f}) {
            wrists[1-primary]=at(0,1.4f,-.648f-offset*m.tracking_units);
            require(solve(m,wrists).has_value(),"held contact tolerance uses physical scale, not Marine scale");
        }
        wrists[1-primary]=at(0,1.4f,-.648f-.21f*m.tracking_units);
        require(!solve(m,wrists),"beyond physical release margin refuses");
        wrists[1-primary]=at(0,1.4f,-.648f);m.tracking_units=NAN;
        require(!solve(m,wrists),"invalid explicit tracking scale refuses");
    }
    {
        const std::vector<std::string> names{"ROOT","HANDL","HANDR","GUN_L","GUN_R","Gun_pose"};
        const std::vector<int32_t> parents{-1,0,0,1,2,0};
        std::vector<Matrix> native{at(0,0,0),at(.223f,1.9f,.791f),at(0,1.4f,0),at(0,0,.1f),at(0,0,.3f),at(0,1.4f,.3f)};
        vr_weapon::Attachment gun;gun.index=5;gun.grip=vr_weapon::Grip::right;gun.seated=true;
        require(learn(native,parents,names,vr_camera::IDENTITY,gun,.9f).has_value(),"Heavy's recorded 1.07 Marine-metre native grip can teach a contact");
        native[1]=at(.223f,2.6f,1.1f);
        require(!learn(native,parents,names,vr_camera::IDENTITY,gun,.9f),"separated transition pose still cannot teach a contact");
        native[1]=at(.223f,1.9f,.791f);gun.seated=false;
        require(!learn(native,parents,names,vr_camera::IDENTITY,gun,.9f),"long reach never bypasses settled-weapon requirement");
    }
    // Actual gun placement, including a nonzero wrist offset, maps back to the
    // physical contact; it need not be along the raw controller's pointing axis.
    for(bool left:{false,true}) {
        Model m;m.actor=42;m.epoch=7;m.generation=9;m.captured=now;m.dominant=left?0:1;
        m.gun_to_primary=at(.25f,.1f,-.3f);m.drawn_to_primary=m.gun_to_primary;m.support_to_gun=at(0,0,-.3f);
        auto camera=vr_camera::IDENTITY;camera[10]=-1; // native camera, wrist_world restores proper axes
        auto raw=at(.15f,1.4f,-.2f),grip=at(.02f,.01f,.03f);
        const auto wrist=vr_arm::wrist_world(camera,raw,vr_camera::IDENTITY,1);
        const auto c=contact(m,*wrist,raw,grip,camera,1,left);require(c.has_value(),"actual weapon contact converts through native/XR basis");
        const auto expected=vr_camera::multiply(c->controller_to_primary,left?vr_hand::mirror(raw):raw);
        const auto physical=left?vr_hand::mirror(expected):expected;
        const auto support_wrist=vr_arm::wrist_world(camera,physical,grip,1);
        require(support_wrist && equal(*support_wrist,vr_camera::multiply(m.support_to_gun,vr_camera::multiply(m.drawn_to_primary,*wrist))),"contact is the native support wrist, not a raw -Z cylinder");
        require(!contact(m,*wrist,raw,grip,camera,NAN,left),"invalid scale never creates contact");
    }
    {
        Contact region{42,7,9,now,at(0,0,-.6f)};
        region.region=std::array<vr_arm::Vector,3>{vr_arm::Vector{},vr_arm::Vector{0,0,-.6f},vr_arm::Vector{0,0,-.3f}};
        const auto primary=at(0,1.4f,0);
        for(float height:{-.1f,0.f,.1f})require(Grip::within_contact(region,primary,at(0,1.4f+height,-.3f)),"body proxy accepts middle, top and bottom instead of only front handle");
        require(!Grip::within_contact(region,primary,at(0,1.55f,-.3f)),"air outside body padding is not a grab");
        require(!Grip::within_contact(region,primary,at(0,1.4f,-.05f)),"coincident hands cannot acquire the body proxy");
    }
    Contact c{42,7,9,now,at(.25f,0,-.3f)};auto primary=at(0,1.4f,-.2f),support=at(.25f,1.4f,-.5f);
    Grip grip;require(!grip.update(c,primary,support,0,true,7,now),"released grip arms acquisition");
    require(!grip.update(c,primary,support,.4f,true,7,now) && grip.state().reserved,"analogue onset owns squeeze before parry threshold");
    require(grip.update(c,primary,support,.7f,true,7,now) && grip.state().engagements==1,"full squeeze supports");
    require(!grip.update({},primary,support,.7f,true,7,now+20ms) && grip.state().reserved,"missing model cancels support but consumes held squeeze");
    require(!grip.update(c,primary,support,.7f,true,7,now+30ms),"tracking recovery held cannot silently regrab");
    grip.update(c,primary,support,0,true,7,now+40ms);require(grip.update(c,primary,support,1,true,7,now+50ms),"fresh squeeze regrabs");
    auto switched=c;switched.generation++;
    require(!grip.update(switched,primary,support,1,true,7,now+60ms) && grip.state().reserved,"weapon switch cannot inherit supported hold or turn into parry");
    require(!grip.update(switched,primary,support,0,true,7,now+61ms),"release after generation change");
    require(grip.update(switched,primary,support,1,true,7,now+62ms),"next weapon supports after release");
    require(!grip.update(switched,primary,support,1,true,8,now+63ms) && grip.state().reserved,"recenter invalidates epoch while preserving squeeze ownership");
    grip.reset();require(!grip.update(c,primary,support,0,true,7,now+101ms),"stale contact refuses");
    require(!grip.update(c,primary,support,1,true,7,now-1ms),"future contact refuses");
    std::cout<<"two-hand rigid solve, contact conversion, mirrored roles and grip lifecycle passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
