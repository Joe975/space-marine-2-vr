#pragma once
namespace twohand_publication_tests {
inline auto at(float x,float y,float z){auto m=vr_camera::IDENTITY;vr_arm::position(m,{x,y,z});return m;}
inline bool same(const vr_arm::Matrix& a,const vr_arm::Matrix& b){for(size_t i=0;i<16;++i)if(std::abs(a[i]-b[i])>1e-4f)return false;return true;}
inline void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
inline void run(){
    for(bool left:{false,true})for(int weapon:{0,1,2})for(bool body_grab:{false,true}) {
        const bool heavy=weapon==1,long_grip=weapon==2;
        EngineArmDriver driver;const auto now=EngineArmDriver::Clock::now();
        auto rig=std::make_shared<EngineArmProbe::RigLayout>();
        rig->names={"ROOT","BACKup","ARM1L","ARM2L","HANDL","GUN_L","ARM1R","ARM2R","HANDR","GUN_R","GUN_ANIM","Gun_pose"};
        rig->parents={-1,0,1,2,3,4,1,6,7,8,1,10};
        rig->bind_locals={at(0,0,0),at(0,1,0),at(-.2f,.4f,0),at(0,0,-.4f),at(.2f,0,-.2f),at(0,0,.3f),
            at(.2f,.4f,0),at(.4f,0,0),at(-.6f,0,-.2f),at(heavy?.3f:0,0,heavy?0:-.1f),at(0,0,0),at(0,.4f,-.3f)};
        // Reproduce the Heavy's recorded long native span on GUN_R, rather than
        // only the short synthetic GUN_L Heavy layout tested before the playtest.
        if(long_grip)rig->bind_locals[4]=at(-.023f,.5f,.991f);
        std::array<vr_arm::Matrix,12> buffer{};const std::array<int16_t,12> map{11,8,6,4,2,0,10,9,7,5,3,1};
        EngineArmProbe::Pose pose;pose.owner.actor=42;pose.owner.instance=99;pose.layout=rig;pose.world=vr_camera::IDENTITY;pose.world_valid=true;pose.received=now;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());
        for(size_t i=0;i<12;++i){buffer[map[i]]=rig->bind_locals[i];pose.bones.push_back({uint32_t(i),map[i],rig->bind_locals[i]});}
        const auto native=buffer;bool locked{},fail{};
        driver.configure({[](const auto&,auto){return true;},[&](auto){locked=true;},[&](auto){locked=false;},[&](uintptr_t address,const auto& value){check(locked,"constraint writes under owned lock");if(fail){fail=false;return false;}std::memcpy(reinterpret_cast<void*>(address),value.data(),sizeof(value));return true;}});
        VrWeaponAim aim;aim.enable(true);aim.allow(true,7);
        driver.set_weapon_observer([&](const auto& p){check(!locked,"constraint ray published after unlock");aim.publish(p);});
        driver.enable(true);driver.follow_weapon(true);driver.set_left_handed(left);driver.enable_two_hand(true);driver.allow(true,7);
        EngineArmDriver::Targets t;t.actor=42;t.reference=1;t.epoch=7;t.captured=now;t.units_per_metre=1;
        if(long_grip){t.units_per_metre=.9f;t.tracking_units_per_metre=1.45f;}
        const size_t main=left?0:1,other=1-main;
        t.wrists[main]=at(0,1.4f,-.2f);t.wrists[other]=at(0,1.4f,-.6f);
        if(long_grip)t.wrists[other]=left?at(.223f,1.9f,.591f):at(-.223f,1.9f,.591f);
        if(body_grab)for(int i=12;i<15;++i)(*t.wrists[other])[i]=.5f*((*t.wrists[other])[i]+(*t.wrists[main])[i]);
        const auto publish=[&]{buffer=native;driver.targets(t);driver.apply(pose,now);};
        const auto world=[&]{std::vector<vr_arm::Matrix> locals,w;for(auto i:map)locals.push_back(buffer[i]);check(vr_arm::hierarchy(locals,rig->parents,pose.world,w),"constraint committed hierarchy");return w;};
        publish();const auto model=driver.support_model(now);const auto base=aim.pose(42,now);
        check(model && base,"settled primary learns actual native support grip, including recorded Heavy span above .9 Marine metres");
        check(driver.snapshot().weapon_grip==(heavy?vr_weapon::Grip::two_handed:vr_weapon::Grip::right),"bolter and Heavy keep native primary ownership");
        t.support={7,model->generation,true};publish();
        check(driver.snapshot().two_hand_active && aim.pose(42,now) && same(aim.pose(42,now)->world,base->world),"acquiring support keeps gun stable and captures chosen hand position");
        const auto grabbed=vr_camera::multiply(*t.wrists[other],vr_camera::inverse(base->world));
        (*t.wrists[other])[12]+=.1f;
        publish();const auto turned=aim.pose(42,now);auto w=world();
        check(turned && driver.snapshot().two_hand_active && !same(turned->world,base->world),"support wrist alone changes committed barrel in either handed mode");
        check(same(turned->world,w[11]),"rendered Gun_pose equals native firing ray matrix");
        check(same(vr_camera::multiply(grabbed,w[11]),w[other==0?4:8]),"chosen support wrist is rigid to gun");
        check(vr_arm::length(vr_arm::sub(vr_arm::position(w[main==0?4:8]),vr_arm::position(*t.wrists[main])))<1e-4f,"actual dominant grip pivot stays on tracked hand");
        check(aim.hit(*turned,{2,3,4},now),"constrained source owns collision result");
        const auto supported_wrist=t.wrists[other];const auto refused_before=driver.snapshot().two_hand_refusals[0];
        t.support.held=true;t.wrists[other]=at(0,1.4f,long_grip?-2.f:.6f);(*t.wrists[main])[12]+=.025f;
        auto fresh_base=base->world;fresh_base[12]+=.025f;publish();
        check(aim.pose(42,now) && same(aim.pose(42,now)->world,fresh_base) && driver.support_model(now) &&
            driver.support_model(now)->generation==model->generation && !driver.snapshot().two_hand_active &&
            driver.snapshot().two_hand_refusals[0]==refused_before+1,"rejected constraint commits fresh one-hand pose and preserves owned contact");
        t.wrists[other]=supported_wrist;(*t.wrists[main])[12]-=.025f;publish();
        check(aim.pose(42,now) && driver.snapshot().two_hand_active && same(aim.pose(42,now)->world,turned->world),"held grip recovers on next valid frame without release or stale ray");
        t.support.held=false;publish();check(aim.pose(42,now) && same(aim.pose(42,now)->world,base->world) && !driver.snapshot().two_hand_active,"release restores one-wrist gun without moving tracked hand");
        t.support.held=true;fail=true;publish();check(!aim.pose(42,now) && !driver.support_model(now) && buffer==native && !driver.snapshot().two_hand_active,"failed writer rolls back both grips and gun, releases ray and contact");
        publish();check(aim.pose(42,now).has_value(),"next successful owned publication recovers");
        driver.weapon_switch(now);publish();check(!driver.snapshot().two_hand_active,"old generation cannot support new weapon");
        const auto updated=driver.support_model(now);check(updated && updated->generation!=model->generation,"weapon change invalidates acquisition generation");
        t.support={7,updated->generation,true};t.wrists[other].reset();publish();check(aim.pose(42,now) && !driver.snapshot().two_hand_active,"support tracking loss restores one-hand gun, not cached constraint");
        t.wrists[main].reset();publish();check(!aim.pose(42,now),"dominant tracking loss releases ray");
        t.wrists[main]=at(0,1.4f,-.2f);t.wrists[other]=at(.1f,1.4f,-.6f);t.epoch=8;publish();check(!aim.pose(42,now),"wrong epoch never commits support or gun");
    }
}
}
