#include "synthetic_code.h"
#include "EnginePlayerOpacity.h"
#include "EngineBodyProbe.h"
#include "EnginePlayerVisibilityProbe.h"
#include "EngineArmDriver.h"
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <limits>
namespace {
void require(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
uint64_t calls{};void* last_object{};float last_value{};
__declspec(noinline) void native_opacity(void* object,float value) { ++calls;last_object=object;last_value=value; }
uint64_t render_calls{};void* last_instance{};float render_value=0.35f;
__declspec(noinline) float native_render_opacity(void* instance) { ++render_calls;last_instance=instance;return render_value; }
uint64_t filter_calls{},filter_result=0x123456789abcde01;void* filter_views{},*filter_instance{};
uint32_t filter_frame{};float filter_time{};uint8_t filter_mode{};
__declspec(noinline) uint64_t native_filter(void* views,uint32_t frame,float time,void* instance,uint64_t* mask,uint8_t mode) {
    ++filter_calls;filter_views=views;filter_frame=frame;filter_time=time;filter_instance=instance;filter_mode=mode;
    if(mask) *mask&=~uint64_t(2);return filter_result;
}
uint64_t draw_calls{},draw_handle{};void* draw_list{},*draw_skeleton{};uint32_t draw_index{},draw_flags{};
uint8_t draw_hashed{};std::array<uint8_t*,7> draw_options{};
__declspec(noinline) void native_draw(void* list,uint64_t handle,uint32_t index,uint32_t flags,void* skeleton,uint8_t* p6,uint8_t* p7,uint8_t* p8,uint8_t* p9,uint8_t* p10,uint8_t* p11,uint8_t* p12) {
    ++draw_calls;draw_list=list;draw_handle=handle;draw_index=index;draw_flags=flags;draw_skeleton=skeleton;
    draw_options={p6,p7,p8,p9,p10,p11,p12};draw_hashed=p10?p10[1]:255;
    for(auto option:draw_options) if(option) option[0]=0;
}
template<class T,size_t N> void put(std::array<uint8_t,N>& object,size_t offset,T value) { std::memcpy(object.data()+offset,&value,sizeof(value)); }
uint64_t params_calls{},params_handle{};uint32_t params_mode=1;
float params_offset=0,params_scale=1,params_padding=1;
std::array<void*,3> params_args{};
__declspec(noinline) void native_params(uint32_t* output,uint64_t handle,void* material,void* runtime,void* resource) {
    ++params_calls;params_handle=handle;params_args={material,runtime,resource};
    for(unsigned i=0;i<28;++i)output[i]=0xabcd0000u+i;
    output[13]=0x74|params_mode;
    std::memcpy(output+8,&params_scale,4);std::memcpy(output+9,&params_offset,4);std::memcpy(output+10,&params_padding,4);
}
struct Actor {
    std::array<uint8_t,0x200> actor{},instance{};
    std::array<int16_t,4> mesh_indices{};std::array<uint8_t,0x40> mesh{};
    Actor() { put(actor,0xd8,reinterpret_cast<uintptr_t>(instance.data()));put(actor,0x20,uintptr_t(789));put(actor,0x38,uintptr_t(456));put(instance,0x58,uintptr_t(789));
        put(instance,0xa0,reinterpret_cast<uintptr_t>(mesh_indices.data()));put(instance,0xb0,reinterpret_cast<uintptr_t>(mesh.data()));put(mesh,0,uintptr_t(123)); }
};
struct RefreshFixture {Actor* actor;std::array<uint32_t,28>* output;uint64_t calls{},builds{};};
__declspec(noinline) void native_refresh(void* context,uint64_t handle) {
    auto& f=*static_cast<RefreshFixture*>(context);++f.calls;
    uint32_t flags{};std::memcpy(&flags,f.actor->mesh.data()+0x20,4);
    if(!(flags&4)) {++f.builds;native_params(f.output->data(),handle,nullptr,nullptr,nullptr);flags|=4;put(f.actor->mesh,0x20,flags);}
}
}
int main() {
    try {
        Actor player,npc;auto selected=reinterpret_cast<uintptr_t>(player.actor.data());
        auto& visibility=EnginePlayerVisibilityProbe::get();
        require(visibility.install(reinterpret_cast<void*>(&native_filter),[&](auto){return selected;}),"install actual native view-mask filter detour");
        uint64_t(*volatile filter)(void*,uint32_t,float,void*,uint64_t*,uint8_t)=native_filter;
        uint64_t mask=7;void* views=reinterpret_cast<void*>(123);
        require(filter(views,456,0.125f,player.instance.data(),&mask,9)==filter_result && mask==5 && filter_calls==1,"passive filter preserves full return, exact native mask side effect and original count");
        require(filter_views==views && filter_frame==456 && filter_time==0.125f && filter_instance==player.instance.data() && filter_mode==9,"filter preserves integer, XMM2 float, R9 instance and stack arguments");
        require(visibility.snapshot().owned_calls==1 && visibility.snapshot().mask_removed==1 && visibility.snapshot().last_before==7 && visibility.snapshot().last_after==5,"owned filter diagnostics capture removed view bits");
        filter_result=0xdeadbeef00000000;mask=2;filter(views,1,0.25f,player.instance.data(),&mask,0);
        require(mask==0 && visibility.snapshot().rejected==1,"diagnostic rejection uses low return byte and never repairs native mask");
        mask=7;filter(views,1,0,npc.instance.data(),&mask,0);
        selected=0;filter(views,1,0,player.instance.data(),&mask,0);
        selected=reinterpret_cast<uintptr_t>(player.actor.data());put(player.instance,0x58,uintptr_t(111));filter(views,1,0,player.instance.data(),&mask,0);
        require(visibility.snapshot().owned_calls==2 && filter_calls==5,"NPC, stale ownership and property mismatch still forward but never count as player");
        put(player.instance,0x58,uintptr_t(789));filter(views,1,0,player.instance.data(),nullptr,0);
        require(visibility.snapshot().owned_calls==2 && filter_calls==6,"null native mask passes through without unsafe diagnostic read");
        selected=reinterpret_cast<uintptr_t>(npc.actor.data());visibility.refresh_owner();mask=7;
        filter(views,1,0,npc.instance.data(),&mask,0);
        require(visibility.snapshot().owned_calls==3,"fresh camera owner replaces candidate without nearest-actor inference");
        visibility.uninstall();mask=7;filter(views,1,0,npc.instance.data(),&mask,0);
        require(mask==5 && filter_calls==8 && visibility.snapshot().owned_calls==3,"filter teardown restores original function");
        selected=reinterpret_cast<uintptr_t>(player.actor.data());
        auto filter_guard=synthetic_at<25>(0x1e798d0);
        require(EnginePlayerVisibilityProbe::matches_build(EngineBodyProbe::BUILD_TIMESTAMP,filter_guard),"verified view-filter prologue accepted");filter_guard[7]^=1;
        require(!EnginePlayerVisibilityProbe::matches_build(EngineBodyProbe::BUILD_TIMESTAMP,filter_guard) && !EnginePlayerVisibilityProbe::matches_build(1,filter_guard),"wrong native view-filter ABI/build rejected");
        auto& opacity=EnginePlayerOpacity::get();
        require(opacity.install(reinterpret_cast<void*>(&native_opacity),[&](auto){return selected;},reinterpret_cast<void*>(&native_render_opacity),reinterpret_cast<void*>(&native_draw),reinterpret_cast<void*>(&native_params),reinterpret_cast<void*>(&native_refresh)),"install actual float setter, getter, mesh draw, GPU parameter and refresh detours");
        void(*volatile set)(void*,float)=native_opacity;
        float(*volatile get)(void*)=native_render_opacity;
        require(get(player.instance.data())==0.35f && render_calls==1 && last_instance==player.instance.data(),"disabled renderer getter preserves native float return and instance argument exactly once");
        set(player.actor.data(),0.25f);require(calls==1 && last_object==player.actor.data() && last_value==0.25f,"disabled hook preserves XMM1 ABI and original exactly once");
        opacity.enable(true);set(player.actor.data(),0.5f);require(last_value==0.5f,"VR disallowed stays native");
        opacity.allow(true);set(player.actor.data(),0.2f);require(last_value==1 && opacity.snapshot().overrides==1,"owned player override goes through original setter");
        require(get(player.instance.data())==1 && opacity.snapshot().read_overrides==1,"owned render getter overrides distance opacity without native property writes");
        require(opacity.snapshot().native_faded_reads==2 && opacity.snapshot().changed_reads==1 && opacity.snapshot().render_min_native==0.35f,"diagnostics distinguish observed native fading from already-opaque remappings");
        require(get(npc.instance.data())==0.35f,"NPC renderer opacity remains native");
        set(npc.actor.data(),0.3f);require(last_object==npc.actor.data() && last_value==0.3f,"NPC never overridden");
        selected=0;set(player.actor.data(),0.4f);require(last_value==0.4f,"lost/stale authoritative actor immediately forwards native value");
        require(get(player.instance.data())==0.35f,"lost ownership restores renderer opacity immediately");
        selected=reinterpret_cast<uintptr_t>(player.actor.data());put(player.instance,0x58,uintptr_t(111));set(player.actor.data(),0.6f);require(last_value==0.6f,"geometry ownership mismatch rejects override");
        require(get(player.instance.data())==0.35f,"renderer getter validates geometry property ownership");
        put(player.instance,0x58,uintptr_t(789));
        for(float invalid:{-1.0f,2.0f,std::numeric_limits<float>::infinity()}) { set(player.actor.data(),invalid);require(last_value==invalid,"unexpected native value passes through without reinterpretation"); }
        set(player.actor.data(),std::numeric_limits<float>::quiet_NaN());require(std::isnan(last_value),"native NaN passes through");
        opacity.enable(false);set(player.actor.data(),0.7f);require(last_value==0.7f,"toggle restores current native value rather than stale cached opacity");
        render_value=0.6f;require(get(player.instance.data())==0.6f,"renderer toggle off returns newest native distance opacity");
        opacity.enable(true);opacity.allow(false);set(player.actor.data(),0.8f);require(last_value==0.8f,"VR off releases native opacity");
        require(get(player.instance.data())==0.6f,"VR off restores renderer getter");
        opacity.allow(true);render_value=NAN;require(std::isnan(get(player.instance.data())),"malformed native renderer value passes through");render_value=0.35f;
        require(render_calls==8 && opacity.snapshot().reads==8,"every renderer read calls original exactly once");
        require(calls==12 && opacity.snapshot().invalid==4,"every native call forwarded once and invalid inputs counted");
        using Draw=void(*)(void*,uint64_t,uint32_t,uint32_t,void*,uint8_t*,uint8_t*,uint8_t*,uint8_t*,uint8_t*,uint8_t*,uint8_t*);
        Draw volatile draw=native_draw;
        std::array<std::array<uint8_t,16>,7> options{};
        const auto owned_handle=(uint64_t(3)<<48) | (reinterpret_cast<uintptr_t>(player.instance.data())+0x70);
        auto emit=[&](uint64_t handle) {
            for(auto& option:options) {option.fill(17);option[0]=1;option[1]=1;}
            draw(views,handle,123,456,reinterpret_cast<void*>(789),options[0].data(),options[1].data(),options[2].data(),options[3].data(),options[4].data(),options[5].data(),options[6].data());
        };
        emit(owned_handle);
        require(draw_hashed==0 && draw_calls==1 && opacity.draw_snapshot().bypassed==1,"owned enabled draw sees bypass switch exactly once");
        require(draw_list==views && draw_handle==owned_handle && draw_index==123 && draw_flags==456 && draw_skeleton==reinterpret_cast<void*>(789),"mesh draw register and stack arguments preserved");
        for(size_t i=0;i<7;++i) {
            require(draw_options[i]==options[i].data() && options[i][0]==0 && options[i][1]==1 && options[i][15]==17,"native presence consumption preserved, temporary value restored and unrelated option bytes untouched");
        }
        emit(reinterpret_cast<uintptr_t>(npc.instance.data())+0x70);require(draw_hashed==1,"NPC draw remains native even with matching property values");
        selected=0;emit(owned_handle);require(draw_hashed==1,"stale owned draw remains native");selected=reinterpret_cast<uintptr_t>(player.actor.data());
        put(player.instance,0x58,uintptr_t(111));emit(owned_handle);require(draw_hashed==1,"draw property mismatch fails closed");put(player.instance,0x58,uintptr_t(789));
        emit(owned_handle | (uint64_t(0xffff)<<48));require(draw_hashed==1,"invalid negative mesh handle stays native");
        opacity.enable(false);emit(owned_handle);require(draw_hashed==1,"draw toggle restores current native switch");
        opacity.enable(true);opacity.allow(false);emit(owned_handle);require(draw_hashed==1,"VR disabled restores native draw switch");
        opacity.allow(true);
        options[4][0]=1;options[4][1]=7;
        draw(views,owned_handle,123,456,nullptr,nullptr,nullptr,nullptr,nullptr,options[4].data(),nullptr,nullptr);
        require(draw_hashed==7 && options[4][1]==7 && draw_calls==8 && opacity.draw_snapshot().failures==0,"unexpected option value passes through and every draw calls original once");
        auto draw_guard=synthetic_at<24>(0x1d8b0f0);
        require(EnginePlayerOpacity::matches_draw(EngineBodyProbe::BUILD_TIMESTAMP,draw_guard),"verified draw ABI prologue accepted");draw_guard[0]^=1;
        require(!EnginePlayerOpacity::matches_draw(EngineBodyProbe::BUILD_TIMESTAMP,draw_guard) && !EnginePlayerOpacity::matches_draw(1,draw_guard),"draw ABI/build mismatch fails closed");
        using Params=void(*)(uint32_t*,uint64_t,void*,void*,void*);
        Params volatile params=native_params;std::array<uint32_t,28> gpu{};
        auto build_params=[&](uint64_t handle) {params(gpu.data(),handle,views,player.actor.data(),npc.actor.data());};
        auto gpu_offset=[&] {float value{};std::memcpy(&value,gpu.data()+9,4);return value;};
        build_params(owned_handle);require(gpu_offset()==0 && gpu[13]==0x75 && params_calls==1,"disabled GPU observer preserves native parameters");
        const auto native_gpu=gpu;
        opacity.keep_distance_opaque(true);build_params(owned_handle);
        require(gpu_offset()==-100 && params_calls==2 && opacity.params_snapshot().changed==1,"actual parameter detour changes owned distance offset after original");
        require(params_handle==owned_handle && params_args==std::array<void*,3>{views,player.actor.data(),npc.actor.data()},"all five native GPU builder arguments forwarded");
        for(unsigned i=0;i<28;++i)if(i!=9)require(gpu[i]==native_gpu[i],"only distance offset changes in GPU output");
        for(uint32_t mode:{0u,2u,3u}) {params_mode=mode;build_params(owned_handle);require(gpu[13]==(0x74|mode),"all native mesh modes stay untouched");}
        params_mode=1;build_params(reinterpret_cast<uintptr_t>(npc.instance.data())+0x70);require(gpu_offset()==0,"NPC GPU parameters stay native");
        selected=0;build_params(owned_handle);require(gpu_offset()==0,"stale owner cannot modify GPU parameters");selected=reinterpret_cast<uintptr_t>(player.actor.data());
        put(player.instance,0x58,uintptr_t(111));build_params(owned_handle);require(gpu_offset()==0,"GPU ownership revalidated");put(player.instance,0x58,uintptr_t(789));
        build_params(owned_handle|(uint64_t(0xffff)<<48));require(gpu_offset()==0,"invalid mesh index rejected for GPU output");
        opacity.allow(false);build_params(owned_handle);require(gpu_offset()==0,"VR release preserves newest native GPU build");opacity.allow(true);
        opacity.keep_distance_opaque(false);params_offset=.25f;build_params(owned_handle);require(gpu_offset()==.25f && params_calls==11,"comparison release preserves newest GPU build and calls original once");
        opacity.keep_distance_opaque(true);params_scale=NAN;build_params(owned_handle);require(gpu_offset()==.25f,"invalid scale passes native");params_scale=1;
        params_padding=0;build_params(owned_handle);require(gpu_offset()==.25f,"invalid padding passes native");params_padding=1;
        params_offset=NAN;build_params(owned_handle);require(std::isnan(gpu_offset()),"invalid offset passes native");params_offset=0;
        auto params_guard=synthetic_at<29>(0x20bd700);
        require(EnginePlayerOpacity::matches_params(EngineBodyProbe::BUILD_TIMESTAMP,params_guard),"GPU builder prologue verified");params_guard[0]^=1;
        require(!EnginePlayerOpacity::matches_params(EngineBodyProbe::BUILD_TIMESTAMP,params_guard) && !EnginePlayerOpacity::matches_params(1,params_guard),"GPU build mismatch fails closed");
        opacity.uninstall();build_params(owned_handle);require(gpu_offset()==0 && params_calls==15,"GPU detour teardown restores original");
        set(player.actor.data(),0.9f);require(calls==13 && last_value==0.9f,"quiescent teardown leaves original setter intact");
        emit(owned_handle);require(draw_hashed==1 && draw_calls==9,"draw teardown restores native function");
        require(get(player.instance.data())==0.35f && render_calls==9,"getter teardown restores original native function");
        auto guard=synthetic_at<41>(0x1343f10);
        require(EnginePlayerOpacity::matches_build(EngineBodyProbe::BUILD_TIMESTAMP,guard),"verified build/prologue accepted");guard[23]^=1;
        require(!EnginePlayerOpacity::matches_build(EngineBodyProbe::BUILD_TIMESTAMP,guard) && !EnginePlayerOpacity::matches_build(1,guard),"ABI/build mismatch fails closed");
        auto getter_guard=synthetic_at<26>(0x1d95680);
        require(EnginePlayerOpacity::matches_getter(EngineBodyProbe::BUILD_TIMESTAMP,getter_guard),"native getter prologue accepted");getter_guard[5]^=1;
        require(!EnginePlayerOpacity::matches_getter(EngineBodyProbe::BUILD_TIMESTAMP,getter_guard),"getter ABI mismatch fails closed");

        // Exercise cached parameters through the real detours, with no artificial
        // direct builder call to make enable/disable appear to work.
        require(opacity.install(reinterpret_cast<void*>(&native_opacity),[&](auto){return selected;},nullptr,reinterpret_cast<void*>(&native_draw),reinterpret_cast<void*>(&native_params),reinterpret_cast<void*>(&native_refresh)),"reinstall for native cache lifecycle");
        opacity.allow(true);opacity.keep_distance_opaque(false);
        void(*volatile refresh)(void*,uint64_t)=native_refresh;
        RefreshFixture cache{&player,&gpu};put(player.mesh,0x20,uint32_t(0x100));
        refresh(&cache,owned_handle);require(gpu_offset()==0 && cache.builds==1,"native initial cache build");
        opacity.keep_distance_opaque(true);refresh(&cache,owned_handle);require(gpu_offset()==-100 && cache.builds==2,"enabling invalidates owned cached native parameters");
        refresh(&cache,owned_handle);require(cache.builds==2,"unchanged override reuses native cache");
        opacity.allow(false);params_offset=.35f;refresh(&cache,owned_handle);require(gpu_offset()==.35f && cache.builds==3,"VR release rebuilds cached native values without direct builder call");
        opacity.allow(true);refresh(&cache,owned_handle);require(gpu_offset()==-100 && cache.builds==4,"VR re-enable rebuilds once");
        selected=0;params_offset=.45f;refresh(&cache,owned_handle);require(gpu_offset()==.45f && cache.builds==5,"ownership loss restores former player from current native call");
        refresh(&cache,owned_handle);require(cache.builds==5,"unowned native cache no longer invalidated");
        selected=reinterpret_cast<uintptr_t>(player.actor.data());opacity.allow(true);refresh(&cache,owned_handle);
        opacity.keep_distance_opaque(false);params_offset=.55f;refresh(&cache,owned_handle);require(gpu_offset()==.55f && cache.builds==7 && cache.calls==9,"toggle release refreshes newest native value and original refresh runs once");
        uint32_t cache_flags{};std::memcpy(&cache_flags,player.mesh.data()+0x20,4);require(cache_flags==0x104,"unrelated cache bits preserved");
        RefreshFixture npc_cache{&npc,&gpu};put(npc.mesh,0x20,uint32_t(0x104));opacity.keep_distance_opaque(true);
        refresh(&npc_cache,reinterpret_cast<uintptr_t>(npc.instance.data())+0x70);require(npc_cache.builds==0 && npc_cache.calls==1,"unmodified NPC cache stays valid");
        require(!opacity.distance_control("unknown") && opacity.params_snapshot().enabled,"invalid control leaves mode unchanged");
        require(opacity.distance_control("native") && !opacity.params_snapshot().enabled,"native control disables distance override");
        require(opacity.distance_control("opaque") && opacity.params_snapshot().enabled,"opaque control enables distance override");
        const auto npc_handle=reinterpret_cast<uintptr_t>(npc.instance.data())+0x70;
        params_offset=0;
        require(opacity.distance_control("all") && opacity.params_snapshot().all_objects,"explicit all-object scope includes separate weapon/scenery instances");
        refresh(&npc_cache,npc_handle);require(gpu_offset()==-100 && npc_cache.builds==1,"all scope rebuilds a previously native non-player cache");
        for(int i=0;i<100;++i)refresh(&npc_cache,npc_handle);
        require(npc_cache.builds==1,"all scope reuses the same GPU cache over repeated draws");
        for(unsigned i=0;i<28;++i)if(i!=9)require(gpu[i]==native_gpu[i],"global mode preserves material alpha and every other output field");
        selected=0;refresh(&npc_cache,npc_handle);require(npc_cache.builds==1,"global mode does not require local actor ownership");
        opacity.distance_control("opaque");params_offset=.2f;refresh(&npc_cache,npc_handle);
        require(gpu_offset()==.2f && npc_cache.builds==2,"narrowing back to player restores other instances");
        opacity.distance_control("all");refresh(&npc_cache,npc_handle);require(gpu_offset()==-100 && npc_cache.builds==3,"global scope re-enables native cache once");
        opacity.allow(false);params_offset=.3f;refresh(&npc_cache,npc_handle);
        require(gpu_offset()==.3f && npc_cache.builds==4,"VR release restores global cached values");
        opacity.allow(true);params_scale=NAN;refresh(&npc_cache,npc_handle);
        require(gpu_offset()==.3f && npc_cache.builds==5,"invalid native parameters are never overridden globally");
        for(int i=0;i<100;++i)refresh(&npc_cache,npc_handle);
        require(npc_cache.builds==5,"rejected global parameters do not force per-draw rebuilds");
        params_scale=1;put(npc.mesh,0x20,uint32_t(0x100));refresh(&npc_cache,npc_handle);
        require(gpu_offset()==-100 && npc_cache.builds==6,"natural native rebuild reevaluates previously invalid parameters");
        params_offset=0;
        build_params(npc_handle|(uint64_t(0xffff)<<48));require(gpu_offset()==0,"global scope still rejects negative mesh handles");
        build_params(0);require(gpu_offset()==0,"global scope rejects null handles");
        for(uint64_t i=1;i<=1024;++i) {build_params(npc_handle|(i<<48));require(gpu_offset()==-100,"global bookkeeping exceeds former 512-player-mesh cap");}
        require(opacity.params_snapshot().tracked>=1025,"global caches retain restoration bookkeeping");
        opacity.distance_control("native");params_offset=.4f;refresh(&npc_cache,npc_handle);
        require(gpu_offset()==.4f && npc_cache.builds==7,"global disable restores native current value");
        opacity.distance_control("all");opacity.enable(false);render_value=.35f;
        require(get(npc.instance.data())==.35f,"all distance fade mode does not disable ordinary scalar transparency");
        opacity.distance_control("native");params_offset=0;selected=reinterpret_cast<uintptr_t>(player.actor.data());
        {
            // Classes playtest (26 Oct, 11:54:53): the all-object bookkeeping filled with earlier levels'
            // caches, and the new player's own meshes were refused (the hands faded). Fill it with other
            // objects' caches, all drawn just now: the player's cache still gets in, another object's does not.
            opacity.distance_control("all");opacity.allow(true); // Re-reads the player (selected above) as the owner.
            const auto before=opacity.params_snapshot();
            for(uint64_t i=opacity.params_snapshot().tracked;i<EnginePlayerOpacity::DISTANCE_CACHE_LIMIT;++i) build_params(0x100000+i*0x100);
            require(opacity.params_snapshot().tracked==EnginePlayerOpacity::DISTANCE_CACHE_LIMIT && opacity.params_snapshot().capacity_skips==before.capacity_skips,"fixture: bookkeeping full");
            build_params(owned_handle);
            require(gpu_offset()==-100 && opacity.params_snapshot().evicted==1 && opacity.params_snapshot().capacity_skips==before.capacity_skips,
                "a full table still takes the player's own cache, in place of another object's");
            build_params(0x900000000);
            require(gpu_offset()==0 && opacity.params_snapshot().capacity_skips==before.capacity_skips+1,"with every entry fresh another new object's cache is refused, as before");
            opacity.distance_control("native");
        }
        {
            // DistanceFadeCache on its own, with explicit time: a full map drops caches not drawn for STALE.
            using namespace std::chrono_literals;
            DistanceFadeCache table{3};const auto t0=DistanceFadeCache::Clock::time_point{}+100s;
            for(uint64_t h:{1,2,3}) require(table.admit(h,false,t0).first,"fixture: room for three");
            require(!table.admit(4,false,t0+500ms).first && table.size()==3,"every entry drawn within STALE: a new object waits");
            table.seen(1,t0+900ms);table.seen(2,t0+900ms);
            require(!table.admit(4,false,t0+1200ms).first,"the sweep runs at most once per STALE");
            const auto [added,how]=table.admit(4,false,t0+1600ms);
            require(added && how==DistanceFadeCache::Admit::swept && table.swept()==1 && !table.find(3) && table.find(1) && table.find(2),
                "a cache not drawn for a second gives its place; drawn ones keep theirs");
            require(table.admit(5,true,t0+1700ms).first && table.evicted()==1 && table.find(5)->owned,"the player's cache takes the place of another object's");
            require(table.admit(6,true,t0+1800ms).first && table.admit(7,true,t0+1900ms).first && !table.admit(8,true,t0+2000ms).first,
                "never in place of the player's own: three player caches fill it");
            // touch: the shared-lock draw path. It keeps an entry fresh like seen, and says whether it is tracked.
            DistanceFadeCache touched{2};require(touched.admit(1,false,t0).first && touched.admit(2,false,t0).first,"fixture: two entries");
            require(touched.touch(1,t0+900ms) && !touched.touch(9,t0+900ms),"touch finds tracked caches only");
            require(touched.admit(3,false,t0+1500ms).first && touched.find(1) && !touched.find(2),"a touched entry survives the sweep; the untouched one goes");
        }
        opacity.uninstall();
        {
            // Release tidy (26 Oct): without SM2VR_PLAYER_OPAQUE only the distance-fade pair is
            // installed. The setter, getter and draw stay native and uncounted; the fade still works.
            require(opacity.install(nullptr,[&](auto){return selected;},nullptr,nullptr,reinterpret_cast<void*>(&native_params),reinterpret_cast<void*>(&native_refresh)),
                "distance fade installs without the opacity hooks");
            require(opacity.installed() && !opacity.opacity_installed() && !opacity.snapshot().getter_installed && !opacity.draw_snapshot().installed && opacity.params_snapshot().installed,
                "only the params/refresh pair is hooked");
            selected=reinterpret_cast<uintptr_t>(player.actor.data());opacity.allow(true);opacity.enable(true);
            const auto reads=opacity.snapshot().reads,draws=opacity.draw_snapshot().owned;
            set(player.actor.data(),0.2f);require(last_value==0.2f,"setter not hooked: native value reaches the game");
            require(get(player.instance.data())==render_value && opacity.snapshot().reads==reads,"getter not hooked: no override, no counter");
            emit(owned_handle);require(draw_hashed==1 && opacity.draw_snapshot().owned==draws,"draw not hooked: no per-draw reads");
            opacity.distance_control("opaque");params_offset=0;build_params(owned_handle);
            require(gpu_offset()==-100,"the live distance-fade suppression still applies to the player");
            opacity.distance_control("native");opacity.enable(false);opacity.uninstall();
            require(!opacity.install(nullptr,[&](auto){return selected;}),"nothing to hook is refused");
        }
        auto refresh_guard=synthetic_at<16>(0x1d7fa70);
        require(EnginePlayerOpacity::matches_refresh(EngineBodyProbe::BUILD_TIMESTAMP,refresh_guard),"refresh prologue verified");refresh_guard[0]^=1;
        require(!EnginePlayerOpacity::matches_refresh(EngineBodyProbe::BUILD_TIMESTAMP,refresh_guard),"refresh mismatch rejected");

        auto layout=std::make_shared<EngineArmProbe::RigLayout>();
        layout->names={"ROOT","NECK","HEAD","helmet","face","HANDL","BACKup"};layout->parents={-1,0,1,2,2,0,0};
        layout->bind_locals.assign(7,vr_camera::IDENTITY);layout->bind_locals[2][13]=0.1f;layout->bind_locals[3][12]=0.2f;layout->bind_locals[4][14]=0.1f;
        const auto hidden=vr_head::hide(layout->bind_locals,layout->parents,layout->names);
        require(hidden && (*hidden)[2][0]==0.001f && (*hidden)[2][5]==0.001f && (*hidden)[2][10]==0.001f && (*hidden)[2][13]==0.1f,"head basis shrinks while head pivot stays fixed");
        for(size_t i:{0u,1u,5u,6u}) require((*hidden)[i]==layout->bind_locals[i],"head prototype never writes body/hand/neck local transforms");
        require((*hidden)[3][0]==0.001f && (*hidden)[4][0]==0.001f && std::abs((*hidden)[3][12]-0.0002f)<1e-7f,"every helmet/facial skin basis and local offset collapses explicitly");
        // Multiplication, not rigid-only arm FK: descendants inherit the head scale.
        const auto head_world=vr_camera::multiply((*hidden)[2],(*hidden)[1]);
        const auto helmet_world=vr_camera::multiply((*hidden)[3],head_world);
        require(std::abs(helmet_world[0]-0.000001f)<1e-9f && std::abs(helmet_world[12]-0.0000002f)<1e-9f,"helmet subtree collapses near the preserved head pivot");
        std::array<int16_t,7> partial_map{0,1,2,3,-1,5,6};const auto partially_mapped=vr_head::hide(layout->bind_locals,layout->parents,layout->names,partial_map);
        require(partially_mapped && (*partially_mapped)[4]==layout->bind_locals[4] && (*partially_mapped)[3][0]==0.001f,"unmapped facial helper has no native skin slot and does not block mapped head/helmet");
        auto bad_names=layout->names;bad_names[4]="HEAD";require(!vr_head::hide(layout->bind_locals,layout->parents,bad_names),"duplicate head rejected");
        bad_names=layout->names;bad_names[3]="other";require(!vr_head::hide(layout->bind_locals,layout->parents,bad_names),"missing helmet rejected");
        auto bad_parents=layout->parents;bad_parents[5]=2;require(!vr_head::hide(layout->bind_locals,bad_parents,layout->names),"head-parented hand rejects dangerous rig");
        bad_parents=layout->parents;bad_parents[3]=3;require(!vr_head::hide(layout->bind_locals,bad_parents,layout->names),"cyclic hierarchy rejected");

        EngineArmDriver driver;const auto now=EngineArmDriver::Clock::now();
        std::array<vr_arm::Matrix,9> buffer{};const std::array<int16_t,7> map{4,8,1,6,3,0,7};
        EngineArmProbe::Pose pose;pose.owner.actor=123;pose.owner.instance=456;pose.received=now;pose.layout=layout;pose.world=vr_camera::IDENTITY;pose.world_valid=true;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());
        for(size_t i=0;i<7;++i) {buffer[map[i]]=layout->bind_locals[i];pose.bones.push_back({static_cast<uint32_t>(i),map[i],layout->bind_locals[i]});}
        const auto native=buffer;bool current=true,locked=false,partial_failure=false;uint64_t writes{};
        driver.configure({[&](const auto& p,auto){return current && p.owner.actor==123;},
            [&](uintptr_t instance){require(instance==456 && !locked,"head uses owned publication lock");locked=true;},
            [&](uintptr_t){require(locked,"head releases lock");locked=false;},
            [&](uintptr_t address,const auto& value){require(locked,"head writes only under publication lock");++writes;if(partial_failure) {partial_failure=false;std::memcpy(reinterpret_cast<void*>(address),value.data(),16);return false;}std::memcpy(reinterpret_cast<void*>(address),value.data(),sizeof(value));return true;}});
        EngineArmDriver::Targets targets;targets.actor=123;targets.reference=2;targets.epoch=5;targets.captured=now; // No controllers.
        driver.allow(true,5);driver.hide_head(true);driver.targets(targets);driver.apply(pose,now);
        require(buffer[map[2]][0]==0.001f && driver.snapshot().head_updates==1 && !locked,"head hides with arm driver off and both controllers asleep");
        for(size_t i=0;i<buffer.size();++i) if(i!=map[2] && i!=map[3] && i!=map[4]) require(buffer[i]==native[i],"actual mapped writer changes only the head subtree skin slots");
        require(buffer[map[3]][0]==0.001f && buffer[map[4]][0]==0.001f,"actual writer includes helmet and facial descendants");
        buffer=native;driver.hide_head(false);driver.apply(pose,now);require(buffer==native,"head toggle off retains native republished head");
        driver.hide_head(true);driver.targets(targets);current=false;driver.apply(pose,now);require(buffer==native,"head ownership loss excludes writes");current=true;
        driver.apply(pose,now+std::chrono::milliseconds(101));require(buffer==native,"stale head/native sample excludes writes");
        driver.allow(false,5);driver.apply(pose,now);require(buffer==native,"VR/focus off releases head");
        driver.allow(true,6);driver.targets(targets);driver.apply(pose,now);require(buffer==native,"old recenter epoch excludes head writes");
        driver.allow(true,5);driver.targets(targets);pose.bones[2].skin_index=-1;driver.apply(pose,now);require(buffer==native,"unmapped head excludes all writes");pose.bones[2].skin_index=map[2];
        partial_failure=true;driver.apply(pose,now);require(buffer==native && driver.snapshot().failures==1 && !locked,"partial head write failure rolls back exact original under lock");
        require(writes>=3,"head production write/rollback path exercised");
        std::cout<<"native player opacity forwarding/ownership/restoration and guarded head/helmet hiding passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
