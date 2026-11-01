#include "synthetic_code.h"
#include "NativeCreationFixture.h"
#include "NativeOrdinaryBloom.h"
#include "NativeOrdinaryGrading.h"
#include "NativeOrdinaryLateEffect.h"
#include "NativeOrdinarySharpen.h"
#include "NativeOrdinaryVolumeFrame.h"
#include "NativeOrdinaryShadingControl.h"
#include "NativeOrdinaryLightVisibility.h"
#include "NativeOrdinaryReflectionSelection.h"
#include "NativeOrdinaryShadowFit.h"
#include "NativeOrdinaryShadowCache.h"
#include "NativeOrdinaryShadowLights.h"
#include "NativeOcclusionHarvestOnce.h"
#include "NativeOrdinaryLightSection.h"
#include "NativeSecondCameraPlainLightTest.h"
#include "NativeSceneWaitTiming.h"
#include "NativeDepthReadyTiming.h"
#include "NativeFlareProbe.h"
#include "NativeAdaptationMirror.h"
#include "game_build_ported.h"
#include <string_view>
#include <tuple>
#include <vector>
using Bloom=NativeOrdinaryBloom;
struct World {
    Host host;Fixture creation{host};Creation* owner=new Creation;
    uintptr_t base=reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,0x5090000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    std::array<uint8_t,0x480> even{};std::array<uint8_t,0x7f0> bank{};std::array<uint8_t,0xee8> records{};
    std::array<uintptr_t,3> factory_vtable{},even_node{};uintptr_t factory{};
    std::vector<uint8_t> list=std::vector<uint8_t>(0x2a000),renderer=std::vector<uint8_t>(0x680);
    Bloom::Scope scope;NativeOrdinaryPacket::Receipt packet;std::optional<Bloom::Owned> owned;
    unsigned acquired{},temporal_calls{},colour_calls{},tone_calls{},stopped{};
    std::array<uintptr_t,7> temporal_seen{};std::array<uintptr_t,8> colour_seen{};std::array<uintptr_t,9> tone_seen{};float colour_float{};bool unscoped{},bad_tone{};
    NativeRenderProbe::Snapshot snapshot;
    World(){require(base!=0,"bounded fixture image reserved");creation.contract.module=base;creation.contract.factory_create=base+0x2119040;
        creation.vtable[3]=base+0x100;creation.vtable[4]=base+0x200;creation.vtable[22]=base+0x2113cc0;creation.vtable[24]=base+0x264b7b0;
        factory_vtable[2]=creation.contract.factory_create;factory=reinterpret_cast<uintptr_t>(factory_vtable.data());
        put(base,0x508caa8,reinterpret_cast<uintptr_t>(&factory));put(base,0x4ff0bb8,creation.contract.pool);
        creation.contract.wrappers[1].methods={creation.vtable[3],creation.vtable[4],creation.vtable[22],creation.vtable[24]};
        const auto a=reinterpret_cast<uintptr_t>(even.data()),b=reinterpret_cast<uintptr_t>(creation.source.data());
        for(auto pointer:{a,b}){put(pointer,0,reinterpret_cast<uintptr_t>(creation.vtable.data()));put(pointer,0x98,uint64_t(0x20000b0)|(uint64_t(1)<<50));put(pointer,0x278,reinterpret_cast<uintptr_t>(creation.source_resource.Get()));}
        creation.pool_buckets.fill(0);even_node={0,a,0};creation.pool_node={0,b,1};
        for(auto* node:{&even_node,&creation.pool_node}){const auto bucket=native_pool_membership::hash((*node)[1])%creation.pool_buckets.size();(*node)[0]=creation.pool_buckets[bucket];creation.pool_buckets[bucket]=reinterpret_cast<uintptr_t>(node->data());}
        put(creation.contract.pool,0x22d0,native_pool_membership::Header{2,8,reinterpret_cast<uintptr_t>(creation.pool_buckets.data())});
        put(creation.contract.pool,0xc,int32_t(2));put(creation.contract.pool,0x2248,reinterpret_cast<uintptr_t>(bank.data()));put(creation.contract.pool,0x2250,int32_t(1));put(creation.contract.pool,0x2258,int32_t(0));put(creation.contract.pool,0x2260,reinterpret_cast<uintptr_t>(records.data()));put(creation.contract.pool,0x2268,int32_t(2));
        put(reinterpret_cast<uintptr_t>(bank.data()),0,std::array<uint32_t,2>{8,4});
        for(size_t i=0;i<2;++i){const auto native=i?b:a;const auto target=0xc4+i;const auto record=reinterpret_cast<uintptr_t>(records.data())+i*0x4f8;
            put(creation.contract.pool,0x10+target*0x28,std::array<uint64_t,5>{0x20000b0,0,0,0,0});put(reinterpret_cast<uintptr_t>(bank.data()),0x118+target*4,int32_t(i));
            put(record,0xc,std::array<uint32_t,6>{8,4,1,1,38,1});put(record,0x2dc,int32_t(1));put(record,0x4c0,int32_t(1));put(record,0x2e0,native);}
        put(rp(),0x50,a);put(rp(),0x38,a);NativeCameraEntries::get().enable();step(0);
        require(owner->configure(host.device.Get(),base,creation.api()),"real owned GPU creation configured");
    }
    uintptr_t lp()const{return reinterpret_cast<uintptr_t>(list.data());}uintptr_t rp()const{return reinterpret_cast<uintptr_t>(renderer.data());}
    void step(unsigned n,unsigned skipped=0){using E=NativeCameraEntries;auto left=vr_camera::IDENTITY,right=left;right[12]=.064f;FrameSyncClock::Frame frame{23+n+skipped,31+n+skipped,0,0,false};
        auto ticket=E::get().begin(lp()+0x160,E::Role::main,frame);E::get().finish(*ticket,0,left);ticket=E::get().begin(lp()+0x160,E::Role::ordinary_right,frame);E::get().finish(*ticket,1,right);
        const auto revision=*E::get().revision(lp());std::array<E::Entry,2> actual{{{lp()+0x168,0,E::Role::main,left},{lp()+0x168+0xa60,1,E::Role::main,right}}};scope.pair=*E::get().ordinary_pair(lp(),actual,revision);
        scope.scene={};auto& s=scope.scene;s.renderer=rp();s.list=lp();s.entry=scope.pair.right.entry;s.dispatch_sequence=57+n;s.render_counter_before=7+n;s.slot=1;s.ordinal=2;s.camera_revision_before=revision;s.view_flags_before=0x3f7c013;s.refusal=NativeRenderProbe::SceneRefusal::none;
        put(rp(),0x140,s.entry);put(lp(),0x29968,int32_t(2));put(lp(),0x29c78,s.render_counter_before);put(s.entry,0,s.view_flags_before);put(scope.pair.left.entry,0,s.view_flags_before);
        packet={};packet.dispatch=s.dispatch_sequence;packet.render_id=scope.pair.left.render_id;packet.renderer=rp();packet.context={lp(),s.render_counter_before,8,4,1,1};packet.reference=packet.applied=packet.source_unchanged=true;packet.continued=n>0;
        snapshot={};snapshot.sequence=s.dispatch_sequence;snapshot.renderer=rp();snapshot.list=lp();snapshot.ordinary_pair=scope.pair;snapshot.scene_calls=2;
        for(size_t slot=0;slot<2;++slot){auto scene=s;scene.slot=uint32_t(slot);scene.ordinal=slot+1;scene.entry=slot?scope.pair.right.entry:scope.pair.left.entry;scene.camera_revision_after=revision;scene.render_counter_after=s.render_counter_before;
            scene.persistent_before=NativeRenderProbe::read_history_bank(rp(),uint32_t(slot));scene.temporal_before=NativeRenderProbe::Scene::Temporal{scene.persistent_before->words,{}};snapshot.scenes.push_back(scene);}
        put(rp(),0x150,snapshot.scenes[1].persistent_before->words);std::array<uintptr_t,11> zero{};put(rp(),0x1a8,zero);
        creation.origin={s.dispatch_sequence,packet.render_id,revision,scope.pair.left.token,s.render_counter_before,1,s.entry,lp(),rp()};
    }
    uintptr_t native_target()const{const auto parity=scope.scene.render_counter_before&1;uintptr_t value{};require(native_texture_contract::read(reinterpret_cast<uintptr_t>(records.data())+parity*0x4f8+0x2e0,value),"actual pool wrapper readable");return value;}
    RenderTargetProbe::Observation observe(uintptr_t resource){auto view=creation.outputs[1].descriptors.views[0];view.resource=resource;view.identity=GpuObjectIdentity::get(reinterpret_cast<ID3D12Resource*>(resource));view.handle=resource+8;view.created=1;return {1,1,{view}};}
};
World* world{};
std::array<uintptr_t,5> grading_seen{};unsigned grading_calls{};
unsigned late_calls{};
unsigned sharpen_calls{};std::array<uintptr_t,6> sharpen_seen{};std::array<float,2> sharpen_float{};
__declspec(noinline)void sharpen(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,float e,uint8_t f,uint16_t g,float h){++sharpen_calls;sharpen_seen={a,b,c,d,f,g};sharpen_float={e,h};}
__declspec(noinline)void invoke_sharpen(NativeOrdinarySharpen::Native volatile fn,float exposure=1.75f){fn(300,400,500,600,.5f,1,0,exposure);volatile auto used=sharpen_calls;(void)used;}
std::array<uintptr_t,5> early_seen{};float early_delta{};unsigned early_calls{};
__declspec(noinline)void early_effect(uintptr_t a,uint8_t b,uintptr_t c,uintptr_t d,uintptr_t e,float f){early_seen={a,b,c,d,e};early_delta=f;++early_calls;}
__declspec(noinline)void invoke_early(NativeOrdinaryLateEffect::Early volatile fn,uintptr_t owner,uintptr_t entry){fn(owner,1,33,44,entry,1.25f);volatile auto used=early_calls;(void)used;}
__declspec(noinline)void late_effect(uintptr_t,uintptr_t source,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t){++late_calls;*reinterpret_cast<uintptr_t*>(source)=123;}
__declspec(noinline)void invoke_late(NativeOrdinaryLateEffect::Native volatile fn,uintptr_t owner,uintptr_t* source,uintptr_t entry){fn(owner,reinterpret_cast<uintptr_t>(source),3,4,entry,6,7,8,9,10);volatile auto used=late_calls;(void)used;}
__declspec(noinline)void grading(uintptr_t a,uintptr_t b,uintptr_t c,const void* d,int32_t index){grading_seen={a,b,c,reinterpret_cast<uintptr_t>(d),uintptr_t(index)};++grading_calls;volatile auto used=grading_calls;(void)used;}
__declspec(noinline)void invoke_grading(NativeOrdinaryGrading::Native volatile fn,const void* data){fn(11,22,33,data,1);volatile auto used=grading_calls;(void)used;}
__declspec(noinline)void invoke_grading_at(NativeOrdinaryGrading::Native volatile fn,uintptr_t core,int32_t index){fn(core,22,33,nullptr,index);volatile auto used=grading_calls;(void)used;}
__declspec(noinline)uintptr_t selector(uintptr_t renderer,uint32_t a,uint32_t b){++world->acquired;require(renderer==world->rp() && a==0xc4 && b==0xc5,"all original selector arguments exact");return world->native_target();}
__declspec(noinline)void temporal(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uint8_t g){++world->temporal_calls;world->temporal_seen={a,b,c,d,e,f,g};volatile auto used=world->temporal_calls;(void)used;}
__declspec(noinline)void tone(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uint32_t f,int32_t g,const void* h,uintptr_t i){++world->tone_calls;world->tone_seen={a,b,c,d,e,f,uintptr_t(g),reinterpret_cast<uintptr_t>(h),i};volatile auto used=world->tone_calls;(void)used;}
__declspec(noinline)void invoke_tone(Bloom::Tone volatile fn,uintptr_t bloom){fn(11,22,33,bloom,55,0x3f800000,1,reinterpret_cast<void*>(88),99);volatile auto used=world->tone_calls;(void)used;}
__declspec(noinline)void colour(uintptr_t a,uintptr_t b,float c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h,const void* i){++world->colour_calls;world->colour_seen={a,b,d,e,f,g,h,reinterpret_cast<uintptr_t>(i)};world->colour_float=c;invoke_tone(tone,world->bad_tone?0:g);volatile auto used=world->colour_calls;(void)used;}
__declspec(noinline)uintptr_t invoke_selector(Bloom::Selector volatile fn,uintptr_t renderer){const auto result=fn(renderer,0xc4,0xc5);volatile auto used=world->acquired;(void)used;return result;}
__declspec(noinline)void invoke_temporal(NativeTemporalProbe::Native volatile fn,const std::array<uintptr_t,7>& a){fn(a[0],a[1],a[2],a[3],a[4],a[5],uint8_t(a[6]));volatile auto used=world->temporal_calls;(void)used;}
__declspec(noinline)void invoke_colour(Bloom::Colour volatile fn,uintptr_t entry,uintptr_t bloom){fn(1,2,1.25f,entry,5,6,bloom,8,reinterpret_cast<void*>(9));volatile auto used=world->colour_calls;(void)used;}
unsigned volume_calls{};std::array<uintptr_t,3> volume_args{};float volume_delta{};
unsigned shading_calls{};uintptr_t shading_owner{};
__declspec(noinline)uintptr_t shading_ssr(uintptr_t owner){++shading_calls;shading_owner=owner;return 0xab01;}
__declspec(noinline)uintptr_t shading_ssao(uintptr_t owner){++shading_calls;shading_owner=owner;return 0xcd01;}
__declspec(noinline)uintptr_t shading_shadows(uintptr_t owner){++shading_calls;shading_owner=owner;return 0xef01;}
__declspec(noinline)uintptr_t invoke_shading(NativeOrdinaryShadingControl::Native volatile fn,uintptr_t owner=11){const auto result=fn(owner);volatile auto used=shading_calls;(void)used;return result;}
unsigned light_queries{},light_jobs{};std::array<int32_t,4> light_results{};std::function<void()> light_between;
uintptr_t selection_partition{};
__declspec(noinline)uintptr_t selection_resolve(uintptr_t proxy){volatile auto used=proxy;(void)used;return selection_partition;}
std::array<uintptr_t,5> selection_args{};std::array<bool,2> selection_visible{};unsigned selection_calls{};
__declspec(noinline)void selection_native(uintptr_t a,uintptr_t b,uintptr_t view,uintptr_t d,uintptr_t owner){
    selection_args={a,b,view,d,owner};++selection_calls;
    uintptr_t records{};ordinary_scene_scope::read(owner+0xa8,records);
    for(size_t i=0;i<2;++i){
        const auto record=records+i*0x148;uint64_t visible{},mask{};int32_t index{};int8_t group{};uintptr_t bank{};
        ordinary_scene_scope::read(record+0x138,visible);ordinary_scene_scope::read(record+0x12c,index);ordinary_scene_scope::read(record+0x134,group);
        NativeOrdinaryReflectionSelection::Resolve volatile resolve=selection_resolve;
        const auto partition=resolve(record+0x118);
        ordinary_scene_scope::read(partition+size_t(group+4)*0x20,bank);ordinary_scene_scope::read(bank+size_t(index)*0x20,mask);
        selection_visible[i]=(visible&mask&2)!=0;
    }
}
__declspec(noinline)void invoke_selection(NativeOrdinaryReflectionSelection::Native volatile fn,uintptr_t block,uintptr_t view,uintptr_t owner){fn(block,22,view,44,owner);volatile auto used=selection_calls;(void)used;}
unsigned shadow_fit_calls{};std::array<uintptr_t,6> shadow_fit_args{};std::function<void()> shadow_fit_between;
unsigned shadow_cache_calls{};std::array<uintptr_t,6> shadow_cache_args{};
__declspec(noinline)void shadow_cache(uintptr_t light,uintptr_t view,uintptr_t count,int32_t quality,uint8_t special,uint8_t refresh){++shadow_cache_calls;shadow_cache_args={light,view,count,uintptr_t(quality),special,refresh};}
unsigned shadow_lights_calls{};std::array<uintptr_t,4> shadow_lights_args{};
__declspec(noinline)void shadow_lights(uintptr_t owner,int32_t slot,uint8_t a,uint8_t b){++shadow_lights_calls;shadow_lights_args={owner,uintptr_t(slot),a,b};}
__declspec(noinline)void invoke_shadow_lights(NativeOrdinaryShadowLights::Native volatile fn,uintptr_t owner,int32_t slot){fn(owner,slot,5,6);volatile auto used=shadow_lights_calls;(void)used;}
unsigned spot_share_calls{};
__declspec(noinline)void spot_share_native(uintptr_t,uintptr_t,uint32_t,uintptr_t){++spot_share_calls;}
__declspec(noinline)void invoke_spot_share(NativeOrdinarySpotShadowShare::Native volatile fn,uintptr_t view,uintptr_t record){fn(1,view,2,record);volatile auto used=spot_share_calls;(void)used;}
__declspec(noinline)void invoke_shadow_cache(NativeOrdinaryShadowCache::Native volatile fn,const std::array<uintptr_t,6>& a){fn(a[0],a[1],a[2],int32_t(a[3]),uint8_t(a[4]),uint8_t(a[5]));volatile auto used=shadow_cache_calls;(void)used;}
__declspec(noinline)void shadow_fit(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f){
    ++shadow_fit_calls;shadow_fit_args={a,b,c,d,e,f};
    put(e,0x15c,std::array<float,16>{float(shadow_fit_calls),2,3,4,5,6,7,8,9,10,11,12,13,14,15,16});
    if(shadow_fit_between)shadow_fit_between();
}
__declspec(noinline)void invoke_shadow_fit(NativeOrdinaryShadowFit::Native volatile fn,const std::array<uintptr_t,6>& a){fn(a[0],a[1],a[2],a[3],a[4],a[5]);volatile auto used=shadow_fit_calls;(void)used;}
unsigned wait_calls{};uint32_t wait_result{};
unsigned depth_ready_calls{};
__declspec(noinline)void depth_ready(uintptr_t object){require(object==103 && GetLastError()==107,"depth wait argument and incoming last error preserved");++depth_ready_calls;SetLastError(109);}
__declspec(noinline)void invoke_depth_ready(NativeDepthReadyTiming::Native volatile fn){fn(103);volatile auto used=depth_ready_calls;(void)used;}
unsigned query_collect_calls{};
__declspec(noinline)uintptr_t query_collect(uintptr_t owner){require(owner==103 && GetLastError()==107,"query collector keeps arguments and incoming last error");++query_collect_calls;SetLastError(109);return uintptr_t(0xfedcba9876543210);}
__declspec(noinline)uintptr_t invoke_query_collect(NativeQueryDependencyProbe::Native volatile fn){auto result=fn(103);volatile auto used=query_collect_calls;(void)used;return result;}
__declspec(noinline)uint32_t native_wait(uintptr_t object,uint32_t timeout){require(object==101 && timeout==0xffffffff,"native wait arguments unchanged");++wait_calls;return wait_result;}
__declspec(noinline)uint32_t invoke_wait(NativeSceneWaitTiming::Native volatile fn){const auto result=fn(101,0xffffffff);volatile auto used=wait_calls;(void)used;return result;}
std::array<int32_t,2> light_native{-1,-1};
__declspec(noinline)int32_t light_query(uintptr_t manager,uintptr_t object,int8_t eye){++light_queries;require(object==100,"native light object preserved");return manager==33 && eye>=0 && eye<2?light_native[eye]:-1;}
__declspec(noinline)int32_t invoke_light_query(NativeOrdinaryLightVisibility::Query volatile fn,uintptr_t manager,int8_t eye){const auto r=fn(manager,100,eye);volatile auto used=light_queries;(void)used;return r;}
__declspec(noinline)void light_job(uintptr_t owner,uintptr_t list){require(owner==11 && list==22,"native light job arguments preserved");++light_jobs;light_results[0]=invoke_light_query(light_query,33,0);if(light_between)light_between();light_results[1]=invoke_light_query(light_query,33,1);light_results[2]=invoke_light_query(light_query,33,2);light_results[3]=invoke_light_query(light_query,44,0);}
__declspec(noinline)void invoke_light_job(NativeOrdinaryLightVisibility::Job volatile fn){fn(11,22);volatile auto used=light_jobs;(void)used;}
__declspec(noinline)void volume_frame(uintptr_t a,uintptr_t b,uintptr_t c,float d){++volume_calls;volume_args={a,b,c};volume_delta=d;volatile auto used=volume_calls;(void)used;}
__declspec(noinline)void invoke_volume(NativeOrdinaryVolumeFrame::Native volatile fn,uintptr_t entry,uintptr_t command=22,float delta=.02f){fn(11,command,entry,delta);volatile auto used=volume_calls;(void)used;}
unsigned flare_builds{},flare_reads{},flare_issues{};std::array<uintptr_t,6> flare_build_seen{};std::array<uintptr_t,5> flare_issue_seen{};
__declspec(noinline)void flare_build(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uint32_t e,uint8_t f){++flare_builds;flare_build_seen={a,b,c,d,e,f};}
__declspec(noinline)void flare_read(uintptr_t,uint8_t,uintptr_t){++flare_reads;}
__declspec(noinline)void flare_issue(uintptr_t a,uint8_t b,uintptr_t c,uintptr_t d,uintptr_t e){++flare_issues;flare_issue_seen={a,b,c,d,e};}
__declspec(noinline)void invoke_flare_build(NativeFlareProbe::Build volatile fn,uintptr_t flare,uintptr_t cameras){fn(flare,cameras,3,4,0x3f000000,1);volatile auto used=flare_builds;(void)used;}
__declspec(noinline)void invoke_flare_read(NativeFlareProbe::Read volatile fn,uintptr_t view){fn(7,1,view);volatile auto used=flare_reads;(void)used;}
__declspec(noinline)void invoke_flare_issue(NativeFlareProbe::Issue volatile fn,uintptr_t view){fn(7,1,9,view,11);volatile auto used=flare_issues;(void)used;}
// NativeFlareProbe: real detours pass every argument through and the window reads both camera blocks.
void flare_probe(){
    auto& p=NativeFlareProbe::get();
    require(p.install(reinterpret_cast<void*>(flare_build),reinterpret_cast<void*>(flare_read),reinterpret_cast<void*>(flare_issue)),"flare probe detours installed");
    std::array<uint8_t,0xc0> flare{};const auto f=reinterpret_cast<uintptr_t>(flare.data());
    put(f,0,uint8_t(4|8));put(f,8,std::array<float,2>{.25f,.5f});put(f,0x28,uint32_t(16));put(f,0x68,std::array<float,2>{1,1});
    std::vector<uint8_t> cameras(0x29b18);const auto c=reinterpret_cast<uintptr_t>(cameras.data());put(c,0x29b14,int32_t(2));
    std::array<uint8_t,0x40> view{};view[0x33]=1;const auto v=reinterpret_cast<uintptr_t>(view.data());
    invoke_flare_build(flare_build,f,c);invoke_flare_read(flare_read,v);invoke_flare_issue(flare_issue,v);
    require(flare_builds==1 && flare_build_seen==std::array<uintptr_t,6>{f,c,3,4,0x3f000000,1},"builder arguments unchanged");
    require(flare_reads==1 && flare_issues==1 && flare_issue_seen==std::array<uintptr_t,5>{7,1,9,v,11},"query read and issue arguments unchanged");
    const auto line=p.line();
    require(line.find("builds=1 cameras=2 block0={queried=1 wanted=1 pending=1 current=0.25 target=0.5} block1={queried=0 wanted=0 pending=0 current=1 target=1}")!=std::string::npos,"both camera blocks sampled after the builder");
    require(line.find("reads=s0:0,0,0/s1:0,0,0/sx:0,1,0 issues=s0:0,0,0/s1:0,0,0/sx:0,1,0")!=std::string::npos,"view index counted outside any ordinary slot");
    require(p.line().find("builds=0 ")==0,"each line starts a fresh window");
    // Hide: off writes nothing; on zeroes both blocks' current and target, and only those fields.
    put(f,0x8,std::array<float,2>{.25f,.5f});put(f,0x68,std::array<float,2>{1,1});const auto before=flare;
    p.configure_hide(false);require(!p.hide(f) && flare==before,"hide off: flare untouched");
    p.configure_hide(true);invoke_flare_build(flare_build,f,c);
    auto expected=before;std::fill_n(expected.begin()+0x8,8,uint8_t{});std::fill_n(expected.begin()+0x68,8,uint8_t{});
    require(flare==expected,"hide on: both blocks' visibility zeroed after the builder, nothing else");
    require(!p.hide(f),"already zero: no write");
    require(p.line().find("hide=on hidden=1")!=std::string::npos,"hide state and count logged");
    p.configure_hide(false);
}
// NativeAdaptationMirror: index 1 takes exactly index 0's adaptation fields, nothing else.
void adaptation_mirror(){
    auto& m=NativeAdaptationMirror::get();std::vector<uint8_t> core(0x198*2);const auto c=reinterpret_cast<uintptr_t>(core.data());
    for(size_t i=0;i<core.size();++i)core[i]=uint8_t(i*7+3);const auto before=core;
    m.configure(false);require(!m.mirror(c) && core==before,"native mode writes nothing");
    m.configure(true);require(m.mirror(c),"mirror writes when the fields differ");
    for(size_t i=0;i<core.size();++i){const bool field=i>=0x198+0x174 && i<0x198+0x194;require(core[i]==(field?before[i-0x198]:before[i]),"only index 1's +0x174..+0x194 change, to index 0's bytes");}
    require(!m.mirror(c),"equal fields: no write");
    require(m.line()=="mode=left calls=2 writes=1 unreadable=0" && m.line()=="mode=left calls=0 writes=0 unreadable=0","counts per window");
    require(!m.mirror(0),"no core, no write");m.configure(false);
}
void harvest_once(){
    using H=NativeOcclusionHarvestOnce;
    require(H::parse("once")==H::Mode::once && H::parse("native\r")==H::Mode::native && !H::parse("") && !H::parse("onc"),"harvest control words");
    require(H::skip(H::Mode::once,true,1u),"slot 1's Scene harvest is skipped in once mode");
    require(!H::skip(H::Mode::once,true,0u) && !H::skip(H::Mode::once,true,std::nullopt) && !H::skip(H::Mode::once,false,1u) && !H::skip(H::Mode::native,true,1u),
        "slot 0, an unauthenticated Scene, other callers and native mode always harvest");
    const uintptr_t scene=0x1805940,target=0x209fac0,ret=0x180820c;const int32_t rel=int32_t(target-ret);
    std::array<uint8_t,5> call{0xe8};std::memcpy(call.data()+1,&rel,4);
    require(H::scene_call(ret,target,scene,call),"the Scene call site is recognised by its call bytes");
    require(!H::scene_call(ret,target+1,scene,call) && !H::scene_call(scene+0x5000,target,scene,call) && !H::scene_call(ret,target,ret+1,call),"a call to another target or outside Scene is not the site");
    call[0]=0xe9;require(!H::scene_call(ret,target,scene,call),"only a call instruction counts");
    // The right eye's light upload takes its own section (NativeOrdinaryLightSection.h).
    using L=NativeOrdinaryLightSection;
    require(L::parse("slot\r\n")==L::Mode::slot && L::parse("native")==L::Mode::native && !L::parse("slo") && !L::parse(""),"light section control words");
    require(L::view(L::Mode::slot,true,1u,0)==1,"slot 1's Scene uploads section 1 instead of the left eye's section 0");
    require(L::view(L::Mode::slot,true,0u,0)==0 && L::view(L::Mode::slot,true,std::nullopt,0)==0 && L::view(L::Mode::slot,false,1u,0)==0 &&
        L::view(L::Mode::native,true,1u,0)==0,"slot 0, an unauthenticated Scene, other callers and native mode keep the native section");
    require(L::view(L::Mode::slot,true,1u,2)==2,"a native index other than 0 (a game PIP view) is left alone");
    {const uintptr_t caller=0x1f99260,target=0x1f8b2c0,ret=0x1f99d64;const int32_t r=int32_t(target-ret);
     std::array<uint8_t,5> c{0xe8};std::memcpy(c.data()+1,&r,4);
     require(L::site_call(ret,target,caller,c),"the upload site in 0x1f99260 is recognised by its call bytes");
     require(!L::site_call(ret,target+1,caller,c) && !L::site_call(caller+0x1001,target,caller,c) && !L::site_call(ret,target,ret+1,c),"another target or caller is not the site");}
    using V=NativeOrdinaryLightVisibility;
    require(V::parse_clamp("clamp")==true && V::parse_clamp("native\r")==false && !V::parse_clamp("clam") && !V::parse_clamp(""),"light clamp control words");
    // Second camera's light queries take the plain proxy test (NativeSecondCameraPlainLightTest.h).
    using P=NativeSecondCameraPlainLightTest;
    require(P::parse("plain")==P::Mode::plain && P::parse("native\r")==P::Mode::native && !P::parse("plai"),"plain-test control words");
    require(P::prepare_rva(P::REFERENCE_BUILD)==0x1efae60 && P::prepare_rva(P::PATCHED_BUILD)==0x1efb1a0 && !P::prepare_rva(P::PATCHED_BUILD+1),"known builds only");
    require(P::last_record(0x10000,3)==0x10000+2*0x278 && !P::last_record(0,3) && !P::last_record(0x10000,0) && !P::last_record(0x10000,5000),"the appended record is the list's last");
    require(P::decide(P::Mode::plain,1,true,0x10000,uint8_t(1))==P::Change::cleared,"camera 1's cached test is cleared in plain mode");
    require(P::decide(P::Mode::plain,1,true,0x10000,uint8_t(0))==P::Change::already_plain,"a plain record is left as it is");
    require(P::decide(P::Mode::plain,0,true,0x10000,uint8_t(1))==P::Change::none && P::decide(P::Mode::native,1,true,0x10000,uint8_t(1))==P::Change::none &&
        P::decide(P::Mode::plain,1,false,0x10000,uint8_t(1))==P::Change::none,"camera 0, native mode and mono are never touched");
    require(P::decide(P::Mode::plain,1,true,0,uint8_t(1))==P::Change::refused && P::decide(P::Mode::plain,1,true,0x10000,std::nullopt)==P::Change::refused &&
        P::decide(P::Mode::plain,1,true,0x10000,uint8_t(7))==P::Change::refused,"an unreadable record or a non-boolean byte is refused");
}
int main(int argc,char** argv){try{
    const std::string_view test=argc>1?argv[1]:"happy";if(test=="harvest_once"){harvest_once();std::cout<<"harvest once PASS\n";return 0;}if(test=="flare_probe"){flare_probe();std::cout<<"flare probe PASS\n";return 0;}if(test=="adaptation_mirror"){adaptation_mirror();std::cout<<"adaptation mirror PASS\n";return 0;}World fixture;world=&fixture;auto& bloom=Bloom::get();auto& probe=NativeTemporalProbe::get();
    bool scoped=true;
    {
        // -Profile/-PerformanceProbes hooks on the patched build: every address has a verified port
        // and every byte comparison fits inside it, or stereo itself would refuse to install.
        const auto stamp=game_build_ported::EXE;using W=NativeSceneWaitTiming;using D=NativeDepthReadyTiming;
        require(game_build::rva(stamp,W::WAIT_RVA) && game_build::code(stamp,W::WAIT_RVA,game_build_ported::code(W::WAIT_RVA),W::WAIT_BYTES),"scene wait helper ported");
        for(const auto caller:{W::DEPTH_CALLER,W::SHADOW_CALLER,W::THIRD_CALLER})require(game_build::rva(stamp,caller)!=0,"scene wait call sites ported");
        require(game_build::rva(stamp,D::HELPER_RVA) && game_build::code(stamp,D::HELPER_RVA,game_build_ported::code(D::HELPER_RVA),D::HELPER_BYTES),"depth-ready fence helper ported");
        using F=NativeFlareProbe;for(const auto& [rva,code]:{std::pair{F::FLARE_BUILD,F::FLARE_BUILD_BYTES},std::pair{F::FLARE_READ,F::FLARE_READ_BYTES},std::pair{F::FLARE_ISSUE,F::FLARE_ISSUE_BYTES}})
            require(game_build::rva(stamp,rva) && game_build::code(stamp,rva,game_build_ported::code(rva),code),"flare probe functions ported");
        // Trace rows for "other" waits: one per burst, summed, with the call count.
        std::vector<std::tuple<std::string,double,double,double>> rows;W::OtherWaits other;
        const auto emit=[&](const char* n,double s,double d,double c){rows.emplace_back(n,s,d,c);};
        other.add("scene_left_other_wait_cpu",10,0.5,emit);other.add("scene_left_other_wait_cpu",11,0.25,emit);
        other.add("scene_left_depth_wait_cpu",12,1,emit);
        require(rows.size()==1 && std::get<0>(rows[0])=="scene_left_depth_wait_cpu","named waits pass through at once");
        other.add("scene_left_other_wait_cpu",13,0.25,emit);other.add("scene_right_other_wait_cpu",14,0.5,emit);
        require(rows.size()==2 && std::get<0>(rows[1])=="scene_left_other_wait_cpu" && std::get<1>(rows[1])==10 && std::get<2>(rows[1])==1.0 && std::get<3>(rows[1])==3,"a burst becomes one summed row with its call count");
        other.add("scene_right_other_wait_cpu",30,0.5,emit);
        require(rows.size()==3 && std::get<0>(rows[2])=="scene_right_other_wait_cpu" && std::get<2>(rows[2])==0.5 && std::get<3>(rows[2])==1,"a gap over 5 ms starts a new burst");
    }
    {
        using Selection=NativeOrdinaryReflectionSelection;
        std::array<uint8_t,0xc0> owner_bytes{};
        std::array<uint8_t,0x290> records_bytes{};
        std::array<uint8_t,2> order{1,0};
        std::array<uint8_t,0xa0> partition{};
        std::array<uint8_t,0x40> bank{};
        const auto owner=reinterpret_cast<uintptr_t>(owner_bytes.data()), records=reinterpret_cast<uintptr_t>(records_bytes.data());
        selection_partition=reinterpret_cast<uintptr_t>(partition.data());
        put(selection_partition,0x80,reinterpret_cast<uintptr_t>(bank.data()));
        put(reinterpret_cast<uintptr_t>(bank.data()),0,uint64_t(1));
        put(reinterpret_cast<uintptr_t>(bank.data()),0x20,uint64_t(2));
        put(fixture.rp(),0x668,owner);put(owner,0xa8,records);put(owner,0xb0,int32_t(2));put(owner,0xb8,reinterpret_cast<uintptr_t>(order.data()));
        put(fixture.scope.scene.entry,0x33,uint8_t(1));
        put(records,0x138,uint64_t(0x83));put(records,0x136,uint8_t(1));
        put(records+0x148,0x138,uint64_t(1));put(records+0x148,0x12c,int32_t(1));put(records+0x148,0x136,uint8_t(1));
        const auto original=records_bytes;
        auto chosen=Selection::prepare(fixture.scope,owner,selection_resolve,3);
        require(chosen && chosen->different==1,"left-only PVS reflection selected for right eye");
        const auto copied=chosen->address();uintptr_t copied_records{},copied_order{};
        ordinary_scene_scope::read(copied+0xa8,copied_records);ordinary_scene_scope::read(copied+0xb8,copied_order);
        uint64_t mask{};ordinary_scene_scope::read(copied_records+0x138,mask);
        require(mask==0x83 && chosen->records[0x136]==1 && chosen->order==std::vector<uint8_t>({1,0}),"private union preserves other flags and ordering");
        ordinary_scene_scope::read(copied_records+0x148+0x138,mask);
        require(mask==1,"disjoint volume/PVS eyes cannot invent visibility");
        require(records_bytes==original && copied_records!=records && copied_order!=reinterpret_cast<uintptr_t>(order.data()),"native owner and records remain untouched");
        require(Selection::prepare(fixture.scope,owner,selection_resolve,0).has_value(),"zero native fallback remains valid with resolved PVS records");
        order[0]=2;require(!Selection::prepare(fixture.scope,owner,selection_resolve,3),"invalid order rejected");order[0]=1;
        put(records,0x12c,int32_t(-1));require(!Selection::prepare(fixture.scope,owner,selection_resolve,3),"invalid PVS index rejected");
        require(Selection::prepare(fixture.scope,owner,selection_resolve,0,false).has_value(),"native disabled PVS branch never reads irrelevant PVS metadata");
        put(records,0x140,uint8_t(1));require(Selection::prepare(fixture.scope,owner,selection_resolve,0).has_value(),"disabled reflection preserves native short circuit before invalid PVS metadata");put(records,0x140,uint8_t(0));
        put(records,0x138,uint64_t(0x80));require(Selection::prepare(fixture.scope,owner,selection_resolve,0).has_value(),"neither-eye volume never resolves irrelevant PVS metadata");put(records,0x138,uint64_t(0x83));
        order={1,1};require(Selection::prepare(fixture.scope,owner,selection_resolve,0).has_value(),"unused inventory record never resolves invalid PVS metadata");order={1,0};
        put(records,0x12c,int32_t(0));
        bool enabled=false;
        require(Selection::get().install(reinterpret_cast<void*>(selection_native),[&]()->std::optional<Selection::Scope>{return scoped?std::optional{fixture.scope}:std::nullopt;},selection_resolve,[]()->std::optional<Selection::Pvs>{return Selection::Pvs{0,true};},[&]{return enabled;}),"actual selection and resolver boundaries installed");
        invoke_selection(selection_native,fixture.rp()+0x610,fixture.scope.scene.entry,owner);
        require(selection_args[4]==owner && !selection_visible[0] && !selection_visible[1],"disabled pass preserves native PVS selection");
        fixture.step(101);enabled=true;
        invoke_selection(selection_native,fixture.rp()+0x610,fixture.scope.scene.entry,owner);
        require(selection_args[0]==fixture.rp()+0x610 && selection_args[1]==22 && selection_args[2]==fixture.scope.scene.entry && selection_args[3]==44 && selection_args[4]!=owner,"enabled pass preserves real eye and all other native arguments");
        require(selection_visible[0] && !selection_visible[1] && records_bytes==original && selection_calls==2,"private resolver changes only eligible reflection without modifying originals or replaying native");
        require(selection_resolve(records+0x118)==selection_partition,"original resolver restored outside private call scope");
        enabled=false;invoke_selection(selection_native,fixture.rp()+0x610,fixture.scope.scene.entry,owner);
        require(selection_args[4]!=owner,"mode choice is frozen for the dispatch");
        scoped=false;invoke_selection(selection_native,fixture.rp()+0x610,fixture.scope.scene.entry,owner);
        require(selection_args[4]==owner && !selection_visible[0],"unscoped native call never borrows private selection");scoped=true;
        invoke_selection(selection_native,fixture.rp()+0x611,fixture.scope.scene.entry,owner);
        require(selection_args[4]==owner,"foreign reflection block preserves native owner");
        require(NativeOrdinaryShadingControl::parse("native_reflections")==0 && NativeOrdinaryLightVisibility::parse("native_reflections")==NativeOrdinaryLightVisibility::Mode::stereo_union,"reflection A/B preserves screen effects and light union");
        put(fixture.rp(),0x668,uintptr_t(0));require(!Selection::prepare(fixture.scope,owner,selection_resolve,3),"foreign owner refused");
        selection_partition=0;fixture.step(0);
    }
    {
        using Cache=NativeOrdinaryShadowCache;auto& cache=Cache::get();bool enabled=false;
        const auto probe_path=std::filesystem::temp_directory_path()/("sm2vr_shadow_probe_"+std::to_string(GetCurrentProcessId())+".csv");
        require(PerformanceTrace::get().open(probe_path,{},10000,true),"native shadow preparation probe enabled explicitly");
        std::vector<uint8_t> storage(0x21000);const auto owner=reinterpret_cast<uintptr_t>(storage.data());
        auto left_world=vr_camera::IDENTITY,right_world=left_world;right_world[12]=.064f;
        put(fixture.scope.pair.left.entry,0x34,left_world);put(fixture.scope.pair.right.entry,0x34,right_world);
        put(fixture.scope.pair.left.entry,0x33,uint8_t(0));put(fixture.scope.pair.right.entry,0x33,uint8_t(1));
        require(Cache::pair_for_view(fixture.scope.pair.left.entry).has_value() && Cache::pair_for_view(fixture.scope.pair.right.entry).has_value(),"preparation resolves actual incoming registered list without assuming global render list");
        require(!Cache::pair_for_view(fixture.scope.pair.right.entry+0xa60),"unregistered view cannot borrow preparation pair");
        right_world[12]+=.1f;put(fixture.scope.pair.right.entry,0x34,right_world);require(!Cache::pair_for_view(fixture.scope.pair.left.entry),"changed other eye invalidates actual preparation pair");right_world[12]-=.1f;put(fixture.scope.pair.right.entry,0x34,right_world);
        require(cache.install(reinterpret_cast<void*>(shadow_cache),[&](uintptr_t view)->std::optional<Cache::Pair>{return scoped?Cache::pair_for_view(view):std::nullopt;},[&]{return owner;},[&]{return enabled;}),"actual cascade refresh detour installed");
        std::array<std::array<uintptr_t,6>,2> args{};
        for(unsigned eye=0;eye<2;++eye){const auto block=owner+0x90+eye*0x3ed0,light=block+0x10;
            put(block,0,int32_t(1));put(light,0xa4,int32_t(1));args[eye]={light,eye?fixture.scope.pair.right.entry:fixture.scope.pair.left.entry,light+0xf00,3,0,0};
            invoke_shadow_cache(shadow_cache,args[eye]);require(shadow_cache_args==args[eye],"default preserves every native cascade argument");
        }
        enabled=true;invoke_shadow_cache(shadow_cache,args[0]);require(shadow_cache_args[5]==0,"file change cannot split existing pair choice");
        fixture.step(1);invoke_shadow_cache(shadow_cache,args[0]);require(shadow_cache_args[5]==1,"next pair enters native refresh branch");
        enabled=false;invoke_shadow_cache(shadow_cache,args[1]);auto expected=args[1];expected[5]=1;
        require(shadow_cache_args==expected,"both eyes use same frozen refresh choice; all other native arguments unchanged");
        for(unsigned kind=0;kind<6;++kind){auto foreign=args[1];
            if(kind==0)scoped=false;if(kind==1)put(fixture.scope.pair.right.entry,0x33,uint8_t(2));if(kind==2)++foreign[2];if(kind==3)foreign[4]=1;if(kind==4)++foreign[0];if(kind==5)foreign[1]+=0xa60;
            invoke_shadow_cache(shadow_cache,foreign);require(shadow_cache_args==foreign,"unauthenticated/native-special cascade preserves original args");
            scoped=true;if(kind==1)put(fixture.scope.pair.right.entry,0x33,uint8_t(1));
        }
        fixture.step(2);invoke_shadow_cache(shadow_cache,args[1]);require(shadow_cache_args==args[1],"new pair restores native reuse when diagnostic disabled");
        require(cache.stats()[4]==1 && cache.stats()[5]==1 && shadow_cache_calls==12,"refresh counts distinguish both owned eyes from refused calls");
        PerformanceTrace::get().flush();std::ifstream probe_file{probe_path};const std::string probe_text{std::istreambuf_iterator<char>{probe_file},{}};
        require(probe_text.find(",shadow_prepare_left_cpu,")!=std::string::npos && probe_text.find(",shadow_prepare_right_cpu,")!=std::string::npos,"actual authenticated native calls publish preparation timings for both eyes without changing native call count or refresh guards");
        require(NativeOrdinaryShadingControl::parse("light_union_shadow_refresh")==0 && NativeOrdinaryLightVisibility::parse("light_union_shadow_refresh")==NativeOrdinaryLightVisibility::Mode::stereo_union,"refresh retains native effects and light union");
        require(Cache::refreshes("light_union_shadow_refresh") && !Cache::refreshes("light_union") && !Cache::refreshes("none") && !Cache::refreshes("light_union_all") &&
            !Cache::refreshes("light_union_shadow_refresh_bogus"),"only refresh modes (and the reflection A/B modes that always carried it) force the refit");
        for(const auto& [name,bits]:std::array<std::pair<const char*,unsigned>,4>{{{"light_union_shadow_refresh_ssr",1},{"light_union_shadow_refresh_ssao",2},
                {"light_union_shadow_refresh_screen_shadows",4},{"light_union_shadow_refresh_all",7}}})
            require(Cache::refreshes(name) && NativeOrdinaryShadingControl::parse(name)==bits && NativeOrdinaryLightVisibility::parse(name)==NativeOrdinaryLightVisibility::Mode::stereo_union,
                "refresh plus effect isolation keeps refresh and light union while disabling the named effects");
        {   // Both eyes shadow the same local lights (NativeOrdinaryShadowLights).
            using SL=NativeOrdinaryShadowLights;auto& shared=SL::get();
            const auto list=[](std::initializer_list<int32_t> lights){std::array<uint8_t,SL::LIST> v{};int32_t n=0;for(auto l:lights)std::memcpy(v.data()+4*n++,&l,4);std::memcpy(v.data()+0x40,&n,4);return v;};
            const auto both=[](const std::array<uint8_t,SL::LIST>& l0,const std::array<uint8_t,SL::LIST>& l1){SL::Lists v{};std::copy(l0.begin(),l0.end(),v.begin());std::copy(l1.begin(),l1.end(),v.begin()+SL::LIST);return v;};
            auto lists=both(list({7,3}),list({7}));require(SL::align(lists)==SL::Result::copied && lists==both(list({7,3}),list({7,3})),"second eye takes the first eye's lights, count and order");
            lists=both(list({3,7}),list({7,3}));require(SL::align(lists)==SL::Result::copied && lists==both(list({3,7}),list({3,7})),"same lights in another order are reordered (atlas rows follow the list order)");
            lists=both(list({}),list({4}));require(SL::align(lists)==SL::Result::copied && lists==both(list({}),list({})),"no shadowed light in the first eye: none in the second");
            lists=both(list({2,9}),list({2,9}));require(SL::align(lists)==SL::Result::same,"equal lists are left alone");
            auto bad=both(list({2}),list({5}));int32_t n=17;std::memcpy(bad.data()+0x40,&n,4);auto kept=bad;require(SL::align(bad)==SL::Result::refused && bad==kept,"an impossible count is refused untouched");
            bad=both(list({-1}),list({5}));kept=bad;require(SL::align(bad)==SL::Result::refused && bad==kept,"a negative light index is refused untouched");
            require(SL::shares("light_union_shadow_refresh") && SL::shares("light_union_shadow_refresh_all") && !SL::shares("light_union_shadow_refresh_native_shadow_lights") &&
                !SL::shares("light_union") && !SL::shares("none"),"on in the default refresh modes, off in its A/B mode and without refresh");
            const std::string ab="light_union_shadow_refresh_native_shadow_lights";
            require(Cache::refreshes(ab) && NativeOrdinaryShadingControl::parse(ab)==0 && NativeOrdinaryLightVisibility::parse(ab)==NativeOrdinaryLightVisibility::Mode::stereo_union,
                "the shadow-lights A/B mode keeps refresh, light union and native screen effects");
            SL::Lists memory=both(list({11,12}),list({12}));bool stereo=true,enabled=true;const uintptr_t lights_global=0x5000;
            require(shared.install(reinterpret_cast<void*>(shadow_lights),[&]{return stereo;},[&]{return owner;},[&]{return lights_global;},
                [&](uintptr_t at,SL::Lists& v,bool write){if(at!=lights_global)return false;if(write)memory=v;else v=memory;return true;},[&]{return enabled;}),"shadow-lights detour installed");
            invoke_shadow_lights(shadow_lights,owner,0);require(memory==both(list({11,12}),list({12})) && shadow_lights_args==std::array<uintptr_t,4>{owner,0,5,6},"first eye's preparation is native");
            invoke_shadow_lights(shadow_lights,owner+8,1);require(memory==both(list({11,12}),list({12})),"a foreign owner is left alone");
            stereo=false;invoke_shadow_lights(shadow_lights,owner,1);require(memory==both(list({11,12}),list({12})),"no current stereo pair: native");stereo=true;
            enabled=false;invoke_shadow_lights(shadow_lights,owner,1);require(memory==both(list({11,12}),list({12})),"A/B mode off: each eye keeps its own lights");enabled=true;
            invoke_shadow_lights(shadow_lights,owner,1);require(memory==both(list({11,12}),list({11,12})) && shadow_lights_args==std::array<uintptr_t,4>{owner,1,5,6},"second eye's preparation sees the first eye's list; native arguments unchanged");
            invoke_shadow_lights(shadow_lights,owner,1);
            const auto st=shared.stats();require(st[0]==6 && st[1]==2 && st[2]==1 && st[3]==1 && st[4]==0 && shadow_lights_calls==6,"counts: calls, second-eye calls, equal, copied, refused");
        }
        {   // Diagnostic: slot 1 reuses slot 0's spot shadow tiles (NativeOrdinarySpotShadowShare).
            using SS=NativeOrdinarySpotShadowShare;auto& share=SS::get();
            std::vector<uint8_t> blocks(SS::PUBLISHED+2*SS::BLOCK);const auto base_owner=reinterpret_cast<uintptr_t>(blocks.data());
            const auto rec=[&](unsigned slot,unsigned row){return base_owner+SS::PUBLISHED+slot*SS::BLOCK+SS::FIRST+row*SS::RECORD;};
            const auto spot=[&](uintptr_t r,float x){put(r,0xa4,int32_t(3));put(r,0x80,x);put(r,0x84,-682.f);put(r,0x88,5.f);};
            spot(rec(0,0),29.f);spot(rec(1,0),29.f);spot(rec(0,1),-93.f);spot(rec(1,1),13.f);
            require(SS::second_row(base_owner,rec(1,1))==1u && !SS::second_row(base_owner,rec(0,1)) && !SS::second_row(base_owner,rec(1,1)+8) && !SS::second_row(base_owner,rec(1,0)+4*SS::RECORD),
                "only slot 1's four record rows are recognised");
            require(SS::shares("light_union_shadow_refresh_shared_spot_shadows") && !SS::shares("light_union_shadow_refresh") && Cache::refreshes("light_union_shadow_refresh_shared_spot_shadows") &&
                NativeOrdinaryShadowLights::shares("light_union_shadow_refresh_shared_spot_shadows") && NativeOrdinaryShadingControl::parse("light_union_shadow_refresh_shared_spot_shadows")==0,
                "the diagnostic mode keeps refresh, list alignment and native effects; the default does not reuse tiles");
            const uintptr_t second=0x7001,first=0x7002;bool enabled=false;
            require(share.install(reinterpret_cast<void*>(spot_share_native),[&](uintptr_t v){return v==second;},[&]{return base_owner;},
                [](uintptr_t at,SS::Record& v){std::memcpy(v.data(),reinterpret_cast<const void*>(at),v.size());return true;},[&]{return enabled;}),"spot share detour installed");
            invoke_spot_share(spot_share_native,second,rec(1,0));require(spot_share_calls==1,"default: slot 1 renders its own spot shadows");
            enabled=true;
            invoke_spot_share(spot_share_native,second,rec(1,0));require(spot_share_calls==1,"same spot in slot 0's row: slot 1 reuses the tile");
            invoke_spot_share(spot_share_native,second,rec(1,1));require(spot_share_calls==2,"another light in slot 0's row: slot 1 renders");
            invoke_spot_share(spot_share_native,first,rec(0,0));require(spot_share_calls==3,"slot 0 always renders");
            invoke_spot_share(spot_share_native,first,rec(1,0));require(spot_share_calls==4,"an unauthenticated view renders");
            put(rec(0,0),0xa4,int32_t(2));invoke_spot_share(spot_share_native,second,rec(1,0));require(spot_share_calls==5,"not a spot in slot 0's row: slot 1 renders");
            const auto st=share.stats();require(st[0]==6 && st[1]==3 && st[2]==1 && st[3]==2,"counts: calls, slot-1 spot records, reused, rendered");
        }
        require(!NativeOrdinaryShadingControl::parse("light_union_shadow_refresh_bogus") && NativeOrdinaryLightVisibility::parse("light_union_shadow_refresh_bogus")==NativeOrdinaryLightVisibility::Mode::native,"unknown refresh suffix is refused by every control");
        fixture.step(0);
    }
    {
        using Fit=NativeOrdinaryShadowFit;auto& fit=Fit::get();
        std::vector<uint8_t> shadow_owner(0x21000);const auto owner=reinterpret_cast<uintptr_t>(shadow_owner.data());
        fixture.step(63);fixture.scope.scene.slot=0;fixture.scope.scene.ordinal=1;fixture.scope.scene.entry=fixture.scope.pair.left.entry;
        put(fixture.rp(),0x140,fixture.scope.scene.entry);put(owner,0x1fc52,int16_t(6));
        require(fit.install(reinterpret_cast<void*>(shadow_fit),[&]()->std::optional<Bloom::Scope>{return scoped?std::optional{fixture.scope}:std::nullopt;},[&]{return owner;}),"actual shadow fit detour installed");
        for(unsigned eye=0;eye<2;++eye){
            const auto block=owner+0xff60+eye*0x3ed0,light=block+0x10,sub=light+0x100,shadow=fixture.lp()+0x19c+(eye+2)*0xa60;
            put(block,0,int32_t(1));put(light,0xa4,int32_t(1));put(light,0xfac,int32_t(0));put(sub,0x1b2,int16_t(eye+2));
            put(sub,0xd0,std::array<float,16>{21,22,23,24});
            const auto view=eye?fixture.scope.pair.right.entry:fixture.scope.pair.left.entry;
            const std::array<uintptr_t,6> args{light,view,33,44,shadow,sub};
            const auto expected_calls=shadow_fit_calls+1;invoke_shadow_fit(shadow_fit,args);const auto e=fit.evidence()[eye];
            require(shadow_fit_args==args && e.complete && e.dispatch==120 && e.eye==eye && e.camera==eye+2 && e.cache_camera==6 && e.matrix[0]==float(expected_calls) && e.matrix[15]==16 && e.cascade[0]==21,"post-fit receipt reads actual output and preserves every native argument");
            require(!Fit::identify(fixture.scope,owner,light,view,shadow+1,sub) && !Fit::identify(fixture.scope,owner,light,view,shadow,sub+1),"wrong output ownership refused");
            require(!Fit::identify(fixture.scope,owner,light+0x10,view,shadow,sub) && !Fit::identify(fixture.scope,owner,light,view+0xa60,shadow,sub),"foreign light or mismatched view refused");
            put(light,0xa4,int32_t(2));invoke_shadow_fit(shadow_fit,args);require(fit.evidence()[eye].matrix==e.matrix,"local light remains native without directional receipt");put(light,0xa4,int32_t(1));
            scoped=false;invoke_shadow_fit(shadow_fit,args);require(fit.evidence()[eye].matrix==e.matrix,"unscoped fit remains native without receipt");scoped=true;
            shadow_fit_between=[&]{++fixture.scope.pair.right.token;};invoke_shadow_fit(shadow_fit,args);--fixture.scope.pair.right.token;shadow_fit_between={};
            require(fit.evidence()[eye].matrix==e.matrix,"producer invalidation during native call refuses receipt");
            auto foreign=fixture.scope;foreign.scene.slot=1;require(!Fit::identify(foreign,owner,light,view,shadow,sub),"only authenticated left Scene hosts producer fits");
            put(light,0xfac,int32_t(8));require(!Fit::identify(fixture.scope,owner,light,view,shadow,sub),"out of range cascade slot refused");put(light,0xfac,int32_t(0));
        }
        require(shadow_fit_calls==8 && fit.stats()[0]==8 && fit.stats()[1]==8 && fit.stats()[4]==4,"native shadow fit executes once on every path; counters distinguish calls from accepted receipts");fixture.step(0);
    }
    {
        unsigned observed{};std::string name;
        require(NativeSceneWaitTiming::get().install(reinterpret_cast<void*>(native_wait),[&]()->std::optional<NativeRenderProbe::Scene>{return scoped?std::optional{fixture.scope.scene}:std::nullopt;},[&](const char* label,double start,double duration){require(start>0 && duration>=0,"wait timing bounded by actual invocation");++observed;name=label;}),"native wait timing installed");
        for(const auto result:{1u,0xffffff00u,0xffffffffu}){wait_result=result;require(invoke_wait(native_wait)==result,"native synchronization result preserved exactly");}
        require(wait_calls==3 && observed==3 && name=="scene_right_other_wait_cpu","each ordinary wait observed once");
        scoped=false;require(invoke_wait(native_wait)==wait_result && wait_calls==4 && observed==3,"unscoped wait executes without observation");scoped=true;
    }
    {
        unsigned observed{};
        require(NativeDepthReadyTiming::get().install(reinterpret_cast<void*>(depth_ready),[&]()->std::optional<NativeRenderProbe::Scene>{SetLastError(999);return scoped?std::optional{fixture.scope.scene}:std::nullopt;},[&](const char* name,double start,double duration,double id){require(std::string{name}=="scene_right_depth_ready_cpu" && start>0 && duration>=0 && id==fixture.scope.scene.dispatch_sequence,"depth-ready wait has actual scene/eye/job timing");++observed;SetLastError(998);throw std::runtime_error("observer");}),"depth-ready helper detoured");
        SetLastError(107);invoke_depth_ready(depth_ready);require(GetLastError()==109 && depth_ready_calls==1 && observed==1,"observer exceptions cannot change native execution or outgoing last error");
        scoped=false;SetLastError(107);invoke_depth_ready(depth_ready);require(GetLastError()==109 && depth_ready_calls==2 && observed==1,"unscoped depth-ready wait remains native");scoped=true;
    }
    {
        unsigned reads{};
        require(NativeQueryDependencyProbe::get().install(reinterpret_cast<void*>(query_collect),[&](uintptr_t owner)->SceneQueryDependencyTiming::Snapshot{require(owner==103,"collector snapshot owner");++reads;SetLastError(999);throw std::runtime_error("snapshot unavailable");}),"passive collector hook installed");
        {SceneQueryDependencyTiming scope{0,9,true};SetLastError(107);require(invoke_query_collect(query_collect)==uintptr_t(0xfedcba9876543210) && GetLastError()==109 && query_collect_calls==1 && reads==1,"failed observation preserves exactly one native call, return bits and outgoing error");}
        SetLastError(107);require(invoke_query_collect(query_collect)==uintptr_t(0xfedcba9876543210) && query_collect_calls==2 && reads==1 && GetLastError()==109,"unscoped collector does not read diagnostic metadata");
    }
    {
        using Light=NativeOrdinaryLightVisibility;auto& light=Light::get();auto enabled=Light::Mode::native;
        require(light.install(reinterpret_cast<void*>(light_job),reinterpret_cast<void*>(light_query),[&](uintptr_t owner,uintptr_t list)->std::optional<Light::Pair>{return scoped && owner==11 && list==22?std::optional{fixture.scope.pair}:std::nullopt;},[]{return uintptr_t(33);},[&]{return enabled;}),"light visibility native detours installed");
        invoke_light_job(light_job);require(light_results==std::array<int32_t,4>{-1,-1,-1,-1},"disabled mode preserves every native query");
        enabled=Light::Mode::force_visible;light_between=[&]{enabled=Light::Mode::native;};invoke_light_job(light_job);
        require(light_results==std::array<int32_t,4>{4,4,-1,-1},"one frozen choice for both eyes, other views and managers remain native");light_between={};
        require(invoke_light_query(light_query,33,0)==-1,"outside native light job no visibility override");
        enabled=Light::Mode::force_visible;scoped=false;invoke_light_job(light_job);require(light_results[0]==-1 && light_results[1]==-1,"unscoped light job stays native");scoped=true;
        ++fixture.scope.pair.right.token;invoke_light_job(light_job);require(light_results[0]==-1,"foreign producer stays native");--fixture.scope.pair.right.token;
        light_between=[&]{NativeCameraEntries::get().reset(fixture.lp()+0x160);};invoke_light_job(light_job);
        require(light_results[0]==4 && light_results[1]==-1,"invalidated pair stops override within job");light_between={};fixture.step(0);
        require(light_jobs==5 && light_queries==21,"original job and queries always execute");
        enabled=Light::parse("light_union_all");
        require(enabled==Light::Mode::stereo_union,"effect isolation retains stereo light selection");
        for(const auto values:{std::array<int32_t,2>{-1,4},{3,-1},{0,0},{-1,-1},{1,2}}){
            light_native=values;const auto before=light_queries;invoke_light_job(light_job);
            const auto result=(std::max)(values[0],values[1]);
            require(light_results==std::array<int32_t,4>{result,result,-1,-1} && light_queries==before+6,"union queries both native eyes, preserves hidden and missing results, and leaves foreign queries alone");
        }
        light_native={-1,4};scoped=false;invoke_light_job(light_job);require(light_results[0]==-1 && light_results[1]==4,"union outside owned job preserves asymmetric native values");scoped=true;
        light_native={-1,-1};enabled=Light::Mode::native;
        auto left=vr_camera::IDENTITY,right=left;right[12]=.064f;
        put(fixture.base,0x4faf810,uintptr_t(11));put(fixture.base,0x4f7c6c8,fixture.lp());put(fixture.lp()+0x160,0x29b14,int32_t(2));
        put(fixture.scope.pair.left.entry,0x34,left);put(fixture.scope.pair.right.entry,0x34,right);
        put(fixture.scope.pair.left.entry,0x33,uint8_t(0));put(fixture.scope.pair.right.entry,0x33,uint8_t(1));
        require(Light::pair_for(fixture.base,11,fixture.lp()+0x160).has_value(),"real list inventory joins native registered pair");
        put(fixture.scope.pair.right.entry,0x33,uint8_t(0));require(!Light::pair_for(fixture.base,11,fixture.lp()+0x160),"wrong native eye ID refuses admission");
        put(fixture.scope.pair.right.entry,0x33,uint8_t(1));require(!Light::pair_for(fixture.base,12,fixture.lp()+0x160) && !Light::pair_for(fixture.base,11,fixture.lp()),"wrong owner or list refuses admission");
    }
    {
        using Control=NativeOrdinaryShadingControl;auto& control=Control::get();std::optional<unsigned> mask=0;unsigned polls{};
        std::vector<uint8_t> lights(0x200),shadows(0x20000);
        const auto light_address=reinterpret_cast<uintptr_t>(lights.data()),shadow_address=reinterpret_cast<uintptr_t>(shadows.data());
        put(fixture.base,0x4faf810,light_address);put(fixture.base,0x4faef88,shadow_address);
        for(size_t i=0;i<4;++i){put(light_address,0x90+i*0x44,int32_t(i+1));put(shadow_address,0xff60+i*0x3ed0,int32_t(4-i));}
        auto evidence=Control::read_state(fixture.base,fixture.scope);
        require(evidence.complete && evidence.dispatch==57 && evidence.slot==1 && evidence.lights==std::array<int32_t,4>{1,2,3,4} && evidence.shadows==std::array<int32_t,4>{4,3,2,1},"lighting evidence reads distinct native per-view lists under authenticated scope");
        ++fixture.scope.pair.right.token;require(!Control::read_state(fixture.base,fixture.scope).complete,"foreign producer cannot authenticate lighting evidence");--fixture.scope.pair.right.token;
        put(fixture.base,0x4faef88,uintptr_t{});require(!Control::read_state(fixture.base,fixture.scope).complete,"missing native owner produces incomplete evidence");
        put(fixture.base,0x4faef88,shadow_address);
        require(Control::parse("none")==0 && Control::parse("ssr")==1 && Control::parse("ssao")==2 && Control::parse("screen_shadows")==4 && Control::parse("all")==7 && !Control::parse("SSR") && !Control::parse("all extra"),"bounded exact diagnostic vocabulary");
        for(const auto& [name,bits]:std::array<std::pair<const char*,unsigned>,4>{{{"light_union_ssr",1},{"light_union_ssao",2},{"light_union_screen_shadows",4},{"light_union_all",7}}})
            require(Control::parse(name)==bits && NativeOrdinaryLightVisibility::parse(name)==NativeOrdinaryLightVisibility::Mode::stereo_union,"both native controls agree on combined effect isolation");
        require(!Control::parse("light_union_invalid") && NativeOrdinaryLightVisibility::parse("light_union_invalid")==NativeOrdinaryLightVisibility::Mode::native,"unknown prefix suffix is not accepted as stereo union");
        require(control.install({reinterpret_cast<void*>(shading_ssr),reinterpret_cast<void*>(shading_ssao),reinterpret_cast<void*>(shading_shadows)},[&]()->std::optional<Bloom::Scope>{return scoped?std::optional{fixture.scope}:std::nullopt;},[]{return uintptr_t(11);},[&]{++polls;return mask;}),"native settings reader detours installed");
        require(invoke_shading(shading_ssr)==0xab01 && invoke_shading(shading_ssao)==0xcd01 && invoke_shading(shading_shadows)==0xef01,"none preserves all native setting returns");
        mask=1;require(invoke_shading(shading_ssr)==0xab01 && polls==1,"mode remains fixed throughout a dispatch");
        fixture.step(1);const auto right=fixture.scope;auto left=right;left.scene.slot=0;left.scene.ordinal=1;left.scene.entry=left.pair.left.entry;
        fixture.scope=left;put(fixture.rp(),0x140,left.scene.entry);
        require(invoke_shading(shading_ssr)==0xab00 && invoke_shading(shading_ssao)==0xcd01 && invoke_shading(shading_shadows)==0xef01,"only selected native enable result changes and upper bits are preserved");
        mask=Control::parse("light_union_all");fixture.scope=right;put(fixture.rp(),0x140,right.scene.entry);
        require(invoke_shading(shading_ssr)==0xab00 && invoke_shading(shading_ssao)==0xcd01 && polls==2,"right eye uses the identical frozen choice after a control update");
        fixture.step(2);require(invoke_shading(shading_ssao)==0xcd00 && invoke_shading(shading_shadows)==0xef00,"new dispatch accepts all disabled effects");
        require(invoke_shading(shading_ssr,12)==0xab01 && shading_owner==12,"foreign settings owner stays native");
        scoped=false;require(invoke_shading(shading_ssao)==0xcd01,"unscoped settings stay native");scoped=true;
        ++fixture.scope.pair.right.token;require(invoke_shading(shading_ssao)==0xcd01,"foreign producer stays native");
        mask={};fixture.step(3);require(invoke_shading(shading_ssao)==0xcd00,"missing or malformed control preserves mode");
        mask=8;fixture.step(4);require(invoke_shading(shading_shadows)==0xef00,"out of range control preserves mode");
        mask=0;fixture.step(5);require(invoke_shading(shading_ssr)==0xab01,"none restores native behavior");
        require(shading_calls==17,"native getter executes on every request including overrides");
        fixture.step(0);
    }
    {
        // Volumetric query sites (light shafts, fog): union the two eyes' delayed occlusion results
        // inside the authenticated Scene, on the query detour the light job already owns.
        using Light=NativeOrdinaryLightVisibility;auto& light=Light::get();
        std::optional<Light::Site> site=Light::Site::lightshafts;bool volumetric=true;unsigned volumetric_polls{};
        const auto sites=[&]{return Light::SceneSites{[&](uintptr_t){return site;},
            [&]()->std::optional<ordinary_scene_scope::Scope>{return scoped?std::optional{fixture.scope}:std::nullopt;},
            [](Light::Site s){return uintptr_t(s==Light::Site::fog_volume?44:33);},[&]{++volumetric_polls;return volumetric;}};};
        require(!light.install_scene_sites({}),"incomplete scene sites are refused");
        require(light.install_scene_sites(sites()),"volumetric sites installed on the existing query detour");
        require(!light.install_scene_sites(sites()),"scene sites are published once");
        fixture.step(1);const auto right=fixture.scope;auto left=right;left.scene.slot=0;left.scene.ordinal=1;left.scene.entry=left.pair.left.entry;
        const auto as=[&](const ordinary_scene_scope::Scope& scope){fixture.scope=scope;put(fixture.rp(),0x140,scope.scene.entry);};
        light_native={-1,4};as(left);auto queries=light_queries;
        require(invoke_light_query(light_query,33,0)==4 && light_queries==queries+2,"left eye's shafts take the right eye's visible result");
        as(right);require(invoke_light_query(light_query,33,1)==4,"right eye keeps its own visible result");
        light_native={3,-1};require(invoke_light_query(light_query,33,1)==3,"right eye gains the left eye's visible result");
        light_native={-1,-1};require(invoke_light_query(light_query,33,1)==-1,"missing in both eyes stays missing");
        light_native={-1,4};queries=light_queries;
        require(invoke_light_query(light_query,33,0)==-1 && light_queries==queries+1,"a query eye other than the Scene's own slot stays native");
        site=Light::Site::fog_volume;require(invoke_light_query(light_query,33,1)==4 && light_queries==queries+2,"wrong query manager for the site stays native");
        site={};require(invoke_light_query(light_query,33,0)==-1 && light_queries==queries+3,"non-volumetric callers stay native");
        site=Light::Site::fog_sun;as(left);
        scoped=false;require(invoke_light_query(light_query,33,0)==-1,"unscoped query stays native");scoped=true;
        ++fixture.scope.pair.right.token;require(invoke_light_query(light_query,33,0)==-1,"foreign producer stays native");--fixture.scope.pair.right.token;
        require(invoke_light_query(light_query,33,0)==4,"fog sun visibility is unioned too");
        volumetric=false;require(invoke_light_query(light_query,33,0)==4 && volumetric_polls==1,"the mode is frozen for the whole dispatch");
        fixture.step(2);{const auto next=fixture.scope;auto next_left=next;next_left.scene.slot=0;next_left.scene.ordinal=1;next_left.scene.entry=next_left.pair.left.entry;as(next_left);}
        queries=light_queries;require(invoke_light_query(light_query,33,0)==-1 && light_queries==queries+1 && volumetric_polls==2,"disabled mode on the next dispatch queries only the native eye");
        const auto stats=light.scene_stats();
        require(stats[0]==4 && stats[1]==2 && stats[2]==0 && stats[3]==1 && stats[4]==2 && stats[5]==1 && stats[6]==2 && stats[7]==2 && stats[8]==0,"counters separate unioned, refused and changed results per site");
        constexpr uintptr_t base=0x140000000;
        require(Light::site_for(base,base+0x1fb2719)==Light::Site::lightshafts && Light::site_for(base,base+0x1fb27bc)==Light::Site::lightshafts &&
            Light::site_for(base,base+0x1e2d9c4)==Light::Site::fog_sun && Light::site_for(base,base+0x1e208c3)==Light::Site::fog_volume &&
            !Light::site_for(base,base+0x1ef97de) && !Light::site_for(0,0x1fb2719),"only the four verified native return addresses are volumetric sites");
        require(Light::manager_global(Light::Site::fog_volume)==0x4ff0bd0 && Light::manager_global(Light::Site::lightshafts)==0x4ff0bc8 && Light::manager_global(Light::Site::fog_sun)==0x4ff0bc8,"each site checks the manager its native getter returns");
        const std::string ab="light_union_shadow_refresh_native_volumetrics";
        require(Light::unions_volumetrics("light_union") && Light::unions_volumetrics("light_union_shadow_refresh") && !Light::unions_volumetrics(ab) && !Light::unions_volumetrics("none") &&
            Light::parse(ab)==Light::Mode::stereo_union && NativeOrdinaryShadingControl::parse(ab)==0 && NativeOrdinaryShadowCache::refreshes(ab),
            "the volumetric A/B mode keeps light union, refresh and native effects and only restores native volumetric visibility");
        light_native={-1,-1};fixture.step(0);
    }
    {
        auto& volume=NativeOrdinaryVolumeFrame::get();
        require(volume.install(reinterpret_cast<void*>(volume_frame),[&]()->std::optional<Bloom::Scope>{return scoped?std::optional{fixture.scope}:std::nullopt;},[]{return uintptr_t(11);}),"volume setup detour installed");
        invoke_volume(volume_frame,fixture.scope.scene.entry);require(volume_calls==1,"right without completed left keeps native setup");
        for(unsigned n=1;n<=12;++n){
            fixture.step(n);const auto right=fixture.scope;auto left=right;left.scene.slot=0;left.scene.ordinal=1;left.scene.entry=left.pair.left.entry;
            fixture.scope=left;put(fixture.rp(),0x140,left.scene.entry);const auto before=volume_calls;
            invoke_volume(volume_frame,left.scene.entry);require(volume_calls==before+1 && volume_args==std::array<uintptr_t,3>{11,22,left.scene.entry} && volume_delta==.02f,"left always advances the native volume with exact arguments");
            fixture.scope=right;put(fixture.rp(),0x140,right.scene.entry);
            if(n==2)++fixture.scope.scene.dispatch_sequence;
            if(n==3)scoped=false;
            if(n==4)++fixture.scope.pair.right.token;
            invoke_volume(volume_frame,right.scene.entry,n==5?23:22,n==6?.03f:.02f);
            const bool shares=n==1 || n>6;
            require(volume_calls==before+(shares?1:2),"only matching job, producer, scope, command and delta share completed setup");
            scoped=true;invoke_volume(volume_frame,right.scene.entry);
            require(volume_calls==before+(shares?2:3),"duplicate right cannot consume a volume setup twice");
        }
        require(volume.shared()==7,"receipt counts only verified sharing after repeated frames");
        fixture.step(0);
    }
    {
        auto& pass=NativeOrdinarySharpen::get();require(pass.install(reinterpret_cast<void*>(sharpen),[&]()->std::optional<Bloom::Scope>{return scoped?std::optional{fixture.scope}:std::nullopt;},0,true,true),"native sharpening exposure sharing installed");
        unsigned boundaries{};pass.boundary=[&](const auto& scope,uintptr_t source,uint32_t stage){require(scope.scene.dispatch_sequence==57 && source==(stage==9?300:400) && sharpen_calls==(stage==9?0:1),"sharpener captures source before and destination after exact native call");++boundaries;};
        invoke_sharpen(sharpen);require(boundaries==2 && sharpen_seen==std::array<uintptr_t,6>{300,400,500,600,1,0} && sharpen_float==std::array<float,2>{.5f,1.75f},"observation preserves every native pointer, flag and float argument");
        invoke_sharpen(sharpen);scoped=false;invoke_sharpen(sharpen);require(boundaries==2 && sharpen_calls==3,"duplicate and unowned sharpening stay native without new observation");scoped=true;pass.boundary={};
        fixture.step(1);const auto right=fixture.scope;auto left=right;left.scene.slot=0;left.scene.ordinal=1;left.scene.entry=left.pair.left.entry;
        const auto infinity=std::numeric_limits<float>::infinity();
        require(NativeOrdinarySharpen::shared_exposure(left,1.75f,right)==1.75f,"same-job eyes share finite positive exposure");
        for(float invalid:{0.f,-1.f,infinity,std::numeric_limits<float>::quiet_NaN()})require(!NativeOrdinarySharpen::shared_exposure(left,invalid,right),"invalid left exposure cannot propagate");
        auto foreign=right;++foreign.scene.dispatch_sequence;require(!NativeOrdinarySharpen::shared_exposure(left,1.75f,foreign),"another job cannot borrow exposure");
        foreign=right;++foreign.pair.right.token;require(!NativeOrdinarySharpen::shared_exposure(left,1.75f,foreign),"different producer cannot borrow exposure");
        fixture.scope=left;put(fixture.rp(),0x140,left.scene.entry);invoke_sharpen(sharpen);
        fixture.scope=right;put(fixture.rp(),0x140,right.scene.entry);invoke_sharpen(sharpen,infinity);
        require(sharpen_float==std::array<float,2>{.5f,1.75f} && sharpen_seen==std::array<uintptr_t,6>{300,400,500,600,1,0},"actual right hook replaces infinite exposure with same-job left and preserves other arguments");
        std::ostringstream evidence;pass.write(evidence,58);require(evidence.str().find("\"shared\":true")!=std::string::npos && evidence.str().find("\"exposure_bits\":2139095040")!=std::string::npos,"receipt distinguishes original infinite exposure from shared invocation");
        invoke_sharpen(sharpen,infinity);require(std::isinf(sharpen_float[1]),"duplicate right invocation cannot reuse exposure receipt");
        fixture.step(2);invoke_sharpen(sharpen,infinity);require(std::isinf(sharpen_float[1]),"right without matching left remains native");
        for(unsigned n=3;n<15;++n){fixture.step(n);const auto right=fixture.scope;auto left=right;left.scene.slot=0;left.scene.ordinal=1;left.scene.entry=left.pair.left.entry;
            fixture.scope=left;put(fixture.rp(),0x140,left.scene.entry);invoke_sharpen(sharpen,float(n));
            fixture.scope=right;put(fixture.rp(),0x140,right.scene.entry);invoke_sharpen(sharpen,infinity);
            require(sharpen_float[1]==float(n),"continuous sharpener uses this job's exposure after receipt rollover");}
        fixture.step(1);invoke_sharpen(sharpen,infinity);require(std::isinf(sharpen_float[1]),"evicted dispatch cannot be replayed");
        fixture.step(0);
    }
    {
        auto& effect=NativeOrdinaryLateEffect::get();
        require(effect.install(reinterpret_cast<void*>(late_effect),11,[&]()->std::optional<Bloom::Scope>{return scoped?std::optional{fixture.scope}:std::nullopt;}),"late effect native detour installed");
        require(effect.install_early(reinterpret_cast<void*>(early_effect)),"preceding effect native detour installed");
        unsigned observed{};effect.early_input=[&](const auto& scope,uintptr_t source){require(early_calls==0 && source==33 && scope.scene.dispatch_sequence==fixture.scope.scene.dispatch_sequence,"input observer sees original source before native pass");++observed;};
        invoke_early(early_effect,11,fixture.scope.scene.entry);require(early_calls==1 && early_seen==std::array<uintptr_t,5>{11,0,33,44,fixture.scope.scene.entry} && early_delta==1.25f,"owned effect uses native disabled branch once and preserves float and pointer arguments");
        invoke_early(early_effect,11,fixture.scope.scene.entry);require(early_calls==2 && early_seen[1]==1,"duplicate owned effect preserves original enabled flag");
        invoke_early(early_effect,12,fixture.scope.scene.entry);require(early_calls==3 && early_seen[0]==12 && early_seen[1]==1,"foreign owner cannot disable effect");
        require(observed==1,"only authenticated first call emits input observation");effect.early_input={};
        uintptr_t source=77;invoke_late(late_effect,11,&source,fixture.scope.scene.entry);require(late_calls==0 && source==77,"owned right pass skips native swap and leaves caller source intact");
        auto saved=fixture.scope;fixture.scope.scene.slot=0;fixture.scope.scene.ordinal=1;fixture.scope.scene.entry=fixture.scope.pair.left.entry;put(fixture.rp(),0x140,fixture.scope.scene.entry);
        invoke_late(late_effect,11,&source,fixture.scope.scene.entry);require(late_calls==0 && source==77,"same-job left pass uses matching bypass");
        fixture.scope=saved;put(fixture.rp(),0x140,fixture.scope.scene.entry);
        invoke_late(late_effect,12,&source,fixture.scope.scene.entry);require(late_calls==1 && source==123,"unowned effect keeps native pointer swap");
        source=77;scoped=false;invoke_late(late_effect,11,&source,fixture.scope.scene.entry);require(late_calls==2 && source==123,"unscoped effect calls original exactly once");
        scoped=true;effect.continuous(true);const auto before=late_calls;
        for(unsigned n=1;n<12;++n){fixture.step(n);source=77;invoke_late(late_effect,11,&source,fixture.scope.scene.entry);require(late_calls==before && source==77,"continuous bypass survives receipt rollover");}
        fixture.step(0);
    }
    {
        auto& shared=NativeOrdinaryGrading::get();require(shared.install(reinterpret_cast<void*>(grading)),"actual grading read hook installed");
        NativeOrdinaryGrading::Input left{},right{};right.scope=fixture.scope;right.core=11;right.index=1;
        left=right;left.index=0;left.scope.scene.slot=0;left.scope.scene.ordinal=1;left.scope.scene.entry=left.scope.pair.left.entry;left.grading.fill(17);
        std::array<uint8_t,21> original{};original.fill(29);
        left.descriptor={0,0,128,191,1,0};right.descriptor=left.descriptor;
        require(!NativeOrdinaryGrading::fixed_descriptor(false,left) && !NativeOrdinaryGrading::fixed_descriptor(true,std::nullopt),"fixed exposure is opt-in and requires owned scope");
        for(const auto& input:std::array{left,right}){const auto fixed=NativeOrdinaryGrading::fixed_descriptor(true,input);float value{};std::memcpy(&value,fixed->data(),4);require(value==1.0f && (*fixed)[4]==1 && (*fixed)[5]==0 && input.descriptor[3]==191,"both eyes get fixed exposure while effect flags and original descriptor stay intact");}
        shared.invoke(left,[]{});
        shared.invoke(right,[&]{invoke_grading(grading,original.data());require(grading_seen[0]==11 && grading_seen[1]==22 && grading_seen[2]==33 && grading_seen[4]==0,"right read changes only grading and index");require(std::memcmp(reinterpret_cast<void*>(grading_seen[3]),left.grading.data(),21)==0,"exact same-job left grading copied");
            shared.invoke(std::nullopt,[&]{invoke_grading(grading,original.data());require(grading_seen[3]==reinterpret_cast<uintptr_t>(original.data()) && grading_seen[4]==1,"nested unowned read remains untouched");});
            invoke_grading(grading,original.data());require(grading_seen[4]==0,"outer binding restored after nested call");});
        invoke_grading(grading,original.data());require(grading_seen[4]==1 && original[0]==29,"binding and caller data restored after right core");
        ++right.scope.scene.dispatch_sequence;shared.invoke(right,[&]{invoke_grading(grading,original.data());require(grading_seen[4]==1,"other render job cannot borrow grading");});
        --right.scope.scene.dispatch_sequence;right.core=12;require(!NativeOrdinaryGrading::matches(left,right),"foreign core rejected");
        right.core=11;right.index=0;require(!NativeOrdinaryGrading::matches(left,right),"aliased index rejected");
        {   // SECOND_EYE_SIDE_EFFECTS 3 (26 Oct census: grading_index0 1.00 mono / 2.00 stereo). Both eyes
            // step core index 0's eye adaptation: the right eye must start where the left did, the state
            // must advance once per frame, and the census must count one advancing step per frame.
            constexpr auto OFFSET=NativeOrdinaryGrading::ADAPTATION_OFFSET;
            std::array<uint8_t,0x400> core{};const auto at=reinterpret_cast<uintptr_t>(core.data());
            auto l=left,r=right;l.core=r.core=at;l.index=r.index=0;l.scope.scene.dispatch_sequence=r.scope.scene.dispatch_sequence+=100;
            // The native step: the grading call, then the state integrates by one.
            const auto step=[&](float& seen){invoke_grading_at(grading,at,0);std::array<float,4> s{};std::memcpy(s.data(),core.data()+OFFSET,16);seen=s[0];for(auto& v:s)v+=1;put(at,OFFSET,s);};
            const auto state=[&]{float v{};std::memcpy(&v,core.data()+OFFSET,4);return v;};
            put(at,OFFSET,std::array<float,4>{10,10,10,10});
            auto& census=NativeCallCensus::get();census.reset();
            float left_seen{},right_seen{};const auto before=shared.adaptation_stats();
            invoke_grading_at(grading,at,0);census.complete(1); // a mono frame, outside the ordinary pair
            put(at,OFFSET,std::array<float,4>{10,10,10,10});
            shared.invoke(l,[&]{step(left_seen);});shared.invoke(r,[&]{step(right_seen);});census.complete(2);
            require(left_seen==10 && right_seen==10 && state()==11 && shared.adaptation_stats().shared==before.shared+1,"right eye steps from the left eye's starting state; the state advances once");
            const auto rates=census.summary().rates;
            require(rates[size_t(NativeCallCensus::Hook::grading)].stereo==2 && rates[size_t(NativeCallCensus::Hook::grading_index0)].stereo==1 && rates[size_t(NativeCallCensus::Hook::grading_index0)].ratio==1,
                "census: both eyes call grading, one index 0 step advances the state");
            ++l.scope.scene.dispatch_sequence;++r.scope.scene.dispatch_sequence;
            shared.invoke(l,[&]{step(left_seen);});++r.scope.scene.dispatch_sequence;shared.invoke(r,[&]{step(right_seen);});
            require(left_seen==11 && right_seen==12 && state()==13 && shared.adaptation_stats().skipped==before.skipped+1,"another job's left state is never borrowed");
            ++l.scope.scene.dispatch_sequence;r.scope.scene.dispatch_sequence=l.scope.scene.dispatch_sequence;r.core=at+0x198;
            shared.invoke(l,[&]{step(left_seen);});shared.invoke(r,[&]{right_seen=-1;});
            require(left_seen==13 && state()==14 && shared.adaptation_stats().skipped==before.skipped+2,"another core is never written");
            shared.share_adaptation(false);r.core=at;++l.scope.scene.dispatch_sequence;r.scope.scene.dispatch_sequence=l.scope.scene.dispatch_sequence;census.reset();
            invoke_grading_at(grading,at,0);census.complete(1);
            shared.invoke(l,[&]{step(left_seen);});shared.invoke(r,[&]{step(right_seen);});census.complete(2);
            require(left_seen==14 && right_seen==15 && state()==16,"switched off: the old double step");
            require(census.summary().rates[size_t(NativeCallCensus::Hook::grading_index0)].ratio==2,"switched off: the census sees both index 0 steps");
            shared.share_adaptation(true);census.reset();
        }
        std::array<uint8_t,0x400> hdr{};auto observed=left;observed.core=reinterpret_cast<uintptr_t>(hdr.data());++observed.scope.scene.dispatch_sequence;
        std::array<float,8> values{1,2,3,4,5,6,7,8};put(observed.core,0x174,values);
        put(observed.core,0,uint32_t(123));
        shared.invoke(observed,[&]{values[5]=9;put(observed.core,0x174,values);put(observed.core,0,uint32_t(456));});
        std::ostringstream evidence;shared.write(evidence,observed.scope.scene.dispatch_sequence);
        require(evidence.str().find("\"grading_bytes\":[[17,17")!=std::string::npos && evidence.str().find("\"hdr_after\":[[1,2,3,4,5,9,7,8],null]")!=std::string::npos,"grading evidence copies bytes and reads HDR after native preparation");
        require(evidence.str().find("\"core_words_left_before_after_right_before_after\":[[123,")!=std::string::npos && evidence.str().find("],[456,")!=std::string::npos,"complete core snapshots distinguish before and after native mutation");
    }
    const auto good_scope=Bloom::inspect(fixture.scope);require(!good_scope.mask && good_scope.reads==15 && good_scope.count==2 && good_scope.active==fixture.scope.scene.entry,"all actual scope predicate reads recorded");
    put(fixture.lp(),0x29968,int32_t(65));const auto count_scope=Bloom::inspect(fixture.scope);require(count_scope.mask==64 && count_scope.reads==15 && count_scope.count==65,"unbounded native count records its exact failing predicate");
    put(fixture.lp(),0x29968,int32_t(3));require(Bloom::inspect(fixture.scope).mask==1024,"a third ordinary native entry cannot enter the authenticated two-view scope");
    put(fixture.lp(),0x29968,int32_t(8));for(size_t slot=2;slot<8;++slot)put(fixture.lp()+0x168+slot*0xa60,0,uint64_t(0x10000000));
    const auto auxiliary=Bloom::inspect(fixture.scope);require(!auxiliary.mask && auxiliary.count==8 && auxiliary.view_flags.size()==8,"realistic native list with two ordinary and six explicitly excluded auxiliary slots is accepted");
    put(fixture.lp()+0x168+2*0xa60,0,uint64_t(0x40));require(Bloom::inspect(fixture.scope).mask==1024,"PIP entry is never an ignorable auxiliary slot");put(fixture.lp(),0x29968,int32_t(2));
    put(fixture.rp(),0x140,uintptr_t(1));require(Bloom::inspect(fixture.scope).mask==16,"wrong actual active entry has its own diagnostic predicate");put(fixture.rp(),0x140,fixture.scope.scene.entry);
    require(Bloom::matches(NativeRenderProbe::TIMESTAMP,SYNTH(Bloom::SELECTOR_BYTES),SYNTH(Bloom::COLOUR_BYTES),SYNTH(Bloom::TONE_BYTES),SYNTH(Bloom::SELECT_CALL_BYTES),SYNTH(Bloom::COLOUR_CALL_BYTES)),"all shipping selector/consumer/caller bytes exact");
    for(size_t n=0;n<SYNTH(Bloom::SELECTOR_BYTES).size();++n){auto bytes=SYNTH(Bloom::SELECTOR_BYTES);bytes[n]^=1;require(!Bloom::matches(NativeRenderProbe::TIMESTAMP,bytes,SYNTH(Bloom::COLOUR_BYTES),SYNTH(Bloom::TONE_BYTES),SYNTH(Bloom::SELECT_CALL_BYTES),SYNTH(Bloom::COLOUR_CALL_BYTES)),"any selector instruction mutation refuses");}
    auto provider=[&]()->std::optional<Bloom::Scope>{return fixture.unscoped?std::nullopt:std::optional{fixture.scope};};
    require(bloom.install(reinterpret_cast<void*>(selector),reinterpret_cast<void*>(colour),reinterpret_cast<void*>(tone),provider,[&]{return fixture.owned;},[&](uint64_t d)->std::optional<NativeOrdinaryPacket::Receipt>{return d==fixture.packet.dispatch?std::optional{fixture.packet}:std::nullopt;},[&]{++fixture.stopped;}),"real selector/colour/tone detours installed");
    unsigned colour_observed{},suspends{};
    using namespace std::chrono_literals;
    // Zero settle: the level-transition hold's timing is covered in test_ordinary_stereo_recovery.
    bloom.recovery([&]{++suspends;},OrdinaryStereoRecovery::Settings{0ms,0ms,0ms,0ms});
    bloom.colour_input=[&](const auto& scope,uintptr_t source){require(source==6 && scope.scene.entry==fixture.scope.scene.entry && colour_observed==fixture.colour_calls,"outer colour input is observed before native writes");++colour_observed;};
    // Actual contract memory is independent of the shipping caller guard. Test
    // functions never fabricate matching shipping return-address RVAs.
    require(probe.install(reinterpret_cast<void*>(temporal),provider,[&](uintptr_t resource){return fixture.observe(resource);},0,fixture.base),"real temporal detour installed");
    const bool resume=test.starts_with("resume_");
    // suspend_*: a changed native texture contract mid-stream suspends owned stereo.
    const bool suspend_test=test.starts_with("suspend_");std::vector<uint8_t> moved_records;
    // suspend_packet*: the 23 Oct level transition. The tracked packet refused job 2 before any
    // write (refusal 4); job 3 follows as a two-job gap from the last accepted job.
    const bool packet_test=test.starts_with("suspend_packet");
    // suspend_rebuilt: the 24 Oct armoury and the 26 Oct loads (0x120). Job 2 sees the pool rebuilt
    // (banks and records laid out anew at other addresses, for good); the gate retires the pair, job 3
    // is a fresh bootstrap that takes the same pair again (same layout, contract re-based, no factory
    // call) and ends the suspension, and job 4 on uses it. No iteration gap.
    // suspend_rebuilt_flip: the 26 Oct class changes (0x80). The pool flips A -> B -> A -> B (one bank,
    // two banks) at jobs 2, 5 and 8, each held for good: B's renewal creates a second pair at job 3,
    // the flips back reuse A's pair (job 6) and B's (job 9), and the jobs after each use the pair
    // against the re-based contract.
    const bool flip_test=test=="suspend_rebuilt_flip";const bool rebuilt_test=test=="suspend_rebuilt" || flip_test;
    std::vector<uint8_t> rebuilt_bank,rebuilt_records;unsigned renewals{};
    const auto rebuild_job=[&](unsigned n){return rebuilt_test && (n==2 || (flip_test && (n==5 || n==8)));};
    const auto renewed_bootstrap=[&](unsigned n){return n && rebuild_job(n-1);};
    const auto renewed_first_use=[&](unsigned n){return n>1 && rebuild_job(n-2);};
    std::vector<std::array<uintptr_t,2>> pairs; // Natives of each pair made current, in order.
    const auto renew=[&](const native_texture_contract::Snapshot& observed){if(!fixture.owner->renew(observed))return false;fixture.owned.reset();++renewals;return true;};
    if(rebuilt_test)bloom.renewal(renew,0ms);
    // restart_cut: the 24 Oct 20:02 scene change. Job 2 is a tracked camera cut one producer frame
    // late, so it cannot chain to job 1; it starts a fresh chain (a bootstrap keeping the owned pair)
    // instead of stopping, and job 3 uses the owned pair again.
    const bool restart_test=test=="restart_cut";
    // cold_restart (GENERIC_GUARDS, OrdinaryColdRestart): job 2 fails for good (a downstream tone
    // mismatch, an ordinary permanent stop). cold_restart() puts the chain back to its startup
    // state with the pair retired; job 3 is the startup bootstrap that creates a fresh pair, and
    // job 4 on uses it, as jobs 0 and 1 did.
    const bool cold_test=test=="cold_restart";
    if(cold_test)bloom.renewal(renew,0ms);
    const auto bootstrap_job=[&](unsigned n){return !n || renewed_bootstrap(n) || (restart_test && n==2) || (cold_test && n==3);};
    const auto creation_job=[&](unsigned n){return !n || renewed_bootstrap(n) || (cold_test && n==3);};
    const bool continuous=resume || suspend_test || restart_test || cold_test || test=="happy" || test=="continuous_cut" || test=="continuous_cut_skip" || test=="continuous_cut_sdk";
    bloom.continuous(continuous);
    for(unsigned iteration=0;iteration<(continuous?12u:4u);++iteration){
        const unsigned n=iteration+(resume && iteration>=2?9:0)+(suspend_test && !packet_test && !rebuilt_test && iteration>=4?9:0);
        if(n)fixture.step(n,(test=="continuous_cut_skip" && n==2) || (restart_test && n>=2)?1:0);
        const bool cut=packet_test?n==3:rebuilt_test?false:restart_test?n==2:resume?iteration==2:suspend_test?iteration==4:continuous && test!="happy" && (n==2 || n==6);
        if(cut)fixture.packet.continued=false;
        const auto bank_variant=reinterpret_cast<uintptr_t>(fixture.bank.data())+0xc;
        const bool injected=test=="suspend_injected";
        // 21 Oct Neurothrope: the native pool moved its record table (difference 0x100).
        const bool moved_test=test=="suspend_records_moved" || test=="suspend_records_injected";
        if(test=="suspend_records_injected" && n==2)bloom.inject_record_moves(std::chrono::hours(1));
        if(test=="suspend_records_moved" && n==2){moved_records.assign(fixture.records.begin(),fixture.records.end());
            put(fixture.creation.contract.pool,0x2260,reinterpret_cast<uintptr_t>(moved_records.data()));put(fixture.creation.contract.pool,0x2268,int32_t(3));}
        if(rebuilt_test && n==2){const auto pool=fixture.creation.contract.pool;uintptr_t bank{},records{};native_texture_contract::read(pool+0x2248,bank);native_texture_contract::read(pool+0x2260,records);
            rebuilt_bank.assign(reinterpret_cast<const uint8_t*>(bank),reinterpret_cast<const uint8_t*>(bank)+fixture.bank.size()*sizeof(fixture.bank[0]));
            rebuilt_records.assign(reinterpret_cast<const uint8_t*>(records),reinterpret_cast<const uint8_t*>(records)+fixture.records.size()*sizeof(fixture.records[0]));
            put(pool,0x2248,reinterpret_cast<uintptr_t>(rebuilt_bank.data()));put(pool,0x2260,reinterpret_cast<uintptr_t>(rebuilt_records.data()));}
        // Flip: job 2 moves the pool and adds a bank (0x1a0, as at 14:52:47), jobs 5 and 8 only flip the bank count (0x80).
        if(flip_test && rebuild_job(n))put(fixture.creation.contract.pool,0x2250,int32_t(n==5?1:2));
        if(suspend_test && !injected && !moved_test && !packet_test && !rebuilt_test && n==2)put(bank_variant,0,int32_t(1));
        if(injected && n==2)bloom.inject_suspensions(std::chrono::hours(1));
        if(suspend_test && n==3)put(bank_variant,0,int32_t(0));
        if(test=="suspend_refused" && n==13)put(bank_variant,0,int32_t(1));
        const bool suspended_job=rebuilt_test?rebuild_job(n):packet_test?n==2:suspend_test && (n==2 || n==3 || (test=="suspend_refused" && n==13));
        if(resume || suspend_test || restart_test){fixture.packet.tracked=NativeOrdinaryPacket::TrackedSource{};fixture.packet.left_patched=NativeCameraPacket{};fixture.packet.patched=NativeCameraPacket{};}
        if(cut && test=="resume_no_cut")fixture.packet.continued=true;
        if(cut && test=="resume_untracked")fixture.packet.tracked.reset();
        // A refused tracked receipt: nothing applied or patched. 4 is recoverable; a refused write (7) never is.
        const unsigned packet_job=test=="suspend_packet_first"?0:2;
        if(packet_test && n==packet_job){auto& p=fixture.packet;p.refusal=test=="suspend_packet_write"?7:4;p.recoverable=p.refusal==4;p.rollback=p.refusal==7;
            p.reference=p.applied=p.source_unchanged=false;p.left_patched.reset();p.patched.reset();}
        if(test=="auxiliary"){put(fixture.lp(),0x29968,int32_t(8));for(size_t slot=2;slot<8;++slot)put(fixture.lp()+0x168+slot*0xa60,0,uint64_t(0x10000000));}
        if(n==1){
            if(test=="growth")put(fixture.creation.contract.pool,0x2268,int32_t(3));
            if(test=="target")put(reinterpret_cast<uintptr_t>(fixture.records.data()),0xc,std::array<uint32_t,6>{16,4,1,1,38,1});
            if(test=="pool")put(fixture.creation.contract.pool,0x22d0,native_pool_membership::Header{1,0,0});
            if(test=="variant")put(reinterpret_cast<uintptr_t>(fixture.bank.data()),0xc,int32_t(1));
            if(test=="resource")put(fixture.owned->native[0],0x278,reinterpret_cast<uintptr_t>(fixture.creation.source_resource.Get()));
            if(test=="cut")fixture.packet.continued=false;
            if(test=="skip")++fixture.scope.pair.left.render_id;
            if(test=="scope")put(fixture.rp(),0x140,uintptr_t(1));
        }
        const auto before=fixture.acquired;const auto destination=invoke_selector(selector,fixture.rp());const auto record=bloom.record(57+n);
        if(packet_test && n==packet_job){
            if(test=="suspend_packet"){
                require(record && record->suspended && !record->selected && record->refusal==Bloom::Refusal::packet && destination==fixture.native_target() && fixture.acquired==before+1 && !fixture.stopped && suspends==1 && bloom.suspended(),
                    "a recoverable packet refusal keeps the job native and suspends instead of stopping");
                require(bloom.recovery_status().policy.transition && bloom.recovery_status().policy.transition_episodes==1,"a level-transition packet refusal starts a transition hold");
                unsigned cutscene_reads{};bloom.cutscene_source([&]{++cutscene_reads;return true;});
                require(bloom.resume_gate() && bloom.recovery_status().policy.attempting && !bloom.recovery_status().policy.exhausted,"the unchanged contract opens the gate for a fresh attempt");
                require(cutscene_reads==1 && bloom.recovery_status().policy.cutscene_holds==1,"the gate reads the game's cutscene flag into the hold");bloom.cutscene_source({});
            }
            else {require(record && record->refusal==Bloom::Refusal::packet && !record->suspended && destination==fixture.native_target() && fixture.stopped && bloom.recovery_status().policy.exhausted && !suspends,
                test=="suspend_packet_write"?"a refused packet write still stops owned stereo":"a recoverable refusal before any accepted job has nothing to resume from and stops");break;}
        }
        if(suspend_test && !packet_test && (n==2 || rebuild_job(n))){
            if(rebuilt_test){
                using namespace native_texture_contract;
                const unsigned renewal=n==2?1:n==5?2:3;const uint32_t expected=!flip_test?banks_moved|records_moved:n==2?banks_moved|records_moved|bank_count_changed:bank_count_changed;
                require(record && record->suspended && !record->selected && record->refusal==Bloom::Refusal::contract && record->contract_difference==expected && rebuilt(record->contract_difference) &&
                    destination==fixture.native_target() && !fixture.stopped && bloom.suspended(),"a rebuilt pool suspends owned stereo");
                const bool gate=bloom.resume_gate();const auto status=bloom.recovery_status();
                // Only B's first appearance is a new layout: the 0x120 rebuild and every flip back expect a retired pair.
                const bool reuse=!flip_test || renewal>1;
                require(gate && renewals==renewal && status.renewals==renewal && status.renewing && status.renewed_difference==expected && !status.rebases && status.policy.attempting &&
                    fixture.owner->generation()==(renewal==1?1u:2u) && fixture.owner->renewal().reused==reuse && fixture.owner->renewal().generation==(flip_test?2u:1u) && !fixture.owned,
                    "a lasting rebuild retires the owned pair and admits a bootstrap; only a new layout expects a new pair");
            }
            else if(moved_test){
                const bool real_move=test=="suspend_records_moved";
                const auto table=reinterpret_cast<uintptr_t>(real_move?moved_records.data():fixture.records.data());const int32_t count=real_move?3:2;
                require(record && record->suspended && !record->selected && record->contract_failure==Bloom::ContractFailure::shape && record->contract_difference==native_texture_contract::records_moved &&
                    record->contract && record->contract->records==table && destination==fixture.native_target() && !fixture.stopped,"a moved record table suspends first, with the new address in the receipt");
                const bool gate=bloom.resume_gate();const auto status=bloom.recovery_status();
                require(gate && status.rebases==1 && !status.probe_difference && status.rebased_to==table && status.rebased_count_to==count && status.policy.attempting && (real_move || status.rebased_from==table+0x10000),
                    "the probe re-bases a pure table move and opens the gate");
            }
            else if(injected){
                require(record && record->suspended && !record->selected && record->refusal==Bloom::Refusal::contract && record->contract_failure==Bloom::ContractFailure::injected && !record->contract_difference &&
                    !record->identity && destination==fixture.native_target() && fixture.acquired==before+1 && !fixture.stopped && suspends==1,"injected refusal of a valid contract takes the same native suspension");
                require(bloom.resume_gate() && bloom.recovery_status().policy.attempting && !bloom.recovery_status().probe_difference,"matching contract opens the gate immediately (zero backoff)");
            }
            else {require(record && record->suspended && !record->selected && record->refusal==Bloom::Refusal::contract && record->contract_failure==Bloom::ContractFailure::shape &&
                record->contract_difference==native_texture_contract::variant_changed && record->contract && record->contract->bank_variant==1,"receipt names the changed contract field and keeps the refused observation");
            require(destination==fixture.native_target() && fixture.acquired==before+1 && !fixture.stopped && suspends==1 && bloom.suspended(),"changed contract suspends owned stereo: one native acquire, no stop");
            require(!bloom.resume_gate() && bloom.recovery_status().probe_difference==native_texture_contract::variant_changed && !bloom.recovery_status().policy.attempting,"read-only probe keeps the producer native while the contract still differs");}
        }
        if(suspend_test && !packet_test && !rebuilt_test && n==3)require(record && record->suspended && !record->selected && record->refusal==Bloom::Refusal::continuity && destination==fixture.native_target() && !fixture.stopped,"in-flight adjacent job stays native even with a matching contract");
        if(test=="suspend_refused" && n==13){const auto status=bloom.recovery_status();
            require(record && record->suspended && destination==fixture.native_target() && !fixture.stopped && status.policy.refused_attempts==1 && status.policy.suspended && suspends==3,"refused resume attempt re-suspends with backoff instead of stopping");break;}
        if(suspend_test && n==13)require(record && record->selected && record->resumed && record->reset && record->previous_dispatch==58 && fixture.acquired==before,"gap job after the gate resumes from the last ACCEPTED job with a history reset");
        if(cut && (test=="resume_no_cut" || test=="resume_untracked")){
            require(record && record->refusal==Bloom::Refusal::continuity && fixture.stopped,"gap without tracked two-eye reset remains fatal");break;
        }
        if(cut && test=="continuous_cut_skip"){
            require(record && record->refusal==Bloom::Refusal::continuity && fixture.stopped && destination==fixture.native_target(),"camera cut cannot hide a missing producer frame");break;
        }
        if(n==1 && (test=="pool" || test=="target" || test=="variant" || test=="resource" || test=="cut" || test=="skip" || test=="scope")){
            require(fixture.acquired==before+1 && destination==fixture.native_target() && record && !record->selected && record->refusal!=Bloom::Refusal::none && fixture.stopped,"invalid owned selection stops appends and transparently preserves one original acquire");
            const auto acquired=fixture.acquired;invoke_selector(selector,fixture.rp());require(fixture.acquired==acquired+1,"poisoned experiment remains transparent without retrying owned selection");break;
        }
        require(record && (!bootstrap_job(n) && !suspended_job?fixture.acquired==before:fixture.acquired==before+1),"owned selection bypasses original acquire; unchanged bootstrap acquires once");
        put(fixture.rp(),0x1b8,destination);uintptr_t previous{};native_texture_contract::read(fixture.rp()+0x160,previous);
        if(n==2 && test=="previous"){previous=fixture.native_target();put(fixture.rp(),0x160,previous);}
        const std::array<uintptr_t,7> arguments{fixture.rp(),reinterpret_cast<uintptr_t>(fixture.even.data()),destination,previous,fixture.rp()+0x10,reinterpret_cast<uintptr_t>(fixture.even.data()),2};
        if(restart_test && n==2){const auto r=bloom.record(57+n);
            require(r && r->bootstrap && r->restart && !r->selected && r->refusal==Bloom::Refusal::none && destination==fixture.native_target() && !fixture.stopped && bloom.recovery_status().restarts==1 && fixture.owned,
                "a tracked cut that cannot chain starts a fresh chain with the owned pair kept, instead of stopping");}
        if(creation_job(n)){std::array<uintptr_t,9> wrappers{arguments[1],arguments[2],arguments[3],arguments[5]};fixture.creation.contract=native_texture_contract::capture(fixture.base,wrappers,2,fixture.scope.scene.render_counter_before);fixture.creation.run(*fixture.owner);fixture.owned=fixture.owner->owned_pair();require(fixture.owned.has_value(),"bootstrap actually creates and retains two disjoint GPU textures");
            pairs.push_back(fixture.owned->native);require(fixture.owned->contract==fixture.creation.contract,"the bootstrap's pair carries this pool's contract, re-based when reused");}
        invoke_temporal(temporal,arguments);
        require(fixture.temporal_seen[0]==arguments[0] && fixture.temporal_seen[1]==arguments[1] && fixture.temporal_seen[2]==destination && fixture.temporal_seen[4]==arguments[4] && fixture.temporal_seen[5]==arguments[5] && fixture.temporal_seen[6]==2,"all other native temporal arguments preserved");
        if(n==1 || renewed_first_use(n) || (restart_test && n==3) || (cold_test && n==4))require(fixture.temporal_seen[3]==0 && previous!=0,"first actual owned filter resets previous argument without editing its release-bank word");
        if(cut && !restart_test)require(record->reset && record->selected && fixture.temporal_seen[3]==0 && previous!=0,"continuous camera cut resets only sampled history while retaining owned output");
        uintptr_t unchanged{};native_texture_contract::read(fixture.rp()+0x160,unchanged);require(unchanged==previous,"native caller retains its original previous-bank value for legitimate pool release");
        if(n==2 && test=="previous"){require(fixture.temporal_seen[3]==0 && fixture.stopped,"wrong predecessor is reset and poisons future appends");break;}
        if(n==1 && test=="duplicate"){invoke_temporal(temporal,arguments);require(fixture.stopped,"duplicate temporal invocation cannot reuse an accepted receipt");break;}
        fixture.bad_tone=(n==1 && test=="tone") || (cold_test && n==2);
        invoke_colour(colour,fixture.scope.scene.entry,n==1 && test=="colour"?0:destination);
        require(colour_observed==fixture.colour_calls,"every authenticated outer colour call emits one observation");
        require(fixture.colour_seen==std::array<uintptr_t,8>{1,2,fixture.scope.scene.entry,5,6,n==1 && test=="colour"?0:destination,8,9} && fixture.colour_float==1.25f,"all nine native colour arguments including floating register preserved");
        require(fixture.tone_seen==std::array<uintptr_t,9>{11,22,33,fixture.bad_tone || (n==1 && test=="colour")?0:destination,55,0x3f800000,1,88,99},"all nine native tone arguments including raw float bits preserved");
        if(n==1 && (test=="tone" || test=="colour")){require(fixture.stopped && bloom.record(58)->refusal==Bloom::Refusal::colour,"actual downstream argument mismatch stops future experimental appends");break;}
        if(cold_test && n==2){
            require(fixture.stopped && bloom.stopped() && bloom.record(59)->refusal==Bloom::Refusal::colour && bloom.recovery_status().policy.exhausted,"a permanent refusal stops owned stereo");
            const auto generation=fixture.owner->generation();
            require(bloom.cold_restart() && !bloom.stopped() && !bloom.suspended() && renewals==1 && !fixture.owned && fixture.owner->generation()==generation && fixture.owner->renewal().reused && bloom.cold_restarts()==1,
                "a cold restart retires the owned pair (the pool's layout unchanged: it is due again) and lifts the stop");
            const auto status=bloom.recovery_status();
            require(!bloom.record(59) && !status.last_accepted && !status.policy.exhausted && !status.policy.episodes,"the chain and the recovery policy start over");
            fixture.stopped=0; // OrdinaryColdRestart restarts the producer too (NativeTrackedOrdinaryCamera::restart)
            continue;}
        fixture.packet.accepted=true;fixture.packet.sdk=NativeOrdinaryPacket::Sdk{1,100+n,31+n};if(n==1 && test=="sdk")fixture.packet.sdk->epoch++;
        if(packet_test && n==packet_job){fixture.packet.accepted=false;fixture.packet.sdk.reset();} // A refused receipt never completes.
        if(cut && test=="continuous_cut_sdk")fixture.packet.sdk->epoch++;
        if(cut && test=="resume_sdk")fixture.packet.sdk->epoch++;
        if(cut && test=="resume_token")fixture.packet.sdk->token--;
        for(size_t slot=0;slot<2;++slot){auto& scene=fixture.snapshot.scenes[slot];scene.temporal_after=NativeRenderProbe::Scene::Temporal{};scene.temporal_after->next[2]=slot?destination:fixture.native_target();
            put(fixture.rp(),0x200+slot*0x58,scene.temporal_after->next);scene.persistent_after=NativeRenderProbe::read_history_bank(fixture.rp(),uint32_t(slot));}
        if(n==1 && test=="export")fixture.snapshot.scenes[1].persistent_after->words[2]=fixture.native_target();
        bloom.complete(fixture.snapshot);const auto accepted=bloom.record(57+n);
        if(cut && (test=="resume_sdk" || test=="resume_token")){
            require(!accepted->accepted && accepted->refusal==Bloom::Refusal::continuity && fixture.stopped,"gap reset must verify same SDK generation and exact elapsed tokens");break;
        }
        if(cut && test=="continuous_cut_sdk"){
            require(!accepted->accepted && accepted->refusal==Bloom::Refusal::continuity && fixture.stopped,"camera cut cannot hide a foreign SDK generation");break;
        }
        if(n==1 && (test=="export" || test=="sdk")){require(!accepted->accepted && fixture.stopped,"wrong exported bank or SDK generation refuses completion");break;}
        if(suspended_job){require(!accepted->accepted && accepted->suspended && !fixture.stopped,"suspended job is never accepted, never a predecessor and never stops");
            if(n==3 && (injected || moved_test))require(bloom.recovery_status().policy.attempts==1 && !bloom.recovery_status().policy.refused_attempts && bloom.resume_gate() && bloom.recovery_status().policy.attempting,"in-flight adjacent job neither spends nor refuses the granted attempt");
            else if(n==3)require(bloom.recovery_status().policy.attempts==0 && bloom.resume_gate() && bloom.recovery_status().policy.attempting,"restored contract opens the producer gate for one attempt");
            continue;}
        require(accepted->accepted && accepted->temporal && accepted->colour && accepted->tone && accepted->bootstrap==bootstrap_job(n) && accepted->selected==!bootstrap_job(n) && !fixture.stopped,"actual selector/filter/colour/tone/native export joins accepted own-camera job");
        if(moved_test && n==14)require(accepted->accepted && accepted->selected && bloom.recovery_status().rebases==1 && !accepted->contract_difference,"later jobs keep using the owned textures under the re-based contract");
        if(suspend_test && n==13)require(accepted->recovered && !bloom.suspended() && bloom.recovery_status().policy.recoveries==1 && bloom.resume_gate(),"accepted resumed job ends the suspension");
        if(packet_test && n==3)require(accepted->resumed && accepted->reset && accepted->recovered && accepted->previous_dispatch==58 && !bloom.suspended() && bloom.recovery_status().policy.recoveries==1 && !bloom.recovery_status().policy.exhausted && bloom.resume_gate(),
            "the next tracked job resumes from the last accepted job with a history reset and ends the suspension");
        if(renewed_bootstrap(n)){const unsigned renewal=n==3?1:n==6?2:3;
            require(accepted->recovered && !bloom.suspended() && !bloom.recovery_status().renewing && bloom.recovery_status().policy.recoveries==renewal && fixture.owned && fixture.owned->origin.dispatch==57+n,
                "the renewed bootstrap takes its pair and ends the suspension");
            // suspend_rebuilt: the same pair (address-only rebuild). Flip: B new at job 3, A's at job 6, B's at job 9.
            const auto expected=!flip_test?pairs[0]:n==3?pairs.back():n==6?pairs[0]:pairs[1];
            require(fixture.owned->native==expected && (n!=3 || !flip_test || (pairs[1][0]!=pairs[0][0] && pairs[1][1]!=pairs[0][1])) && fixture.owner->generation()==(flip_test?2u:1u) &&
                fixture.owner->reuses()==(flip_test?renewal-1:1u) && fixture.owner->retired()+1==fixture.owner->generation() && fixture.creation.factory_calls==2*fixture.owner->generation(),
                "a layout seen before takes its retired pair with no factory call; only a new layout creates one");}
        if(renewed_first_use(n) || (n>2 && rebuild_job(n-3)))require(bloom.record(57+n)->selected && bloom.record(57+n)->destination==fixture.owned->native[(fixture.scope.scene.render_counter_before)&1] && !bloom.record(57+n)->contract_difference,
            "later jobs use the pair against the rebuilt pool's re-based contract");
        if(restart_test && n==3)require(accepted->selected && accepted->reset && accepted->destination==fixture.owned->native[fixture.scope.scene.render_counter_before&1] && !fixture.stopped,"the job after the restart uses the owned pair with a history reset");
        if(cold_test && n==3)require(accepted->bootstrap && !accepted->selected && fixture.owned && fixture.owned->origin.dispatch==57+n && !fixture.stopped && fixture.owned->native==pairs[0] && fixture.owner->generation()==1 && fixture.owner->reuses()==1,
            "the startup bootstrap after a cold restart takes the pair again (same layout, re-checked, no new textures)");
        if(cold_test && n==5)require(bloom.record(57+n)->selected && bloom.record(57+n)->destination==fixture.owned->native[(fixture.scope.scene.render_counter_before)&1],"later jobs use the fresh pair");
        if(n>1 && !cut && !renewed_bootstrap(n) && !renewed_first_use(n) &&!(restart_test && n==3) && !(cold_test && n<=4))require(fixture.temporal_seen[3]==bloom.record(56+n)->destination && fixture.temporal_seen[3]!=destination,"later actual filter uses the preceding accepted own-right destination");
        const auto sample=probe.sample(57+n);std::ostringstream temporal_json;NativeTemporalProbe::write(temporal_json,sample);
        require(temporal_json.str().find(!bootstrap_job(n)?"ordinary_native_temporal_calls_v2":"ordinary_native_temporal_calls_v1")!=std::string::npos,"only actual owned invocation produces explicit v2 observation/invocation schema");
        if(n==1)require(temporal_json.str().find("\"invoked_arguments\"")!=std::string::npos && sample.events.back().arguments[3]==previous && sample.events.back().invoked_arguments[3]==0,"first-use reset never relabels the observed shared predecessor");
    }
    fixture.unscoped=true;const auto calls=fixture.acquired;invoke_selector(selector,fixture.rp());require(fixture.acquired==calls+1,"unscoped AFR keeps its original selector");
    std::ostringstream receipt;bloom.write(receipt,57);require(receipt.str().find("\"instruction_guards\":false")!=std::string::npos && receipt.str().find("\"native_gpu_histories_verified\":false")!=std::string::npos,"fixture never fabricates shipping guards or full GPU history proof");
    std::cout<<"ordinary owned bloom "<<test<<": real hooks/native ABI, actual owned GPU resources and bounded acceptance PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
