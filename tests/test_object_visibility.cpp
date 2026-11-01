#include "synthetic_code.h"
#include "NativeObjectVisibilityProbe.h"
#include "NativeOrdinaryLightVisibility.h"
#include "NativeWorkerVisibilityProbe.h"
#include <thread>
#include <future>
#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
unsigned selects{},draws{};DWORD incoming{};
DWORD admission_native_incoming{};
std::function<void()> admission_query_steps;
uintptr_t query_caller{};int native_query_result{};unsigned native_query_calls{};
std::function<void()> census_draw_steps;
std::function<uintptr_t()> selection_steps;std::function<void()> job_steps;
std::function<void(uintptr_t,uintptr_t,uintptr_t,uint64_t,uintptr_t,uint8_t)> binding_draw_steps;
std::array<uintptr_t,0x2e0/8> binding_vtable{};
std::function<void(uintptr_t,int32_t,uintptr_t,uintptr_t,uintptr_t)> worker_steps;
unsigned worker_calls{};DWORD worker_incoming{};std::mutex worker_fixture_mutex;
std::atomic_uint builder_calls{};std::atomic<DWORD> builder_incoming{};
std::function<void(uintptr_t,uintptr_t,uintptr_t,int32_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,int32_t,uintptr_t)> builder_steps;
__declspec(noinline) void builder_fixture(uintptr_t category,uintptr_t material,uintptr_t entry,int32_t slot,uintptr_t scratch,uintptr_t a6,uintptr_t a7,uintptr_t context,uintptr_t output,int32_t a10,uintptr_t a11){builder_incoming=GetLastError();++builder_calls;if(builder_steps)builder_steps(category,material,entry,slot,scratch,a6,a7,context,output,a10,a11);SetLastError(0x61626364);}
__declspec(noinline) void builder_call(uintptr_t category,uintptr_t material,uintptr_t entry,int32_t slot,uintptr_t scratch,uintptr_t a6,uintptr_t a7,uintptr_t context,uintptr_t output,int32_t a10,uintptr_t a11){NativeWorkerVisibilityProbe::Builder volatile fn=&builder_fixture;fn(category,material,entry,slot,scratch,a6,a7,context,output,a10,a11);volatile auto keep=builder_calls.load();(void)keep;}
__declspec(noinline) void builder_other_call(uintptr_t category,uintptr_t material,uintptr_t entry,int32_t slot,uintptr_t scratch,uintptr_t a6,uintptr_t a7,uintptr_t context,uintptr_t output,int32_t a10,uintptr_t a11){NativeWorkerVisibilityProbe::Builder volatile fn=&builder_fixture;fn(category,material,entry,slot,scratch,a6,a7,context,output,a10,a11);volatile auto keep=builder_incoming.load();(void)keep;}
__declspec(noinline) void worker_fixture(uintptr_t list,int32_t worker,uintptr_t handle,uintptr_t mask,uintptr_t options){{std::lock_guard lock{worker_fixture_mutex};worker_incoming=GetLastError();++worker_calls;}if(worker_steps)worker_steps(list,worker,handle,mask,options);SetLastError(0x51525354);std::lock_guard lock{worker_fixture_mutex};volatile auto used=worker_calls;(void)used;}
__declspec(noinline) int32_t query_fixture(uintptr_t,uintptr_t,int8_t){admission_native_incoming=GetLastError();++native_query_calls;query_caller=reinterpret_cast<uintptr_t>(_ReturnAddress());if(admission_query_steps)admission_query_steps();SetLastError(0x12344321);return native_query_result;}
__declspec(noinline) int32_t query_call(uintptr_t manager,uintptr_t object,int8_t column){NativeOrdinaryLightVisibility::Query volatile fn=&query_fixture;const auto result=fn(manager,object,column);volatile auto used=native_query_calls;(void)used;return result;}
__declspec(noinline) int32_t query_other_call(uintptr_t manager,uintptr_t object,int8_t column){NativeOrdinaryLightVisibility::Query volatile fn=&query_fixture;const auto result=fn(manager,object,column);volatile auto used=draws;(void)used;return result;}
__declspec(noinline) void job_fixture(uintptr_t,uintptr_t){if(job_steps)job_steps();volatile auto used=native_query_calls;(void)used;}
unsigned scene_clear_native_calls{};DWORD scene_clear_native_incoming{};
std::function<void(uintptr_t,uint32_t,uintptr_t)> scene_clear_steps;
__declspec(noinline) void scene_clear_fixture(uintptr_t command,uint32_t flags,uintptr_t values){scene_clear_native_incoming=GetLastError();++scene_clear_native_calls;if(scene_clear_steps)scene_clear_steps(command,flags,values);SetLastError(0xc1ea1234);}
__declspec(noinline) void scene_clear_call(uintptr_t command,uint32_t flags,uintptr_t values){NativeSceneClearProbe::Clear volatile fn=&scene_clear_fixture;fn(command,flags,values);volatile auto used=scene_clear_native_calls;(void)used;}
NativeSceneClearProbe::Clock::time_point scene_clear_now{};
NativeSceneClearProbe::Clock::time_point scene_clear_clock(){return scene_clear_now;}
std::function<bool(uintptr_t,void*,size_t)> scene_clear_read_steps;
bool scene_clear_reader(uintptr_t p,void* out,size_t n){if(scene_clear_read_steps)return scene_clear_read_steps(p,out,n);SIZE_T got{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),out,n,&got) && got==n;}
std::function<void(void*,uint32_t)> dispatch_steps;
std::function<void(void*,uint32_t,int32_t)> scene_steps;
__declspec(noinline) void dispatch_fixture(void* renderer,uint32_t flags){dispatch_steps(renderer,flags);volatile auto keep=selects;(void)keep;}
__declspec(noinline) void scene_fixture(void* renderer,uint32_t flags,int32_t slot){scene_steps(renderer,flags,slot);volatile auto keep=draws;(void)keep;}
__declspec(noinline) uintptr_t select_fixture(uintptr_t a,uintptr_t b,uintptr_t c,int d,int e,uintptr_t f,uint16_t g){
    if(selection_steps)return selection_steps();
    incoming=GetLastError();++selects;
    require(a==11 && b==22 && c==33 && d==-4 && e==5 && f==66 && g==0xfedc,"selector argument ABI");
    SetLastError(0x80818283);return uintptr_t{0xfedcba9876543210};
}
__declspec(noinline) void draw_fixture(uintptr_t a,uintptr_t b,uintptr_t c,uint64_t d,uintptr_t e,uint8_t f){
    if(census_draw_steps){census_draw_steps();return;}
    incoming=GetLastError();++draws;
    if(binding_draw_steps){binding_draw_steps(a,b,c,d,e,f);SetLastError(0x90919293);return;}
    require(a==11 && b==22 && c==33 && d==0xfedcba9876543210 && e==55 && f==0xab,"draw argument ABI");
    SetLastError(0x90919293);
}
}
__declspec(noinline) void census_target_draw_call(uintptr_t a,uintptr_t b,uintptr_t c){NativeObjectVisibilityProbe::Draw volatile fn=&draw_fixture;fn(a,b,c,0,0,0);volatile auto used=draws;(void)used;}
struct ObjectVisibilityProbeTests {
    static void run(const std::filesystem::path& directory){
        std::filesystem::remove(directory/"joined_capture_request.txt");
        std::filesystem::remove(directory/"joined_fixture.draw_visibility.json");
        std::filesystem::remove(directory/"joined_wrong_camera.draw_visibility.json");
        using Probe=NativeObjectVisibilityProbe;Probe::Eye eye;
        bool note_attempted{};NativeWorkerVisibilityProbe::passive_note([&]{note_attempted=true;throw std::bad_alloc{};});
        require(note_attempted,"fallible passive-note boundary swallows allocation failure before native forwarding continues");
        alignas(8) std::array<uint32_t,16> primitive{};primitive[3]=0x12345678;
        std::array<uint8_t,0x200> descriptor{};const auto object=reinterpret_cast<uintptr_t>(primitive.data());
        std::memcpy(descriptor.data()+0xf8,&object,8);const uint16_t flags=0x101;std::memcpy(descriptor.data()+0x194,&flags,2);
        alignas(8) std::array<uint8_t,0x300> geometry{};std::array<uint8_t,0x40> properties{},type_link{};std::array<uint8_t,0x50> class_owner{};
        const auto properties_address=reinterpret_cast<uintptr_t>(properties.data()),type_link_address=reinterpret_cast<uintptr_t>(type_link.data()),class_owner_address=reinterpret_cast<uintptr_t>(class_owner.data());
        uintptr_t weak_owner=properties_address;const auto weak_address=reinterpret_cast<uintptr_t>(&weak_owner);
        std::memcpy(geometry.data()+0x58,&properties_address,8);geometry[0x178]=1;
        std::memcpy(properties.data()+8,&type_link_address,8);std::memcpy(properties.data()+0x30,&weak_address,8);std::memcpy(type_link.data()+0x10,&class_owner_address,8);
        std::string console_class="test_scene_mod__usable_button_interplay__x__floor_placement__actor";
        const auto console_text=reinterpret_cast<uintptr_t>(console_class.data());const uint32_t console_length=uint32_t(console_class.size());
        std::memcpy(class_owner.data()+0x30,&console_text,8);std::memcpy(class_owner.data()+0x38,&console_length,4);class_owner[0x3f]=1;
        const uint64_t geometry_handle=uint64_t(reinterpret_cast<uintptr_t>(geometry.data())+0x70)|(uint64_t(3)<<48);
        std::memcpy(descriptor.data()+0x100,&geometry_handle,8);const auto descriptor_pointer=reinterpret_cast<uintptr_t>(descriptor.data());
        auto identity=native_geometry_identity::capture(descriptor_pointer);
        require(identity.mask==0 && identity.class_chain_stable && identity.class_name==console_class && identity.declared_length==66 && identity.heap && identity.weak_roundtrip && identity.reads<=20,"real bounded native heap string supports the exact66-byte console class and stable ownership chain");
        require(identity.palette_read && identity.palette_stable && !identity.palette_pointer,"readable stable null palette is distinct from unreadable and makes no skinned-owner claim");
        const uintptr_t palette_pointer=0x123456789abcdef0;std::memcpy(geometry.data()+0xd0,&palette_pointer,8);
        identity=native_geometry_identity::capture(descriptor_pointer);
        require(identity.mask==0 && identity.class_chain_stable && identity.palette_read && identity.palette_stable && identity.palette_pointer==palette_pointer && identity.reads<=20,"raw instance D0 is copied and rechecked without dereferencing even an invalid palette address");
        unsigned palette_reads{};identity=native_geometry_identity::capture(descriptor_pointer,[&](uintptr_t p,void* target,size_t size){if(p==reinterpret_cast<uintptr_t>(geometry.data())+0xd0 && ++palette_reads==2){const uintptr_t replacement=7;std::memcpy(geometry.data()+0xd0,&replacement,8);}SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&n) && n==size;});
        require(identity.mask==0 && identity.class_chain_stable && identity.palette_read && !identity.palette_stable && identity.palette_pointer==palette_pointer,"changed palette refuses palette stability without mislabelling an otherwise stable class chain");
        identity=native_geometry_identity::capture(descriptor_pointer,[&](uintptr_t p,void* target,size_t size){if(p==reinterpret_cast<uintptr_t>(geometry.data())+0xd0)return false;SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&n) && n==size;});
        require(identity.mask==0 && identity.class_chain_stable && !identity.palette_read && !identity.palette_stable,"unreadable optional palette remains explicitly unknown without rejecting class evidence");
        std::memcpy(geometry.data()+0xd0,&palette_pointer,8);
        identity=native_geometry_identity::capture(descriptor_pointer);std::ostringstream identity_json;native_geometry_identity::write(identity_json,identity);
        require(identity_json.str().find("\"palette_pointer\":"+std::to_string(palette_pointer))!=std::string::npos && identity_json.str().find("\"palette_read\":true,\"palette_stable\":true")!=std::string::npos,"receipt serializes independently qualified raw palette provenance");
        geometry[0x178]=2;identity=native_geometry_identity::capture(descriptor_pointer);
        require(identity.mask==native_geometry_identity::unsupported_kind && !identity.instance && identity.reads==2,"lightweight/static handles are retained as unsupported before any instance-minus70 decode");geometry[0x178]=1;
        require(native_geometry_identity::capture(UINTPTR_MAX).mask==native_geometry_identity::handle_unreadable,"overflow/unreadable descriptor remains unknown");
        const uintptr_t bad_properties=1;std::memcpy(geometry.data()+0x58,&bad_properties,8);require(native_geometry_identity::capture(descriptor_pointer).mask==native_geometry_identity::chain_unreadable,"unreadable native chain stays unknown");std::memcpy(geometry.data()+0x58,&properties_address,8);
        const uint32_t long_name=129;std::memcpy(class_owner.data()+0x38,&long_name,4);require(native_geometry_identity::capture(descriptor_pointer).mask==native_geometry_identity::bad_length,"overlong class rejected before dynamic read, never truncated into a match");std::memcpy(class_owner.data()+0x38,&console_length,4);
        unsigned payload_reads{};identity=native_geometry_identity::capture(descriptor_pointer,[&](uintptr_t p,void* target,size_t size){if(p==console_text && ++payload_reads==2)console_class[0]='X';SIZE_T read{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&read) && read==size;});
        require(identity.mask==native_geometry_identity::changed && !identity.class_chain_stable,"changed actual native name payload refuses stable identity");console_class[0]='t';
        unsigned handle_reads{};identity=native_geometry_identity::capture(descriptor_pointer,[&](uintptr_t p,void* target,size_t size){if(p==descriptor_pointer+0x100 && ++handle_reads==2){const auto changed_handle=geometry_handle+(uint64_t(1)<<48);std::memcpy(descriptor.data()+0x100,&changed_handle,8);}SIZE_T read{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&read) && read==size;});
        require(identity.mask==native_geometry_identity::changed && !identity.class_chain_stable,"changed native descriptor handle refuses stable identity");std::memcpy(descriptor.data()+0x100,&geometry_handle,8);
        descriptor[0x60]=17;SetLastError(0x61626364);
        Probe::describe(eye,reinterpret_cast<uintptr_t>(descriptor.data()),false);
        require(GetLastError()==0x61626364,"successful actual describe preserves LastError");
        descriptor[0x60]=25; // Pool reuse after first observation cannot relabel this record.
        Probe::describe(eye,reinterpret_cast<uintptr_t>(descriptor.data()),true);
        require(eye.records[0].render_type_read && eye.records[0].render_type==17,"actual describe captures first observed render type rather than later pooled value");
        SYSTEM_INFO system{};GetSystemInfo(&system);const auto page=size_t(system.dwPageSize);
        auto* allocation=static_cast<uint8_t*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        require(allocation!=nullptr,"allocate optional unreadable type fixture");
        auto* split=allocation+page-0x80;std::memcpy(split,descriptor.data(),descriptor.size());
        DWORD previous{};require(bool(VirtualProtect(allocation,page,PAGE_NOACCESS,&previous)),"protect only optional type page");
        Probe::Eye unknown;SetLastError(0x71727374);Probe::describe(unknown,reinterpret_cast<uintptr_t>(split),true);
        const auto after_unknown=GetLastError();VirtualFree(allocation,0,MEM_RELEASE);
        require(after_unknown==0x71727374 && unknown.records.size()==1 && !unknown.records[0].render_type_read && !unknown.records[0].render_type && unknown.records[0].object==object,"unreadable optional type retains actual record with explicit unknown and preserves LastError");
        Probe::Frame snapshot;snapshot.name="render_type_fixture";snapshot.eyes[0]=eye;snapshot.eyes[1]=unknown;
        auto& type_probe=Probe::get();type_probe.m_directory=directory;type_probe.save(snapshot);
        const auto type_output=directory/"render_type_fixture.draw_visibility.json";
        std::ifstream type_saved{type_output};std::string type_json{std::istreambuf_iterator<char>{type_saved},{}};type_saved.close();std::filesystem::remove(type_output);
        require(type_json.find("\"render_type\":17,\"render_type_read\":true")!=type_json.npos && type_json.find("\"render_type\":0,\"render_type_read\":false")!=type_json.npos,"actual receipt serializes known and unreadable render types independently");
        require(eye.records.size()==1 && eye.records[0].selected==1 && eye.records[0].admitted==1 &&
                eye.records[0].object==object && eye.records[0].prefix_read && eye.records[0].primitive_prefix==primitive,"real bounded native descriptor reads join selection and admission");
        Probe::describe(eye,reinterpret_cast<uintptr_t>(descriptor.data()),false,0);
        Probe::describe(eye,reinterpret_cast<uintptr_t>(descriptor.data()),false,1);
        require(eye.records.size()==3 && eye.records[1].native_view==0 && eye.records[2].native_view==1,"native context selectors remain distinct data and are never relabelled as Scene eyes");
        Probe::s_eye=&eye;Probe::query(1,object,2,0,3);Probe::query(1,object,2,0,3);Probe::query(1,object,2,-1,3);
        require(eye.queries.size()==2 && eye.queries[0].calls==2 && eye.queries[1].result==-1,"zero/missing native query results stay distinct");
        {Probe::Scope disabled{{},{}};require(!Probe::active(),"disabled nested scope masks parent");}require(Probe::active(),"nested scope restores parent");
        auto& probe=Probe::get();require(probe.install(reinterpret_cast<void*>(&select_fixture),reinterpret_cast<void*>(&draw_fixture),directory),"actual native hook install");
        SetLastError(0x11223344);const auto result=select_fixture(11,22,33,-4,5,66,0xfedc);
        require(selects==1 && incoming==0x11223344 && result==0xfedcba9876543210 && GetLastError()==0x80818283,"selector forwards once and preserves all return bits and both last-error values despite refused observation");
        SetLastError(0x55667788);draw_fixture(11,22,33,0xfedcba9876543210,55,0xab);
        require(draws==1 && incoming==0x55667788 && GetLastError()==0x90919293,"draw forwards once with exact native last-error semantics");
        Probe::s_eye=nullptr;
        // Complete a real owned ordinary-pair receipt over current native-shaped
        // memory. This exercises request admission, eye separation and output,
        // rather than testing only rejected/mocked scope branches.
        const auto request=directory/"draw_visibility_request.txt",output=directory/"native_fixture.draw_visibility.json";
        std::filesystem::remove(output);std::ofstream(request)<<"native_fixture\n";
        std::vector<uint8_t> native_list(0x29c7c);std::array<uint8_t,0x2b0> renderer{};
        const auto list=reinterpret_cast<uintptr_t>(native_list.data()),render=reinterpret_cast<uintptr_t>(renderer.data());
        const auto put=[&](size_t offset,const auto& value){std::memcpy(native_list.data()+offset,&value,sizeof(value));};
        const int32_t count=2;const uint32_t counter=4;put(0x29968,count);put(0x29c78,counter);
        auto& entries=NativeCameraEntries::get();entries.enable();const FrameSyncClock::Frame job{0,7,0,0,false};
        std::array<NativeCameraEntries::Entry,2> actual{};
        for(unsigned slot=0;slot<2;++slot){
            const auto entry=list+0x168+slot*0xa60;const uint64_t view_flags=0x20000;put(0x168+slot*0xa60,view_flags);
            auto world=vr_camera::IDENTITY;world[12]=float(slot)*.064f;put(0x168+slot*0xa60+0x34,world);
            auto ticket=entries.begin(list+0x160,slot?NativeCameraEntries::Role::ordinary_right:NativeCameraEntries::Role::main,job);
            entries.finish(*ticket,int32_t(slot),world);actual[slot]={entry,int32_t(slot),NativeCameraEntries::Role::main,world};
        }
        const auto pair=entries.ordinary_pair(list,actual,*entries.revision(list));require(bool(pair),"actual owned pair fixture");
        NativeRenderProbe::Scene scene;scene.refusal=NativeRenderProbe::SceneRefusal::none;scene.dispatch_sequence=1;scene.list=list;scene.renderer=render;scene.camera_revision_before=pair->left.revision;scene.render_counter_before=counter;scene.view_flags_before=0x20000;
        for(unsigned slot=0;slot<2;++slot){
            scene.slot=slot;scene.ordinal=slot+1;scene.entry=actual[slot].address;std::memcpy(renderer.data()+0x140,&scene.entry,8);
            const uint8_t id=uint8_t(slot);put(0x168+slot*0xa60+0x33,id);
            Probe::Scope scope{scene,pair};require(Probe::active(),"current owned eye admits CPU observations");
            Probe::describe(*Probe::s_eye,reinterpret_cast<uintptr_t>(descriptor.data()),false);
            if(!slot)Probe::describe(*Probe::s_eye,reinterpret_cast<uintptr_t>(descriptor.data()),true);
            Probe::query(1,object,int(slot)*2,slot?0:10,3);scope.finish();
        }
        std::ifstream saved{output};std::string json{std::istreambuf_iterator<char>{saved},{}};
        require(json.find("\"eye\":0,\"complete\":true")!=json.npos && json.find("\"eye\":1,\"complete\":true")!=json.npos && json.find("\"admitted\":0")!=json.npos,"both owned eyes serialize distinct native admission evidence without inventing a visibility fix");
        auto stale=scene;stale.camera_revision_before=0;{Probe::Scope scope{stale,pair};require(!Probe::active(),"stale camera provenance refuses capture");}
        saved.close();std::filesystem::remove(request);std::filesystem::remove(output);
        const auto integration_output=directory/"integration_fixture.draw_visibility.json";
        std::filesystem::remove(integration_output);std::ofstream(request)<<"integration_fixture\n";probe.m_next={};
        for(unsigned slot=0;slot<2;++slot){
            const auto offset=0x168+slot*0xa60;
            put(offset+0x3dc,vr_camera::inverse(actual[slot].world));put(offset+0x45c,vr_camera::IDENTITY);
            put(offset+0x160,std::array<float,4>{-.1f,-.1f,.2f,.2f});put(offset+0x304,80.f);put(offset+0x308,80.f);put(offset+0x30c,1.f);
        }
        auto& render_probe=NativeRenderProbe::get();
        require(render_probe.install(reinterpret_cast<void*>(&dispatch_fixture),[&]{return list;},[]{return true;}),"actual render dispatcher hook installs");
        require(render_probe.attach_scene_hook(reinterpret_cast<void*>(&scene_fixture)),"actual Scene integration enabled");
        unsigned observed{};
        scene_steps=[&](void* native_renderer,uint32_t,int32_t slot){
            // Precisely the production order: active entry still belongs to the
            // preceding Scene at hook entry, then native selects this eye.
            uintptr_t previous{};std::memcpy(&previous,static_cast<uint8_t*>(native_renderer)+0x140,8);
            require(previous!=actual[slot].address,"fixture really begins with previous eye active");
            const auto entry=actual[slot].address;std::memcpy(static_cast<uint8_t*>(native_renderer)+0x140,&entry,8);
            require(Probe::active(),"actual observe_scene entry preserves pending owned probe through native active-entry selection");
            if(slot==1)geometry[0x178]=2;
            Probe::describe(*Probe::s_eye,reinterpret_cast<uintptr_t>(descriptor.data()),false,slot);
            Probe::describe(*Probe::s_eye,reinterpret_cast<uintptr_t>(descriptor.data()),true,slot);++observed;
            geometry[0x178]=1;
        };
        dispatch_steps=[&](void* native_renderer,uint32_t flags){for(int slot=0;slot<2;++slot)NativeRenderProbe::observe_scene(native_renderer,flags,slot,&scene_fixture);};
        std::memcpy(renderer.data()+0x140,&actual[1].address,8);
        void(*volatile actual_dispatch)(void*,uint32_t)=&dispatch_fixture;actual_dispatch(renderer.data(),0);
        require(observed==2 && render_probe.latest() && render_probe.latest()->ordinary_pair && render_probe.latest()->scene_calls==2,"real dispatcher observes the complete owned two-Scene path");
        std::ifstream integration_saved{integration_output};std::string integrated{std::istreambuf_iterator<char>{integration_saved},{}};
        require(integrated.find("\"eye\":0,\"complete\":true")!=integrated.npos && integrated.find("\"eye\":1,\"complete\":true")!=integrated.npos,"production hook ordering saves both complete eye inventories");
        require(integrated.find("\"class_name\":\""+console_class+"\"")!=integrated.npos && integrated.find("\"class_chain_stable\":true")!=integrated.npos,"actual hooked Scene path serializes verified identity evidence rather than raw-prefix guesses");
        require(integrated.find("\"kind_flags\":2,\"mask\":8")!=integrated.npos,"actual hooked Scene path retains unrelated static identity as unsupported without rejecting or inventing a class");
        integration_saved.close();std::filesystem::remove(request);std::filesystem::remove(integration_output);
        const auto joined_control=directory/"joined_capture_request.txt",joined_output=directory/"joined_fixture.draw_visibility.json";
        std::filesystem::remove(joined_output);const auto joined_count=probe.m_count;
        std::ofstream(joined_control)<<"joined_fixture\n";probe.m_next={};
        actual_dispatch(renderer.data(),0);
        const auto joined_scene=render_probe.latest();require(joined_scene && std::filesystem::exists(joined_output),"joined request traverses the actual owned two-Scene hook and writes its receipt");
        const auto joined_dispatch=joined_scene->sequence,joined_render=pair->left.render_id;
        require(!OrdinaryJoinedCapture::get().claim(joined_dispatch+1,joined_render,joined_render,true),"a later submitted frame never substitutes for the observed draw");
        const auto joined_target=OrdinaryJoinedCapture::get().claim(joined_dispatch,joined_render,joined_render,true);
        require(joined_target && joined_target->id=="joined_fixture","only actual completed draw publishes the exact named GPU target");
        std::ifstream joined_saved{joined_output};std::string joined_json{std::istreambuf_iterator<char>{joined_saved},{}};joined_saved.close();
        require(joined_json.find("\"joined_capture_requested\":true")!=joined_json.npos,"draw receipt labels the explicit joined request");
        std::filesystem::remove(joined_control);std::filesystem::remove(joined_output);probe.m_count=joined_count;
        // Authenticate the command implementation through the actual hooked
        // Scene/draw path. A pointer read never invokes that implementation.
        const std::string player_class="pc_marine_pve_story_titus_authority";
        const uint64_t hand_handle=(geometry_handle&0xffffffffffffULL)|(uint64_t(469)<<48);std::memcpy(descriptor.data()+0x100,&hand_handle,8);
        const auto player_text=reinterpret_cast<uintptr_t>(player_class.data());const uint32_t player_length=uint32_t(player_class.size());
        std::memcpy(class_owner.data()+0x30,&player_text,8);std::memcpy(class_owner.data()+0x38,&player_length,4);
        const auto command_vtable=reinterpret_cast<uintptr_t>(binding_vtable.data()),callback=reinterpret_cast<uintptr_t>(&draw_fixture);
        binding_vtable[0x2d8/8]=callback;uintptr_t command=command_vtable;const auto command_pointer=reinterpret_cast<uintptr_t>(&command);
        const auto image_base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* image_nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(image_base+reinterpret_cast<const IMAGE_DOS_HEADER*>(image_base)->e_lfanew);
        native_command_binding::Image binding_image;binding_image.base=image_base;binding_image.size=image_nt->OptionalHeader.SizeOfImage;
        binding_image.executable[0]={callback,callback+1};binding_image.count=1;binding_image.valid=true;
        const auto player_identity=native_geometry_identity::capture(descriptor_pointer);
        auto memory=[](uintptr_t p,void* target,size_t size){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&n) && n==size;};
        auto binding=native_command_binding::capture(command_pointer,descriptor_pointer,player_identity,binding_image,memory);
        require(!binding.mask && binding.owner_stable && binding.pointers_stable && binding.callback_rva==callback-image_base && binding.reads<=64,"bounded native command binding follows stable current player owner and executable implementation without invoking it");
        auto other_part=player_identity;other_part.handle^=uint64_t(1)<<48;
        require(native_command_binding::capture(command_pointer,descriptor_pointer,other_part,binding_image,memory).mask==native_command_binding::unqualified_owner,"unrelated player part does not spend command binding reads");
        SetLastError(0xabcdef01);native_command_binding::capture(command_pointer,descriptor_pointer,player_identity,binding_image,[&](uintptr_t p,void* t,size_t n){const bool ok=memory(p,t,n);SetLastError(0xbadbad);return ok;});
        require(GetLastError()==0xabcdef01,"standalone command observation preserves LastError even when reader changes it");
        auto unavailable=native_command_binding::capture(command_pointer,descriptor_pointer,player_identity,binding_image,[&](uintptr_t p,void* t,size_t n){return p!=command_pointer && memory(p,t,n);});
        require(unavailable.mask==native_command_binding::unreadable,"unreadable native command binding is optional unknown");
        unavailable=native_command_binding::capture(command_pointer,descriptor_pointer,player_identity,binding_image,[&](uintptr_t p,void* t,size_t n){return p!=command_vtable+0x2d8 && memory(p,t,n);});
        require(unavailable.mask==native_command_binding::unreadable,"unreadable callback remains optional unknown");
        unsigned command_reads{};unavailable=native_command_binding::capture(command_pointer,descriptor_pointer,player_identity,binding_image,[&](uintptr_t p,void* t,size_t n){const bool ok=memory(p,t,n);if(p==command_pointer && ++command_reads==1)command=0;return ok;});command=command_vtable;
        require(unavailable.mask==native_command_binding::changed,"changing command wrapper cannot publish a callback identity");
        unsigned callback_reads{};unavailable=native_command_binding::capture(command_pointer,descriptor_pointer,player_identity,binding_image,[&](uintptr_t p,void* t,size_t n){const bool ok=memory(p,t,n);if(p==command_vtable+0x2d8 && ++callback_reads==1)binding_vtable[0x2d8/8]=0;return ok;});binding_vtable[0x2d8/8]=callback;
        require(unavailable.mask==native_command_binding::changed,"changing callback cannot publish its old implementation identity");
        binding_vtable[0x2d8/8]=command_vtable;unavailable=native_command_binding::capture(command_pointer,descriptor_pointer,player_identity,binding_image,memory);binding_vtable[0x2d8/8]=callback;
        require(unavailable.mask==native_command_binding::outside_image,"nonexecutable callback pointer is never authenticated");
        command=1;unavailable=native_command_binding::capture(command_pointer,descriptor_pointer,player_identity,binding_image,memory);command=command_vtable;
        require(unavailable.mask==native_command_binding::outside_image,"vtable outside verified native image refuses before slot dereference");
        unavailable=native_command_binding::capture(command_pointer,descriptor_pointer,player_identity,binding_image,[](uintptr_t,void*,size_t)->bool{throw std::runtime_error("read failure");});
        require(unavailable.mask==native_command_binding::unreadable,"thrown observation remains optional unknown");
        auto stale_identity=player_identity;stale_identity.properties^=8;
        require(native_command_binding::capture(command_pointer,descriptor_pointer,stale_identity,binding_image,memory).mask==native_command_binding::unqualified_owner,"stale descriptor owner refuses command authentication");
        std::vector<uint8_t> pe(0x3000);const uintptr_t pe_base=0x100000;const uint16_t mz=0x5a4d,machine=0x8664,section_count=1,optional_size=240,optional_magic=0x20b;
        const uint32_t nt_offset=0x80,nt_signature=0x4550,retail_stamp=0x6ac3e716,image_size=0x3000,code_rva=0x1000,code_size=0x1000,code_flags=0x20000000;
        const auto pe_put=[&](size_t p,const auto& value){std::memcpy(pe.data()+p,&value,sizeof(value));};
        pe_put(0,mz);pe_put(0x3c,nt_offset);pe_put(0x80,nt_signature);pe_put(0x84,machine);pe_put(0x86,section_count);pe_put(0x88,retail_stamp);pe_put(0x94,optional_size);pe_put(0x98,optional_magic);pe_put(0xd0,image_size);
        pe_put(0x190,code_size);pe_put(0x194,code_rva);pe_put(0x1ac,code_flags);
        auto pe_read=[&](uintptr_t p,void* t,size_t n){if(p<pe_base || n>pe.size() || p-pe_base>pe.size()-n)return false;std::memcpy(t,pe.data()+p-pe_base,n);return true;};
        require(native_command_binding::inspect_image(pe_base,pe_read).code(pe_base+0x1000),"exact Retail PE64 signature and executable section establish code bounds");
        pe[0x88]^=1;require(!native_command_binding::inspect_image(pe_base,pe_read).valid,"other native image stamp refuses command provenance");
        const auto binding_output=directory/"binding_fixture.draw_visibility.json";std::filesystem::remove(binding_output);std::ofstream(request)<<"binding_fixture\n";probe.m_next={};
        const auto old_count=probe.m_count;probe.m_binding_image=binding_image;unsigned actual_binding_calls{};
        binding_draw_steps=[&](uintptr_t a,uintptr_t,uintptr_t c,uint64_t d,uintptr_t e,uint8_t f){require(a==command_pointer && c==descriptor_pointer && d==7 && e==8 && f==9 && incoming==0x12341234,"binding metadata preserves every actual native draw argument and incoming error");++actual_binding_calls;};
        scene_steps=[&](void* native_renderer,uint32_t,int32_t slot){const auto entry=actual[slot].address;std::memcpy(static_cast<uint8_t*>(native_renderer)+0x140,&entry,8);SetLastError(0x12341234);draw_fixture(command_pointer,entry+0x33,descriptor_pointer,7,8,9);require(GetLastError()==0x90919293,"binding observation preserves native outgoing error");};
        actual_dispatch(renderer.data(),0);binding_draw_steps={};
        std::ifstream binding_saved{binding_output};std::string binding_json{std::istreambuf_iterator<char>{binding_saved},{}};
        require(actual_binding_calls==2 && binding_json.find("\"command_binding\":{\"mask\":0")!=binding_json.npos && binding_json.find("\"callback_rva\":"+std::to_string(callback-image_base))!=binding_json.npos && binding_json.find("\"native_calls\":false,\"gpu_binding_verified\":false,\"atomic\":false")!=binding_json.npos,"actual native draw hook publishes bounded implementation identity for both player eyes without a GPU claim");
        binding_saved.close();std::filesystem::remove(binding_output);std::filesystem::remove(request);probe.m_count=old_count;
        std::memcpy(class_owner.data()+0x30,&console_text,8);std::memcpy(class_owner.data()+0x38,&console_length,4);
        std::memcpy(descriptor.data()+0x100,&geometry_handle,8);
        // Native-shaped, non-executed Retail image and command cache. GPU VAs
        // are deliberately invalid CPU pointers: the observer must not read them.
        std::vector<uint8_t> binding_module(0x5000000),binding_command(0x12000);
        pe[0x88]^=1;std::memcpy(binding_module.data(),pe.data(),pe.size());
        const auto module_base=reinterpret_cast<uintptr_t>(binding_module.data());
        const uint32_t module_size=uint32_t(binding_module.size()),binder_rva=0x2141f10,section_start=0x2140000,section_size=0x10000;
        std::memcpy(binding_module.data()+0xd0,&module_size,4);std::memcpy(binding_module.data()+0x190,&section_size,4);std::memcpy(binding_module.data()+0x194,&section_start,4);
        std::memcpy(binding_module.data()+binder_rva,SYNTH(native_command_resources::BINDER_SIGNATURE).data(),16);
        const std::array<uint32_t,2> object_slot{5,2},material_slot{8,2};
        std::memcpy(binding_module.data()+0x36d0c98,object_slot.data(),8);std::memcpy(binding_module.data()+0x36d0cb0,material_slot.data(),8);
        const auto resource_callback=module_base+binder_rva,resource_vtable=module_base+0x1000,resource_command=reinterpret_cast<uintptr_t>(binding_command.data());
        std::memcpy(binding_module.data()+0x1000+0x2d8,&resource_callback,8);std::memcpy(binding_command.data(),&resource_vtable,8);
        const auto resource_image=native_command_binding::inspect_image(module_base,memory);require(resource_image.valid,"native-shaped Retail image qualifies actual resource binder and slot constants without executing fake code");
        std::memcpy(class_owner.data()+0x30,&player_text,8);std::memcpy(class_owner.data()+0x38,&player_length,4);std::memcpy(descriptor.data()+0x100,&hand_handle,8);
        const auto resource_owner=native_geometry_identity::capture(descriptor_pointer);
        const auto resource_binding=native_command_binding::capture(resource_command,descriptor_pointer,resource_owner,resource_image,memory);
        require(!resource_binding.mask && resource_binding.callback_rva==binder_rva,"exact supported resource implementation is authenticated");
        const auto put_resource=[&](size_t offset,uint64_t gpu_base,uint32_t first,uint32_t count){std::memcpy(descriptor.data()+offset,&gpu_base,8);std::memcpy(descriptor.data()+offset+8,&first,4);std::memcpy(descriptor.data()+offset+12,&count,4);};
        put_resource(0x110,0x100000000ULL,7,0x80000009);put_resource(0x138,0x200000000ULL,11,0x80000100);
        // Actual draw-time fixed skin snapshot: normal instance palette and
        // per-resource renderer CPU copy, deliberately separate allocations.
        std::array<uint8_t,0x2c8> skin_resource{},skin_header{},skin_container{},skin_record{};
        std::vector<uint8_t> skin_manager(0x720),skin_component(0x82338);
        std::array<uintptr_t,2> skin_components{};std::array<uintptr_t,1> skin_records{};
        std::vector<float> skin_c0(188*16),skin_matrix(188*12),skin_dual(188*8),skin_staging(188*20);
        const auto skin_put=[&](auto& target,size_t offset,const auto& value){std::memcpy(reinterpret_cast<uint8_t*>(target.data())+offset,&value,sizeof(value));};
        const auto skin_address=[](const auto& target){return reinterpret_cast<uintptr_t>(target.data());};
        std::memcpy(binding_module.data()+0x1e778f0,SYNTH(native_draw_skin::PUBLISHER).data(),16);std::memcpy(binding_module.data()+0x1ed7ba0,SYNTH(native_draw_skin::PRODUCER).data(),16);
        const int32_t skin_index=0;std::memcpy(binding_module.data()+0x4d47f3c,&skin_index,4);
        skin_put(geometry,0x250,skin_address(skin_resource));skin_put(skin_resource,0x28,skin_address(skin_header));skin_put(skin_resource,0x2c4,int32_t{0});skin_put(skin_header,0x58,int32_t{188});
        skin_put(geometry,0x30,uint32_t{0x200});skin_put(geometry,0x2f0,uint32_t{1});skin_put(geometry,0xc0,skin_address(skin_c0));skin_put(geometry,0x210,skin_address(skin_container));
        skin_put(skin_container,0,skin_address(skin_matrix));skin_put(skin_container,8,int32_t{188});skin_put(skin_container,0x10,skin_address(skin_dual));skin_put(skin_container,0x18,int32_t{188});
        skin_put(geometry,0x240,int32_t{0});skin_put(geometry,0x2c0,uintptr_t{});skin_put(geometry,0x42,int16_t{});skin_put(geometry,0x280,skin_address(skin_manager));
        skin_components[1]=skin_address(skin_component);skin_records[0]=skin_address(skin_record);
        skin_put(skin_manager,0x710,skin_address(skin_components));skin_put(skin_manager,0x718,int32_t{1});skin_put(skin_component,0x82330,skin_address(skin_records));
        skin_put(skin_record,0xc,int32_t{0});skin_put(skin_record,0x10,int32_t{0});skin_put(skin_record,0x7c,int32_t{1});skin_put(skin_record,0xc0,int16_t{188});skin_put(skin_record,0x30,skin_address(skin_staging));skin_put(skin_record,0x40,uint32_t{0});skin_put(skin_record,0x48,uint32_t{188*3});
        for(const auto id:{114,118,120}){skin_matrix[id*12]=24.25f;skin_staging[id*12]=24.25f;skin_dual[id*8]=1;skin_staging[188*12+id*8]=1;skin_c0[id*16+12]=float(id);}
        auto skin_snapshot=native_draw_skin::capture(descriptor_pointer,resource_owner,resource_image,memory);
        require(!skin_snapshot.mask && skin_snapshot.dirty==1 && skin_snapshot.entries[0].id==114 && skin_snapshot.entries[1].id==118 && skin_snapshot.entries[2].id==120 && skin_snapshot.entries[0].palette[0]==24.25f && skin_snapshot.entries[0].staging[0]==24.25f && skin_snapshot.entries[0].dual[0]==1 && skin_snapshot.entries[0].active && skin_snapshot.reads<=256,"exact fixed logical skin cells are captured despite dirty C0 without forcing evaluation or assuming GPU use");
        SetLastError(0xabababab);native_draw_skin::capture(descriptor_pointer,resource_owner,resource_image,[&](uintptr_t p,void* t,size_t n){const auto ok=memory(p,t,n);SetLastError(0x99999999);return ok;});require(GetLastError()==0xabababab,"fixed skin snapshot independently preserves LastError");
        unsigned skin_source_reads{};auto torn_skin=native_draw_skin::capture(descriptor_pointer,resource_owner,resource_image,[&](uintptr_t p,void* t,size_t n){const bool ok=memory(p,t,n);if(p==skin_address(skin_matrix)+114*48 && ++skin_source_reads==1)skin_matrix[114*12]=99;return ok;});skin_matrix[114*12]=24.25f;
        require(torn_skin.mask==native_draw_skin::changed,"palette change during fixed draw snapshot remains unknown");
        skin_put(skin_container,8,int32_t{120});require(native_draw_skin::capture(descriptor_pointer,resource_owner,resource_image,memory).mask==native_draw_skin::bounds,"fixed hand IDs outside current native palette refuse rather than indexing wrong cells");skin_put(skin_container,8,int32_t{188});
        auto skin_other=resource_owner;skin_other.handle^=uint64_t(1)<<48;require(native_draw_skin::capture(descriptor_pointer,skin_other,resource_image,memory).mask==native_draw_skin::unqualified,"other player part does not enter fixed hand skin observer");
        unsigned publication_reads{};auto unstable_publication=native_draw_skin::capture(descriptor_pointer,resource_owner,resource_image,[&](uintptr_t p,void* t,size_t n){const bool ok=memory(p,t,n);if(p==skin_address(geometry)+0xd0){if(++publication_reads==1)skin_put(geometry,0xd0,resource_owner.palette_pointer^uintptr_t{8});else if(publication_reads==2)skin_put(geometry,0xd0,resource_owner.palette_pointer);}return ok;});
        require(unstable_publication.mask==native_draw_skin::unqualified,"known unstable D0 publication identity is rejected even when it returns to the expected pointer before pose reads");
        unsigned dirty_reads{};auto dirty_torn=native_draw_skin::capture(descriptor_pointer,resource_owner,resource_image,[&](uintptr_t p,void* t,size_t n){const bool ok=memory(p,t,n);if(p==skin_address(geometry)+0x2f0 && ++dirty_reads==1)skin_put(geometry,0x2f0,uint32_t{3});return ok;});skin_put(geometry,0x2f0,uint32_t{1});
        require(dirty_torn.mask==native_draw_skin::changed,"native dirty-state transition across CPU reads refuses stable snapshot");
        const auto old_skin_dual=skin_dual[114*8];skin_dual[114*8]=std::numeric_limits<float>::quiet_NaN();require(native_draw_skin::capture(descriptor_pointer,resource_owner,resource_image,memory).mask==native_draw_skin::nonfinite,"nonfinite selected native data cannot produce invalid JSON or a pose verdict");skin_dual[114*8]=old_skin_dual;
        binding_module[0x1e778f0]^=1;require(native_draw_skin::capture(descriptor_pointer,resource_owner,resource_image,memory).mask==native_draw_skin::unsupported,"unknown native CPU staging implementation refuses layout");binding_module[0x1e778f0]^=1;
        require(native_draw_skin::capture(descriptor_pointer,resource_owner,resource_image,[](uintptr_t,void*,size_t)->bool{throw std::runtime_error("fixture read");}).mask==native_draw_skin::unreadable,"reader exceptions remain optional unknown and cannot escape native draw");
        auto missing_staging=native_draw_skin::capture(descriptor_pointer,resource_owner,resource_image,[&](uintptr_t p,void* t,size_t n){return p!=skin_address(skin_staging)+114*48 && memory(p,t,n);});require(missing_staging.mask==native_draw_skin::unreadable,"unreadable selected CPU staging data is not replaced by palette-only success");
        constexpr uintptr_t crossing_matrix=0x800000000000ULL-114*48-16;skin_put(skin_container,0,crossing_matrix);bool crossing_read{};
        const auto crossing_skin=native_draw_skin::capture(descriptor_pointer,resource_owner,resource_image,[&](uintptr_t p,void* t,size_t n){if(p>=0x800000000000ULL-16)crossing_read=true;return memory(p,t,n);});skin_put(skin_container,0,skin_address(skin_matrix));
        require(crossing_skin.mask==native_draw_skin::unreadable && !crossing_read && crossing_skin.reads<=256,"CPU cell spanning the canonical user-address boundary is refused before invoking the reader");
        // Normal object constant stream: descriptor GPU base must match the
        // native producer-thread stream's current-frame chunk before CPU reads.
        std::array<uint8_t,0x3a0> cb_device{};std::array<uint8_t,0x30> cb_stream{};std::array<uint8_t,0x38> cb_chunk{};
        std::array<uintptr_t,4> cb_chunks{};std::array<uint32_t,1024> cb_cpu{};const auto cb_start=40u;
        cb_chunks[0]=skin_address(cb_chunk);cb_cpu[cb_start]=17;cb_cpu[cb_start+1]=1234;cb_cpu[cb_start+2]=0;
        skin_put(binding_module,0x4ff0c08,skin_address(cb_device));skin_put(cb_device,0x180,int32_t{1});skin_put(cb_device,0x398,skin_address(cb_stream));
        skin_put(cb_stream,0,skin_address(cb_chunks));skin_put(cb_stream,8,int32_t{1});skin_put(cb_stream,0x14,int32_t{256});skin_put(cb_stream,0x18,int32_t{0});skin_put(cb_stream,0x1c,int32_t{0});
        skin_put(cb_chunk,0x10,skin_address(cb_cpu));skin_put(cb_chunk,0x20,uint64_t{0x100000000ULL});skin_put(cb_chunk,0x28,int32_t{3});descriptor[0x1f3]=0;
        std::memcpy(binding_module.data()+0x2082760,SYNTH(native_object_constants::ALLOCATOR).data(),16);std::memcpy(binding_module.data()+0x1f7bcc0,SYNTH(native_object_constants::OBJECT_PRODUCER).data(),16);std::memcpy(binding_module.data()+0x1f7c980,SYNTH(native_object_constants::OBJECT_FILL).data(),16);
        const auto object_cb=native_object_constants::capture(descriptor_pointer,resource_owner,resource_image,memory);
        require(!object_cb.mask && object_cb.cpu_address==skin_address(cb_cpu)+160 && object_cb.words[0]==17 && object_cb.words[1]==1234 && object_cb.words[2]==0 && object_cb.native_instance_index==0 && object_cb.selected_chunk==0 && object_cb.reads<=256,"object CB CPU source requires exact producer stream and native GPU-base match before copying nine rows");
        std::array<uint8_t,0x38> cb_later_chunk{};skin_put(cb_later_chunk,0x20,uint64_t{0x300000000ULL});cb_chunks[1]=skin_address(cb_chunk);cb_chunks[3]=skin_address(cb_later_chunk);
        skin_put(cb_stream,8,int32_t{2});skin_put(cb_stream,0x18,int32_t{1});skin_put(cb_stream,0x1c,int32_t{1});
        const auto prior_cb=native_object_constants::capture(descriptor_pointer,resource_owner,resource_image,memory);require(!prior_cb.mask && prior_cb.frame==1 && prior_cb.selected_chunk==0 && prior_cb.current_chunk==1 && prior_cb.words[0]==17,"current-frame earlier chunk remains attributable after allocator advances to a later chunk");
        cb_chunks[3]=skin_address(cb_chunk);require(native_object_constants::capture(descriptor_pointer,resource_owner,resource_image,memory).mask==native_object_constants::gpu_base_unmatched,"ambiguous current-frame GPU-base matches cannot choose a CPU allocation");
        skin_put(cb_stream,8,int32_t{1});skin_put(cb_stream,0x18,int32_t{0});skin_put(cb_stream,0x1c,int32_t{0});
        SetLastError(0xabcdef12);native_object_constants::capture(descriptor_pointer,resource_owner,resource_image,[&](uintptr_t p,void* t,size_t n){const auto ok=memory(p,t,n);SetLastError(4);return ok;});require(GetLastError()==0xabcdef12,"object constants standalone snapshot preserves LastError");
        cb_cpu[cb_start+2]=3;skin_put(geometry,0x42,int16_t{7});auto independent_cb=native_object_constants::capture(descriptor_pointer,resource_owner,resource_image,memory);
        require(!independent_cb.mask && independent_cb.words[2]==3 && independent_cb.native_instance_index==7,"nonzero draw-entry ordinal and different resource instance-row index are valid independent domains");
        std::ostringstream independent_cb_json;native_object_constants::write(independent_cb_json,independent_cb);const auto independent_text=independent_cb_json.str();
        require(independent_text.find("\"word2_semantic\":\"draw_entry_ordinal_within_logical_mesh_span\"")!=independent_text.npos && independent_text.find("\"native_instance_index_semantic\":\"resource_instance_row_local_index\"")!=independent_text.npos && independent_text.find("\"index_domains_independent\":true")!=independent_text.npos && independent_text.find("comparison_available")==independent_text.npos && independent_text.find("word2_matches")==independent_text.npos,"object CB schema cannot advertise unrelated native index equality as a validity or skinning gate");
        cb_cpu[cb_start+2]=0;skin_put(geometry,0x42,int16_t{});
        skin_put(cb_chunk,0x20,uint64_t{0x200000000ULL});bool cb_wrong_base_read{};auto wrong_cb=native_object_constants::capture(descriptor_pointer,resource_owner,resource_image,[&](uintptr_t p,void* t,size_t n){if(p==skin_address(cb_cpu)+160)cb_wrong_base_read=true;return memory(p,t,n);});require(wrong_cb.mask==native_object_constants::gpu_base_unmatched && !cb_wrong_base_read,"unmatched descriptor GPU base refuses before reading guessed CPU constants");skin_put(cb_chunk,0x20,uint64_t{0x100000000ULL});
        descriptor[0x1f3]=1;require(native_object_constants::capture(descriptor_pointer,resource_owner,resource_image,memory).mask==native_object_constants::bounds,"out-of-range producer thread cannot index another stream");descriptor[0x1f3]=0;
        skin_put(cb_stream,0x28,uintptr_t{1});require(native_object_constants::capture(descriptor_pointer,resource_owner,resource_image,memory).mask==native_object_constants::getter_unavailable,"fallback allocator requires a prearmed original-getter source mapping");skin_put(cb_stream,0x28,uintptr_t{});
        skin_put(cb_stream,0x14,int32_t{15});require(native_object_constants::capture(descriptor_pointer,resource_owner,resource_image,memory).mask==native_object_constants::bounds,"flagged record range must fit the native stream capacity");skin_put(cb_stream,0x14,int32_t{256});
        unsigned cb_cell_reads{};auto torn_cb=native_object_constants::capture(descriptor_pointer,resource_owner,resource_image,[&](uintptr_t p,void* t,size_t n){const auto ok=memory(p,t,n);if(p==skin_address(cb_cpu)+160 && ++cb_cell_reads==1)cb_cpu[cb_start]=18;return ok;});require(torn_cb.mask==native_object_constants::changed,"changed object constant cells remain unavailable");cb_cpu[cb_start]=17;
        unsigned cb_base_reads{};auto changed_cb_source=native_object_constants::capture(descriptor_pointer,resource_owner,resource_image,[&](uintptr_t p,void* t,size_t n){const auto ok=memory(p,t,n);if(p==skin_address(cb_chunk)+0x20 && ++cb_base_reads==1)skin_put(cb_chunk,0x20,uint64_t{0x200000000ULL});return ok;});require(changed_cb_source.mask==native_object_constants::changed,"changed chunk GPU-base ownership refuses consumed CPU data");skin_put(cb_chunk,0x20,uint64_t{0x100000000ULL});
        binding_module[0x2082760]^=1;require(native_object_constants::capture(descriptor_pointer,resource_owner,resource_image,memory).mask==native_object_constants::unsupported,"unknown allocator implementation refuses normal CPU pointer arithmetic");binding_module[0x2082760]^=1;
        auto other_cb_owner=resource_owner;other_cb_owner.handle^=uint64_t(1)<<48;require(native_object_constants::capture(descriptor_pointer,other_cb_owner,resource_image,memory).mask==native_object_constants::unqualified,"object constant observation remains admitted HAND469 only");
        const auto cb_old_base=skin_address(cb_cpu);skin_put(cb_chunk,0x10,uintptr_t{0x800000000000ULL-200});bool cb_crossing_read{};auto crossing_cb=native_object_constants::capture(descriptor_pointer,resource_owner,resource_image,[&](uintptr_t p,void* t,size_t n){if(p>=0x800000000000ULL-40)cb_crossing_read=true;return memory(p,t,n);});require(crossing_cb.mask==native_object_constants::unreadable && !cb_crossing_read,"object CPU span crossing canonical limit refuses before reader invocation");skin_put(cb_chunk,0x10,cb_old_base);
        require(native_object_constants::capture(descriptor_pointer,resource_owner,resource_image,[](uintptr_t,void*,size_t)->bool{throw std::runtime_error("fixture read");}).mask==native_object_constants::unreadable,"object CPU reader exception remains optional unavailable");
        const auto set_native_cache=[&]{for(const auto offset:{size_t(0x110),size_t(0x138)}){uint64_t base{};uint32_t first{},count{};std::memcpy(&base,descriptor.data()+offset,8);std::memcpy(&first,descriptor.data()+offset+8,4);std::memcpy(&count,descriptor.data()+offset+12,4);
            const auto cache=offset==0x110?0x11d20:0x11d50;const auto va=base+uint64_t(first)*16;const uint32_t bytes=(count*16u+255u)&0xffffff00u;std::memcpy(binding_command.data()+cache,&va,8);std::memcpy(binding_command.data()+cache+8,&bytes,4);}};
        auto resources=native_command_resources::before(descriptor_pointer,resource_owner,resource_binding,resource_image,memory);
        require(!resources.mask && resources.records[0].expected_gpu_va==0x100000070ULL && resources.records[0].expected_bytes==256 && resources.records[1].expected_bytes==4096 && !resources.post_observed,"encoded count flags use exact native uint32 size/alignment and GPU addresses are metadata only");
        set_native_cache();SetLastError(0xabcd1234);native_command_resources::after(resources,resource_command,descriptor_pointer,resource_owner,resource_binding,resource_image,[&](uintptr_t p,void* t,size_t n){const auto ok=memory(p,t,n);SetLastError(0x9999);return ok;});
        require(!resources.post_mask && resources.post_observed && resources.records[0].cache_matches && resources.records[1].cache_matches && resources.reads>88 && resources.reads<=256 && GetLastError()==0xabcd1234,"post-native command cache matches exact current resource records while preserving LastError and counting owner validations");
        auto missing_resource=native_command_resources::before(descriptor_pointer,resource_owner,resource_binding,resource_image,[&](uintptr_t p,void* t,size_t n){return p!=descriptor_pointer+0x110 && memory(p,t,n);});
        require(!missing_resource.mask && missing_resource.records[0].mask==native_command_resources::unreadable && !missing_resource.records[1].mask,"one unreadable optional resource preserves the independent record");
        put_resource(0x110,0x100000000ULL,7,9);auto disabled_resource=native_command_resources::before(descriptor_pointer,resource_owner,resource_binding,resource_image,memory);native_command_resources::after(disabled_resource,resource_command,descriptor_pointer,resource_owner,resource_binding,resource_image,memory);
        require(!disabled_resource.records[0].native_bind_condition && !disabled_resource.records[0].cache_matches,"native-disabled resource is not mislabeled as an executed matching binding");put_resource(0x110,0x100000000ULL,7,0x80000009);
        auto wrong_resources=native_command_resources::before(descriptor_pointer,resource_owner,resource_binding,resource_image,memory);binding_command[0x11d20]^=1;native_command_resources::after(wrong_resources,resource_command,descriptor_pointer,resource_owner,resource_binding,resource_image,memory);binding_command[0x11d20]^=1;
        require(!wrong_resources.post_mask && !wrong_resources.records[0].cache_matches && wrong_resources.records[1].cache_matches,"real mismatched cache is measured without inventing a bind or rejecting independent resource");
        auto stale_resources=native_command_resources::before(descriptor_pointer,resource_owner,resource_binding,resource_image,memory);descriptor[0x118]^=1;native_command_resources::after(stale_resources,resource_command,descriptor_pointer,resource_owner,resource_binding,resource_image,memory);descriptor[0x118]^=1;
        require(stale_resources.records[0].mask==native_command_resources::changed && !stale_resources.records[0].post_read,"resource mutation during native draw refuses its post comparison");
        auto unreadable_resources=native_command_resources::before(descriptor_pointer,resource_owner,resource_binding,resource_image,memory);native_command_resources::after(unreadable_resources,resource_command,descriptor_pointer,resource_owner,resource_binding,resource_image,[&](uintptr_t p,void* t,size_t n){return p!=resource_command+0x11d20 && memory(p,t,n);});
        require(unreadable_resources.records[0].mask==native_command_resources::unreadable,"unreadable post-native cache remains unknown");
        auto owner_resources=native_command_resources::before(descriptor_pointer,resource_owner,resource_binding,resource_image,memory);std::memcpy(descriptor.data()+0x100,&geometry_handle,8);native_command_resources::after(owner_resources,resource_command,descriptor_pointer,resource_owner,resource_binding,resource_image,memory);std::memcpy(descriptor.data()+0x100,&hand_handle,8);
        require(owner_resources.post_mask==native_command_resources::owner_changed,"changed native owner refuses post cache authentication");
        binding_module[0x36d0c98]^=1;require(native_command_resources::before(descriptor_pointer,resource_owner,resource_binding,resource_image,memory).mask==native_command_resources::unsupported,"unknown native slot constants refuse resource layout");binding_module[0x36d0c98]^=1;
        put_resource(0x110,0x100000000ULL,0xffffffff,0x80000009);require(native_command_resources::before(descriptor_pointer,resource_owner,resource_binding,resource_image,memory).records[0].mask==native_command_resources::out_of_bound,"unbounded GPU range metadata is refused before any cache read");put_resource(0x110,0x100000000ULL,7,0x80000009);
        const auto resources_output=directory/"resources_fixture.draw_visibility.json";std::filesystem::remove(resources_output);std::ofstream(request)<<"resources_fixture\n";probe.m_next={};const auto resources_count=probe.m_count;
        probe.m_binding_image=resource_image;unsigned actual_resource_calls{};
        binding_draw_steps=[&](uintptr_t a,uintptr_t,uintptr_t c,uint64_t,uintptr_t,uint8_t){require(a==resource_command && c==descriptor_pointer && incoming==0x12341234,"resource observation preserves actual native arguments and incoming error");++actual_resource_calls;set_native_cache();cb_cpu[cb_start]=42;skin_matrix[114*12]=42.25f;skin_staging[114*12]=42.25f;};
        scene_steps=[&](void* native_renderer,uint32_t,int32_t slot){const auto entry=actual[slot].address;std::memcpy(static_cast<uint8_t*>(native_renderer)+0x140,&entry,8);cb_cpu[cb_start]=17;SetLastError(0x12341234);draw_fixture(resource_command,entry+0x33,descriptor_pointer,7,8,9);require(GetLastError()==0x90919293,"post-native resource observation preserves original outgoing error");
            const auto found=std::ranges::find_if(Probe::s_eye->records,[&](const auto& record){return record.descriptor==descriptor_pointer && record.native_view==slot;});
            require(found!=Probe::s_eye->records.end() && found->skin_snapshot && !found->skin_snapshot->object_before.mask && !found->skin_snapshot->object_after.mask && found->skin_snapshot->object_before.words[0]==17 && found->skin_snapshot->object_after.words[0]==42,"actual hand draw hook samples object constants on both sides of this exact native call");};
        actual_dispatch(renderer.data(),0);binding_draw_steps={};
        std::ifstream resources_saved{resources_output};std::string resources_json{std::istreambuf_iterator<char>{resources_saved},{}};
        require(actual_resource_calls==2 && resources_json.find("\"command_resources\":{\"mask\":0,\"post_mask\":0,\"post_observed\":true")!=resources_json.npos && resources_json.find("\"cache_matches\":true")!=resources_json.npos && resources_json.find("\"gpu_memory_read\":false,\"gpu_binding_verified\":false,\"shader_consumption_verified\":false,\"atomic\":false")!=resources_json.npos,"actual native draw post hook observes matching resource cache without GPU reads or shader claims");
        require(resources_json.find("\"skin_snapshot\":{\"before\":{\"mask\":0")!=resources_json.npos && resources_json.find("\"post_observed\":true,\"after\":{\"mask\":0")!=resources_json.npos && resources_json.find("\"palette_transposed_3x4\":[24.25")!=resources_json.npos && resources_json.find("\"palette_transposed_3x4\":[42.25")!=resources_json.npos,"actual draw skin hook joins before and after CPU cells to the native dispatch instead of later held-pose RPM");
        require(resources_json.find("\"object_constants\":{\"before\":{\"mask\":0")!=resources_json.npos && resources_json.find("\"words\":[17,1234,0")!=resources_json.npos && resources_json.find("\"words\":[42,1234,0")!=resources_json.npos,"actual hand draw hook pairs object CPU constants before and after the exact native call");
        resources_saved.close();std::filesystem::remove(resources_output);std::filesystem::remove(request);probe.m_count=resources_count;
        for(const bool nested:{false,true}){
            const auto temporal_output=directory/(nested?"skin_nested_fixture.draw_visibility.json":"skin_repeat_fixture.draw_visibility.json");std::filesystem::remove(temporal_output);
            std::ofstream(request)<<(nested?"skin_nested_fixture\n":"skin_repeat_fixture\n");probe.m_next={};const auto temporal_count=probe.m_count;unsigned temporal_calls{},per_scene{};
            binding_draw_steps=[&](uintptr_t a,uintptr_t b,uintptr_t c,uint64_t d,uintptr_t e,uint8_t f){
                require(a==resource_command && c==descriptor_pointer,"temporal fixture preserves actual native draw arguments");++temporal_calls;const auto call=++per_scene;set_native_cache();
                if(nested && call==1){SetLastError(0x12341234);draw_fixture(a,b,c,d,e,f);require(GetLastError()==0x90919293,"nested native outgoing LastError remains exact");}
                skin_matrix[114*12]=call==1?42.25f:99.25f;skin_staging[114*12]=skin_matrix[114*12];cb_cpu[cb_start]=call==1?42:99;
            };
            scene_steps=[&](void* native_renderer,uint32_t,int32_t slot){const auto entry=actual[slot].address;std::memcpy(static_cast<uint8_t*>(native_renderer)+0x140,&entry,8);per_scene=0;cb_cpu[cb_start]=17;skin_matrix[114*12]=24.25f;skin_staging[114*12]=24.25f;
                SetLastError(0x12341234);draw_fixture(resource_command,entry+0x33,descriptor_pointer,7,8,9);
                const auto observed=std::ranges::find_if(Probe::s_eye->records,[&](const auto& record){return record.descriptor==descriptor_pointer && record.native_view==slot;});
                require(observed!=Probe::s_eye->records.end() && observed->skin_snapshot && observed->skin_snapshot->post_observed && !observed->skin_snapshot->after.mask && observed->skin_snapshot->before.entries[0].palette[0]==24.25f && observed->skin_snapshot->after.entries[0].palette[0]==42.25f && !observed->skin_snapshot->object_after.mask && observed->skin_snapshot->object_after.words[0]==42,"nested draw cannot steal the outer draw's exact CPU post sample");
                SetLastError(0x12341234);draw_fixture(resource_command,entry+0x33,descriptor_pointer,7,8,9);
                require(GetLastError()==0x90919293 && skin_matrix[114*12]==99.25f && observed->skin_snapshot->after.entries[0].palette[0]==42.25f && observed->skin_snapshot->object_after.words[0]==42,"later same-record draw cannot overwrite the first exact CPU post sample");
            };
            actual_dispatch(renderer.data(),0);binding_draw_steps={};require(temporal_calls==(nested?6u:4u),"repeated and nested original native draws each forward exactly once");
            std::ifstream temporal_saved{temporal_output};std::string temporal_json{std::istreambuf_iterator<char>{temporal_saved},{}};require(temporal_json.find("\"palette_transposed_3x4\":[24.25")!=temporal_json.npos && temporal_json.find("\"palette_transposed_3x4\":[42.25")!=temporal_json.npos && temporal_json.find("\"palette_transposed_3x4\":[99.25")==temporal_json.npos,"temporal receipt serializes first exact call rather than first-pre/last-post aggregate");
            temporal_saved.close();std::filesystem::remove(temporal_output);std::filesystem::remove(request);probe.m_count=temporal_count;
        }
        const auto release_output=directory/"skin_release_fixture.draw_visibility.json";std::filesystem::remove(release_output);
        std::ofstream(request)<<"skin_release_fixture\n";probe.m_next={};const auto release_count=probe.m_count;unsigned release_calls{};std::weak_ptr<native_draw_skin::Evidence> released_token;
        binding_draw_steps=[&](uintptr_t,uintptr_t,uintptr_t c,uint64_t,uintptr_t,uint8_t){++release_calls;
            const auto found=std::ranges::find_if(Probe::s_eye->records,[&](const auto& record){return record.descriptor==c;});
            require(found!=Probe::s_eye->records.end() && found->skin_snapshot,"dispatch-change fixture owns a real draw token");released_token=found->skin_snapshot;found->skin_snapshot.reset();++probe.m_frame->dispatch;
            require(!released_token.expired(),"local draw token remains the last owner across native dispatch change");
        };
        scene_steps=[&](void* native_renderer,uint32_t,int32_t slot){const auto entry=actual[slot].address;std::memcpy(static_cast<uint8_t*>(native_renderer)+0x140,&entry,8);
            SetLastError(0x12341234);draw_fixture(resource_command,entry+0x33,descriptor_pointer,7,8,9);
            require(GetLastError()==0x90919293 && released_token.expired(),"native LastError survives destruction of the last local token after a refused changed-dispatch post");--probe.m_frame->dispatch;
        };
        actual_dispatch(renderer.data(),0);binding_draw_steps={};require(release_calls==2,"dispatch-change original native draw forwards exactly once per eye");std::filesystem::remove(release_output);std::filesystem::remove(request);probe.m_count=release_count;
        std::memcpy(class_owner.data()+0x30,&console_text,8);std::memcpy(class_owner.data()+0x38,&console_length,4);std::memcpy(descriptor.data()+0x100,&geometry_handle,8);
        #include "scene_clear_fixture.inc"
        const auto wrong_output=directory/"wrong_final_camera.draw_visibility.json";
        std::filesystem::remove(wrong_output);std::ofstream(request)<<"wrong_final_camera\n";probe.m_next={};
        scene_steps=[&](void* native_renderer,uint32_t,int32_t){
            // A native Scene which never selects its own entry must not become
            // a completed receipt merely because pre-entry checks are pending.
            const auto wrong=actual[1].address;std::memcpy(static_cast<uint8_t*>(native_renderer)+0x140,&wrong,8);
        };
        actual_dispatch(renderer.data(),0);
        require(!std::filesystem::exists(wrong_output),"wrong final active camera cannot publish a complete two-eye diagnostic receipt");
        std::filesystem::remove(request);
        const auto wrong_joined_output=directory/"joined_wrong_camera.draw_visibility.json";
        std::filesystem::remove(wrong_joined_output);std::ofstream(joined_control)<<"joined_wrong_camera\n";probe.m_next={};
        const auto wrong_joined_count=probe.m_count;actual_dispatch(renderer.data(),0);const auto wrong_joined_scene=render_probe.latest();
        require(wrong_joined_scene && !OrdinaryJoinedCapture::get().claim(wrong_joined_scene->sequence,pair->left.render_id,pair->left.render_id,true) && !std::filesystem::exists(wrong_joined_output),"actual incomplete camera provenance cannot release a joined snapshot");
        std::filesystem::remove(joined_control);probe.m_count=wrong_joined_count;
        // The causal diagnostic goes through the actual MinHook query and
        // dispatcher/Scene path, not a constructed already-owned TLS scope.
        query_call(91,object,0);const auto exact_caller=query_caller;
        auto time=NativeObjectVisibilityCausalProbe::Clock::time_point{}+std::chrono::seconds(10);
        auto& causal=NativeObjectVisibilityCausalProbe::get();
        require(causal.configure(directory,exact_caller,[&]{return time;}),"causal gate configured explicitly");
        using Light=NativeOrdinaryLightVisibility;
        std::optional<NativeCameraEntries::OrdinaryPair> light_pair;auto light_mode=Light::Mode::native;bool fail_manager_read=false;unsigned manager_reads{};
        require(Light::get().install(reinterpret_cast<void*>(&job_fixture),reinterpret_cast<void*>(&query_fixture),
                [&](uintptr_t,uintptr_t){return light_pair;},[&]{++manager_reads;if(fail_manager_read)SetLastError(0xbadbad);return uintptr_t(91);},[&]{return light_mode;},exact_caller-0x1f79d7c),"actual native query hook installed");
        const auto causal_request=directory/"object_visibility_causal_request.txt";
        const auto write_request=[&](const std::string& id,uint64_t pid,const std::string& keys){
            std::filesystem::remove(directory/(id+".object_visibility_causal.jsonl"));
            std::ofstream(causal_request)<<id<<' '<<pid<<" 1000 "<<keys<<'\n';
        };
        std::array<int32_t,2> seen{};bool omit_left=false,wrong_manager=false,wrong_active=false,ambiguous=false,expire_between=false,same_column=false,wrong_caller=false,stale_counter=false,invalidate_pair=false,foreign_context=false,unexpected_column=false;
        int left_result=37,right_result=0;
        const uintptr_t descriptor_address=reinterpret_cast<uintptr_t>(descriptor.data());const uintptr_t descriptor_data=reinterpret_cast<uintptr_t>(&descriptor_address);const uintptr_t descriptor_array=reinterpret_cast<uintptr_t>(&descriptor_data);
        scene_steps=[&](void* native_renderer,uint32_t,int32_t slot){
            const auto selected=actual[wrong_active?1:slot].address;std::memcpy(static_cast<uint8_t*>(native_renderer)+0x140,&selected,8);
            if(slot==0 && omit_left)return;
            native_query_result=slot?right_result:left_result;
            if(slot==1 && stale_counter)put(0x29c78,counter+1);
            const auto before=native_query_calls;
            const uint8_t context=uint8_t(foreign_context?3:slot);
            const int8_t column=slot && unexpected_column?4:slot && !same_column?2:0;
            selection_steps=[&]{const auto value=wrong_caller?query_other_call(slot && wrong_manager?92:91,object,column):query_call(slot && wrong_manager?92:91,object,column);
                if(slot==0 && ambiguous){native_query_result=left_result+1;query_call(91,object,0);}return uintptr_t(value);};
            seen[slot]=int32_t(select_fixture(11,22,descriptor_array,0,1,reinterpret_cast<uintptr_t>(&context),0));selection_steps={};
            require(native_query_calls==before+1+(slot==0 && ambiguous?1:0) && GetLastError()==0x12344321,"causal path calls native exactly once and preserves native last error");
            if(slot==1 && stale_counter)put(0x29c78,counter);
            if(slot==0 && expire_between)time+=std::chrono::milliseconds(1001);
            if(slot==0 && invalidate_pair){auto next=entries.begin(list+0x160,NativeCameraEntries::Role::main,FrameSyncClock::Frame{0,8,0,0,false});require(bool(next),"real ownership revision can advance between native eyes");entries.finish(*next,0,actual[0].world);}
        };
        const auto run_causal=[&]{std::memcpy(renderer.data()+0x140,&actual[1].address,8);actual_dispatch(renderer.data(),0);};
        run_causal();require(seen[1]==0,"no control request preserves native zero");
        write_request("wrong_pid",uint64_t(GetCurrentProcessId())+1,std::to_string(object));time+=std::chrono::milliseconds(300);run_causal();require(seen[1]==0 && !std::filesystem::exists(directory/"wrong_pid.object_visibility_causal.jsonl"),"foreign process request rejected");
        write_request("too_many",GetCurrentProcessId(),"8 16 24 32 40 48 56 64 72");time+=std::chrono::milliseconds(300);run_causal();require(seen[1]==0 && !std::filesystem::exists(directory/"too_many.object_visibility_causal.jsonl"),"more than eight keys rejected");
        write_request("duplicate",GetCurrentProcessId(),std::to_string(object)+" "+std::to_string(object));time+=std::chrono::milliseconds(300);run_causal();require(!std::filesystem::exists(directory/"duplicate.object_visibility_causal.jsonl"),"duplicate whitelist key rejected");
        write_request("unaligned",GetCurrentProcessId(),std::to_string(object+1));time+=std::chrono::milliseconds(300);run_causal();require(!std::filesystem::exists(directory/"unaligned.object_visibility_causal.jsonl"),"unaligned key rejected");
        std::ofstream(causal_request)<<"overlong "<<GetCurrentProcessId()<<" 5001 "<<object<<'\n';time+=std::chrono::milliseconds(300);run_causal();require(!std::filesystem::exists(directory/"overlong.object_visibility_causal.jsonl"),"overlong control window rejected");
        write_request("qualified",GetCurrentProcessId(),std::to_string(object));time+=std::chrono::milliseconds(300);run_causal();
        require(seen[0]==37 && seen[1]==37,"actual same-dispatch positive native left call admits only matching zero right");
        native_query_result=0;require(query_call(91,object,2)==0,"query outside owned Scene remains native");
        right_result=-1;run_causal();require(seen[1]==-1,"missing native right result never replaced");right_result=11;run_causal();require(seen[1]==11,"positive right result untouched");right_result=0;
        omit_left=true;run_causal();require(seen[1]==0,"actual next dispatch cannot reuse previous left query");omit_left=false;
        wrong_manager=true;run_causal();require(seen[1]==0,"manager mismatch cannot borrow left result");wrong_manager=false;
        wrong_caller=true;run_causal();require(seen[1]==0,"other native query caller remains untouched within the same Scene");wrong_caller=false;
        foreign_context=true;run_causal();require(seen[1]==0,"foreign native selector nested under an owned main Scene remains untouched");foreign_context=false;
        unexpected_column=true;run_causal();require(seen[1]==0,"unexpected in-range column cannot borrow visibility from a verified native selector");unexpected_column=false;
        stale_counter=true;run_causal();require(seen[1]==0,"live native counter mismatch prevents borrowed visibility");stale_counter=false;
        ambiguous=true;run_causal();require(seen[1]==0,"disagreeing repeated actual left calls fail closed");ambiguous=false;
        same_column=true;run_causal();require(seen[1]==0,"same native column is not assumed to represent another eye");same_column=false;
        left_result=0;run_causal();require(seen[1]==0,"zero native left result cannot make object visible");left_result=-1;run_causal();require(seen[1]==0,"missing native left result cannot make object visible");left_result=37;
        wrong_active=true;run_causal();require(seen[1]==0,"incorrect active camera rejects actual hook decisions");wrong_active=false;
        light_pair=render_probe.latest()->ordinary_pair;require(light_pair && entries.current(*light_pair),"actual current pair enables rejected light-path fixture");light_mode=Light::Mode::stereo_union;fail_manager_read=true;
        job_steps=[&]{native_query_result=0;const auto before=native_query_calls;require(query_call(91,object,0)==0 && native_query_calls==before+1 && GetLastError()==0x12344321,"rejected active-light manager lookup preserves original native query last error without an extra query");};
        job_fixture(11,22);require(manager_reads>0,"active stereo-light predicate actually reached failing manager-read bookkeeping");job_steps={};light_pair.reset();light_mode=Light::Mode::native;fail_manager_read=false;
        expire_between=true;run_causal();require(seen[1]==0,"request expiry between actual eyes prevents replacement");expire_between=false;
        time+=std::chrono::milliseconds(300);run_causal();require(seen[1]==0,"same request ID cannot silently rearm after expiry");
        write_request("not_whitelisted",GetCurrentProcessId(),std::to_string(object+8));time+=std::chrono::milliseconds(300);run_causal();require(seen[1]==0,"unlisted key untouched even under valid request");
        time+=std::chrono::milliseconds(1001);run_causal();
        const auto causal_output=directory/"qualified.object_visibility_causal.jsonl";
        std::ifstream causal_saved{causal_output};std::string causal_json{std::istreambuf_iterator<char>{causal_saved},{}};
        require(causal_json.find("\"qualification\":\"qualified\"")!=causal_json.npos && causal_json.find("\"original\":0,\"result\":37")!=causal_json.npos &&
                causal_json.find("\"left_column\":0,\"left_result\":37")!=causal_json.npos && causal_json.find("\"event\":\"expired\"")!=causal_json.npos,"receipt records actual original/result, qualification, native columns and terminal expiry");
        causal_saved.close();write_request("stale_owned_pair",GetCurrentProcessId(),std::to_string(object));time+=std::chrono::milliseconds(300);invalidate_pair=true;run_causal();
        require(seen[0]==37 && seen[1]==0,"real camera ownership revision change between native eyes prevents cached result reuse");invalidate_pair=false;
        for(unsigned slot=0;slot<2;++slot){auto restore=entries.begin(list+0x160,slot?NativeCameraEntries::Role::ordinary_right:NativeCameraEntries::Role::main,FrameSyncClock::Frame{0,9,0,0,false});require(bool(restore),"restore fresh actual owned pair");entries.finish(*restore,int32_t(slot),actual[slot].world);}
        time+=std::chrono::milliseconds(1001);run_causal();std::filesystem::remove(causal_request);
        for(auto id:{"qualified","not_whitelisted","stale_owned_pair"})std::filesystem::remove(directory/(std::string(id)+".object_visibility_causal.jsonl"));
        std::vector<std::array<uint8_t,0x198>> descriptors(Probe::LIMIT+3);
        Probe::Eye bounded;for(auto& source:descriptors)Probe::describe(bounded,reinterpret_cast<uintptr_t>(source.data()),false);
        require(bounded.records.size()==Probe::LIMIT && bounded.truncated==3,"descriptor logging has an explicit hard bound, never an unbounded census");
        require(bounded.identity_attempts==64 && bounded.identity_budget_exhausted==Probe::LIMIT-64,"native identity reads have a separate64-per-eye budget even when descriptor inventory is larger");
        Probe::describe(bounded,UINTPTR_MAX,true);require(bounded.truncated==4,"full inventory refuses excess reads");
        for(auto name:{"", "../unsafe", "x y", "a.b"})require(!Probe::safe_name(name),"unsafe output names refused");
        require(Probe::safe_name("console_yaw90_pitchminus10"),"bounded safe request name accepted");
        // Worker observation has no Scene TLS. An explicitly owned producer is
        // copied later to the actual dispatcher list before the receipt joins.
        auto worker_time=NativeWorkerVisibilityProbe::Clock::time_point{}+std::chrono::seconds(10);
        require(NativeWorkerVisibilityProbe::get().install(reinterpret_cast<void*>(&worker_fixture),directory,[&]{return worker_time;},reinterpret_cast<void*>(&builder_fixture)),"actual worker and eleven-argument descriptor builder entry hooks install");
        SetLastError(0x41424344);builder_call(0xff,0,0,-3,0,0,0,0,0,-4,0);
        require(builder_calls==1 && builder_incoming==0x41424344 && GetLastError()==0x61626364,"builder outside admitted worker forwards once with native errors and no observation");
        uint64_t worker_mask=3;const auto handle_pointer=reinterpret_cast<uintptr_t>(&geometry_handle);auto mask_pointer=reinterpret_cast<uintptr_t>(&worker_mask);
        worker_steps=[&](uintptr_t,int32_t,uintptr_t,uintptr_t,uintptr_t){};SetLastError(0x41424344);worker_fixture(0,-17,handle_pointer,mask_pointer,0x1234);
        require(worker_calls==1 && worker_incoming==0x41424344 && GetLastError()==0x51525354,"default-off worker observation forwards native once with exact sticky-error behavior");
        const auto worker_output=directory/"worker_fixture.worker_visibility.json",worker_negative=directory/"worker_negative.worker_visibility.json";
        std::filesystem::remove(worker_output);std::filesystem::remove(worker_negative);std::ofstream(request)<<"worker_fixture\n";
        scene_steps=[&](void* native_renderer,uint32_t,int32_t slot){const auto current=actual[slot].address;std::memcpy(static_cast<uint8_t*>(native_renderer)+0x140,&current,8);};
        actual_dispatch(renderer.data(),0); // Arm from the real owned Scene path.
        std::vector<uint8_t> producer(native_list);const auto producer_render=reinterpret_cast<uintptr_t>(producer.data()),producer_add=producer_render+0x160;
        const auto publish_producer=[&](uint64_t frame){for(unsigned slot=0;slot<2;++slot){auto ticket=entries.begin(producer_add,slot?NativeCameraEntries::Role::ordinary_right:NativeCameraEntries::Role::main,FrameSyncClock::Frame{9,frame,0,0,false});require(bool(ticket),"actual producer add ticket");entries.finish(*ticket,int32_t(slot),actual[slot].world);}};
        publish_producer(100);require(bool(NativeWorkerVisibilityProbe::source(producer_add)),"worker source validates actual producer camera slots and ownership");
        const auto producer_owner=NativeWorkerVisibilityProbe::source(producer_add)->pair;auto wrong_token=producer_owner;++wrong_token.left.token;++wrong_token.right.token;
        require(!NativeWorkerVisibilityProbe::matches(producer_owner,wrong_token),"same producer list/revision/render ID cannot join with a different token");
        auto wrong_revision=producer_owner;++wrong_revision.producer_revision;require(!NativeWorkerVisibilityProbe::matches(producer_owner,wrong_revision),"same render ID/token cannot join another producer revision");
        bool mutate_mask=false,invalidate_source=false;worker_steps=[&](uintptr_t observed_list,int32_t index,uintptr_t observed_handle,uintptr_t observed_mask,uintptr_t options){
            require(observed_list==producer_add && index==-17 && observed_handle==handle_pointer && observed_mask==mask_pointer && options==0x1234,"worker native ABI retains explicit list, signed worker partition and all pointers");
            require(!NativeRenderProbe::current_scene(),"worker fixture has no inherited Scene TLS");
            if(mutate_mask)worker_mask^=2;
            if(invalidate_source)entries.reset(producer_add);
        };
        std::array<uint8_t,0x200> build_scratch{},build_left{},build_right{};std::memcpy(build_scratch.data()+0x100,&geometry_handle,8);
        const auto scratch_pointer=reinterpret_cast<uintptr_t>(build_scratch.data()),left_output=reinterpret_cast<uintptr_t>(build_left.data()),right_output=reinterpret_cast<uintptr_t>(build_right.data());
        const uintptr_t aux6=0xfedcba9876543210,aux7=0x1122334455667788,aux11=0x8877665544332211,material_pointer=0x123456780;
        builder_steps=[&](uintptr_t category,uintptr_t material,uintptr_t,int32_t slot,uintptr_t scratch,uintptr_t a6,uintptr_t a7,uintptr_t,uintptr_t output,int32_t a10,uintptr_t a11){
            require(material==material_pointer && scratch==scratch_pointer && a6==aux6 && a7==aux7 && a10==-42 && a11==aux11,"builder detour forwards all eleven arguments including high bits and signed stack integer");
            const auto h=category==0xfe?geometry_handle+1:geometry_handle;const uint32_t packed=uint32_t(slot)<<13;const uint16_t flags=0x801;
            std::memcpy(reinterpret_cast<void*>(output+0x100),&h,8);std::memcpy(reinterpret_cast<void*>(output+0xf8),&object,8);
            std::memcpy(reinterpret_cast<void*>(output+0x190),&packed,4);std::memcpy(reinterpret_cast<void*>(output+0x194),&flags,2);
            std::memcpy(reinterpret_cast<void*>(output+0x18),&a6,8);
            if(category==0xfd)entries.reset(producer_add);
        };
        const auto emit_build=[&](bool other,int32_t slot,uintptr_t entry,uintptr_t context,uintptr_t category=1){
            const auto before=builder_calls.load();SetLastError(0x41424344);
            (other?builder_other_call:builder_call)(category,material_pointer,entry,slot,scratch_pointer,aux6,aux7,context,slot==0?left_output:right_output,-42,aux11);
            require(builder_calls==before+1 && builder_incoming==0x41424344 && GetLastError()==0x61626364,"nested builder executes native once with exact errors on admitted and refused observation paths");
        };
        auto original_worker_steps=worker_steps;
        worker_steps=[&](uintptr_t list_arg,int32_t index,uintptr_t handle_arg,uintptr_t mask_arg,uintptr_t options){original_worker_steps(list_arg,index,handle_arg,mask_arg,options);
            emit_build(false,0,producer_add+8,handle_pointer);emit_build(true,1,producer_add+8+0xa60,handle_pointer,0xfedc000000000001);
            std::thread foreign_thread{[&]{require(!NativeWorkerVisibilityProbe::s_worker,"new native thread does not inherit authenticated worker TLS");emit_build(false,0,producer_add+8,handle_pointer);}};foreign_thread.join();
            emit_build(false,1,producer_add+8,handle_pointer);emit_build(false,2,producer_add+8+2*0xa60,handle_pointer);
            emit_build(false,0,producer_add+8,1);emit_build(false,0,producer_add+8,handle_pointer,0xfe);
        };
        const auto run_worker=[&]{std::exception_ptr failure;std::thread thread{[&]{try{const auto before=worker_calls,queries=native_query_calls;SetLastError(0x41424344);worker_fixture(producer_add,-17,handle_pointer,mask_pointer,0x1234);require(worker_calls==before+1 && native_query_calls==queries && worker_incoming==0x41424344 && GetLastError()==0x51525354,"real worker thread preserves native once, error and zero additional native queries");}catch(...){failure=std::current_exception();}}};thread.join();if(failure)std::rethrow_exception(failure);};
        run_worker();require(!std::filesystem::exists(worker_output),"producer event is not mislabeled as a current Scene dispatch before copy");
        require(NativeWorkerVisibilityProbe::get().m_rows.size()==1 && NativeWorkerVisibilityProbe::get().m_rows[0].builds.size()==2 && NativeWorkerVisibilityProbe::get().m_builder_validations==6 && NativeWorkerVisibilityProbe::get().m_builder_rejected==4,"actual worker TLS retains both ordinary camera descriptor builds from distinct native callsites, rejecting wrong entry/slot/unreadable context and changed output handle");
        const auto causal_worker_caller=NativeWorkerVisibilityProbe::get().m_rows[0].caller;
        require(NativeWorkerVisibilityProbe::get().m_rows[0].builds[1].category==0xfedc000000000001,"builder retains full first-argument register bits as passed by native category callsites");
        require(!NativeWorkerVisibilityProbe::s_worker,"worker TLS is restored outside native worker scope");
        uint64_t side_effect{};std::memcpy(&side_effect,build_left.data()+0x18,8);require(side_effect==aux6,"passive builder observation preserves native output side effects");
        actual_dispatch(renderer.data(),0);require(!std::filesystem::exists(worker_output),"actual unrelated consumer dispatch cannot claim an uncopied producer event");
        auto copy=entries.begin_copy(list+0x160,producer_add);require(bool(copy),"actual producer-to-render copy starts");
        std::memcpy(native_list.data()+0x160,producer.data()+0x160,native_list.size()-0x160);require(entries.finish_copy(*copy,true),"native exact copy provenance completes");
        actual_dispatch(renderer.data(),0);
        std::ifstream worker_saved{worker_output};std::string worker_json{std::istreambuf_iterator<char>{worker_saved},{}};
        require(worker_json.find("\"outcome\":\"joined\"")!=worker_json.npos && worker_json.find("\"token\":9")!=worker_json.npos && worker_json.find("\"worker_index\":-17")!=worker_json.npos && worker_json.find("\"mask_before\":3,\"mask_after\":3")!=worker_json.npos && worker_json.find(console_class)!=worker_json.npos,"actual copied producer joins forward to both native Scenes with stable console identity/mask evidence");
        require(worker_json.find("\"descriptor_builds\":[{\"category\":1,\"slot\":0")!=worker_json.npos && worker_json.find("\"slot\":1,\"material\":")!=worker_json.npos && worker_json.find("\"interim_flags194\":2049")!=worker_json.npos,"joined receipt records actual per-camera output descriptors separately from later caller-modified flags");
        worker_steps=original_worker_steps;
        worker_saved.close();std::ofstream(request)<<"worker_negative\n";worker_time+=std::chrono::seconds(2);actual_dispatch(renderer.data(),0);
        publish_producer(101);geometry[0x178]=2;run_worker();geometry[0x178]=1; // Unsupported kind remains native.
        // New supported handle uses the same geometry but another mesh index;
        // the conservative cache may reject it after observing the unsupported
        // kind, so use a new request for the positive-class negative cases.
        worker_time+=std::chrono::seconds(6);actual_dispatch(renderer.data(),0);
        std::ifstream negative_saved{worker_negative};std::string negative_json{std::istreambuf_iterator<char>{negative_saved},{}};
        require(negative_json.find("\"outcome\":\"expired_unjoined\"")!=negative_json.npos && negative_json.find("\"mask\":8,\"kind_flags\":2")!=negative_json.npos,"unsupported worker identities remain typed/unjoined rather than console events");negative_saved.close();
        const auto stability_output=directory/"worker_stability.worker_visibility.json";std::filesystem::remove(stability_output);std::ofstream(request)<<"worker_stability\n";
        worker_time+=std::chrono::seconds(2);actual_dispatch(renderer.data(),0);
        mutate_mask=true;run_worker();require(worker_mask==1,"passive worker probe does not undo native mask side effects");mutate_mask=false;worker_mask=3;
        invalidate_source=true;run_worker();invalidate_source=false;
        worker_time+=std::chrono::seconds(6);actual_dispatch(renderer.data(),0);
        std::ifstream stability_saved{stability_output};std::string stability_json{std::istreambuf_iterator<char>{stability_saved},{}};
        require(stability_json.find("\"outcome\":\"expired_unjoined\"")!=stability_json.npos && stability_json.find("\"rejected\":2")!=stability_json.npos && stability_json.find("\"mask_before\":3,\"mask_after\":1")!=stability_json.npos && stability_json.find("\"source_stable\":false")!=stability_json.npos,"actual changed native mask and producer revision refuse joined evidence and retain typed rejection details");stability_saved.close();
        // A real native worker can outlive its request. Hold it inside the
        // trampoline while the Scene path expires A and admits request B.
        auto& worker_probe=NativeWorkerVisibilityProbe::get();
        const auto old_output=directory/"worker_old.worker_visibility.json",new_output=directory/"worker_new.worker_visibility.json";
        std::filesystem::remove(old_output);std::filesystem::remove(new_output);
        std::ofstream(request)<<"worker_old\n";worker_time+=std::chrono::seconds(2);actual_dispatch(renderer.data(),0);publish_producer(102);
        std::promise<void> entered,release;auto resume=release.get_future();
        const auto original_builder_steps=builder_steps;
        builder_steps=[&](auto... args){original_builder_steps(args...);entered.set_value();resume.wait();};
        worker_steps=[&](uintptr_t,int32_t,uintptr_t,uintptr_t,uintptr_t){emit_build(false,0,producer_add+8,handle_pointer);};
        std::thread delayed{[&]{SetLastError(0x41424344);worker_fixture(producer_add,-17,handle_pointer,mask_pointer,0x1234);require(GetLastError()==0x51525354,"delayed stale callback preserves native last error");}};
        entered.get_future().wait();worker_time+=std::chrono::seconds(6);actual_dispatch(renderer.data(),0);
        std::ofstream(request)<<"worker_new\n";worker_time+=std::chrono::seconds(2);actual_dispatch(renderer.data(),0);
        const auto new_generation=worker_probe.m_generation;const auto before_lookups=worker_probe.m_lookups;
        release.set_value();delayed.join();
        builder_steps=original_builder_steps;
        require(worker_probe.m_active && worker_probe.m_generation==new_generation && worker_probe.m_rows.empty() && !worker_probe.m_source && worker_probe.m_lookups==before_lookups && worker_probe.m_rejected==0 && worker_probe.m_builder_validations==0 && worker_probe.m_builder_rejected==0,"expired native builder/worker cannot append or charge counters into a newly armed request sharing its still-current producer");
        worker_steps=[&](uintptr_t,int32_t,uintptr_t,uintptr_t,uintptr_t){};run_worker();
        require(worker_probe.m_rows.size()==1,"fresh current-generation worker remains observable after stale callback rejection");
        copy=entries.begin_copy(list+0x160,producer_add);require(bool(copy),"expiry fixture producer copy starts");
        std::memcpy(native_list.data()+0x160,producer.data()+0x160,native_list.size()-0x160);require(entries.finish_copy(*copy,true),"expiry fixture producer copy completes");
        scene_steps=[&](void* native_renderer,uint32_t,int32_t slot){const auto current=actual[slot].address;std::memcpy(static_cast<uint8_t*>(native_renderer)+0x140,&current,8);if(slot==0)worker_time+=std::chrono::seconds(6);};
        actual_dispatch(renderer.data(),0);
        std::ifstream new_saved{new_output};std::string new_json{std::istreambuf_iterator<char>{new_saved},{}};
        require(!worker_probe.m_active && new_json.find("\"outcome\":\"expired_unjoined\"")!=new_json.npos && new_json.find("\"outcome\":\"joined\"")==new_json.npos,"deadline passing during actual left native Scene prevents a later right Scene from falsely joining expired worker evidence");new_saved.close();
        std::filesystem::remove(old_output);std::filesystem::remove(new_output);
        const auto budget_output=directory/"worker_budget.worker_visibility.json";std::filesystem::remove(budget_output);
        scene_steps=[&](void* native_renderer,uint32_t,int32_t slot){const auto current=actual[slot].address;std::memcpy(static_cast<uint8_t*>(native_renderer)+0x140,&current,8);};
        std::ofstream(request)<<"worker_budget\n";worker_time+=std::chrono::seconds(2);actual_dispatch(renderer.data(),0);publish_producer(103);
        worker_steps=[&](uintptr_t,int32_t,uintptr_t,uintptr_t mask,uintptr_t){*reinterpret_cast<uint64_t*>(mask)^=2;};
        for(unsigned n=0;n<120;++n){worker_mask=3;run_worker();}
        require(worker_probe.m_after_validations==120 && worker_probe.m_rows.empty() && worker_probe.m_rejected==120 && worker_probe.m_lookups==1,"cached identity with repeated changed native masks consumes full-validation budget despite retaining no rows");
        std::promise<void> start_concurrent;auto concurrent_ready=start_concurrent.get_future().share();std::vector<std::thread> concurrent;
        const auto native_before=worker_calls,queries_before=native_query_calls;std::atomic_uint error_failures{};
        for(unsigned n=0;n<16;++n)concurrent.emplace_back([&]{uint64_t own_mask=3;concurrent_ready.wait();SetLastError(0x41424344);worker_fixture(producer_add,-17,handle_pointer,reinterpret_cast<uintptr_t>(&own_mask),0x1234);if(own_mask!=1 || GetLastError()!=0x51525354)++error_failures;});
        start_concurrent.set_value();for(auto& thread:concurrent)thread.join();
        require(worker_probe.m_after_validations==128 && worker_probe.m_after_exhausted==8 && worker_probe.m_rows.empty() && worker_probe.m_rejected==128 && worker_calls==native_before+16 && native_query_calls==queries_before && !error_failures,"concurrent callbacks reserve at most128 full validations while every native call and mask side effect/error remains intact");
        worker_time+=std::chrono::seconds(6);actual_dispatch(renderer.data(),0);
        std::ifstream budget_saved{budget_output};std::string budget_json{std::istreambuf_iterator<char>{budget_saved},{}};
        require(budget_json.find("\"after_validations\":128,\"after_validation_exhausted\":8")!=budget_json.npos && budget_json.find("\"outcome\":\"expired_unjoined\"")!=budget_json.npos,"expired bounded-work receipt exposes attempted full validations and exhaustion instead of claiming a complete census");budget_saved.close();std::filesystem::remove(budget_output);
        const auto build_budget_output=directory/"builder_budget.worker_visibility.json";std::filesystem::remove(build_budget_output);
        std::ofstream(request)<<"builder_budget\n";worker_time+=std::chrono::seconds(2);actual_dispatch(renderer.data(),0);publish_producer(104);worker_mask=3;
        worker_steps=[&](uintptr_t,int32_t,uintptr_t,uintptr_t,uintptr_t){for(unsigned n=0;n<136;++n)emit_build(false,2,producer_add+8+2*0xa60,handle_pointer);};
        run_worker();require(worker_probe.m_builder_validations==128 && worker_probe.m_builder_exhausted==8 && worker_probe.m_builder_rejected==128 && worker_probe.m_rows.size()==1 && worker_probe.m_rows[0].builds.empty(),"rejected descriptor builds consume the hard128 reservation budget while all136 native calls still execute once");
        worker_time+=std::chrono::seconds(6);actual_dispatch(renderer.data(),0);
        std::ifstream build_budget_saved{build_budget_output};std::string build_budget_json{std::istreambuf_iterator<char>{build_budget_saved},{}};
        require(build_budget_json.find("\"builder_validations\":128,\"builder_validation_exhausted\":8,\"builder_rejected\":128")!=build_budget_json.npos,"builder work exhaustion is explicit in receipt");build_budget_saved.close();std::filesystem::remove(build_budget_output);
        const auto stale_build_output=directory/"builder_stale.worker_visibility.json";std::filesystem::remove(stale_build_output);
        std::ofstream(request)<<"builder_stale\n";worker_time+=std::chrono::seconds(2);actual_dispatch(renderer.data(),0);publish_producer(105);
        worker_steps=[&](uintptr_t,int32_t,uintptr_t,uintptr_t,uintptr_t){emit_build(false,0,producer_add+8,handle_pointer,0xfd);};
        run_worker();require(worker_probe.m_rows.empty() && !worker_probe.m_source && worker_probe.m_builder_validations==1 && worker_probe.m_builder_rejected==1 && worker_probe.m_rejected==1,"actual camera ownership invalidated inside native builder refuses both descriptor and parent worker evidence");
        worker_time+=std::chrono::seconds(6);actual_dispatch(renderer.data(),0);std::filesystem::remove(stale_build_output);
        // The separate causal input control survives the one-pair passive
        // request and substitutes a call-local complete mask only when needed.
        const auto control=directory/"console_mask_causal_request.txt";
        for(const auto id:{"native_input","union_input","left_hide_input","expiry_before_apply","inflight_input","after_inflight","budget_input","contract_input"})std::filesystem::remove(directory/(std::string(id)+".console_mask_causal.json"));
        unsigned causal_source_reads{};bool expire_source=false;auto& mask_causal=worker_probe.m_causal;
        require(mask_causal.configure(directory,causal_worker_caller,[&](uintptr_t value){++causal_source_reads;if(expire_source && causal_source_reads==2)worker_time+=std::chrono::seconds(2);return NativeWorkerVisibilityProbe::source(value);},NativeWorkerVisibilityProbe::same,[&]{return worker_time;}),"actual causal worker control configures exact observed native caller and real producer reader");
        alignas(8) std::array<uint8_t,24> causal_mask{};std::array<float,2> distances{3.0f,4.0f};const auto distance_pointer=reinterpret_cast<uintptr_t>(distances.data());
        const int32_t sentinel=-1;const uint32_t has_distances=1;std::memcpy(causal_mask.data()+8,&sentinel,4);std::memcpy(causal_mask.data()+12,&has_distances,4);std::memcpy(causal_mask.data()+16,&distance_pointer,8);mask_pointer=reinterpret_cast<uintptr_t>(causal_mask.data());
        uint64_t expected_mask{};bool expect_clone{};unsigned native_effects{};
        worker_steps=[&](uintptr_t native_list_arg,int32_t index,uintptr_t native_handle,uintptr_t native_mask,uintptr_t options){
            require(native_list_arg==producer_add && index==-17 && native_handle==handle_pointer && options==0x1234,"causal experiment retains all non-mask worker arguments");
            require((native_mask!=mask_pointer)==expect_clone,"only a qualified changed input substitutes the call-local mask pointer");
            std::array<uint8_t,24> actual_mask{};std::memcpy(actual_mask.data(),reinterpret_cast<void*>(native_mask),24);uint64_t actual_bits{};std::memcpy(&actual_bits,actual_mask.data(),8);
            require(actual_bits==expected_mask && std::equal(actual_mask.begin()+8,actual_mask.end(),causal_mask.begin()+8),"causal native input changes only verified ordinary bits, preserving all auxiliary bytes/pointers");++native_effects;
        };
        const auto set_mask=[&](uint64_t value){std::memcpy(causal_mask.data(),&value,8);};
        const auto arm_mask=[&](const std::string& id,const char* mode,uint64_t pid=GetCurrentProcessId(),unsigned duration=1000){std::ofstream(control)<<id<<' '<<pid<<' '<<duration<<' '<<mode<<'\n';worker_time+=std::chrono::milliseconds(300);actual_dispatch(renderer.data(),0);causal_source_reads=0;};
        const auto expire_mask=[&]{worker_time+=std::chrono::seconds(2);actual_dispatch(renderer.data(),0);};
        publish_producer(106);arm_mask("wrong_pid","union",GetCurrentProcessId()+1);require(!mask_causal.active(),"cross-process causal requests cannot arm");
        arm_mask("too_long","union",GetCurrentProcessId(),5001);require(!mask_causal.active(),"over-five-second requests cannot arm");
        arm_mask("native_input","native");set_mask(0x81);expected_mask=0x81;expect_clone=false;run_worker();expire_mask();
        arm_mask("union_input","union");set_mask(0x81);expected_mask=0x83;expect_clone=true;const auto original_bytes=causal_mask;run_worker();require(causal_mask==original_bytes,"union never writes the caller's original native mask storage");
        set_mask(0x80);expected_mask=0x80;expect_clone=false;run_worker(); // No ordinary eye positive.
        set_mask(0x83);expected_mask=0x83;run_worker(); // Already both positive.
        set_mask(0x81);expected_mask=0x81;distances[1]=std::numeric_limits<float>::quiet_NaN();run_worker();distances[1]=4;
        console_class[0]='x';run_worker();console_class[0]='t'; // Cached hint cannot authorize another class.
        entries.reset(producer_add);run_worker();publish_producer(107); // Current ownership is mandatory.
        const auto queries_before_other=native_query_calls;SetLastError(0x41424344);worker_fixture(producer_add,-17,handle_pointer,mask_pointer,0x1234);
        require(GetLastError()==0x51525354 && native_query_calls==queries_before_other,"wrong worker caller remains native with original errors and no queries");
        expire_mask();std::ifstream union_saved{directory/"union_input.console_mask_causal.json"};std::string union_json{std::istreambuf_iterator<char>{union_saved},{}};
        require(union_json.find("\"changes\":1")!=union_json.npos && union_json.find("\"ordinary_bits\":3,\"before\":129,\"effective\":131")!=union_json.npos && union_json.find("\"mask_contract\":true")!=union_json.npos,"causal receipt counts the actual single changed input and native-only/no-eye/already-both/invalid-distance/class/caller negatives");union_saved.close();
        arm_mask("expiry_before_apply","union");expire_source=true;set_mask(0x81);expected_mask=0x81;run_worker();expire_source=false;expire_mask();
        std::ifstream expiry_saved{directory/"expiry_before_apply.console_mask_causal.json"};std::string expiry_json{std::istreambuf_iterator<char>{expiry_saved},{}};require(expiry_json.find("\"changes\":0")!=expiry_json.npos,"deadline expiring during actual source validation refuses causal input before native call");expiry_saved.close();
        require(native_effects==9,"all causal and refused native worker side effects execute exactly once");
        arm_mask("left_hide_input","left_hide");
        set_mask(0x83);expected_mask=0x82;expect_clone=true;const auto hide_original=causal_mask;run_worker();
        require(causal_mask==hide_original,"left-hide preserves the original storage and right/auxiliary bits");
        set_mask(0x81);expected_mask=0x80;run_worker(); // Remove left even when right native visibility is absent.
        set_mask(0x82);expected_mask=0x82;expect_clone=false;run_worker(); // Right-only native remains right-only.
        set_mask(0x81);expected_mask=0x81;console_class[0]='x';run_worker();console_class[0]='t';
        entries.reset(producer_add);run_worker();publish_producer(108); // Stale pair cannot authorize removal.
        expire_mask();run_worker();require(!mask_causal.active(),"left-hide expires and restores native inputs without a replacement request");
        std::ifstream hide_saved{directory/"left_hide_input.console_mask_causal.json"};std::string hide_json{std::istreambuf_iterator<char>{hide_saved},{}};
        require(hide_json.find("\"mode\":\"left_hide\",\"outcome\":\"expired\",\"changes\":2")!=hide_json.npos &&
                hide_json.find("\"ordinary_bits\":3,\"before\":131,\"effective\":130")!=hide_json.npos &&
                hide_json.find("\"before\":129,\"effective\":128")!=hide_json.npos,
                "actual hooked left-hide receipt proves only left removal, two changed parts and terminal native restoration");hide_saved.close();
        arm_mask("inflight_input","union");set_mask(0x81);expected_mask=0x83;expect_clone=true;
        const auto native_causal_steps=worker_steps;std::promise<void> causal_entered,causal_release;auto causal_resume=causal_release.get_future();
        worker_steps=[&](auto... args){native_causal_steps(args...);causal_entered.set_value();causal_resume.wait();};
        std::thread inflight_native{[&]{run_worker();}};causal_entered.get_future().wait();expire_mask();
        arm_mask("after_inflight","native");require(!mask_causal.active(),"a new control cannot replace the expired request while an actually applied native call remains in flight");
        causal_release.set_value();inflight_native.join();worker_steps=native_causal_steps;
        std::ifstream inflight_saved{directory/"inflight_input.console_mask_causal.json"};std::string inflight_json{std::istreambuf_iterator<char>{inflight_saved},{}};
        require(inflight_json.find("\"changes\":1")!=inflight_json.npos && inflight_json.find("\"inflight\":0")!=inflight_json.npos && inflight_json.find("\"outcome\":\"expired\"")!=inflight_json.npos,"native completion after deadline is accounted to its original request before final expiry");inflight_saved.close();
        worker_time+=std::chrono::milliseconds(300);actual_dispatch(renderer.data(),0);expected_mask=0x81;expect_clone=false;run_worker();expire_mask();
        arm_mask("budget_input","union");const auto effects_before_budget=native_effects;
        for(unsigned n=0;n<4097;++n){expected_mask=n<4096?0x83:0x81;expect_clone=n<4096;run_worker();}expire_mask();
        std::ifstream mask_budget_saved{directory/"budget_input.console_mask_causal.json"};std::string mask_budget_json{std::istreambuf_iterator<char>{mask_budget_saved},{}};
        require(native_effects==effects_before_budget+4097 && mask_budget_json.find("\"changes\":4096,\"attempts\":4096,\"exhausted\":1")!=mask_budget_json.npos,"actual hooked causal calls respect the hard qualified-work bound and all later native calls continue unchanged");mask_budget_saved.close();
        arm_mask("contract_input","union");expected_mask=0x83;expect_clone=true;
        worker_steps=[&](uintptr_t a,int32_t b,uintptr_t c,uintptr_t m,uintptr_t e){native_causal_steps(a,b,c,m,e);*reinterpret_cast<uint64_t*>(m)^=0x100;};
        run_worker();require(!mask_causal.active() && causal_mask==original_bytes,"an unexpected native write to its cloned mask aborts the unsupported contract and never leaks into original caller storage");
        worker_steps=native_causal_steps;expected_mask=0x81;expect_clone=false;run_worker();
        std::ifstream contract_saved{directory/"contract_input.console_mask_causal.json"};std::string contract_json{std::istreambuf_iterator<char>{contract_saved},{}};
        require(contract_json.find("\"outcome\":\"native_mask_contract_failed\"")!=contract_json.npos && contract_json.find("\"contract_failures\":1")!=contract_json.npos,"unexpected mask-side-effect contract is recorded as a failed diagnostic, never accepted evidence");contract_saved.close();
        arm_mask("after_contract_failure","union");require(!mask_causal.active(),"unsupported native mask-write contract permanently refuses further causal requests in this process");
        std::filesystem::remove(control);for(const auto id:{"native_input","union_input","left_hide_input","expiry_before_apply","inflight_input","after_inflight","budget_input","contract_input"})std::filesystem::remove(directory/(std::string(id)+".console_mask_causal.json"));
        std::filesystem::remove(request);std::filesystem::remove(worker_output);std::filesystem::remove(worker_negative);
        std::filesystem::remove(stability_output);
        // Actual hook chain on a separate native selector thread with NO Scene
        // TLS. The old Scene-only path cannot observe any of these events.
        using Census=NativeConsoleDrawCensus;auto& census=Census::get();
        // Each invocation owns these fixture receipts. A previous successful
        // run must not make the production no-overwrite guard reject this run.
        const std::array census_ids{"census_worker","census_stale","census_unreadable","census_changed","census_expiry","census_budget","census_rearm","census_new"};
        const auto clean_census=[&]{for(const auto id:census_ids)std::filesystem::remove(directory/(std::string(id)+".console_draw_census.json"));};
        clean_census();
        std::function<void()> census_clock_steps;auto census_time=Census::Clock::now();require(census.configure(directory,[&]{if(census_clock_steps)census_clock_steps();return census_time;},reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),"census configured once with deterministic clock");
        const auto census_control=directory/"console_draw_census_request.txt";
        const auto arm_census=[&](const std::string& id,uint64_t pid=GetCurrentProcessId(),unsigned duration=5000){
            census_time+=std::chrono::milliseconds(300);std::ofstream(census_control)<<id<<' '<<pid<<' '<<duration<<' '<<geometry_handle<<'\n';census.poll();};
        const auto expire_census=[&]{census_time+=std::chrono::seconds(6);census.poll();};
        const auto read_census=[&](const std::string& id){std::ifstream in{directory/(id+".console_draw_census.json")};return std::string{std::istreambuf_iterator<char>{in},{}};};
        arm_census("census_wrong_pid",GetCurrentProcessId()+1);require(!census.m_collecting,"census rejects wrong PID");
        arm_census("census_long",GetCurrentProcessId(),5001);require(!census.m_collecting,"census rejects over-five-second requests");
        census_time+=std::chrono::milliseconds(300);std::ofstream(census_control)<<"duplicate_handles "<<GetCurrentProcessId()<<" 5000 "<<geometry_handle<<' '<<geometry_handle<<'\n';census.poll();require(!census.m_collecting,"duplicate full handle request refused");
        census_time+=std::chrono::milliseconds(300);std::ofstream(census_control)<<"../unsafe "<<GetCurrentProcessId()<<" 5000 "<<geometry_handle<<'\n';census.poll();require(!census.m_collecting,"unsafe request ID refused");
        std::array<uint32_t,6> census_view{2,11,22,33,44,7};
        std::array<uintptr_t,1> census_descriptors{descriptor_pointer};uintptr_t census_data=reinterpret_cast<uintptr_t>(census_descriptors.data());
        const auto census_array=reinterpret_cast<uintptr_t>(&census_data),census_view_pointer=reinterpret_cast<uintptr_t>(census_view.data());
        std::array<uintptr_t,24> census_command_vtable{};census_command_vtable[0xb0/8]=reinterpret_cast<uintptr_t>(&draw_fixture);uintptr_t census_command=reinterpret_cast<uintptr_t>(census_command_vtable.data());
        // Vtable must reside in authenticated image; use a static table in .data.
        static std::array<uintptr_t,24> census_image_vtable{};census_image_vtable[0xb0/8]=reinterpret_cast<uintptr_t>(&draw_fixture);census_command=reinterpret_cast<uintptr_t>(census_image_vtable.data());
        const auto census_command_address=reinterpret_cast<uintptr_t>(&census_command);descriptor[0xd8]=37;
        unsigned census_native_selects{},census_native_draws{};DWORD census_thread{};
        census_draw_steps=[&]{++census_native_draws;require(GetLastError()==0x43444546,"census preserves draw incoming LastError");SetLastError(0x91929394);};
        selection_steps=[&]{++census_native_selects;census_thread=GetCurrentThreadId();require(!Probe::active() && !NativeRenderProbe::current_scene(),"worker has no Scene identity");
            require(GetLastError()==0x31323334,"census preserves selector incoming LastError");
            native_query_result=19;const auto before=native_query_calls;require(query_call(123,object,4)==19 && native_query_calls==before+1 && GetLastError()==0x12344321,"census query remains exactly one unchanged native call");
            SetLastError(0x43444546);draw_fixture(census_command_address,census_view_pointer,descriptor_pointer,0,0,0);require(GetLastError()==0x91929394,"census preserves draw native error after its finish");
            SetLastError(0x81828384);return uintptr_t{0xfedcba9876543210};};
        const auto census_native=[&]{SetLastError(0x31323334);require(select_fixture(11,22,census_array,0,1,census_view_pointer,0)==0xfedcba9876543210 && GetLastError()==0x81828384,"census preserves selector native return/error");};
        arm_census("census_worker");
        std::thread census_worker{[&]{census_native();census_native();}};census_worker.join();
        require(census.m_selections.size()==1 && census.m_selections[0].completed && census.m_selections[0].stable && !census.m_selections[0].scene_tls &&
            census.m_selections[0].thread==census_thread && census.m_selections[0].before==census_view && census.m_selections[0].parts[0].handle==geometry_handle,
            "actual unjoined worker selector retains typed immutable stable raw camera context");
        require(census.m_queries.size()==1 && census.m_queries[0].result==19 && census.m_queries[0].column==4 && census.m_draws.size()==1 && census.m_dedup_skips>=1,
            "actual missing-Scene query and draw hooks are observed; unsampled duplicate is explicitly separate");
        require(census.m_draws[0].frontier.command==census_command_address && census.m_draws[0].frontier.mapping_read && census.m_draws[0].frontier.mapping_stable && census.m_draws[0].frontier.arguments_read && census.m_draws[0].frontier.arguments_stable && census.m_draws[0].frontier.arguments[0]==37 && !census.m_draws[0].frontier.counter_read,"actual native draw argumenta authenticates command callback and raw args without claiming another backend's counters");
        // A synthetic supported code-image region backs read-only native state. Native
        // selector/draw hooks remain real; no GPU command or callback is invoked.
        constexpr size_t target_image_size=0x2144000;
        auto* target_image=static_cast<uint8_t*>(VirtualAlloc(nullptr,target_image_size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));require(target_image,"target fixture image allocation");
        const auto target_base=reinterpret_cast<uintptr_t>(target_image),target_vtable=target_base+0x1000;
        const auto target_sig=synthetic_at<16>(0x2142ba0);const auto target_bind_sig=synthetic_at<16>(0x21426d0);const auto target_indexed_sig=synthetic_at<16>(0x213d690);std::memcpy(target_image+0x213d690,target_indexed_sig.data(),16);
        std::memcpy(target_image+0x2142ba0,target_sig.data(),16);std::memcpy(target_image+0x21426d0,target_bind_sig.data(),16);
        const auto target_reader=[](uintptr_t at,void* out,size_t size){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),out,size,&n) && n==size;};
        require(NativeConsoleTargets::supported(target_base,target_image_size,target_reader),"all native target/backend layout signatures authenticate together");target_image[0x21426d0]^=1;require(!NativeConsoleTargets::supported(target_base,target_image_size,target_reader),"changed native binding signature refuses target layout");target_image[0x21426d0]^=1;
        const uintptr_t target_callback=target_base+0x213d690;std::memcpy(target_image+0x1000+0xb0,&target_callback,8);
        std::vector<uint8_t> target_command(0x119a4),target_color(0x280),target_depth(0x280);
        const auto target_command_address=reinterpret_cast<uintptr_t>(target_command.data()),target_color_address=reinterpret_cast<uintptr_t>(target_color.data()),target_depth_address=reinterpret_cast<uintptr_t>(target_depth.data());
        const uintptr_t target_color_resource=0x123456780,target_depth_resource=0x123456790;std::memcpy(target_color.data()+0x278,&target_color_resource,8);std::memcpy(target_depth.data()+0x278,&target_depth_resource,8);
        const auto target_put=[&](size_t offset,const auto& value){std::memcpy(target_command.data()+offset,&value,sizeof(value));};
        target_put(0,target_vtable);target_put(0xfd88,target_color_address);target_put(0xfdc8,target_depth_address);const std::array<int32_t,2> target_dimensions{1200,1200};target_put(0xfdd0,target_dimensions);
        const std::array<float,6> target_viewport{0,0,1200,1200,0,1};const std::array<int32_t,4> target_scissor{0,0,1200,1200};target_put(0x11910,target_viewport);target_put(0x11928,target_scissor);target_command[0x1194c]=1;
        const uintptr_t target_rtv=0x888,target_dsv=0x777;target_put(0x11958,target_rtv);target_put(0x11950,target_dsv);const int32_t target_one=1;target_put(0x11998,target_one);target_put(0x119a0,target_one);
        const auto saved_target_base=census.m_base,saved_target_size=census.m_image_size;const bool saved_indexed=census.m_indexed_backend,saved_targets=census.m_targets_backend;
        census.m_base=target_base;census.m_image_size=target_image_size;census.m_indexed_backend=true;census.m_targets_backend=NativeConsoleTargets::supported(target_base,target_image_size,target_reader);
        const auto saved_target_command=census_command;const auto saved_target_draw_steps=census_draw_steps;
        const auto target_selection_steps=selection_steps;unsigned target_extra_selects{},target_extra_draws{};
        selection_steps=[&]{++census_native_selects;census_thread=GetCurrentThreadId();SetLastError(0x43444546);census_target_draw_call(target_command_address,census_view_pointer,descriptor_pointer);require(GetLastError()==0x91929394,"target metadata preserves outgoing native LastError");SetLastError(0x81828384);return uintptr_t{0xfedcba9876543210};};
        const auto target_target_selection_steps=selection_steps;
        census_draw_steps=[&]{++census_native_draws;++target_extra_draws;require(GetLastError()==0x43444546,"target metadata preserves incoming native LastError");uint32_t n{};std::memcpy(&n,target_command.data()+0x10a30,4);++n;target_put(0x10a30,n);const float width=1000;target_put(0x11918,width);const uintptr_t rtv=0x999;target_put(0x11958,rtv);SetLastError(0x91929394);};
        const auto target_native=[&]{++census_view[1];++target_extra_selects;census_native();};
        target_native();const auto& target_pair=census.m_draws.back().frontier.targets;
        require(target_pair.supported && target_pair.identity_before && target_pair.identity_after && target_pair.same_call && target_pair.before.fields_read && target_pair.before.fields_stable && target_pair.before.resources_stable && target_pair.after.fields_stable && target_pair.after.resources_stable && target_pair.before.resources_read && target_pair.after.resources_read && target_pair.before.resources[0]==target_color_resource && target_pair.after.depth_resource==target_depth_resource && target_pair.before.rtvs[0]==0x888 && target_pair.after.rtvs[0]==0x999 && target_pair.before.viewport_words[2]==std::bit_cast<uint32_t>(1200.0f) && target_pair.after.viewport_words[2]==std::bit_cast<uint32_t>(1000.0f) && target_pair.after.scissor==target_scissor,"actual native hook snapshots target viewport and scissor before and after same call without eye claims");
        bool target_torn=false,target_read_started=false;unsigned target_read_start{};auto target_clock_serial=census.m_draw_serial;
        census_clock_steps=[&]{if(!target_torn && census.m_draw_serial>target_clock_serial && !census.m_draws.empty() && census.m_draws.back().frontier.targets.identity_before){if(!target_read_started){target_read_started=true;target_read_start=census.m_reads;}else if(census.m_reads>=target_read_start+3){const float torn_width=999;target_put(0x11918,torn_width);target_torn=true;}}};
        target_native();census_clock_steps={};require(target_torn && census.m_draws.back().frontier.targets.before.fields_read && !census.m_draws.back().frontier.targets.before.fields_stable,"torn target command cache during actual hooked snapshot is explicitly unstable");
        target_torn=false;target_read_started=false;target_clock_serial=census.m_draw_serial;census_clock_steps=[&]{if(!target_torn && census.m_draw_serial>target_clock_serial && !census.m_draws.empty() && census.m_draws.back().frontier.targets.identity_before){if(!target_read_started){target_read_started=true;target_read_start=census.m_reads;}else if(census.m_reads>=target_read_start+3){const uintptr_t changed_resource=0x333333330;std::memcpy(target_color.data()+0x278,&changed_resource,8);target_torn=true;}}};
        target_native();census_clock_steps={};require(target_torn && census.m_draws.back().frontier.targets.before.resources_read && !census.m_draws.back().frontier.targets.before.resources_stable,"torn target wrapper resource during actual hooked snapshot is explicitly unstable");std::memcpy(target_color.data()+0x278,&target_color_resource,8);
        const int32_t target_nine=9;target_put(0x11998,target_nine);target_native();require(census.m_draws.back().frontier.targets.before.fields_read && !census.m_draws.back().frontier.targets.before.counts_bounded && !census.m_draws.back().frontier.targets.before.resources_read,"out of range native target count prevents resource traversal");target_put(0x11998,target_one);
        const uintptr_t unreadable_target=1;target_put(0xfd88,unreadable_target);target_native();require(!census.m_draws.back().frontier.targets.before.resources_read && !census.m_draws.back().frontier.targets.before.resource_read[0],"unreadable target wrapper remains unknown while native continues");target_put(0xfd88,target_color_address);
        const auto target_steps=census_draw_steps;census_draw_steps=[&]{target_steps();weak_owner=0;};target_native();require(!census.m_draws.back().frontier.targets.identity_after && !census.m_draws.back().frontier.targets.after.fields_read,"changed post-native full weak identity refuses target pairing");weak_owner=properties_address;census_draw_steps=target_steps;
        census.m_targets_backend=false;target_native();require(!census.m_draws.back().frontier.targets.supported && !census.m_draws.back().frontier.targets.before.fields_read,"unsupported target signatures never read target cache fields");census.m_targets_backend=true;
        bool target_nested=false;census_draw_steps=[&]{++census_native_draws;++target_extra_draws;require(GetLastError()==0x43444546,"nested target incoming error preserved");if(!target_nested){const float width=777;target_put(0x11918,width);target_nested=true;SetLastError(0x43444546);census_target_draw_call(target_command_address,census_view_pointer,descriptor_pointer);const float final_parent_width=555;target_put(0x11918,final_parent_width);}else{const float width=666;target_put(0x11918,width);}SetLastError(0x91929394);};target_native();
        require(census.m_draws.back().calls==2 && census.m_draws.back().frontier.targets.same_call && census.m_draws.back().frontier.targets.before.viewport_words[2]==std::bit_cast<uint32_t>(777.0f) && census.m_draws.back().frontier.targets.after.viewport_words[2]==std::bit_cast<uint32_t>(666.0f),"nested dedup retains latest actual call pair and old parent cannot mix its snapshots");
        census.m_base=saved_target_base;census.m_image_size=saved_target_size;census.m_indexed_backend=saved_indexed;census.m_targets_backend=saved_targets;census_command=saved_target_command;census_draw_steps=saved_target_draw_steps;selection_steps=target_selection_steps;
        census_time+=std::chrono::seconds(1);census_view[5]=8;census_native();
        require(census.m_selections.size()>=2 && census.m_selections[0].before[5]==7 && census.m_queries[0].selection==0,"later native sample cannot relabel historical query identity");
        expire_census();const auto census_json=read_census("census_worker");
        require(census_json.find("\"dispatch_join_verified\":false")!=census_json.npos && census_json.find("\"complete_census\":false")!=census_json.npos &&
            census_json.find("\"native_camera_id\":7")!=census_json.npos && census_json.find("\"scene_tls_present\":false")!=census_json.npos && census_json.find("\"dedup_unsampled_calls\":1")!=census_json.npos,
            "census receipt distinguishes raw camera context, incomplete sampling and no dispatch/GPU proof");
        const auto successful_samples=census.m_selections.size();const auto committed_parts=census.m_selections[0].parts;const auto census_steps=selection_steps;
        arm_census("census_stale");console_class[0]='x';selection_steps=[&]{auto* unfinished=Census::s_selection;require(unfinished && !unfinished->m_recorded,"uncommitted allocation-failure-shaped selector context");unfinished->m_parts=committed_parts;return census_steps();};census_native();selection_steps=census_steps;console_class[0]='t';
        require(census.m_selections.empty() && census.m_queries.empty(),"fresh full class validation refuses stale cohort hints");expire_census();
        arm_census("census_unreadable");census_descriptors[0]=1;census_native();const auto empty_reads=census.m_reads;census_native();census_descriptors[0]=descriptor_pointer;
        require(census.m_selections.empty() && census.m_contexts.size()==1 && census.m_contexts[0].calls==2 && census.m_reads==empty_reads+1,"unmatched native contexts deduplicate without rescanning or inventing identity");expire_census();
        arm_census("census_changed");const auto steps_before_change=selection_steps;selection_steps=[&]{const auto result=steps_before_change();descriptor[0x60]^=1;return result;};census_native();selection_steps=steps_before_change;
        require(census.m_selections.size()==1 && !census.m_selections[0].stable,"descriptor type changing inside native selection refuses full sample stability");descriptor[0x60]^=1;expire_census();
        arm_census("census_expiry");selection_steps=[&]{const auto result=steps_before_change();census_time+=std::chrono::seconds(6);return result;};census_native();selection_steps=steps_before_change;census.poll();
        require(census.m_selections.size()==1 && !census.m_selections[0].completed,"selection crossing deadline remains incomplete");
        arm_census("census_budget");std::vector<uintptr_t> unrelated(9000,1);const auto saved_census_data=census_data;census_data=reinterpret_cast<uintptr_t>(unrelated.data());
        SetLastError(0x31323334);require(select_fixture(11,22,census_array,0,9000,census_view_pointer,0)==0xfedcba9876543210 && GetLastError()==0x81828384,"budget rejection still forwards original native selector once");census_data=saved_census_data;
        require(!census.m_collecting && census.m_exhausted==1 && census.m_reads<=16384 && census.m_scanned<=8192 && census.m_selections.empty(),"actual unrelated geometry consumes the global hard work bound and cannot produce identity");expire_census();
        arm_census("census_rearm");std::promise<void> entered_census,release_census;auto released_census=release_census.get_future();
        selection_steps=[&]{const auto result=steps_before_change();entered_census.set_value();released_census.wait();SetLastError(0x81828384);return result;};
        std::thread delayed_census{[&]{census_native();}};entered_census.get_future().wait();expire_census();
        census_time+=std::chrono::milliseconds(300);std::ofstream(census_control)<<"census_new "<<GetCurrentProcessId()<<" 5000 classes\n";census.poll();release_census.set_value();delayed_census.join();selection_steps=steps_before_change;
        require(census.m_selections.empty() && census.m_queries.empty(),"old generation completion cannot append into fresh request");
        require(census.m_class_scope && census.m_handles.empty(),"passive class census is explicit and saved hints grant no authority");
        const auto census_omitted=(geometry_handle&0xffffffffffffULL)|(uint64_t(43)<<48);std::memcpy(descriptor.data()+0x100,&census_omitted,8);descriptor[0x60]=79;census_view[1]=43;census_native();
        require(census.m_selections.size()==1 && census.m_selections[0].stable && census.m_selections[0].parts[0].handle==census_omitted && census.m_selections[0].parts[0].type==79,"passive class census captures omitted LOD and native type79 without query intervention");
        for(unsigned logical=100;logical<131;++logical){const auto h=(geometry_handle&0xffffffffffffULL)|(uint64_t(logical)<<48);std::memcpy(descriptor.data()+0x100,&h,8);census_view[1]=logical;census_native();}
        const auto census_extra=(geometry_handle&0xffffffffffffULL)|(uint64_t(131)<<48);std::memcpy(descriptor.data()+0x100,&census_extra,8);census_view[1]=131;census_native();
        require(census.m_discovered.size()==32 && census.m_scope_limit_rejections==1,"passive33rd class handle is explicit incomplete sampling and native calls continue");
        // Expiry inside the original known-backend call leaves no paired post
        // observation, and still forwards native exactly once.
        std::memcpy(descriptor.data()+0x100,&census_omitted,8);census.m_base=target_base;census.m_image_size=target_image_size;census.m_indexed_backend=true;census.m_targets_backend=true;
        selection_steps=target_target_selection_steps;census_draw_steps=[&]{target_steps();census_time+=std::chrono::seconds(6);};target_native();
        require(census.m_draws.back().frontier.targets.identity_before && !census.m_draws.back().frontier.targets.identity_after && !census.m_draws.back().frontier.targets.same_call,"expiry within actual native draw cannot publish a paired target snapshot");
        census.m_base=saved_target_base;census.m_image_size=saved_target_size;census.m_indexed_backend=saved_indexed;census.m_targets_backend=saved_targets;census_draw_steps=saved_target_draw_steps;selection_steps=target_selection_steps;VirtualFree(target_image,0,MEM_RELEASE);
        std::memcpy(descriptor.data()+0x100,&geometry_handle,8);descriptor[0x60]=0;expire_census();
        require(census_native_selects==43+target_extra_selects && census_native_draws==43+target_extra_draws && successful_samples>=2,"all sampled/refused native selector/draw side effects execute unchanged exactly once");
        arm_census("census_limit");require(!census.m_collecting && census.m_seen.size()==8,"eighth completed request is the hard process request bound");
        census_draw_steps={};selection_steps={};std::filesystem::remove(census_control);clean_census();
        // Pass-only admission uses the native iterator's CURRENT descriptor,
        // set by f79c50 before querying, not cached cohort/Scene ownership.
        const std::array admission_ids{"admission_native","admission_post","admission_expiry","admission_budget","admission_inflight","admission_new","admission_seven","admission_eight"};
        const auto clean_admission=[&]{for(const auto id:admission_ids)std::filesystem::remove(directory/(std::string(id)+".console_query_admission.json"));};clean_admission();
        using Admission=NativeConsoleQueryAdmissionProbe;auto& admission=Admission::get();auto admission_time=Admission::Clock::now();bool expire_admission_after_row=false;
        require(admission.configure(directory,exact_caller,[&]{if(expire_admission_after_row && !admission.m_rows.empty()){expire_admission_after_row=false;admission_time+=std::chrono::seconds(6);}return admission_time;}),"pass admission configured for authenticated boolean query caller");
        const auto admission_control=directory/"console_query_admission_request.txt";
        const auto arm_admission=[&](const std::string& id,uint64_t pid=GetCurrentProcessId(),unsigned duration=5000){admission_time+=std::chrono::milliseconds(300);std::ofstream(admission_control)<<id<<' '<<pid<<' '<<duration<<' '<<geometry_handle<<'\n';admission.poll();};
        const auto expire_admission=[&]{admission_time+=std::chrono::seconds(6);admission.poll();};
        arm_admission("admission_pid",GetCurrentProcessId()+1);require(!admission.m_active,"admission rejects foreign PID");arm_admission("admission_duration",GetCurrentProcessId(),5001);require(!admission.m_active,"admission hard five-second request bound");
        admission_time+=std::chrono::milliseconds(300);std::ofstream(admission_control)<<"../bad "<<GetCurrentProcessId()<<" 5000 "<<geometry_handle<<'\n';admission.poll();require(!admission.m_active,"admission unsafe ID refused");
        admission_time+=std::chrono::milliseconds(300);std::ofstream(admission_control)<<"admission_dupe "<<GetCurrentProcessId()<<" 5000 "<<geometry_handle<<' '<<geometry_handle<<'\n';admission.poll();require(!admission.m_active,"admission duplicate cohort handles refused");
        std::array<uint32_t,6> admission_view{0,1864,0,1536,12517440,0};std::array<uintptr_t,3> admission_iterator{};
        const auto admission_view_address=reinterpret_cast<uintptr_t>(admission_view.data()),admission_iterator_address=reinterpret_cast<uintptr_t>(admission_iterator.data());
        const uint8_t admission_type=56;const uint16_t admission_flags=2053;std::memcpy(descriptor.data()+0x60,&admission_type,1);std::memcpy(descriptor.data()+0x194,&admission_flags,2);
        int32_t admission_result{};bool admission_other_caller{};int8_t admission_column{};uintptr_t admission_object=object;
        selection_steps=[&]{require(!NativeRenderProbe::current_scene(),"pass experiment requires no invented Scene identity");admission_iterator[2]=descriptor_pointer;
            const auto before=native_query_calls;SetLastError(0x70717273);admission_result=admission_other_caller?query_other_call(123,admission_object,admission_column):query_call(123,admission_object,admission_column);
            require(native_query_calls==before+1 && admission_native_incoming==0x70717273 && GetLastError()==0x12344321,"admission preserves original native query once and its sticky error");SetLastError(0x81828384);return uintptr_t{0xfedcba9876543210};};
        const auto run_admission=[&]{SetLastError(0x31323334);require(select_fixture(admission_iterator_address,22,census_array,0,1,admission_view_address,0)==0xfedcba9876543210 && GetLastError()==0x81828384,"admission selector still forwards once with original return/error");};
        arm_admission("admission_native");native_query_result=0;std::exception_ptr admission_worker_error;std::thread admission_worker{[&]{try{run_admission();require(admission_result==1,"actual unjoined native context0 zero query admits only target boolean gate");admission_view={1,1864,8192,1536,12517440,1};admission_column=2;run_admission();require(admission_result==1,"context1 receives the same pass-only intervention without eye ownership");}catch(...){admission_worker_error=std::current_exception();}}};admission_worker.join();if(admission_worker_error)std::rethrow_exception(admission_worker_error);
        native_query_result=0x71234567;run_admission();require(admission_result==0x71234567,"all positive native result bits are preserved");native_query_result=-7;run_admission();require(admission_result==-7,"negative nonzero native result bits are preserved");native_query_result=0;
        admission_other_caller=true;run_admission();require(admission_result==0,"different native consumer never gets invented positive semantics");admission_other_caller=false;
        admission_column=4;run_admission();require(admission_result==0,"unexpected query column refused");admission_column=2;
        for(unsigned word=0;word<6;++word){admission_view[word]^=4;run_admission();require(admission_result==0,"every raw native pass word is authenticated");admission_view[word]^=4;}
        descriptor[0x60]=25;run_admission();require(admission_result==0,"other renderer types remain native");descriptor[0x60]=56;
        const uint16_t no_query=2052;std::memcpy(descriptor.data()+0x194,&no_query,2);run_admission();require(admission_result==0,"descriptors without native query flag remain native");std::memcpy(descriptor.data()+0x194,&admission_flags,2);
        admission_object=object+8;run_admission();require(admission_result==0,"iterator descriptor must own the actual queried object");admission_object=object;
        const auto unknown_admission_handle=geometry_handle+(uint64_t(1)<<48);std::memcpy(descriptor.data()+0x100,&unknown_admission_handle,8);run_admission();require(admission_result==0,"unlisted full handle cannot inherit cohort authority");std::memcpy(descriptor.data()+0x100,&geometry_handle,8);
        console_class[0]='x';run_admission();require(admission_result==0,"cohort hint cannot authorize stale native class");console_class[0]='t';
        expire_admission();std::ifstream admission_saved{directory/"admission_native.console_query_admission.json"};std::string admission_json{std::istreambuf_iterator<char>{admission_saved},{}};
        require(admission_json.find("\"changes\":2")!=admission_json.npos && admission_json.find("\"owned_eye_verified\":false")!=admission_json.npos && admission_json.find("\"dispatch_join_verified\":false")!=admission_json.npos,"actual pass probe records only two native-zero changes without ownership/GPU claims");admission_saved.close();
        arm_admission("admission_post");
        admission_query_steps=[&]{descriptor[0x60]=25;};run_admission();require(admission_result==0,"post-native changed renderer type refuses admission");descriptor[0x60]=56;
        admission_query_steps=[&]{geometry[0x178]=3;};run_admission();require(admission_result==0,"post-native changed kind flags refuse even if both snapshots independentlydecode kind1");geometry[0x178]=1;
        admission_query_steps=[&]{admission_view[3]^=1;};run_admission();require(admission_result==0,"post-native changed raw pass refuses admission");admission_view[3]^=1;
        admission_query_steps=[&]{weak_owner=0;};run_admission();require(admission_result==0,"post-native changed weak chain refuses admission");weak_owner=properties_address;
        admission_query_steps=[&]{admission_iterator[2]=1;};run_admission();require(admission_result==0,"post-native changed current iterator descriptor refuses admission");admission_query_steps={};expire_admission();
        arm_admission("admission_expiry");admission_query_steps=[&]{admission_time+=std::chrono::seconds(6);};run_admission();require(admission_result==0,"deadline crossing inside native query cannot change its result");admission_query_steps={};admission.poll();
        arm_admission("admission_budget");const auto queries_before_admission_budget=native_query_calls;for(unsigned i=0;i<4097;++i)run_admission();
        require(native_query_calls==queries_before_admission_budget+4097 && !admission.m_active && admission.m_exhausted==1 && admission.m_attempts<=4096 && admission.m_changes<=4096 && admission.m_reads<=262144,"actual native calls continue once after request-global admission work bound");expire_admission();
        arm_admission("admission_inflight");std::promise<void> admission_entered,admission_release;auto admission_resume=admission_release.get_future();admission_query_steps=[&]{admission_entered.set_value();admission_resume.wait();};
        std::thread admission_inflight{[&]{run_admission();require(admission_result==0,"expired old native call cannot alter result in new request");}};admission_entered.get_future().wait();expire_admission();arm_admission("admission_new");admission_release.set_value();admission_inflight.join();admission_query_steps={};
        require(admission.m_changes==0 && admission.m_rows.empty(),"old-generation query cannot append or authorize in replacement request");expire_admission();
        const auto arm_classes=[&](const std::string& id){admission_time+=std::chrono::milliseconds(300);std::ofstream(admission_control)<<id<<' '<<GetCurrentProcessId()<<" 5000 classes\n";admission.poll();};
        arm_classes("admission_seven");require(admission.m_class_scope && admission.m_handles.empty(),"class admission is an explicit separate mode with no saved-hint authority");
        const auto omitted_lod=(geometry_handle&0xffffffffffffULL)|(uint64_t(43)<<48);std::memcpy(descriptor.data()+0x100,&omitted_lod,8);run_admission();
        require(admission_result==1 && admission.m_discovered==std::vector<uint64_t>{omitted_lod},"fresh same-class LOD omitted from baseline is independently discovered and admitted");
        console_class[0]='x';run_admission();require(admission_result==0,"class mode still refuses unrelated/stale classes");console_class[0]='t';
        admission_query_steps=[&]{weak_owner=0;};run_admission();require(admission_result==0,"class mode stale weak after native cannot authorize or discover");weak_owner=properties_address;admission_query_steps={};
        for(unsigned logical=100;logical<131;++logical){const auto handle=(geometry_handle&0xffffffffffffULL)|(uint64_t(logical)<<48);std::memcpy(descriptor.data()+0x100,&handle,8);run_admission();require(admission_result==1,"first32 fresh class handles are admitted within bounds");}
        require(admission.m_discovered.size()==32,"class discovery hard cap is32 full handles");const auto thirty_third=(geometry_handle&0xffffffffffffULL)|(uint64_t(131)<<48);std::memcpy(descriptor.data()+0x100,&thirty_third,8);run_admission();
        require(admission_result==0 && admission.m_discovered.size()==32 && admission.m_scope_limit_rejections==1,"33rd class handle preserves native and exposes incomplete class coverage");expire_admission();
        arm_classes("admission_eight");std::memcpy(descriptor.data()+0x100,&omitted_lod,8);expire_admission_after_row=true;run_admission();
        require(admission_result==0 && admission.m_rows.empty() && admission.m_changes==0 && admission.m_discovered.empty(),"expiry during actual class discovery/row allocation rolls back all publication and preserves native");admission.poll();
        std::memcpy(descriptor.data()+0x100,&geometry_handle,8);arm_admission("admission_nine");require(!admission.m_active && admission.m_seen.size()==8,"admission accepts at most eight explicit process requests");
        admission_query_steps={};selection_steps={};std::filesystem::remove(admission_control);clean_admission();


    }
};
int main(int argc,char** argv){try{require(argc==2,"absolute scratch directory required");ObjectVisibilityProbeTests::run(argv[1]);std::cout<<"object visibility tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
