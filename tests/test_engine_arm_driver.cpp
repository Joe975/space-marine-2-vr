#include "synthetic_code.h"
#include "EngineArmDriver.h"
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include "test_body_render_placement.h"
namespace {
void require(bool value,const char* why) { if (!value) { throw std::runtime_error{why}; } }
auto matrix(vr_arm::Vector p) { auto m=vr_camera::IDENTITY;vr_arm::position(m,p);return m; }
void post_write_resolution() {
    for(const std::string mode:{"dirty_only","resolve","resolve_both","resolve_both_head_hidden","resolve_head_only","wrong_right","right_read_fail","pending_callback","world_disabled","stale_flush","flush_throw","world_read_fail","world_read_throw","persistent_failure","partial_write","changed_under_lock","span_write","span_partial_write","unbound","empty"}) {
        EngineArmDriver driver;const auto now=EngineArmDriver::Clock::now();
        auto layout=std::make_shared<EngineArmProbe::RigLayout>();layout->names={"ROOT","ARM1L","ARM2L","HANDL","ARM1R","ARM2R","HANDR","NECK","HEAD","helmet"};layout->parents={-1,0,1,2,0,4,5,0,7,8};
        layout->bind_locals={matrix({0,0,0}),matrix({-.2f,1.5f,0}),matrix({.42f,0,0}),matrix({.42f,0,0}),matrix({.2f,1.5f,0}),matrix({.42f,0,0}),matrix({.42f,0,0}),matrix({0,1.7f,0}),matrix({0,.2f,0}),matrix({0,.1f,0})};
        std::array<vr_arm::Matrix,11> buffer{},c0{};const std::array<int16_t,10> mapping{10,2,0,3,9,1,4,5,6,7};
        std::vector<vr_arm::Matrix> world;require(vr_arm::hierarchy(layout->bind_locals,layout->parents,vr_camera::IDENTITY,world),"resolution native world");
        for(size_t i=0;i<mapping.size();++i){buffer[mapping[i]]=layout->bind_locals[i];c0[mapping[i]]=world[i];}
        const auto original=buffer;const auto native_world=c0;const auto target=matrix({-.4f,1.2f,-.7f});
        EngineArmProbe::Pose pose;pose.owner.actor=123;pose.owner.instance=456;pose.layout=layout;pose.world=vr_camera::IDENTITY;pose.world_valid=true;pose.received=now;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());
        for(size_t i=0;i<mapping.size();++i)pose.bones.push_back({static_cast<uint32_t>(i),mapping[i],layout->bind_locals[i]});
        bool locked{},pending_cleared{};uint32_t dirty=0,flags=0;unsigned writes{},invalidate_calls{},flush_calls{},world_reads{};std::vector<std::string> events;vr_arm::Matrix saved_bone{};unsigned span_writes{};
        EngineArmDriver::Api api;api.current=[](const auto&,auto){return true;};
        api.enter=[&](auto){require(!locked,"resolve lock enters once");locked=true;dirty=mode=="pending_callback"&&!pending_cleared?2:0;flags=mode=="world_disabled"&&!pending_cleared?4:0;events.push_back("native_clean");if(mode=="changed_under_lock"){saved_bone=buffer[mapping[1]];buffer[mapping[1]][12]+=1;}};
        api.leave=[&](auto){require(locked,"resolve leaves owned lock");events.push_back("leave");locked=false;if(mode=="changed_under_lock")buffer[mapping[1]]=saved_bone;};
        api.write=[&](auto address,const auto& value){require(locked,"resolve D0 write under lock");++writes;events.push_back("write");if(mode=="partial_write"&&writes==2)return false;std::memcpy(reinterpret_cast<void*>(address),value.data(),sizeof(value));SetLastError(444);return true;};
        // Adjacent slots in one write (the native configuration); the per-bone write must not be used then.
        if(mode.starts_with("span"))api.write_bytes=[&](uintptr_t address,const void* data,size_t size){require(locked && size%sizeof(vr_arm::Matrix)==0,"span writes whole slots under lock");
            ++span_writes;events.push_back("write");if(mode=="span_partial_write"&&span_writes==2)return false;std::memcpy(reinterpret_cast<void*>(address),data,size);SetLastError(444);return true;};
        const auto read=[&](auto address,uint32_t& value){require(locked,"resolve metadata under lock");if(address==456+0x2f0)value=dirty;else if(address==456+0x30)value=flags;else return false;SetLastError(555);return true;};
        const auto invalidate=[&](auto instance,uint32_t mask){require(instance==456&&mask==1&&locked,"resolve invalidation ABI");++invalidate_calls;events.push_back("invalidate");dirty|=1;SetLastError(666);};
        const auto flush=[&](auto instance,uint32_t mask){require(instance==456&&mask==1&&locked&&!(dirty&2),"flush mode1 cannot invoke pending animation callback");++flush_calls;events.push_back("flush");
            std::vector<vr_arm::Matrix> locals;for(auto slot:mapping)locals.push_back(buffer[slot]);world.resize(locals.size());for(size_t i=0;i<locals.size();++i)world[i]=vr_camera::multiply(locals[i],layout->parents[i]<0?pose.world:world[layout->parents[i]]);
            for(size_t i=0;i<mapping.size();++i)c0[mapping[i]]=world[i];dirty&=~1u;
            if(mode=="persistent_failure"||(mode=="stale_flush"&&flush_calls==1))c0[mapping[3]][12]+=1;
            if(mode=="wrong_right"&&flush_calls==1)c0[mapping[6]][12]+=1;SetLastError(777);if(mode=="flush_throw"&&flush_calls==1)throw std::runtime_error{"fixture flush exception after C0 mutation"};};
        api.invalidate_pose=[&](auto instance){if(mode=="dirty_only")return vr_arm_cache::invalidate(instance,[&](auto,uint32_t& value){value=dirty;return true;},invalidate);return vr_arm_cache::resolve(instance,read,invalidate,flush);};
        api.read_evaluated=[&](auto instance,uint32_t index,auto& result){require(instance==456&&index<11&&locked,"actual C0 reader uses mapped native index before unlock");++world_reads;if(mode=="right_read_fail"&&world_reads==2)return false;
            if(world_reads==1&&mode=="world_read_throw")throw std::runtime_error{"C0 read callback exception"};if(world_reads==1&&mode=="world_read_fail")return false;result=c0[index];return true;};
        if(mode=="unbound")api.invalidate_pose={};driver.configure(api);driver.enable(mode!="empty"&&mode!="resolve_head_only");driver.hide_head(mode=="resolve_both_head_hidden"||mode=="resolve_head_only");driver.enable_pose_invalidation(true);if(mode!="dirty_only")driver.enable_pose_resolution(true);driver.allow(true,5);
        EngineArmDriver::Targets targets;targets.actor=123;targets.epoch=5;targets.reference=1;targets.captured=now;targets.wrists[0]=target;if(mode=="resolve_both"||mode=="resolve_both_head_hidden"||mode=="wrong_right"||mode=="right_read_fail")targets.wrists[1]=matrix({.4f,1.2f,-.7f});driver.targets(targets);driver.apply(pose,now);
        const auto state=driver.snapshot();require(!locked,"resolve exits lock on all paths");
        // The native skin producer applies inverse-bind * C0 and writes its
        // transpose as three float4 rows. A bind-space vertex at the wrist must
        // therefore land at the current tracked wrist, not its old animation.
        const auto inverse_bind=vr_camera::inverse(native_world[mapping[3]]);
        const auto skin=vr_camera::multiply(inverse_bind,c0[mapping[3]]);
        const auto vertex=vr_arm::position(native_world[mapping[3]]);const auto skinned=vr_camera::multiply(matrix(vertex),skin);
        const auto skin_error=vr_arm::length(vr_arm::sub(vr_arm::position(skinned),vr_arm::position(target)));
        if((mode=="resolve_both_head_hidden"||mode=="resolve_head_only")&&state.invalidation.status==std::string_view{"expected_world_unavailable"}) {
            require(writes==0&&invalidate_calls==0&&flush_calls==0,"old head-order refusal must occur before writes or native calls");
            require(false,"head-hidden path refused expected_world_unavailable with writes0/invalidate0/flush0");
        }
        if(mode=="span_write")require(writes==0 && span_writes>=1,"the native configuration writes adjacent slots together, never bone by bone");
        if(mode=="resolve"||mode=="span_write"||mode=="resolve_both"||mode=="resolve_both_head_hidden"||mode=="resolve_head_only") {
            const unsigned hands=mode=="resolve_head_only"?0:(mode=="resolve"||mode=="span_write"?1:2);
            require(state.hands==hands&&state.invalidation.valid&&state.invalidation.resolved&&state.invalidation.verified_hands==hands&&flush_calls==1&&invalidate_calls==1,"actual driver resolves and verifies mapped C0 before successful publication");
            if(hands)require(skin_error<1e-4f,"draw-time inverse-bind/C0 skin consumer receives corrected VR pose");
            if(mode=="resolve_both_head_hidden"||mode=="resolve_head_only")require(state.head_updates==1&&!vr_camera::rigid(buffer[mapping[8]])&&!vr_camera::rigid(c0[mapping[8]])&&!vr_camera::rigid(buffer[mapping[9]])&&!vr_camera::rigid(c0[mapping[9]]),"real HEAD/NECK/helmet visibility path commits affine head cache without rejecting tracked hand verification");
            require(events[events.size()-3]=="invalidate"&&events[events.size()-2]=="flush"&&events.back()=="leave","invalidate then world flush after last D0 write and before unlock");
            require(GetLastError()==444,"resolution preserves native last-error");
        }else if(mode=="dirty_only")require(flush_calls==0&&skin_error>.1f,"dirty-only candidate leaves actual skin consumer reading old C0");
        else {
            require(state.hands==0&&buffer==original,"refused resolution restores D0 and never publishes VR hands");
            if(mode=="pending_callback"||mode=="world_disabled")require(flush_calls==0&&invalidate_calls==0,"unsafe pending callback/disabled evaluator never invoked");
            if(mode=="unbound"||mode=="partial_write"||mode=="span_partial_write"||mode=="empty")require(flush_calls==0,"no flush for empty, unsupported or incomplete D0 batch");
            if(mode=="changed_under_lock")require(writes==0&&state.rejected==1&&flush_calls==0,"a patched bone the game changed before the write rejects the whole batch (one span read)");
            if(mode=="wrong_right"||mode=="right_read_fail"||mode=="stale_flush"||mode=="flush_throw"||mode=="world_read_fail"||mode=="world_read_throw")require(flush_calls==2&&c0==native_world&&state.recovery_calls==1&&!state.pose_recovery_failed,"post-flush failure repairs restored native C0 before unlock");
            if(mode=="pending_callback"||mode=="world_disabled"){require(!state.pose_recovery_failed&&state.recovery_calls==0,"pre-native refusal does not poison or try impossible recovery");pending_cleared=true;driver.apply(pose,now);require(driver.snapshot().hands==1,"pending animation/evaluator gate can retry after it clears");}
            if(mode=="persistent_failure") {require(state.pose_recovery_failed&&state.recovery_failures==1,"unrecoverable evaluated cache poisons candidate");const auto before=writes;driver.apply(pose,now);require(writes==before,"poisoned candidate cannot publish another D0 batch");}
        }
    }
    const auto signature=synthetic_at<16>(0x1d93150);
    require(vr_arm_cache::matches_flush(signature),"exact native flush signature accepted");auto bad=signature;bad[4]^=1;require(!vr_arm_cache::matches_flush(bad),"different native flush refused");
}
void post_write_invalidation() {
    for(const std::string mode:{"off","on","writer_fail","unbound","conflict","bad_invalidation","native_throw","callback_throw","read_before_fail","read_after_fail","empty"}) {
        EngineArmDriver driver;const auto now=EngineArmDriver::Clock::now();
        auto layout=std::make_shared<EngineArmProbe::RigLayout>();layout->names={"ROOT","ARM1L","ARM2L","HANDL"};layout->parents={-1,0,1,2};
        layout->bind_locals={matrix({0,0,0}),matrix({-.2f,1.5f,0}),matrix({.42f,0,0}),matrix({.42f,0,0})};
        std::array<vr_arm::Matrix,5> buffer{};const std::array<int16_t,4> mapping{4,2,0,3};
        EngineArmProbe::Pose pose;pose.owner.actor=123;pose.owner.instance=456;pose.layout=layout;pose.world=vr_camera::IDENTITY;pose.world_valid=true;pose.received=now;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());
        for(size_t i=0;i<mapping.size();++i){buffer[mapping[i]]=layout->bind_locals[i];pose.bones.push_back({static_cast<uint32_t>(i),mapping[i],layout->bind_locals[i]});}
        const auto original=buffer;uint32_t dirty=1;bool locked{};size_t writes{},native_calls{};std::vector<std::string> events;
        std::vector<vr_arm::Matrix> world;vr_arm::hierarchy(layout->bind_locals,layout->parents,pose.world,world);auto cached=world[3];
        const auto target=matrix({-.4f,1.2f,-.7f});
        EngineArmDriver::Api api;
        api.current=[](const auto&,auto){return true;};
        api.enter=[&](auto instance){require(instance==456&&!locked,"invalidate enters exact instance lock");locked=true;dirty=0;events.push_back("native_clean");if(mode=="conflict")buffer[mapping[2]][12]+=.1f;};
        api.leave=[&](auto instance){require(instance==456&&locked,"invalidate leaves exact instance lock");events.push_back("leave");locked=false;};
        api.write=[&](auto address,const auto& value){require(locked,"invalidation candidate writes under native lock");++writes;events.push_back("write");if(mode=="writer_fail"&&writes==2)return false;std::memcpy(reinterpret_cast<void*>(address),value.data(),sizeof(value));SetLastError(444);return true;};
        unsigned reads{};api.invalidate_pose=[&](auto instance){return vr_arm_cache::invalidate(instance,
            [&](auto p,uint32_t& value){require(p==456&&locked,"dirty read stays under instance lock");++reads;if(mode=="read_before_fail"||(mode=="read_after_fail"&&reads==2))return false;value=dirty;SetLastError(555);return true;},
            [&](auto p,uint32_t mask){require(p==456&&mask==1&&locked,"native invalidation ABI instance and mask1");++native_calls;events.push_back("invalidate");
                std::vector<vr_arm::Matrix> locals;for(auto slot:mapping)locals.push_back(buffer[slot]);require(vr_arm::hierarchy(locals,layout->parents,pose.world,world)&&vr_arm::length(vr_arm::sub(vr_arm::position(world[3]),vr_arm::position(target)))<1e-4f,"native invalidation occurs only after entire corrected D0 batch");
                if(mode=="native_throw")throw std::runtime_error{"fixture native adapter exception"};if(mode!="bad_invalidation")dirty|=mask;SetLastError(666);});};
        if(mode=="unbound")api.invalidate_pose={};driver.configure(api);driver.enable(mode!="empty");driver.hide_head(mode=="empty");
        if(mode=="callback_throw"){api.invalidate_pose=[](auto)->vr_arm_cache::Receipt{throw std::runtime_error{"outer adapter exception"};};driver.configure(api);}
        if(mode!="off")driver.enable_pose_invalidation(true);driver.allow(true,5);
        EngineArmDriver::Targets targets;targets.actor=123;targets.epoch=5;targets.reference=1;targets.captured=now;targets.wrists[0]=target;driver.targets(targets);driver.apply(pose,now);
        const auto state=driver.snapshot();require(!locked,"candidate exits lock on every path");
        if(mode=="on") {
            require(GetLastError()==444,"native invalidation preserves writer last-error across reads and native call");
            require(native_calls==1&&state.invalidation_calls==1&&state.invalidation_refused==0&&state.invalidation.before==0&&state.invalidation.after==1,"clean gap is invalidated exactly once after successful mapped publication");
            require(events.size()>2&&events[events.size()-2]=="invalidate"&&events.back()=="leave","native invalidation before same-instance unlock");
            if(dirty&1){std::vector<vr_arm::Matrix> locals;for(auto slot:mapping)locals.push_back(buffer[slot]);vr_arm::hierarchy(locals,layout->parents,pose.world,world);cached=world[3];dirty=0;}
            require(vr_arm::length(vr_arm::sub(vr_arm::position(cached),vr_arm::position(target)))<1e-4f,"next normal evaluation sees corrected tracked wrist after cache was clean in the gap");
        }else if(mode=="off")require(native_calls==0&&!state.invalidate_pose_enabled&&vr_arm::length(vr_arm::sub(vr_arm::position(cached),vr_arm::position(target)))>.1f,"default-off leaves old behavior and exposes stale-cache fixture");
        else if(mode=="bad_invalidation"||mode=="native_throw"||mode=="callback_throw"||mode=="read_after_fail"||mode=="read_before_fail")require(native_calls==((mode=="read_before_fail"||mode=="callback_throw")?0u:1u)&&state.invalidation_refused==1&&buffer==original&&state.hands==0,"failed native dirty receipt or exception rolls back D0 with no second native call or pose publication");
        else {require(native_calls==0,"no native invalidation for no writes, refused batch or partial-write rollback");if(mode!="conflict")require(buffer==original,"noncommitted candidate preserves original D0 when rollback writes recover");if(mode=="unbound")require(writes==0&&state.invalidation_refused==1,"unsupported requested binding refuses before writes");}
    }
    const auto sig=synthetic_at<16>(0x1d92e40);
    require(vr_arm_cache::matches(0x6ac3e716,0x5000000,sig)&&!vr_arm_cache::matches(0,0x5000000,sig)&&!vr_arm_cache::matches(0x6ac3e716,1,sig),"native invalidation binding requires exact stamp and image bound");
    auto bad=sig;bad[3]^=1;require(!vr_arm_cache::matches(0x6ac3e716,0x5000000,bad),"different native helper signature is refused");
    for(const std::string mode:{"before","after","throw"}) {
        unsigned reads{},calls{};SetLastError(999);
        const auto receipt=vr_arm_cache::invalidate(456,[&](auto,uint32_t& value){SetLastError(111);++reads;value=1;return mode!="before"&&(mode!="after"||reads!=2);},
            [&](auto,uint32_t){++calls;SetLastError(222);if(mode=="throw")throw std::runtime_error{"native failure"};});
        require(GetLastError()==999&&!receipt.valid&&calls==(mode=="before"?0u:1u),"adapter refuses failed reads/exceptions with at most one native call and preserves LastError");
    }
}
void recorded_embodiment_publication() {
    // Real hierarchy/segment lengths, with 19 Oct recorded median/peak sprint lean.
    // Scrambled skin indices exercise the actual guarded mapped publication, not
    // just a mathematical shadow solve. A synthetic root keeps the fixture portable.
    for(float degrees:{55.87f,78.75f}) for(float yaw:{0.f,1.5707963f}) for(float twist:{0.f,48.6f}) {
        EngineArmDriver driver;const auto now=EngineArmDriver::Clock::now();
        auto layout=std::make_shared<EngineArmProbe::RigLayout>();
        layout->names={"ROOT","CENTRE","BACKdown","BACKmiddle","BACKup","NECK","HEAD","clavicleR","ARM1R","ARM2R","forearm_R","HANDR","shoulder_R","clavicleL","ARM1L","ARM2L","forearm_L","HANDL","LEG1R"};
        layout->parents={-1,0,1,2,3,4,5,4,7,8,9,9,8,4,13,14,15,15,1};
        constexpr float upper=.422250628f,lower=.422810793f;
        layout->bind_locals={matrix({0,0,0}),matrix({0,1.2f,0}),matrix({0,.1f,0}),matrix({0,.35f,0}),matrix({0,.35f,0}),matrix({0,.3f,0}),matrix({0,.2f,0}),
            matrix({.2f,0,0}),matrix({.25f,0,0}),matrix({-upper,0,0}),matrix({-lower,0,0}),matrix({-lower,0,0}),matrix({0,.1f,0}),
            matrix({-.2f,0,0}),matrix({-.25f,0,0}),matrix({upper,0,0}),matrix({lower,0,0}),matrix({lower,0,0}),matrix({.15f,-.6f,0})};
        // Plus the native stance turn (left shoulder leading; Joe 21 Oct standing median 48.6 degrees).
        auto native=layout->bind_locals;native[2]=vr_camera::multiply(vr_upright::rotation({1,0,0},-degrees/57.2957795f),vr_camera::multiply(vr_upright::rotation({0,1,0},-twist/57.2957795f),native[2]));
        std::array<vr_arm::Matrix,19> buffer{};
        EngineArmProbe::Pose pose;pose.owner.actor=7;pose.owner.instance=8;pose.layout=layout;pose.world=vr_upright::rotation({0,1,0},yaw);vr_arm::position(pose.world,{5,2,-3});
        pose.world_valid=true;pose.received=now;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());
        for(size_t i=0;i<native.size();++i) {const auto skin=static_cast<int16_t>((i*7)%19);buffer[skin]=native[i];pose.bones.push_back({static_cast<uint32_t>(i),skin,native[i]});}
        const auto before=buffer;bool locked{};
        driver.configure({[](const auto&,auto){return true;},[&](auto){locked=true;},[&](auto){locked=false;},[&](uintptr_t address,const auto& value){require(locked,"embodiment writes share native lock");std::memcpy(reinterpret_cast<void*>(address),value.data(),sizeof(value));return true;}});
        driver.enable(true);driver.enable_upright(true);driver.allow(true,3);
        EngineArmDriver::Targets targets;targets.actor=7;targets.reference=1;targets.epoch=3;targets.captured=now;
        targets.wrists[0]=vr_camera::multiply(matrix({-.45f,2.3f,-1.1f}),pose.world);targets.wrists[1]=vr_camera::multiply(matrix({.45f,2.3f,-1.1f}),pose.world);
        driver.targets(targets);driver.apply(pose,now);
        auto published=native;for(size_t i=0;i<published.size();++i) published[i]=buffer[(i*7)%19];
        std::vector<vr_arm::Matrix> world;
        require(vr_arm::hierarchy(published,layout->parents,pose.world,world),"recorded publication reconstructs");
        const auto lean=vr_upright::lean(published,layout->bind_locals,layout->parents,layout->names,pose.world);
        require(lean && *lean<.1f,"native publication removes measured sprint lean beyond the old cap");
        const auto twisted=vr_upright::twist(published,layout->bind_locals,layout->parents,layout->names,pose.world);
        require(twisted && std::abs(*twisted)<.1f && std::abs(std::abs(driver.snapshot().upright_twist_before_degrees)-twist)<.1f,"published shoulders are square to the body; the driver reports the native twist");
        std::vector<vr_arm::Matrix> bound;require(vr_arm::hierarchy(layout->bind_locals,layout->parents,pose.world,bound),"bind reconstructs");
        for(size_t i:{size_t(8),size_t(14)}) require(vr_arm::length(vr_arm::sub(vr_arm::position(world[i]),vr_arm::position(bound[i])))<1e-3f,"both arm roots are back where the bind pose has them");
        for(const auto [armour,wrist,hand]:{std::array<size_t,3>{10,11,1},std::array<size_t,3>{16,17,0}}) {
            require(vr_arm::length(vr_arm::sub(vr_arm::position(world[armour]),vr_arm::position(world[wrist])))<1e-4f,"real native publication keeps armour endpoint at tracked wrist");
            for(size_t i=0;i<16;++i)require(std::abs(world[wrist][i]-targets.wrists[hand]->at(i))<1e-4f,"tracked wrist position and orientation remain exact");
        }
        for(size_t i:{size_t(0),size_t(1),size_t(18)})require(buffer[(i*7)%19]==before[(i*7)%19],"root, pelvis and legs remain untouched");
        require(driver.snapshot().hands==2 && driver.snapshot().upright_updates==1 && driver.snapshot().failures==0,"both hands and torso committed atomically");
    }
}
// Upright upper body through the real driver: a 12 degree run lean on BACKdown is removed
// in the published buffer before arm IK; off by default; traced native vs final.
void upright_publication() {
    EngineArmDriver driver;const auto now=EngineArmDriver::Clock::now();
    auto layout=std::make_shared<EngineArmProbe::RigLayout>();
    layout->names={"ROOT","CENTRE","BACKdown","BACKmiddle","BACKup","NECK","HEAD","clavicleR","ARM1R","ARM2R","HANDR","shoulder_R"};
    layout->parents={-1,0,1,2,3,4,5,4,7,8,9,8};
    layout->bind_locals={matrix({0,0,0}),matrix({0,1.2f,0}),matrix({0,.1f,0}),matrix({0,.35f,0}),matrix({0,.35f,0}),matrix({0,.3f,0}),matrix({0,.2f,0}),
        matrix({.2f,0,0}),matrix({.25f,0,0}),matrix({.4f,0,0}),matrix({.4f,0,0}),matrix({0,.1f,0})};
    auto native=layout->bind_locals;native[2]=vr_camera::multiply(vr_upright::rotation({1,0,0},-12/57.2957795f),native[2]);
    std::array<vr_arm::Matrix,12> buffer{};
    EngineArmProbe::Pose pose;pose.owner.actor=7;pose.owner.instance=8;pose.layout=layout;pose.world=vr_camera::IDENTITY;pose.world_valid=true;pose.received=now;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());
    pose.rig=0x123;pose.mapping=0x456;
    for(size_t i=0;i<native.size();++i) {buffer[i]=native[i];pose.bones.push_back({static_cast<uint32_t>(i),static_cast<int16_t>(i),native[i]});}
    const auto published=buffer;bool locked{};
    driver.configure({[](const auto&,auto){return true;},[&](auto){locked=true;},[&](auto){locked=false;},[&](uintptr_t address,const auto& value){require(locked,"upright writes share the native publication lock");std::memcpy(reinterpret_cast<void*>(address),value.data(),sizeof(value));return true;}});
    driver.enable(true);driver.allow(true,3);
    EngineArmDriver::Targets targets;targets.actor=7;targets.reference=1;targets.epoch=3;targets.captured=now;targets.wrists[1]=matrix({.6f,2.3f,-.3f});
    driver.targets(targets);driver.apply(pose,now);
    require(buffer[2]==published[2] && driver.snapshot().upright_updates==0,"upright correction is off by default: BACKdown untouched");
    require(!driver.snapshot().upright_enabled,"default snapshot reports upright off");
    vr_upright::Settings bad;bad.max_degrees=91;require(!driver.set_upright_settings(bad),"invalid upright settings refused");
    buffer=published;driver.enable_upright(true);
    const auto trace_path=std::filesystem::temp_directory_path()/"sm2vr_upright_trace_test.jsonl";
    require(vr_trace::Recorder::get().open(trace_path,10,now),"trace opens");
    driver.targets(targets);driver.apply(pose,now);
    vr_trace::Recorder::get().tick(now);vr_trace::Recorder::get().close();
    std::array<vr_arm::Matrix,12> locals_after{};for(size_t i=0;i<12;++i) locals_after[i]=buffer[i];
    const auto lean_after=vr_upright::lean(locals_after,layout->bind_locals,layout->parents,layout->names,pose.world);
    require(buffer[2]!=published[2] && lean_after && *lean_after<0.1f,"published spine is upright");
    require(driver.snapshot().upright_updates==1 && std::abs(driver.snapshot().upright_before_degrees-12)<0.1f,"driver counts and reports the corrected lean");
    std::vector<vr_arm::Matrix> world,upright;
    require(vr_arm::hierarchy(locals_after,layout->parents,pose.world,world) && vr_arm::hierarchy(layout->bind_locals,layout->parents,pose.world,upright),"hierarchies");
    require(vr_arm::length(vr_arm::sub(vr_arm::position(world[6]),vr_arm::position(upright[6])))<1e-3f,"published head is back over the pelvis");
    require(vr_arm::length(vr_arm::sub(vr_arm::position(world[10]),vr_arm::position(*targets.wrists[1])))<1e-3f,"arm IK still reaches the tracked wrist from the upright shoulder");
    require(buffer[0]==published[0] && buffer[1]==published[1],"root and pelvis stay native");
    std::string all,line;{std::ifstream in{trace_path};while(std::getline(in,line)) all+=line+'\n';}
    std::filesystem::remove(trace_path);
    require(all.find("\"publication_instance\":\"8\"")!=std::string::npos &&
        all.find("\"publication_buffer\":\""+vr_trace::Events::hex(pose.buffer)+"\"")!=std::string::npos &&
        all.find("\"publication_rig\":\"123\"")!=std::string::npos && all.find("\"publication_mapping\":\"456\"")!=std::string::npos,
        "real driver trace preserves the authoritative captured publication identities");
    for(const char* part:{"\"lean_degrees\":{\"native\":12.00,\"final\":0.00}","\"body\":[{\"name\":\"BACKdown\"","\"twist_degrees\":{\"native\":null,\"final\":null}","\"head_final\":[","{\"name\":\"shoulder_R\""})
        require(all.find(part)!=std::string::npos,part);
    // An unmapped BACKdown (not in the published buffer) is refused rather than half-applied.
    // The lean comes from BACKmiddle here, so correcting it would need the unmapped BACKdown.
    auto unmapped=pose;unmapped.bones.erase(unmapped.bones.begin()+2);
    unmapped.bones[2].local=vr_camera::multiply(vr_upright::rotation({1,0,0},-12/57.2957795f),layout->bind_locals[3]);buffer=published;
    require(vr_trace::Recorder::get().open(trace_path,10,now),"unmapped publication trace opens");
    driver.targets(targets);driver.apply(unmapped,now);
    vr_trace::Recorder::get().tick(now);vr_trace::Recorder::get().close();
    require(driver.snapshot().upright_rejected==1 && buffer[2]==published[2],"unmapped spine bone rejects the correction");
    all.clear();{std::ifstream in{trace_path};while(std::getline(in,line))all+=line+'\n';}std::filesystem::remove(trace_path);
    require(all.find("\"name\":\"BACKdown\",\"native\":null,\"final\":null,\"skin_index\":null")!=std::string::npos,
        "unmapped native body nodes produce unavailable evidence rather than bind-pose positions");
}
// A hitch clears the probe's pose, and the next publication builds a new layout for the same rig.
void weapon_grip_survives_a_rebuilt_layout() {
    using vr_weapon::Grip;EngineArmDriver driver;const auto now=EngineArmDriver::Clock::now();
    auto layout=std::make_shared<EngineArmProbe::RigLayout>();
    layout->names={"ROOT","BACKup","ARM1R","ARM2R","HANDR","ARM1L","ARM2L","HANDL","GUN_ANIM","Gun_pose","GUN_L","GUN_R"};
    layout->parents={-1,0,1,2,3,1,5,6,1,8,7,4};
    // The pistol: Gun_pose on GUN_L, GUN_R two metres away.
    layout->bind_locals={matrix({0,0,0}),matrix({0,1,0}),matrix({.2f,.5f,0}),matrix({.4f,0,0}),matrix({.4f,0,0}),matrix({-.2f,.5f,0}),matrix({-.4f,0,0}),matrix({-.4f,0,0}),
        matrix({0,0,0}),matrix({-.9f,.5f,0}),matrix({.1f,0,0}),matrix({.1f,0,0})};
    std::array<vr_arm::Matrix,12> buffer{};
    EngineArmProbe::Pose pose;pose.owner.actor=42;pose.owner.instance=99;pose.layout=layout;pose.world=vr_camera::IDENTITY;pose.world_valid=true;pose.received=now;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());
    for(size_t i=0;i<buffer.size();++i) {buffer[i]=layout->bind_locals[i];pose.bones.push_back({static_cast<uint32_t>(i),static_cast<int16_t>(i),layout->bind_locals[i]});}
    const auto original=buffer;
    driver.configure({[](const auto&,auto){return true;},[](auto){},[](auto){},[](uintptr_t address,const auto& value){std::memcpy(reinterpret_cast<void*>(address),value.data(),sizeof(value));return true;}});
    driver.enable(true);driver.follow_weapon(true);driver.allow(true,5);
    EngineArmDriver::Targets targets;targets.actor=42;targets.reference=2;targets.epoch=5;targets.captured=now;targets.units_per_metre=.9f;
    targets.wrists[0]=matrix({-.4f,1.2f,-.4f});targets.wrists[1]=matrix({.4f,1.2f,-.4f});
    driver.targets(targets);driver.apply(pose,now);
    require(driver.snapshot().weapon_grip==Grip::left,"the pistol is in the left hand");
    pose.layout=std::make_shared<EngineArmProbe::RigLayout>(*layout);buffer=original;
    driver.targets(targets);driver.apply(pose,now);
    require(driver.snapshot().weapon_grip==Grip::left,"a rebuilt layout for the same actor leaves it there");
    pose.owner.actor=43;targets.actor=43;buffer=original;
    driver.targets(targets);driver.apply(pose,now);
    require(driver.snapshot().weapon_grip==Grip::right,"a new actor holds the primary");
}
void weapon_publication() {
    EngineArmDriver driver;const auto now=EngineArmDriver::Clock::now();
    auto layout=std::make_shared<EngineArmProbe::RigLayout>();
    layout->names={"ROOT","BACKup","ARM1R","ARM2R","HANDR","GUN_ANIM","Gun_pose","OTHER","GUN_R"};
    layout->parents={-1,0,1,2,3,1,5,0,4};
    layout->bind_locals={matrix({0,0,0}),matrix({0,1,0}),matrix({.2f,.5f,0}),matrix({.4f,0,0}),matrix({.4f,0,0}),matrix({.1f,.2f,.3f}),matrix({.2f,0,0}),matrix({0,0,3}),matrix({.1f,0,0})};
    // Deliberately scrambled model indices, as live animsets use different maps.
    const std::array<int16_t,9> mapping{8,6,4,2,0,7,5,3,1};
    std::array<vr_arm::Matrix,9> buffer{};
    EngineArmProbe::Pose pose;pose.owner.actor=42;pose.owner.instance=99;pose.layout=layout;pose.world=vr_camera::IDENTITY;pose.world_valid=true;pose.received=now;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());
    for(size_t i=0;i<mapping.size();++i) {buffer[mapping[i]]=layout->bind_locals[i];pose.bones.push_back({static_cast<uint32_t>(i),mapping[i],layout->bind_locals[i]});}
    const auto original=buffer;bool locked{},fail{};VrWeaponAim aim;aim.enable(true);aim.allow(true,5);
    driver.configure({[](const auto&,auto){return true;},[&](auto){locked=true;},[&](auto){locked=false;},[&](uintptr_t address,const auto& value){require(locked,"weapon writes share native arm publication lock");if(fail){fail=false;return false;}std::memcpy(reinterpret_cast<void*>(address),value.data(),sizeof(value));return true;}});
    driver.set_weapon_observer([&](const auto& value){require(!locked,"weapon ray publishes after releasing native instance lock");aim.publish(value);});
    driver.enable(true);driver.follow_weapon(true);driver.allow(true,5);
    EngineArmDriver::Targets targets;targets.actor=42;targets.reference=2;targets.epoch=5;targets.captured=now;targets.units_per_metre=.9f;targets.wrists[1]=matrix({.4f,1.2f,-.4f});
    const auto trace_path=std::filesystem::temp_directory_path()/"sm2vr_weapon_trace_test.jsonl";
    require(vr_trace::Recorder::get().open(trace_path,10,now),"trace opens");
    driver.targets(targets);driver.apply(pose,now);const auto source=aim.pose(42,now);
    {
        vr_trace::Recorder::get().tick(now);vr_trace::Recorder::get().close();
        std::string all,line;{std::ifstream in{trace_path};while(std::getline(in,line)) all+=line+'\n';}
        std::filesystem::remove(trace_path);
        require(all.find("{\"name\":\"GUN_R\",\"parent\":\"HANDR\"}")!=std::string::npos && all.find("{\"name\":\"Gun_pose\",\"parent\":\"GUN_ANIM\"}")!=std::string::npos,
            "trace lists the weapon bones with their parents");
        const auto gun_pose=all.find("{\"name\":\"Gun_pose\",\"native\":");
        require(gun_pose!=std::string::npos,"Gun_pose is sampled");
        const auto final_at=all.find("\"final\":",gun_pose);
        require(final_at!=std::string::npos && all.compare(final_at+8,4,"null")!=0,"the retargeted Gun_pose has a published (final) pose in the trace");
        // GUN_R hangs 0.1 units off HANDR at 0.9 units per metre: it follows the solved hand.
        const auto gun_r=all.find("{\"name\":\"GUN_R\",\"native\":");
        require(gun_r!=std::string::npos && all.find("\"to_right_hand_metres\":0.111",gun_r)!=std::string::npos,"a weapon bone on the hand stays 0.111 m from it");
    }
    require(source && source->hand==1,"committed primary gun pose publishes its observed hand for sidearm selection");
    require(source && buffer[mapping[6]]!=original[mapping[6]] && buffer[mapping[5]]==original[mapping[5]] && buffer[mapping[7]]==original[mapping[7]],"native writer moves separate Gun_pose while preserving parent and unrelated bones");
    // Same tracked wrist, but native shot animation flips the independent gun branch.
    auto firing_pose=pose;firing_pose.bones[6].local[0]=firing_pose.bones[6].local[10]=-1;
    buffer=original;buffer[mapping[6]]=firing_pose.bones[6].local;driver.apply(firing_pose,now);
    const auto firing_source=aim.pose(42,now);
    require(firing_source && firing_source->world==source->world,"real writer and shot-ray publication retain the committed grip through native firing rotation");
    buffer=original;driver.apply(pose,now);
    auto committed=layout->bind_locals;for(size_t i=0;i<mapping.size();++i) committed[i]=buffer[mapping[i]];
    std::vector<vr_arm::Matrix> world;require(vr_arm::hierarchy(committed,layout->parents,pose.world,world),"committed hierarchy reconstructs");
    for(size_t i=0;i<16;++i)require(std::abs(world[6][i]-source->world[i])<1e-4f,"native attachment and published collision source must be the same pose");
    buffer=original;targets.wrists[1]=matrix({.4f,1.2f,-1.4f});driver.targets(targets);driver.apply(pose,now);
    for(size_t i=0;i<mapping.size();++i)committed[i]=buffer[mapping[i]];
    require(vr_arm::hierarchy(committed,layout->parents,pose.world,world),"extended arm publication reconstructs");
    require(vr_arm::length(vr_arm::sub(vr_arm::position(world[4]),vr_arm::position(*targets.wrists[1])))<1e-4f,"real driver publishes tracked wrist even beyond native reach");
    {
        // The melee hit check's sword (EngineMeleeSweep): GUN_R relative to the root bone, as
        // written and as animated, with both evaluated-node indices.
        std::optional<EngineArmDriver::Sword> sword;driver.set_sword_observer([&](const auto& s){sword=s;});
        buffer=original;driver.apply(pose,now);driver.set_sword_observer({});
        require(sword && sword->instance==99 && sword->sword==mapping[8] && sword->root==mapping[0],"the sword sample names GUN_R's and the root's evaluated nodes");
        for(size_t i=0;i<mapping.size();++i)committed[i]=buffer[mapping[i]];
        require(vr_arm::hierarchy(committed,layout->parents,pose.world,world),"sword publication reconstructs");
        std::vector<vr_arm::Matrix> animated;require(vr_arm::hierarchy(layout->bind_locals,layout->parents,pose.world,animated),"animated hierarchy");
        const auto drawn=vr_camera::multiply(sword->final,world[0]),native=vr_camera::multiply(sword->native,animated[0]);
        for(size_t i=0;i<16;++i)require(std::abs(drawn[i]-world[8][i])<1e-4f && std::abs(native[i]-animated[8][i])<1e-4f,"relative times the root's world is GUN_R as written and as animated");
        require(drawn!=native,"the written sword is not the animated one");
    }
    targets.wrists[1]=matrix({.4f,1.2f,-.4f});driver.targets(targets);
    buffer=original;fail=true;driver.apply(pose,now);require(buffer==original && !aim.pose(42,now),"failed writer rolls back and releases collision aim");
    buffer=original;targets.wrists[1].reset();driver.targets(targets);driver.apply(pose,now);require(buffer==original && !aim.pose(42,now),"right hand loss cannot leave the gun/ray attached to cached input");
    targets.wrists[1]=matrix({.4f,1.2f,-.4f});driver.targets(targets);driver.apply(pose,now);driver.follow_weapon(false);require(!aim.pose(42,now),"live weapon off clears pose immediately");
}
}
// Viewer entry (docs/VIEWER_MODE.md): the head is restored while the arms keep the player's
// pose, so photo mode freezes tracked arms under a full-size head.
void head_release_keeps_arms() {
    const auto now=EngineArmDriver::Clock::now();
    auto layout=std::make_shared<EngineArmProbe::RigLayout>();layout->names={"ROOT","ARM1L","ARM2L","HANDL","ARM1R","ARM2R","HANDR","NECK","HEAD","helmet"};layout->parents={-1,0,1,2,0,4,5,0,7,8};
    layout->bind_locals={matrix({0,0,0}),matrix({-.2f,1.5f,0}),matrix({.42f,0,0}),matrix({.42f,0,0}),matrix({.2f,1.5f,0}),matrix({.42f,0,0}),matrix({.42f,0,0}),matrix({0,1.7f,0}),matrix({0,.2f,0}),matrix({0,.1f,0})};
    std::array<vr_arm::Matrix,10> buffer{};
    EngineArmProbe::Pose pose;pose.owner.actor=123;pose.owner.instance=456;pose.layout=layout;pose.world=vr_camera::IDENTITY;pose.world_valid=true;pose.received=now;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());
    for(size_t i=0;i<buffer.size();++i){buffer[i]=layout->bind_locals[i];pose.bones.push_back({static_cast<uint32_t>(i),static_cast<int16_t>(i),layout->bind_locals[i]});}
    const auto native=buffer;
    for(const bool arms:{true,false}) {
        EngineArmDriver driver;bool locked{};
        driver.configure({[](const auto&,auto){return true;},[&](uintptr_t){locked=true;},[&](uintptr_t){locked=false;},
            [&](uintptr_t p,const auto& value){require(locked,"head release writes under the native lock");std::memcpy(reinterpret_cast<void*>(p),value.data(),sizeof(value));return true;}});
        driver.enable(arms);driver.hide_head(true);
        EngineArmDriver::Targets targets;targets.actor=123;targets.reference=2;targets.epoch=5;targets.captured=now;targets.wrists={matrix({-.4f,1.2f,-.4f}),matrix({.4f,1.2f,-.4f})};
        // First person: arms posed and head shrunk.
        buffer=native;driver.allow(true,5);driver.targets(targets);driver.apply(pose,now);
        require(driver.snapshot().head_updates==1 && !vr_camera::rigid(buffer[8]) && driver.snapshot().hands==(arms?2u:0u),"first person shrinks the head and poses the arms");
        // The game republishes its own pose; the viewer's release stops only the head.
        buffer=native;driver.allow(true,5,false);driver.targets(targets);driver.apply(pose,now);
        const auto state=driver.snapshot();
        require(state.head_updates==1 && buffer[7]==native[7] && buffer[8]==native[8] && buffer[9]==native[9],"released head stays native and full size");
        if(arms) require(state.hands==4 && buffer[3]!=native[3] && buffer[6]!=native[6],"arms keep the tracked pose through the head release");
        else require(state.hands==0 && buffer==native,"head-only configuration writes nothing once the head is released");
        require(driver.snapshot().head_hidden,"the configured head hiding itself is unchanged");
        // Back in first person the head hides again.
        buffer=native;driver.allow(true,5);driver.targets(targets);driver.apply(pose,now);
        require(driver.snapshot().head_updates==2 && !vr_camera::rigid(buffer[8]),"head hiding resumes after the viewer");
    }
}
// 2 Nov: on the way into camera mode the head turns to the gaze in the publication the freeze
// keeps, through the real writer; first person (head hiding on) and a plain publication never do.
void gaze_reaches_the_publication() {
    using vr_head_aim::FACE;
    const auto now=EngineArmDriver::Clock::now();
    // The Marine's axes: x up, y forward, z left (VrHeadAim.h), facing world +z.
    const vr_arm::Matrix upright{0,1,0,0, 0,0,1,0, 1,0,0,0, 0,0,0,1};
    const auto placed=[&](vr_arm::Vector p){auto m=upright;vr_arm::position(m,p);return m;};
    auto layout=std::make_shared<EngineArmProbe::RigLayout>();layout->names={"ROOT","BACKup","ARM1L","ARM2L","HANDL","NECK","HEAD","helmet","eye_L","eye_R"};layout->parents={-1,0,1,2,3,1,5,6,6,6};
    layout->bind_locals={matrix({0,0,0}),placed({0,1.9f,0}),matrix({.2f,.2f,0}),matrix({.42f,0,0}),matrix({.42f,0,0}),matrix({.3f,0,0}),matrix({.13f,0,0}),matrix({0,0,0}),matrix({.09f,.1f,.04f}),matrix({.09f,.1f,-.04f})};
    std::array<vr_arm::Matrix,10> buffer{};
    EngineArmProbe::Pose pose;pose.owner.actor=123;pose.owner.instance=456;pose.layout=layout;pose.world=vr_camera::IDENTITY;pose.world_valid=true;pose.received=now;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());
    for(size_t i=0;i<buffer.size();++i){buffer[i]=layout->bind_locals[i];pose.bones.push_back({static_cast<uint32_t>(i),static_cast<int16_t>(i),layout->bind_locals[i]});}
    const auto native=buffer;
    EngineArmDriver driver;bool locked{};
    driver.configure({[](const auto&,auto){return true;},[&](uintptr_t){locked=true;},[&](uintptr_t){locked=false;},
        [&](uintptr_t p,const auto& value){require(locked,"head aim writes under the native lock");std::memcpy(reinterpret_cast<void*>(p),value.data(),sizeof(value));return true;}});
    driver.enable(true);driver.hide_head(true);
    const vr_arm::Vector gaze{.5f,0,.8660254f}; // 30 degrees to the Marine's left, world +x when it faces +z.
    EngineArmDriver::Targets targets;targets.actor=123;targets.reference=2;targets.epoch=5;targets.captured=now;targets.wrists={matrix({.4f,1.8f,.5f}),std::nullopt};targets.gaze=gaze;
    // First person: the head is shrunk, not aimed.
    driver.allow(true,5);driver.targets(targets);driver.apply(pose,now);
    require(driver.snapshot().head_updates==1 && driver.snapshot().head_aims==0 && !vr_camera::rigid(buffer[6]),"first person shrinks the head and never aims it");
    // The viewer's release: the head full size, and on the gaze.
    buffer=native;driver.allow(true,5,false);driver.targets(targets);driver.apply(pose,now);
    {const auto state=driver.snapshot();
     require(state.head_aims==1 && state.hands==2 && state.head_updates==1 && state.head_aim_refused==0,"the release aims the head and keeps the arms");
     std::vector<vr_arm::Matrix> locals(buffer.begin(),buffer.end()),world;require(vr_arm::hierarchy(locals,layout->parents,pose.world,world),"the published head stays rigid");
     require(vr_upright::angle_degrees(vr_arm::rotate(FACE,world[6]),gaze)<.2f,"the published eyes look along the gaze");
     require(buffer[5]!=native[5] && buffer[6]!=native[6] && buffer[7]==native[7],"NECK and HEAD are written; the helmet rides on HEAD");
     require(state.head_aim_degrees>10,"the turn from the animation is reported");}
    // No gaze (photo mode's next publications, a play): nothing turns.
    buffer=native;targets.gaze.reset();driver.targets(targets);driver.apply(pose,now);
    require(driver.snapshot().head_aims==1 && buffer[5]==native[5] && buffer[6]==native[6],"no gaze leaves the head to the animation");
    // A refusal costs the head, never the arms.
    auto no_eyes=std::make_shared<EngineArmProbe::RigLayout>(*layout);no_eyes->names[8]="eye_lost";auto refused=pose;refused.layout=no_eyes;
    buffer=native;targets.gaze=gaze;driver.targets(targets);driver.apply(refused,now);
    {const auto state=driver.snapshot();require(state.head_aim_refused==1 && state.head_aims==1 && state.hands==4 && buffer[6]==native[6],"a rig the aim can't trust keeps its head and still gets its arms");}
}
// Bulwark (26 Oct): the storm shield's mount turns with the tracked left wrist's roll, written
// into the publication with the arm. A rig whose animation does not publish the mount still
// gets its left arm (the twist is dropped rather than the whole hand refused).
void shield_publication() {
    for(const bool published:{true,false}) {
        EngineArmDriver driver;const auto now=EngineArmDriver::Clock::now();
        auto layout=std::make_shared<EngineArmProbe::RigLayout>();
        layout->names={"ROOT","ARM1L","ARM2L","HANDL","shield_pos"};
        layout->parents={-1,0,1,2,2};
        layout->bind_locals={matrix({0,0,0}),matrix({-.2f,1.5f,0}),matrix({.42f,0,0}),matrix({.42f,0,0}),matrix({.1f,.05f,-.08f})};
        std::array<vr_arm::Matrix,5> buffer{};
        EngineArmProbe::Pose pose;pose.owner.actor=7;pose.owner.instance=8;pose.layout=layout;pose.world=vr_camera::IDENTITY;pose.world_valid=true;pose.received=now;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());
        const size_t bones=published?5:4;
        for(size_t i=0;i<bones;++i) {buffer[i]=layout->bind_locals[i];pose.bones.push_back({static_cast<uint32_t>(i),static_cast<int16_t>(i),layout->bind_locals[i]});}
        const auto before=buffer;bool locked{};
        driver.configure({[](const auto&,auto){return true;},[&](auto){locked=true;},[&](auto){locked=false;},[&](uintptr_t address,const auto& value){require(locked,"shield writes share the native lock");std::memcpy(reinterpret_cast<void*>(address),value.data(),sizeof(value));return true;}});
        driver.enable(true);driver.allow(true,3);
        EngineArmDriver::Targets targets;targets.actor=7;targets.reference=1;targets.epoch=3;targets.captured=now;
        // The wrist rolled well away from the animation's hand roll.
        auto wrist=vr_upright::rotation({.6f,0,.8f},1.2f);vr_arm::position(wrist,{-.4f,1.2f,-.4f});targets.wrists[0]=wrist;
        driver.targets(targets);driver.apply(pose,now);
        require(driver.snapshot().hands==1 && driver.snapshot().failures==0,"the left arm commits with or without a published shield mount");
        require(buffer[2]!=before[2] && buffer[3]!=before[3],"forearm and hand follow the tracked wrist");
        if(published) require(buffer[4]!=before[4],"a published shield mount turns with the wrist roll");
        else require(buffer[4]==before[4],"an unpublished mount is never written");
    }
}
int main(int argc,char** argv) {
    try {
        head_release_keeps_arms();gaze_reaches_the_publication();post_write_resolution();post_write_invalidation();weapon_publication();weapon_grip_survives_a_rebuilt_layout();upright_publication();recorded_embodiment_publication();shield_publication();body_placement_tests::run();
        EngineArmDriver driver;
        const auto now=EngineArmDriver::Clock::now();
        auto layout=std::make_shared<EngineArmProbe::RigLayout>();
        layout->names={"ROOT","ARM1L","ARM2L","HANDL","ARM1R","ARM2R","HANDR","ARM2R_glove","HANDR_glove","forearm_R","OTHER"};
        layout->parents={-1,0,1,2,0,4,5,4,7,5,0};
        layout->bind_locals={matrix({0,0,0}),matrix({-0.2f,1.5f,0}),matrix({0.42f,0,0}),matrix({0.42f,0,0}),matrix({0.2f,1.5f,0}),matrix({-0.42f,0,0}),matrix({-0.42f,0,0}),matrix({-0.42f,0,0}),matrix({-0.3f,0,0}),matrix({-0.42f,0,0}),matrix({0,0,2})};
        std::array<int16_t,11> map{4,8,1,6,3,11,12,0,10,9,14};
        std::array<vr_arm::Matrix,16> buffer{};
        EngineArmProbe::Pose pose; pose.owner.actor=123;pose.owner.instance=456;pose.layout=layout;pose.world=vr_camera::IDENTITY;pose.world_valid=true;pose.received=now;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());
        for (size_t i=0;i<map.size();++i) { buffer[map[i]]=layout->bind_locals[i];pose.bones.push_back({static_cast<uint32_t>(i),map[i],layout->bind_locals[i]}); }
        const auto baseline=buffer;
        bool current=true,locked{},fail_write{},interfere{}; uint64_t writes{},enters{},leaves{};
        driver.configure({[&](const auto& p,auto){return current && p.owner.actor==123;},
            [&](uintptr_t instance){require(instance==456 && !locked,"only owned instance publication lock enters");locked=true;++enters;if (interfere) {buffer[map[5]][12]+=0.1f;}},
            [&](uintptr_t instance){require(instance==456 && locked,"lock exits once");locked=false;++leaves;},
            [&](uintptr_t p,const auto& value){require(locked,"bone writes must run under native lock");++writes;if (fail_write && writes==2) {fail_write=false;return false;}std::memcpy(reinterpret_cast<void*>(p),value.data(),sizeof(value));return true;}});
        EngineArmDriver::Targets targets;targets.actor=123;targets.reference=2;targets.epoch=5;targets.captured=now;targets.wrists={matrix({-0.4f,1.2f,-0.4f}),matrix({0.4f,1.2f,-0.4f})};
        driver.allow(true,5);driver.targets(targets);driver.apply(pose,now);require(buffer==baseline && writes==0,"off driver never writes");
        const auto trace_path=std::filesystem::temp_directory_path()/"sm2vr_arm_trace_test.jsonl";
        require(vr_trace::Recorder::get().open(trace_path,10,now),"trace opens");
        driver.enable(true);driver.targets(targets);driver.apply(pose,now);
        require(writes>0 && enters==leaves && !locked && driver.snapshot().hands==2,"both arms apply through actual native adapter and release lock");
        {
            vr_trace::Recorder::get().tick(now);vr_trace::Recorder::get().close();
            std::string all,line;{std::ifstream in{trace_path};while(std::getline(in,line)) all+=line;}
            std::filesystem::remove(trace_path);
            // Native HANDL: ARM1L (-0.2,1.5,0) + 0.42 + 0.42 along x.
            for(const char* part:{"\"actor\":\"7b\"","\"driven\":true","\"left_native\":[0.640,1.500,0.000]","\"left_target\":[-0.400,1.200,-0.400]",
                "\"left_solved\":[-0.400,1.200,-0.400]","\"left_residual_metres\":0.0000","\"head\":null"})
                require(all.find(part)!=std::string::npos,part);
        }
        auto final_locals=layout->bind_locals;for(size_t i=0;i<map.size();++i){final_locals[i]=buffer[map[i]];}
        std::vector<vr_arm::Matrix> final_world;
        require(vr_arm::hierarchy(final_locals,layout->parents,pose.world,final_world),"native published final hierarchy valid");
        require(vr_arm::length(vr_arm::sub(vr_arm::position(final_world[3]),vr_arm::position(*targets.wrists[0])))<0.0001f && vr_arm::length(vr_arm::sub(vr_arm::position(final_world[6]),vr_arm::position(*targets.wrists[1])))<0.0001f,"actual written wrist positions track targets");
        require(buffer[map[0]]==baseline[map[0]] && buffer[map[9]]==baseline[map[9]] && buffer[map[10]]==baseline[map[10]],"root, helper locals and unrelated body bone never overwritten");
        buffer=baseline;current=false;driver.apply(pose,now);require(buffer==baseline,"lost/NPC ownership excludes all writes");current=true;
        driver.apply(pose,now+std::chrono::milliseconds(101));require(buffer==baseline,"stale native/controller data releases");driver.apply(pose,now-std::chrono::milliseconds(1));require(buffer==baseline,"future samples release");
        driver.allow(false,5);driver.apply(pose,now);require(buffer==baseline,"focus loss clears targets");driver.allow(true,6);driver.targets(targets);driver.apply(pose,now);require(buffer==baseline,"reference/snap epoch changes reject old targets");
        driver.allow(true,5);targets.wrists[0].reset();driver.targets(targets);driver.apply(pose,now);
        require(buffer[map[1]]==baseline[map[1]] && buffer[map[2]]==baseline[map[2]] && buffer[map[3]]==baseline[map[3]] && buffer[map[5]]!=baseline[map[5]],"lost left hand restores native left while valid right keeps tracking");
        buffer=baseline;interfere=true;driver.apply(pose,now);interfere=false;
        require(buffer[map[5]][12]!=baseline[map[5]][12] && buffer[map[6]]==baseline[map[6]],"selected buffer changes under the lock exclude the entire patch batch");
        // Interfere with a selected right-arm patch to exercise the before-write guard.
        buffer=baseline;buffer[map[5]][12]+=0.1f;driver.apply(pose,now);require(buffer[map[6]]==baseline[map[6]],"selected native buffer disagreement rejects before any write");
        buffer=baseline;writes=0;fail_write=true;driver.apply(pose,now);
        require(buffer==baseline && driver.snapshot().failures==1 && !locked,"partial writer failure rolls back under the same lock");
        driver.targets(targets);driver.apply(pose,now);const auto first_elbow=buffer[map[5]];
        buffer=baseline;auto pole_settings=driver.settings();pole_settings.elbow_poles[1]={0,0,1};
        require(driver.set_settings(pole_settings),"live elbow pole accepted");driver.targets(targets);driver.apply(pose,now);
        require(buffer[map[5]]!=first_elbow,"elbow pole changes native written pose through production driver");
        require(driver.set_settings({}),"reset arm calibration");buffer=baseline;
        const auto demo=EngineArmDriver::diagnostic_targets(pose);
        require(demo.wrists[0] && demo.wrists[1] && demo.captured==pose.received,"diagnostic targets use actual owned native wrists and sample time");
        driver.allow(true,1);driver.targets(demo);driver.apply(pose,now);
        require(buffer!=baseline && driver.snapshot().failures==1,"flatscreen diagnostic exercises same guarded native writer");
        buffer=baseline;
        auto settings=driver.settings();settings.grips[1].metres={0.1f,0,0};settings.grips[1].degrees={0,90,0};require(driver.set_settings(settings),"live valid grip settings accepted");
        auto anchor=vr_camera::IDENTITY;anchor[0]=-1;auto wrist=vr_arm::wrist_world(anchor,vr_camera::IDENTITY,driver.grip(1),1);
        require(wrist && std::abs((*wrist)[12]+0.1f)<0.0001f && std::abs((*wrist)[0])<0.0001f,"real grip settings affect mapped wrist position and rotation");
        auto invalid=settings;invalid.grips[0].metres[0]=1;require(!driver.set_settings(invalid) && driver.settings()==settings,"invalid settings preserve active calibration");
        std::istringstream preset{"left_grip_metres=0 0 0\nleft_grip_degrees=0 0 0\nleft_elbow_pole=0.4 -0.8 0\nright_grip_metres=0.1 0 0\nright_grip_degrees=0 90 0\nright_elbow_pole=-0.4 -0.8 0\n"};
        auto loaded=EngineArmDriver::Settings::read(preset);require(loaded && *loaded==settings,"complete arm preset preserves values");
        std::istringstream incomplete{"left_grip_metres=0 0 0\n"};require(!EngineArmDriver::Settings::read(incomplete),"partial preset rejected");
        std::ostringstream saved;preset.clear();preset.seekg(0);saved<<preset.rdbuf();
        for (const auto& suffix:{"right_grip_metres=0 0 0\n","unknown=0 0 0\n"}) {
            std::istringstream malformed{saved.str()+suffix};require(!EngineArmDriver::Settings::read(malformed),"duplicate and unknown preset keys rejected");
        }
        driver.enable(false);driver.apply(pose,now);require(buffer==baseline,"off mode restores native pose on next publication");
        require(argc==3,"native hand preset paths required");
        const auto candidate=EngineArmDriver::Settings::load(argv[1]);require(candidate && driver.set_settings(*candidate),"versioned hand preset parses through production settings");
        const auto check_axis=[](auto actual,auto expected){return vr_arm::length(vr_arm::sub(actual,expected))<0.0001f;};
        for(size_t hand=0;hand<2;++hand) {
            const float side=hand==0?1.0f:-1.0f;
            const auto offset=driver.grip(hand);
            require(check_axis(vr_arm::rotate({side,0,0},offset),vr_arm::Vector{0,0,-1}),"mirrored native finger axes align to controller forward");
            require(check_axis(vr_arm::rotate({0,-side,0},offset),vr_arm::Vector{0,1,0}),"native thumb axes align upward without reflection");
            const auto corrected=vr_arm::wrist_world(anchor,vr_camera::IDENTITY,offset,0.9f);
            const auto raw=vr_arm::wrist_world(anchor,vr_camera::IDENTITY,vr_camera::IDENTITY,0.9f);
            require(corrected && raw && vr_camera::rigid(*corrected) && check_axis(vr_arm::position(*corrected),vr_arm::position(*raw)),"orientation preset preserves native handedness and wrist position");
        }
        const auto outward=EngineArmDriver::Settings::load(argv[2]);require(outward.has_value(),"45-degree comparison preset parses");
        for(size_t hand=0;hand<2;++hand) {
            require(driver.set_settings(*candidate),"load axis baseline");const auto before=driver.grip(hand);
            require(driver.set_settings(*outward),"load outward comparison");const auto after=driver.grip(hand);
            const auto relative=vr_camera::multiply(after,vr_camera::inverse(before));
            require(vr_camera::rigid(after) && std::abs(relative[0]-std::sqrt(0.5f))<0.0001f && std::abs(relative[1]-std::sqrt(0.5f))<0.0001f && std::abs(relative[4]+std::sqrt(0.5f))<0.0001f,"live preset adds exactly 45 degrees about native palm normal");
            require(check_axis(vr_arm::rotate({0,0,1},before),vr_arm::rotate({0,0,1},after)) && check_axis(vr_arm::position(before),vr_arm::position(after)),"45-degree comparison preserves palm normal and wrist position");
        }
        // Exercise fingers through the same publication lock and actual mapped write adapter.
        auto finger_layout=std::make_shared<EngineArmProbe::RigLayout>(*layout);
        for(size_t hand=0;hand<2;++hand)for(size_t branch=0;branch<(hand?2u:1u);++branch)for(size_t digit=1;digit<=5;++digit) {
            int32_t parent=branch?8:hand?6:3;
            for(size_t segment=1;segment<=3;++segment) {
                finger_layout->names.push_back(std::string{hand?"R":"L"}+std::to_string(digit)+"_fing"+std::to_string(segment)+(branch?"_glove":""));
                finger_layout->parents.push_back(parent);parent=static_cast<int32_t>(finger_layout->names.size()-1);
                finger_layout->bind_locals.push_back(matrix({0.05f,0,0}));
            }
        }
        auto finger_pose=pose;finger_pose.layout=finger_layout;finger_pose.bones.clear();
        std::vector<vr_arm::Matrix> finger_buffer=finger_layout->bind_locals;
        for(size_t i=11;i<finger_buffer.size();++i) {
            finger_buffer[i][5]=finger_buffer[i][10]=std::cos(1.2f);finger_buffer[i][6]=std::sin(1.2f);finger_buffer[i][9]=-std::sin(1.2f);
        }
        for(size_t i=0;i<finger_buffer.size();++i)finger_pose.bones.push_back({static_cast<uint32_t>(i),static_cast<int16_t>(i),finger_buffer[i]});
        finger_pose.buffer=reinterpret_cast<uintptr_t>(finger_buffer.data());const auto native_fingers=finger_buffer;
        driver.enable(true);driver.allow(true,5);targets.wrists[0]=matrix({-0.4f,1.2f,-0.4f});
        targets.fingers={vr_finger::Curl{1,0,1,1,1},vr_finger::Curl{0,0,0,0,0}};
        driver.targets(targets);driver.apply(finger_pose,now);
        require(driver.snapshot().finger_hands==2 && finger_buffer[14]==finger_layout->bind_locals[14] && finger_buffer[11]==native_fingers[11] && finger_buffer.back()==finger_layout->bind_locals.back(),"real native writer independently points left index and opens right gauntlet fingers");
        finger_buffer=native_fingers;driver.enable_fingers(false);driver.apply(finger_pose,now);
        require(std::equal(finger_buffer.begin()+11,finger_buffer.end(),native_fingers.begin()+11),"finger off mode retains native animation while arm IK remains active");
        driver.enable_fingers(true);finger_buffer=native_fingers;targets.fingers[0].reset();driver.targets(targets);driver.apply(finger_pose,now);
        require(finger_buffer[14]==native_fingers[14] && finger_buffer.back()!=native_fingers.back(),"missing left skeletal action releases only left fingers");
        finger_buffer=native_fingers;targets.wrists[1].reset();driver.targets(targets);driver.apply(finger_pose,now);
        require(std::equal(finger_buffer.begin()+26,finger_buffer.end(),native_fingers.begin()+26),"lost right wrist excludes finger writes even with cached curl data");
        finger_buffer=native_fingers;targets.wrists[1]=matrix({0.4f,1.2f,-0.4f});(*targets.fingers[1])[0]=NAN;driver.targets(targets);driver.apply(finger_pose,now);
        require(driver.snapshot().finger_rejected>0 && finger_buffer.back()==native_fingers.back(),"malformed sensor curl excludes finger batch without blocking arm IK");
        std::cout<<"owned native arm/finger writes, locking, target lifecycle, isolation, rollback and settings passed\n";return 0;
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
