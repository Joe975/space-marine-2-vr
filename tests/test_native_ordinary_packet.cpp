#include "NativeOrdinaryPacket.h"
#include "NativePipLens.h"
#include "fixtures/ordinary_camera_math_native.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
using Control=NativeOrdinaryPacket;using Block=Control::Block;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T>void put(uintptr_t address,size_t offset,const T& value){std::memcpy(reinterpret_cast<void*>(address+offset),&value,sizeof(value));}
vr_camera::Matrix matrix(const std::array<uint32_t,16>& words){vr_camera::Matrix out;for(size_t i=0;i<16;++i)out[i]=std::bit_cast<float>(words[i]);return out;}
unsigned fixture_id{};
struct Fixture {
    std::vector<uint8_t> list=std::vector<uint8_t>(0x2a000),renderer=std::vector<uint8_t>(0x600);
    uint64_t base{uint64_t(++fixture_id)*100},sequence{};size_t sample{};Control::Globals context;Block left_original{},right_original{};
    uintptr_t list_address()const{return reinterpret_cast<uintptr_t>(list.data());}
    uintptr_t renderer_address()const{return reinterpret_cast<uintptr_t>(renderer.data());}
    void begin(size_t index){sample=index;sequence=base+index;const auto list_pointer=list_address();const auto left=list_pointer+0x168,right=left+0xa60;
        const auto& capture=ordinary_math_fixture::CASES[index%4];Block source{};
        for(size_t i=0;i<ordinary_camera_math::Result::OFFSETS.size();++i)put(reinterpret_cast<uintptr_t>(source.data()),ordinary_camera_math::Result::OFFSETS[i],capture.matrices[i]);
        put(reinterpret_cast<uintptr_t>(source.data()),0x6a8,capture.jitter);put(reinterpret_cast<uintptr_t>(source.data()),0x6b0,uint32_t(19+index));put(reinterpret_cast<uintptr_t>(source.data()),0xa38,capture.motion);put(reinterpret_cast<uintptr_t>(source.data()),0xa34,capture.flags);
        std::memcpy(reinterpret_cast<void*>(left),source.data(),source.size());std::memset(reinterpret_cast<void*>(right),0xa5,source.size());
        for(auto entry:{left,right}){put(entry,0,uint64_t(0x3f7c013));put(entry,0x160,std::array<float,4>{-.1f,-.1f,.2f,.2f});put(entry,0x304,80.f);put(entry,0x308,64.3f);put(entry,0x30c,1.7777778f);}
        auto world=matrix(capture.matrices[0]),view=matrix(capture.matrices[1]),projection=matrix(capture.matrices[4]);for(size_t n=0;n<3;++n)world[12+n]+=world[n]*.064f;
        for(size_t n=0;n<3;++n)view[12+n]=-(world[12]*view[n]+world[13]*view[4+n]+world[14]*view[8+n]);
        put(right,0x34,world);put(right,0x3dc,view);put(right,0x45c,projection);put(right,0x4dc,*ordinary_camera_math::inverse(projection));put(right,0x55c,vr_camera::multiply(view,projection));
        context={list_pointer,uint32_t(1000+index),1920,1080,capture.auxiliary_scale,capture.depth_remap};put(list_pointer,0x29968,int32_t(2));put(list_pointer,0x29c78,context.counter);
        using E=NativeCameraEntries;E::get().enable();const FrameSyncClock::Frame frame{uint32_t(sequence),sequence,0,0,false};
        auto ticket=E::get().begin(list_pointer+0x160,E::Role::main,frame);E::get().finish(*ticket,0,matrix(capture.matrices[0]));ticket=E::get().begin(list_pointer+0x160,E::Role::ordinary_right,frame);E::get().finish(*ticket,1,world);
        std::memcpy(left_original.data(),reinterpret_cast<void*>(left),source.size());std::memcpy(right_original.data(),reinterpret_cast<void*>(right),source.size());
    }
    NativeRenderProbe::Snapshot snapshot(){NativeRenderProbe::Snapshot s;s.sequence=sequence;s.list=list_address();s.renderer=renderer_address();s.before=*NativeRenderProbe::read_views(s.list);s.camera_revision=NativeCameraEntries::get().revision(s.list);
        std::vector<NativeCameraEntries::Entry> actual;for(const auto& v:s.before)actual.push_back({s.list+0x168+size_t(v.slot)*0xa60,v.slot,NativeCameraEntries::Role::main,v.world});s.ordinary_pair=NativeCameraEntries::get().ordinary_pair(s.list,actual,*s.camera_revision);require(s.ordinary_pair.has_value(),"fixture uses exact real producing-job tickets");return s;}
    void scenes(NativeRenderProbe::Snapshot& s){s.scene_calls=2;s.scenes.clear();for(size_t i=0;i<2;++i){NativeRenderProbe::Scene scene;scene.slot=uint32_t(i);scene.renderer=s.renderer;scene.list=s.list;scene.entry=s.list+0x168+i*0xa60;scene.render_counter_before=scene.render_counter_after=context.counter;scene.camera_revision_before=scene.camera_revision_after=s.camera_revision;scene.refusal=NativeRenderProbe::SceneRefusal::none;scene.packet_before=scene.packet_after=NativeCameraPacket::capture(scene.entry);s.scenes.push_back(scene);}}
    Control::Sdk sdk()const{return {1,10+sample,uint32_t(100+sample)};}
    Control::TrackedSource tracked_begin(size_t index){
        begin(index);Control::TrackedSource source;source.prepared.render_id=sequence;source.prepared.game_token=uint32_t(sequence);source.prepared.anchor=vr_camera::IDENTITY;
        const auto left=list_address()+0x168,right=left+0xa60;
        for(size_t eye=0;eye<2;++eye){auto& e=source.prepared.eyes[eye];
            e.world=vr_camera::IDENTITY;e.world[10]=-1;e.world[12]=float(index)*.001f+float(eye)*.067f;e.view=vr_camera::inverse(e.world);
            e.tracking.eye=int(eye);e.tracking.render_token=sequence;e.tracking.fov=vr_projection::Tangents{eye?-.9f:-1.1f,eye?1.2f:1.f,-.8f,1.05f};
            const float near_z=.1f,far_z=1000.f;std::memcpy(e.source.data()+0x80,&near_z,4);std::memcpy(e.source.data()+0x84,&far_z,4);
            require(vr_projection::build(*e.tracking.fov,near_z,far_z,e.projection),"tracked fixture lens");
            ordinary_camera_math::TrackedInput in;in.native={e.world,e.view,e.projection,{.25f,-.125f},1920,1080,3,context.scale,context.depth};in.lens={*e.tracking.fov,near_z,far_z};
            const auto result=ordinary_camera_math::build_tracked(in);require(result.has_value(),"tracked fixture math");const auto address=eye?right:left;
            for(auto offset:Control::Math::OFFSETS)put(address,offset,result->packet.at(offset));put(address,0x6a8,in.native.jitter);put(address,0x6b0,in.native.jitter_index);put(address,0xa35,uint8_t(0));
            if(eye)put(address,0x45c,e.projection);
        }
        using E=NativeCameraEntries;E::get().reset(list_address()+0x160);const FrameSyncClock::Frame frame{uint32_t(sequence),sequence,0,0,false};
        auto ticket=E::get().begin(list_address()+0x160,E::Role::main,frame);E::get().finish(*ticket,0,source.prepared.eyes[0].world);ticket=E::get().begin(list_address()+0x160,E::Role::ordinary_right,frame);E::get().finish(*ticket,1,source.prepared.eyes[1].world);
        const auto s=snapshot();source.producer_list=s.ordinary_pair->producer_list;source.producer_revision=s.ordinary_pair->producer_revision;
        std::memcpy(left_original.data(),reinterpret_cast<void*>(left),sizeof(Block));std::memcpy(right_original.data(),reinterpret_cast<void*>(right),sizeof(Block));return source;
    }
};
uintptr_t active_list{};unsigned dispatch_calls{},scene_calls{};
__declspec(noinline)void scene(void* renderer,uint32_t,int32_t slot){++scene_calls;put(reinterpret_cast<uintptr_t>(renderer),0x140,active_list+0x168+size_t(slot)*0xa60);volatile auto used=scene_calls;(void)used;}
__declspec(noinline)void invoke_scene(void(*volatile fn)(void*,uint32_t,int32_t),void* renderer,int32_t slot){fn(renderer,0,slot);volatile auto used=scene_calls;(void)used;}
__declspec(noinline)void dispatch(void* renderer,uint32_t){++dispatch_calls;invoke_scene(scene,renderer,0);invoke_scene(scene,renderer,1);volatile auto used=dispatch_calls;(void)used;}
__declspec(noinline)void invoke_dispatch(void(*volatile fn)(void*,uint32_t),void* renderer){fn(renderer,0);volatile auto used=dispatch_calls;(void)used;}
void cache(void*,const float*,const float*,const float*){}void frustum(void*){}
int main(){try{
    {Fixture f;Control c;std::optional<Control::TrackedSource> source;
        require(c.tracked([&](auto){return source;}),"explicit tracked provider accepted before arm");c.continuous(true);
        c.arm([&]{return std::optional{f.context};},[&](auto){return std::optional{f.sdk()};});
        for(size_t n=0;n<12;++n){source=f.tracked_begin(n);auto s=f.snapshot();c.prepare(s);auto r=c.receipt(s.sequence);
            require(r && r->applied && r->tracked && r->left_patched && !r->refusal,"asymmetric tracked packet transaction admitted");f.scenes(s);c.complete(s);r=c.receipt(s.sequence);
            require(r->accepted && r->continued==bool(n),"continuous tracked packet retains each own predecessor");
            for(auto offset:{0x5dc,0x874,0x8b4})for(size_t eye=0;eye<2;++eye)require(std::memcmp(reinterpret_cast<void*>(f.list_address()+0x168+eye*0xa60+offset),(eye?f.right_original:f.left_original).data()+offset,64)==0,"tracked path leaves unsupported bytes untouched");
            if(n>=4)require(!c.receipt(s.sequence-4),"tracked receipt storage stays bounded");}}
    {Fixture f;Control c;std::optional<Control::TrackedSource> source;
        c.tracked([&](auto){return source;});c.continuous(true);
        c.arm([&]{return std::optional{f.context};},[&](auto){return std::optional{f.sdk()};});
        for(size_t n:{0u,1u,10u,11u}){source=f.tracked_begin(n);auto s=f.snapshot();c.prepare(s);f.scenes(s);c.complete(s);
            const auto r=c.receipt(s.sequence);const bool reset=n==0 || n==10;
            require(r && r->accepted && r->tracked && r->left_patched && r->patched && r->continued==!reset,"tracked packet resumes after skipped mono jobs and continues on the following pair");
            for(size_t eye=0;eye<2;++eye){uint8_t cut{};std::memcpy(&cut,reinterpret_cast<void*>(f.list_address()+0x168+eye*0xa60+0xa35),1);
                require(bool(cut)==reset,"both actual native eye packets reset history at the gap only");}}}
    for(unsigned mutation=0;mutation<4;++mutation){Fixture f;Control c;auto source=f.tracked_begin(0);auto s=f.snapshot();
        if(mutation==0)++source.producer_revision;if(mutation==1)++source.prepared.game_token;if(mutation==2)source.prepared.eyes[1].projection[8]+=.01f;if(mutation==3)put(s.ordinary_pair->left.entry,0x55c,vr_camera::IDENTITY);
        c.tracked([&](auto){return std::optional{source};});c.arm([&]{return std::optional{f.context};},[&](auto){return std::optional{f.sdk()};});c.prepare(s);
        require(c.receipt(s.sequence)->refusal && !c.receipt(s.sequence)->applied,"tracked provenance/lens/native-current mismatch refuses writes");}
    // 23 Oct 08:45:58: refusal 4 on the first frame of a level transition stopped stereo for the run.
    // Refusals 4/12/13 happen before any write: continuous tracked mode marks them recoverable and
    // stays armed; the next job is re-checked from scratch as a cut, then continues.
    for(unsigned mutation=0;mutation<3;++mutation){Fixture f;Control c;std::optional<Control::TrackedSource> source;const unsigned code=mutation==0?4:mutation==1?12:13;
        c.tracked([&](auto){return source;});c.continuous(true);c.arm([&]{return std::optional{f.context};},[&](auto){return std::optional{f.sdk()};});
        for(size_t n:{0u,1u,2u,5u,6u}){source=f.tracked_begin(n);auto s=f.snapshot();
            if(n==2){if(mutation==0)put(s.ordinary_pair->left.entry,0x55c,vr_camera::IDENTITY);if(mutation==1)++source->producer_revision;
                if(mutation==2){const float near_z=.2f;std::memcpy(source->prepared.eyes[0].source.data()+0x80,&near_z,4);}}
            std::array<Block,2> before{};for(size_t eye=0;eye<2;++eye)std::memcpy(before[eye].data(),reinterpret_cast<void*>(f.list_address()+0x168+eye*0xa60),sizeof(Block));
            c.prepare(s);auto r=c.receipt(s.sequence);
            require(c.recoverable(s.sequence)==(n==2),"only the refused job is kept out of the pair retention");
            if(n==2){require(r && r->refusal==code && r->recoverable && !r->applied && !r->accepted && !r->rollback,"transition refusal is recoverable");
                for(size_t eye=0;eye<2;++eye)require(std::memcmp(before[eye].data(),reinterpret_cast<void*>(f.list_address()+0x168+eye*0xa60),sizeof(Block))==0,"recoverable refusal writes nothing");
                f.scenes(s);c.complete(s);require(!c.receipt(s.sequence)->accepted,"a refused job never becomes a predecessor");continue;}
            require(r && r->applied && !r->refusal && !r->recoverable,"packet stays armed after a recoverable refusal");f.scenes(s);c.complete(s);r=c.receipt(s.sequence);
            require(r->accepted && r->continued==(n==1 || n==6),"the job after the refusal is a cut; the next one continues from it");}}
    {Fixture f;Control c;auto source=f.tracked_begin(0);auto s=f.snapshot();put(s.ordinary_pair->left.entry,0x55c,vr_camera::IDENTITY);
        c.tracked([&](auto){return std::optional{source};});c.arm([&]{return std::optional{f.context};},[&](auto){return std::optional{f.sdk()};});c.prepare(s);
        require(c.receipt(s.sequence)->refusal==4 && !c.receipt(s.sequence)->recoverable,"desktop (non-continuous) refusal stays permanent");
        source=f.tracked_begin(1);s=f.snapshot();c.prepare(s);require(!c.receipt(s.sequence),"stopped desktop packet prepares nothing more");}
    {Fixture f;Control c;std::optional<Control::TrackedSource> source;c.tracked([&](auto){return source;});c.continuous(true);
        c.arm([&]{return std::optional{f.context};},[&](auto){return std::optional{f.sdk()};},[](uintptr_t,const Block&){return false;});
        source=f.tracked_begin(0);auto s=f.snapshot();c.prepare(s);const auto r=c.receipt(s.sequence);
        require(r && r->refusal==7 && r->rollback && !r->recoverable && !c.recoverable(s.sequence),"a refused write is never recoverable");
        source=f.tracked_begin(1);s=f.snapshot();c.prepare(s);require(!c.receipt(s.sequence),"a write refusal still stops the continuous packet");}
    // Cold restart (OrdinaryColdRestart): an SDK refusal (9: a poisoned private eye) stops the packet for
    // good; restart() lifts it as at arming and the next job is fully checked as a cut.
    {Fixture f;Control c;std::optional<Control::TrackedSource> source;c.tracked([&](auto){return source;});c.continuous(true);
        c.arm([&]{return std::optional{f.context};},[&](auto)->std::optional<Control::Sdk>{if(f.sample==1)return {};return f.sdk();});
        source=f.tracked_begin(0);auto s=f.snapshot();c.prepare(s);f.scenes(s);c.complete(s);require(c.receipt(s.sequence)->accepted,"first job accepted");
        source=f.tracked_begin(1);s=f.snapshot();c.prepare(s);require(!c.restart(),"no restart while a job is pending (its write is rolled back by its completion)");
        f.scenes(s);c.complete(s);require(c.receipt(s.sequence)->refusal==9 && c.stopped(),"an SDK refusal stops the packet");
        source=f.tracked_begin(2);s=f.snapshot();c.prepare(s);require(!c.receipt(s.sequence),"stopped: nothing prepared");
        require(c.restart() && !c.stopped() && c.restarts()==1,"restart lifts the stop");
        for(size_t n:{3u,4u}){source=f.tracked_begin(n);s=f.snapshot();c.prepare(s);f.scenes(s);c.complete(s);const auto r=c.receipt(s.sequence);
            require(r && r->accepted && !r->refusal && r->continued==(n==4),"after a restart the next job is a cut, the one after continues");}}
    {Fixture f;f.begin(0);Control c;auto s=f.snapshot();c.prepare(s);require(!c.receipt(s.sequence) && std::memcmp(reinterpret_cast<void*>(s.ordinary_pair->right.entry),f.right_original.data(),sizeof(Block))==0,"default path never writes or creates a transaction");}
    for(unsigned mutation=0;mutation<6;++mutation){Fixture f;f.begin(0);Control c;unsigned reads{};require(c.arm([&]()->std::optional<Control::Globals>{auto value=f.context;if(mutation==0 && ++reads==2)++value.counter;if(mutation==1)value.width=3840;return value;},[&](auto){return std::optional{f.sdk()};}),"control arms once");auto s=f.snapshot();
        if(mutation==2)s.ordinary_pair->right.revision++;
        if(mutation==3)put(s.ordinary_pair->right.entry,0x45c,vr_camera::IDENTITY);
        if(mutation==4){auto wrong=vr_camera::IDENTITY;wrong[0]=2;put(s.ordinary_pair->left.entry,0x9b4,wrong);}
        if(mutation==5)put(s.ordinary_pair->left.entry,0xa35,uint8_t(255));
        Block before{};std::memcpy(before.data(),reinterpret_cast<void*>(s.ordinary_pair->right.entry),before.size());c.prepare(s);auto r=c.receipt(s.sequence);
        require(r && r->refusal && !r->applied && std::memcmp(before.data(),reinterpret_cast<void*>(s.ordinary_pair->right.entry),before.size())==0,"context, producer, raster, native reference or flags refusal writes nothing");
    }
    for(unsigned failure=0;failure<3;++failure){Fixture f;f.begin(0);Control c;unsigned writes{};
        c.arm([&]{return std::optional{f.context};},[&](auto){return std::optional{f.sdk()};},[&](uintptr_t address,const Block& value){++writes;if(writes==1){std::memcpy(reinterpret_cast<void*>(address),value.data(),0x500);if(failure==1)throw std::runtime_error("write failed");return false;}if(failure==2)return false;std::memcpy(reinterpret_cast<void*>(address),value.data(),value.size());return true;});
        auto s=f.snapshot();c.prepare(s);const auto r=c.receipt(s.sequence);require(r && r->refusal && !r->applied && r->rollback && r->rollback_failed==(failure==2),"partial write/exception performs verified rollback and reports failed restoration");
        if(failure!=2)require(std::memcmp(reinterpret_cast<void*>(s.ordinary_pair->right.entry),f.right_original.data(),sizeof(Block))==0,"complete original right packet restored after partial failure");
    }
    for(unsigned failure=0;failure<5;++failure){Fixture f;f.begin(0);Control c;c.arm([&]{return std::optional{f.context};},[&](auto)->std::optional<Control::Sdk>{if(failure==0)return {};return f.sdk();});auto s=f.snapshot();c.prepare(s);require(c.receipt(s.sequence)->applied,"accepted pre-dispatch write before completion negative");f.scenes(s);
        if(failure==1)s.scenes[1].packet_after->motion_flags[0]^=1;
        if(failure==2)s.scenes[1].camera_revision_after.reset();
        if(failure==3)++f.context.counter;
        if(failure==4){auto nested=s;nested.sequence++;c.prepare(nested);}
        c.complete(s);const auto r=c.receipt(s.sequence);require(r && r->refusal && !r->accepted,"Scene, context, SDK or overlapping completion cannot commit a predecessor");
    }
    {Fixture f;Control c;c.arm([&]{return std::optional{f.context};},[&](auto){auto sdk=f.sdk();if(f.sample==1)++sdk.epoch;return std::optional{sdk};});
        for(size_t i=0;i<2;++i){f.begin(i);auto s=f.snapshot();c.prepare(s);f.scenes(s);c.complete(s);const auto r=c.receipt(s.sequence);require(r && r->accepted==!bool(i) && r->refusal==(i?9U:0U),"SDK epoch change cannot commit provisional native continuity");}}
    Fixture f;active_list=f.list_address();auto& control=Control::get();require(control.arm([&]{return std::optional{f.context};},[&](auto){return std::optional{f.sdk()};}),"actual integration control arms");
    auto& probe=NativeRenderProbe::get();require(probe.install(reinterpret_cast<void*>(&dispatch),[]{return active_list;},[]{return true;}),"actual native dispatcher detour installed");
    require(NativePipLens::get().install(reinterpret_cast<void*>(&scene),cache,frustum,[]{return active_list;},[]{return std::optional<NativePipLens::Target>{};}),"sole actual Scene hook observes ordinary calls with PIP disabled");
    for(size_t index=0;index<4;++index){f.begin(index);invoke_dispatch(dispatch,f.renderer.data());const auto s=probe.latest();require(s && s->ordinary_pair && s->scene_calls==2 && !s->scene_refused,"real single dispatcher and two Scene calls retain exact native provenance");
        const auto r=control.receipt(s->sequence);require(r && r->applied && r->accepted && r->reference && r->source_unchanged && r->continued==bool(index),"actual hook path prepares and commits only each accepted own-right predecessor");
        require(std::memcmp(reinterpret_cast<void*>(f.list_address()+0x168),f.left_original.data(),sizeof(Block))==0,"complete original left camera unchanged");
        for(auto offset:{0x5dc,0x874,0x8b4})require(std::memcmp(reinterpret_cast<void*>(f.list_address()+0x168+0xa60+offset),f.right_original.data()+offset,64)==0,"unsupported opaque fields never overwritten");
        std::array<bool,0xa60> written{};for(auto offset:ordinary_camera_math::Result::OFFSETS)for(size_t byte=0;byte<64;++byte)written[offset+byte]=true;
        for(auto [offset,size]:std::array<std::pair<size_t,size_t>,5>{{{0x6a8,8},{0x6b0,4},{0xa34,2},{0xa38,12},{0xa44,12}}})for(size_t byte=0;byte<size;++byte)written[offset+byte]=true;
        const auto* current=reinterpret_cast<const uint8_t*>(f.list_address()+0x168+0xa60);for(size_t byte=0;byte<written.size();++byte)if(!written[byte])require(current[byte]==f.right_original[byte],"every unsupported byte, culling field and native flag remains exact");
        require(s->before[1].packet==r->patched && s->scenes[1].packet_before==r->patched,"exported before-dispatch and actual Scene packets are the exact committed native data");
        std::ostringstream out;control.write_receipt(out,s->sequence);require(out.str().find("\"native_gpu_histories_verified\":false")!=std::string::npos,"transaction never claims native GPU ownership");}
    f.begin(0);invoke_dispatch(dispatch,f.renderer.data());const auto final=probe.latest();require(final && !control.receipt(final->sequence) && dispatch_calls==5 && scene_calls==10,"four writes maximum, every dispatcher/Scene still called exactly once");
    std::cout<<"native ordinary packet: real dispatch/Scene, refusal and rollback controls PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
