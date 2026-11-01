#include "synthetic_code.h"
#include "NativePipLens.h"
#include "game_build_ported.h"
#include "NativePipExperiment.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <limits>

namespace {
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
bool close(float a,float b) {return std::abs(a-b)<1e-5f;}
template<class T> T load(const uint8_t* data,size_t offset) {T result;std::memcpy(&result,data+offset,sizeof(T));return result;}
template<class T> void put(uint8_t* data,size_t offset,const T& value) {std::memcpy(data+offset,&value,sizeof(T));}
std::array<float,24> planes_for(const vr_camera::Matrix& world,const std::array<float,4>& rectangle,float near_z,float far_z) {
    const float left=rectangle[0]/near_z,right=(rectangle[0]+rectangle[2])/near_z,bottom=rectangle[1]/near_z,top=(rectangle[1]+rectangle[3])/near_z;
    const std::array<std::array<float,3>,6> local{{{0,0,1},{1,0,-left},{-1,0,right},{0,1,-bottom},{0,-1,top},{0,0,-1}}};
    std::array<float,24> result{};
    for(size_t p=0;p<6;++p) {
        float norm{};
        for(size_t axis=0;axis<3;++axis) {result[p*4+axis]=world[axis]*local[p][0]+world[4+axis]*local[p][1]+world[8+axis]*local[p][2];norm+=result[p*4+axis]*result[p*4+axis];}
        norm=std::sqrt(norm);for(size_t axis=0;axis<3;++axis) {result[p*4+axis]/=norm;result[p*4+3]+=result[p*4+axis]*world[12+axis];}
        if(p==0) result[p*4+3]+=near_z;
        if(p==5) result[p*4+3]-=far_z;
    }
    return result;
}
uintptr_t active_list{};uint8_t* storage{};std::optional<NativePipLens::Target> target;
unsigned calls{},cache_calls{},frustum_calls{};uint32_t seen_flags{};void* seen_renderer{};int32_t seen_slot{};
bool throw_provider{},throw_cache{},bad_planes{},bad_inverse{},bad_compact{};vr_camera::Matrix seen_projection{};
std::function<std::optional<NativePipLens::Target>()> target_override;
__declspec(noinline) void renderer(void* self,uint32_t flags,int32_t slot) {
    ++calls;seen_renderer=self;seen_flags=flags;seen_slot=slot;
    if(slot>=0 && slot<2) seen_projection=load<vr_camera::Matrix>(storage+0x168+size_t(slot)*0xa60,0x45c);
    volatile unsigned keep=calls;(void)keep;
}
void cache(void* pointer,const float* view,const float* projection,const float* extra) {
    ++cache_calls;require(!extra,"PIP optional clipping plane must be absent in this guarded path");auto* entry=static_cast<uint8_t*>(pointer);
    std::memcpy(entry+0x3dc,view,64);std::memcpy(entry+0x45c,projection,64);
    vr_camera::Matrix combined{};
    for(size_t r=0;r<4;++r) for(size_t c=0;c<4;++c) for(size_t k=0;k<4;++k) combined[r*4+c]+=view[r*4+k]*projection[k*4+c];
    put(entry,0x55c,combined);
    vr_camera::Matrix inverse{};inverse[0]=1/projection[0];inverse[5]=1/projection[5];inverse[11]=1/projection[14];
    inverse[12]=-projection[8]/projection[0];inverse[13]=-projection[9]/projection[5];inverse[14]=1;inverse[15]=-projection[10]/projection[14];
    if(bad_inverse) inverse[0]=99;
    put(entry,0x4dc,inverse);
    if(throw_cache) throw std::runtime_error("partial native cache failure");
}
void frustum(void* pointer) {
    ++frustum_calls;auto* camera=static_cast<uint8_t*>(pointer);
    const auto world=load<vr_camera::Matrix>(camera,0);const auto rect=load<std::array<float,4>>(camera,0x12c);
    const auto corners=load<std::array<float,8>>(camera,0x8c);
    const std::array<float,8> expected{rect[0],rect[1],rect[0]+rect[2],rect[1],rect[0]+rect[2],rect[1]+rect[3],rect[0],rect[1]+rect[3]};
    for(size_t i=0;i<8;++i) require(close(corners[i],expected[i]),"native frustum receives all four updated corners, not only a projection matrix");
    const auto near_z=load<float>(camera,0x80),far_z=load<float>(camera,0x84);
    const auto planes=planes_for(world,bad_planes?std::array<float,4>{-rect[2]*.5f,-rect[3]*.5f,rect[2],rect[3]}:rect,near_z,far_z);
    put(camera,0x15c,uint32_t(6));for(size_t p=0;p<6;++p) std::memcpy(camera+0x160+p*0x14,planes.data()+p*4,16);
    const float radius=std::sqrt((rect[2]*rect[2]+rect[3]*rect[3])*.25f+near_z*near_z);put(camera,0x2f8,radius);
    std::array<float,12> directions{},sides{};std::array<float,3> normal{-world[8],-world[9],-world[10]},origin{world[12],world[13],world[14]};
    for(size_t corner=0;corner<4;++corner) {
        float norm{};
        for(size_t axis=0;axis<3;++axis) {auto& value=directions[corner*3+axis];value=world[axis]*corners[corner*2]+world[4+axis]*corners[corner*2+1]+world[8+axis]*near_z;norm+=value*value;}
        norm=std::sqrt(norm);for(size_t axis=0;axis<3;++axis) directions[corner*3+axis]/=norm;
    }
    for(size_t corner=0;corner<4;++corner) for(size_t axis=0;axis<3;++axis) {
        const size_t a=(axis+1)%3,b=(axis+2)%3,next=(corner+1)%4;
        sides[corner*3+axis]=directions[corner*3+a]*directions[next*3+b]-directions[corner*3+b]*directions[next*3+a];
    }
    const auto cap=(normal[0]*directions[0]+normal[1]*directions[1]+normal[2]*directions[2])*radius;put(camera,0x30c,cap);
    if(bad_compact) sides[0]=99;
    put(camera,0x310,directions);put(camera,0x340,sides);put(camera,0x370,normal);put(camera,0x37c,origin);
}
void init_entry(uint8_t* entry,bool pip,const vr_camera::Matrix& world) {
    put(entry,0,uint64_t(pip?0xe50043:0x3f7c013));put(entry,0xa50,int32_t(pip?0:-1));
    put(entry,0x34,world);const auto view=vr_camera::inverse(world);put(entry,0x74,view);
    const float near_z=.1f,far_z=100;put(entry,0xb4,near_z);put(entry,0xb8,far_z);put(entry,0xbc,uint32_t(4));
    const std::array<float,4> rect{-.1f,-.1f,.2f,.2f};put(entry,0x160,rect);
    const std::array<float,8> corners{-.1f,-.1f,.1f,-.1f,.1f,.1f,-.1f,.1f};put(entry,0xc0,corners);
    pip_lens::Geometry geometry;require(pip_lens::build(rect,world,near_z,far_z,0,0,geometry),"initial lens");
    cache(entry,view.data(),geometry.projection.data(),nullptr);frustum(entry+0x34);
}
void invoke(int32_t slot) {void(*volatile fn)(void*,uint32_t,int32_t)=&renderer;fn(reinterpret_cast<void*>(0x5678),0x78,slot);++seen_flags;}
}
int main() {
    try {
        const auto cache_bytes=synthetic_at<15>(0x1d75d30);const auto frustum_bytes=synthetic_at<14>(0x2224490);
        using Lens=NativePipLens;
        require(Lens::matches(Lens::TIMESTAMP,SYNTH(Lens::RENDER_BYTES),cache_bytes,frustum_bytes),"known renderer and both native helper prologues accepted");
        auto wrong_render=SYNTH(Lens::RENDER_BYTES);wrong_render[0]^=1;auto wrong_cache=cache_bytes;wrong_cache[0]^=1;auto wrong_frustum=frustum_bytes;wrong_frustum[0]^=1;
        require(!Lens::matches(0,SYNTH(Lens::RENDER_BYTES),cache_bytes,frustum_bytes) && !Lens::matches(Lens::TIMESTAMP,wrong_render,cache_bytes,frustum_bytes) &&
            !Lens::matches(Lens::TIMESTAMP,SYNTH(Lens::RENDER_BYTES),wrong_cache,frustum_bytes) && !Lens::matches(Lens::TIMESTAMP,SYNTH(Lens::RENDER_BYTES),cache_bytes,wrong_frustum),"unknown build or either changed native helper cannot be called");
        require(Lens::matches(game_build_ported::EXE,game_build_ported::code(Lens::RENDER_RVA),game_build_ported::code(Lens::CACHE_RVA),game_build_ported::code(Lens::FRUSTUM_RVA)),"the 2026-10-21 renderer and helpers match");
        for(float angle:{0.f,.4f,1.5f}) for(float horizontal:{-.25f,0.f,.15f}) for(float vertical:{-.1f,0.f,.25f}) {
            auto world=vr_camera::IDENTITY;world[0]=std::cos(angle);world[2]=-std::sin(angle);world[8]=-std::sin(angle);world[10]=-std::cos(angle);world[12]=-156;world[13]=67;world[14]=-371;
            pip_lens::Geometry geometry;require(pip_lens::build({-.075f,-.042f,.15f,.084f},world,.1f,250000,horizontal,vertical,geometry),"asymmetric geometry for rotated native camera");
            require(pip_lens::culling_matches(geometry,world,.1f,250000,planes_for(world,geometry.rectangle,.1f,250000)),"projection edge rays agree with independently built six-plane culling through yaw and native far plane");
            auto scaled=planes_for(world,geometry.rectangle,.1f,250000);for(size_t p=0;p<6;++p) for(size_t c=0;c<4;++c) scaled[p*4+c]*=p%2?.3f:2.f;
            require(pip_lens::culling_matches(geometry,world,.1f,250000,scaled),"native non-unit side planes retain equivalent half spaces and scale-independent tolerances");
            const auto& t=geometry.tangents;const auto& p=geometry.projection;
            require(close(t.left*p[0]+p[8],-1) && close(t.right*p[0]+p[8],1) && close(t.bottom*p[5]+p[9],-1) && close(t.top*p[5]+p[9],1),"four asymmetric rays map to correct clip edges");
            for(size_t corner=0;corner<4;++corner) require(geometry.radius>=std::sqrt(geometry.corners[2*corner]*geometry.corners[2*corner]+geometry.corners[2*corner+1]*geometry.corners[2*corner+1]+.01f)-1e-7f,"conservative near bound encloses all four off-centre corners");
        }
        auto world=vr_camera::IDENTITY;world[10]=-1;
        pip_lens::Geometry unchanged;unchanged.radius=123;
        require(!pip_lens::build({-.1f,-.1f,.2f,.2f},world,.1f,100,.3f,0,unchanged) && unchanged.radius==123,"invalid lens cannot modify output");
        require(!pip_lens::build({-.1f,-.1f,.2f,.2f},world,0,100,0,0,unchanged),"invalid near plane rejected");
        const auto nan=std::numeric_limits<float>::quiet_NaN();require(!pip_lens::build({nan,0,1,1},world,.1f,100,0,0,unchanged),"non-finite rectangle rejected");
        storage=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x2996c,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));require(storage,"allocate independent native list fixture");active_list=reinterpret_cast<uintptr_t>(storage);
        put(storage,0x29968,int32_t(2));auto* main=storage+0x168;auto* pip=main+0xa60;
        init_entry(main,false,world);init_entry(pip,true,world);cache_calls=frustum_calls=0;
        std::array<uint8_t,0xa60> main_before{},pip_before{};std::memcpy(main_before.data(),main,main_before.size());std::memcpy(pip_before.data(),pip,pip_before.size());
        target=NativePipLens::Target{0,world,.15f,.1f,0,120};auto& hook=NativePipLens::get();
        require(hook.install(reinterpret_cast<void*>(&renderer),&cache,&frustum,[]{return active_list;},[](){if(throw_provider) throw std::runtime_error("provider");return target_override?target_override():target;}),"install actual native renderer detour fixture");
        invoke(0);require(calls==1 && cache_calls==0 && std::memcmp(main,main_before.data(),main_before.size())==0,"main view always passes through unchanged");
        invoke(1);auto sample=hook.snapshot();require(calls==2 && seen_renderer==reinterpret_cast<void*>(0x5678) && seen_flags==0x79 && seen_slot==1,"renderer arguments preserved and original called once");
        require(sample.applied==1 && sample.reason==0 && close(seen_projection[8],-.3f) && close(seen_projection[9],-.2f),"owned final native cache has horizontal and vertical asymmetry");
        require(cache_calls==1 && frustum_calls==1 && load<float>(pip,0x32c)>.2f,"native cache/frustum rebuild plus expanded conservative near radius");
        require(close(load<float>(pip,0x340),-.1f),"compact near-cap distance remains at the real near plane under asymmetric corners");
        require(std::memcmp(main,main_before.data(),main_before.size())==0 && target->world==world,"persistent target and main camera untouched");
        invoke(1);require(close(seen_projection[8],-.3f) && close(seen_projection[9],-.2f),"repeated render entry cannot accumulate lens shifts");
        target->world[12]=1;std::array<uint8_t,0xa60> shifted{};std::memcpy(shifted.data(),pip,shifted.size());invoke(1);
        require(hook.snapshot().reason==2 && std::memcmp(pip,shifted.data(),shifted.size())==0,"foreign pose cannot mutate a matching slot number");target->world=world;
        target->world[12]=1;target->recent_worlds={world};invoke(1);
        require(hook.snapshot().reason==0 && hook.snapshot().historical_matches==1 && close(seen_projection[8],-.3f),"slightly older owned render-job pose remains eligible during camera motion");target->world=world;target->recent_worlds.clear();
        std::memcpy(pip,pip_before.data(),pip_before.size());throw_cache=true;invoke(1);throw_cache=false;
        require(hook.snapshot().reason==10 && std::memcmp(pip,pip_before.data(),pip_before.size())==0,"partial cache failure rolls back every owned entry byte before original renderer");
        bad_planes=true;invoke(1);bad_planes=false;
        require(hook.snapshot().reason==9 && std::memcmp(pip,pip_before.data(),pip_before.size())==0,"symmetric or wrong culling planes reject asymmetric rendering and restore all caches");
        bad_inverse=true;invoke(1);bad_inverse=false;
        require(hook.snapshot().reason==8 && std::memcmp(pip,pip_before.data(),pip_before.size())==0,"stale inverse projection cannot pass final-lens validation");
        bad_compact=true;invoke(1);bad_compact=false;
        require(hook.snapshot().reason==11 && std::memcmp(pip,pip_before.data(),pip_before.size())==0,"inconsistent compact culling data cannot survive validation");
        put(storage,0x29968,int32_t(3));auto* other=pip+0xa60;init_entry(other,true,world);invoke(1);
        require(hook.snapshot().reason==6 && std::memcmp(pip,pip_before.data(),pip_before.size())==0,"duplicate owned PIP record is ambiguous and cannot be modified");put(storage,0x29968,int32_t(2));
        DWORD protection{};require(VirtualProtect(storage,0x2996c,PAGE_READONLY,&protection),"protect native fixture");invoke(1);
        require(hook.snapshot().reason==3,"read-only native memory rejected before writing");VirtualProtect(storage,0x2996c,PAGE_READWRITE,&protection);
        const auto rejected=hook.snapshot();
        require(rejected.rejected==7 && rejected.rejection_events.size()==7 && rejected.reason_counts[2]==1 && rejected.reason_counts[10]==1,
            "each failed reason is retained independently of the latest sampled reason");
        invoke(1);sample=hook.snapshot();
        require(sample.reason==0 && sample.rejected==rejected.rejected && sample.rejection_events.back().reason==3,
            "later success cannot erase transient rejection evidence");
        const auto& mismatch=sample.rejection_events.front();
        require(mismatch.reason==2 && mismatch.updates==120 && mismatch.candidates==1 && close(mismatch.current_pose_error,1) &&
            close(mismatch.nearest_pose_error,1) && mismatch.world==world && mismatch.target_world[12]==1 && mismatch.thread!=0,
            "pose refusal retains exact input matrices, candidate count and nearest distance");
        target->world[12]=1;
        for(unsigned i=0;i<70;++i) invoke(1);
        sample=hook.snapshot();target->world=world;
        require(sample.rejected==77 && sample.reason_counts[2]==71 && sample.rejection_events.size()==64 &&
            sample.rejection_events.front().serial==14 && sample.rejection_events.back().serial==77,
            "bounded event retention reports serial gaps while keeping exact cumulative reason counts");
        target->current_eligible=false;
        target->history={{world,99,120,uint32_t(101),251,false}};
        std::memcpy(pip,pip_before.data(),pip_before.size());invoke(1);sample=hook.snapshot();
        require(sample.reason==12 && sample.reason_counts[12]==1 && sample.rejection_events.back().history.size()==1 &&
            sample.rejection_events.back().history[0].render_id==99 && sample.rejection_events.back().history[0].age_ms==251 &&
            !sample.rejection_events.back().current_eligible && std::memcmp(pip,pip_before.data(),pip_before.size())==0,
            "even the exact current pose cannot bypass expired eligibility and its job/age evidence survives refusal");
        target->recent_worlds={world};target->history[0].age_ms=249;target->history[0].eligible=true;
        invoke(1);require(hook.snapshot().reason==0,"eligible historical pose can still drive the guarded lens");
        target->current_eligible=true;target->recent_worlds.clear();target->history.clear();
        put(pip,0xa50,int32_t(4));invoke(1);put(pip,0xa50,int32_t(0));
        require(hook.snapshot().skip_counts[3]==1,"foreign PIP record is logged as a skip without claiming active ownership");
        const auto calls_before=calls;throw_provider=true;invoke(1);throw_provider=false;target.reset();invoke(1);invoke(99);
        require(calls==calls_before+3,"disabled, invalid-slot and throwing diagnostic paths preserve original calls");
        sample=hook.snapshot();
        require(sample.skipped==3 && sample.skip_counts[1]==1 && sample.skip_counts[2]==1 && !sample.unexpected_errors &&
            sample.skip_events.back().record==0 && sample.skip_events.back().world==world,
            "missing target and provider exception retain exact PIP candidate evidence without swallowing their original render calls");
        for(unsigned i=0;i<70;++i) invoke(1);
        target=NativePipLens::Target{0,world,.15f,.1f,0,120};invoke(1);sample=hook.snapshot();
        require(sample.reason==0 && sample.skipped==73 && sample.skip_events.size()==64 && sample.skip_events.front().serial==10 &&
            sample.skip_events.back().serial==73 && sample.skip_counts[1]==71,"successful publication preserves bounded skip evidence and cumulative counts");
        {
            using E=NativePipExperiment;E experiment;
            std::array<uint8_t,0x48> records{};std::array<uint8_t,0x40> manager{};std::array<uint8_t,0x388> source{};
            std::memcpy(source.data(),pip_before.data()+0x34,source.size());const float fov=73.7f;put(source.data(),0x2d0,fov);
            E::Array array{reinterpret_cast<uintptr_t>(records.data()),0,1};uintptr_t camera{};
            require(experiment.bind({[&]{return reinterpret_cast<uintptr_t>(&array);},[&]{return reinterpret_cast<uintptr_t>(manager.data());},
                [&](uintptr_t,int32_t* index,const E::Size*,const E::Request* request,bool) {
                    *index=0;array.count=1;camera=request->camera;put(records.data(),8,camera);
                },[&](uintptr_t,int32_t,bool,bool) {array.count=0;put(records.data(),8,uintptr_t{});}}),"bind release-race native ownership fixture");
            E::Config config;config.size={64,64};config.frames=4;config.warmup_frames=0;config.shift=0;config.pose_tags=true;
            require(experiment.arm(config),"arm four-update retirement control");
            target_override=[&]() -> std::optional<Lens::Target> {
                const auto owned=experiment.inspect_owned_camera();if(!owned) return std::nullopt;
                return Lens::Target{owned->record,owned->world,.15f,.1f,0,owned->updates,owned->recent_worlds,
                    owned->history,owned->current_eligible,owned->lock_wait_ms};
            };
            const auto before=hook.snapshot();vr_camera::Matrix preceding{};
            for(uint64_t id=1;id<=3;++id) {
                experiment.on_main(source.data(),id,true,uint32_t(id));
                if(id>1) {init_entry(pip,true,preceding);invoke(1);}
                std::memcpy(preceding.data(),reinterpret_cast<void*>(camera),64);
            }
            experiment.on_main(source.data(),4,true,uint32_t(4));
            experiment.on_main(source.data(),5,true,uint32_t(5)); // Release precedes the queued older render entry.
            init_entry(pip,true,preceding);const auto delayed_before=load<vr_camera::Matrix>(pip,0x45c);invoke(1);
            const auto after=hook.snapshot();
            require(experiment.snapshot().finished && after.applied-before.applied==2 && after.skipped-before.skipped==1 &&
                after.skip_events.back().reason==1 && after.skip_events.back().world==preceding &&
                load<vr_camera::Matrix>(pip,0x45c)==delayed_before,
                "an early native release can omit a pending lens attempt; the real hook preserves its pose as an unowned skip and does not mutate it");
            target_override={};
        }
        {
            init_entry(pip,true,world);target=Lens::Target{0,world,.15f,.1f,0,120};
            auto& entries=NativeCameraEntries::get();entries.enable();const FrameSyncClock::Frame frame{102,2,0,0,false};
            auto ticket=entries.begin(active_list+0x160,NativeCameraEntries::Role::main,frame);entries.finish(*ticket,0,world);
            ticket=entries.begin(active_list+0x160,NativeCameraEntries::Role::pip,frame);entries.finish(*ticket,1,world);
            const std::array<NativeCameraEntries::Entry,2> actual{{{reinterpret_cast<uintptr_t>(main),0,NativeCameraEntries::Role::main,world},
                {reinterpret_cast<uintptr_t>(pip),1,NativeCameraEntries::Role::pip,world}}};
            target->camera_entries=entries.pair(active_list,actual,*entries.revision(active_list));require(target->camera_entries.has_value(),"causal lens fixture setup");
            invoke(1);require(hook.snapshot().reason==0,"current physical receipt permits the same guarded lens geometry");
            init_entry(pip,true,world);const auto before=load<vr_camera::Matrix>(pip,0x45c);
            target->camera_entries->pip_slot=0;invoke(1);
            require(hook.snapshot().reason==6 && load<vr_camera::Matrix>(pip,0x45c)==before,"wrong causal slot refuses before any native cache mutation");
            target->camera_entries->pip_slot=1;entries.reset(active_list+0x160);invoke(1);
            require(hook.snapshot().reason==6 && load<vr_camera::Matrix>(pip,0x45c)==before,"reset causal revision refuses before native cache mutation");
        }
        std::cout<<"native PIP final lens tests passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
