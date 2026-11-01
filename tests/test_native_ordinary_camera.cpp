#include "NativeOrdinaryCamera.h"
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <thread>

void require(bool v,const char* message){if(!v)throw std::runtime_error(message);}
struct Fixture {
    std::vector<uint8_t> list=std::vector<uint8_t>(0x29c7c),renderer=std::vector<uint8_t>(0x600);
    std::array<uint8_t,0x388> source{};
    NativeOrdinaryCamera::Args args{0x3f7c013,0xffffffff,0x12345678,0x87654321,0xffff,0xfffe,0xfffd,0xff,0xfffffffe};
    unsigned calls{};NativeOrdinaryCamera::Args seen{};std::array<uint8_t,0x388> submitted{};
    FrameSyncClock::Frame frame{};
    template<class T>void put(size_t offset,const T& v){std::memcpy(list.data()+offset,&v,sizeof(v));}
    Fixture(){NativeCameraEntries::get().enable();auto world=vr_camera::IDENTITY;std::memcpy(source.data(),world.data(),64);std::memcpy(source.data()+64,world.data(),64);float fov=90;std::memcpy(source.data()+0x2d0,&fov,4);source.back()=0xa5;}
    void begin(uint64_t id){frame={uint32_t(id),id,int(id&1),0,false};int32_t count=1;put(0x29808,count);put(8,args[0]);vr_camera::Matrix world{};std::memcpy(world.data(),source.data(),64);put(8+0x34,world);auto ticket=NativeCameraEntries::get().begin(reinterpret_cast<uintptr_t>(list.data()),NativeCameraEntries::Role::main,frame);NativeCameraEntries::get().finish(*ticket,0,world);}
    NativeOrdinaryCamera::Add add(){return [&](void* list_arg,void* camera,const auto& a)->int64_t{
        require(list_arg==list.data() && camera!=source.data(),"native add receives the actual list and a separate camera");++calls;seen=a;std::memcpy(submitted.data(),camera,submitted.size());int32_t count=2;put(0x29808,count);put(8+0xa60,a[0]);vr_camera::Matrix world{};std::memcpy(world.data(),camera,64);put(8+0xa60+0x34,world);return 1;
    };}
    void invoke(NativeOrdinaryCamera& control){control.on_main(list.data(),source.data(),&frame,0,args,add());}
    std::array<NativeCameraEntries::Entry,2> actual(){std::array<NativeCameraEntries::Entry,2> values{};for(int i=0;i<2;++i){values[i].slot=i;values[i].address=reinterpret_cast<uintptr_t>(list.data())+8+i*0xa60;values[i].role=NativeCameraEntries::Role::main;std::memcpy(values[i].world.data(),reinterpret_cast<void*>(values[i].address+0x34),64);}return values;}
};
int main(){try{
    using C=NativeOrdinaryCamera;using E=NativeCameraEntries;
    for(auto name:{L"SM2VR_OPENVR",L"sm2vr_openxr",L"Sm2vr_Synthetic_Eyes",L"SM2VR_PIP",L"SM2VR_PIP_DUMP_DIR",L"SM2VR_ORDINARY_NATIVE_BLOOM"}){
        require(C::performance_conflict(name,L"1") && C::performance_conflict(name,L"unexpected"),"performance mode rejects conflicting Windows environment names case-insensitively");
        require(!C::performance_conflict(name,L"0") && !C::performance_conflict(name,L""),"disabled options do not conflict");
    }
    for(auto name:{L"SM2VR_ORDINARY_PERF_SECOND",L"sm2vr_ordinary_perf_control",L"SM2VR_PROFILE"})require(!C::performance_conflict(name,L"1"),"performance configuration and trace options are allowed");
    require(C::performance_control("on") && !C::performance_control("off") && !C::performance_control("") && !C::performance_control("invalid"),"missing or malformed control turns the additional view off");
    {NativeCameraPacket::Block raw{};const auto address=reinterpret_cast<uintptr_t>(raw.data());uint32_t value=0x3e800000;std::memcpy(raw.data()+0x6a8,&value,4);value=17;std::memcpy(raw.data()+0x6b0,&value,4);
    for(size_t i=0;i<NativeCameraPacket::OFFSETS.size();++i){const uint32_t bits=uint32_t(i+1);std::memcpy(raw.data()+NativeCameraPacket::OFFSETS[i],&bits,4);}
    raw[0xa35]=1;value=0x3e800000;std::memcpy(raw.data()+0xa38,&value,4);std::memcpy(raw.data()+0xa44,&value,4);
    auto packet=NativeCameraPacket::capture(address);require(packet && packet->entry==address && packet->jitter_words[0]==0x3e800000 && packet->jitter_index==17,"actual stable native packet copies exact pixel metadata");
    require(packet->motion_flags[1]==1 && packet->motion_words[0]==value && packet->motion_words[3]==value && packet->matrices.size()==26,"native temporal packet owns raw motion flags, linear/angular metadata and previous/shader matrices");
    for(size_t i=0;i<NativeCameraPacket::OFFSETS.size();++i)require(packet->matrices[i][0]==i+1,"native packet exports exact offset-labelled POD words");
    unsigned reads{};require(!NativeCameraPacket::capture(address,[&](uintptr_t observed,auto& output){require(observed==address,"exact reader source");output=raw;if(reads++==1)output.back()^=1;return true;}),"any intervening packet mutation refuses the snapshot");
    require(!NativeCameraPacket::capture(address,[](uintptr_t,auto&){return false;}) && !NativeCameraPacket::capture(address,[](uintptr_t,auto&)->bool{throw std::runtime_error("read failure");}),"unreadable or failed packet reads refuse");
    require(!NativeCameraPacket::capture(0) && !NativeCameraPacket::capture(1) && !NativeCameraPacket::capture(UINTPTR_MAX),"invalid and overflowing packet pointers never dereference");
    raw[0]=99;require(packet->matrices[0][0]==1,"owned packet has no borrowed native pointer contents");std::ostringstream exported;packet->write(exported);require(exported.str().find("\"0x49c\":[4,")!=std::string::npos && exported.str().find("\"resource_leases\":false")!=std::string::npos,"packet JSON labels raw offsets without GPU leases");}
    C concurrent;std::atomic_uint armed{};std::vector<std::thread> arming;for(unsigned i=0;i<8;++i)arming.emplace_back([&]{if(concurrent.arm([]{return uintptr_t(1);},{}))++armed;});for(auto& thread:arming)thread.join();require(armed==1,"configuration publication is atomic and concurrent arms cannot replace the producer provider");
    Fixture f;C c;require(c.arm([&]{return reinterpret_cast<uintptr_t>(f.renderer.data());},{4,1,.064f}),"bounded configuration arms");const auto original=f.source;
    f.begin(101);f.invoke(c);require(f.calls==1 && f.seen==f.args && f.source==original && f.submitted.back()==0xa5,"one actual native add forwards all nine arguments and preserves the complete original source");
    vr_camera::Matrix shifted{},view{};std::memcpy(shifted.data(),f.submitted.data(),64);std::memcpy(view.data(),f.submitted.data()+64,64);
    require(std::abs(shifted[12]-.064f)<1e-6f && std::abs(view[12]+.064f)<1e-6f,"actual submitted world and inverse view share the right-eye offset");
    auto receipts=c.receipts();require(receipts.size()==1 && receipts[0].appended && receipts[0].source_unchanged && receipts[0].bank_zero && !receipts[0].refusal && receipts[0].right==1,"exact native append receipt");
    const auto render_list=reinterpret_cast<uintptr_t>(f.list.data())-0x160;auto actual=f.actual();const auto revision=*E::get().revision(render_list);const auto pair=E::get().ordinary_pair(render_list,actual,revision);
    require(pair && E::get().current(*pair) && pair->left.render_id==101 && pair->right.render_id==101 && pair->producer_list==receipts[0].list && pair->producer_revision==receipts[0].producer_revision,"separate ordinary pair proves actual native add slots in one producing job");
    require(!E::get().primary(render_list,actual,revision) && !E::get().current(pair->left) && !E::get().pair(render_list,actual,revision),"two ordinary views cannot impersonate sole-main or main/PIP provenance");
    for(unsigned n=0;n<12;++n){auto p=*pair;switch(n){case 0:++p.left.render_id;break;case 1:++p.right.render_id;break;case 2:++p.left.token;break;case 3:++p.right.token;break;case 4:++p.left.revision;break;case 5:++p.right.revision;break;case 6:++p.left.entry;break;case 7:++p.right.entry;break;case 8:++p.left.slot;break;case 9:++p.right.slot;break;case 10:++p.right.add_list;break;case 11:++p.left.render_list;break;}require(!E::get().current(p),"each ordinary producing-frame identity is independently required");}
    auto changed=actual;changed[1].world[12]+=.01f;require(!E::get().ordinary_pair(render_list,changed,revision),"different observed right world cannot inherit a ticket");changed=actual;changed[1].role=E::Role::pip;require(!E::get().ordinary_pair(render_list,changed,revision),"PIP cannot enter an ordinary pair");
    // The engine copies the add list into different render-list storage. Require
    // the actual byte-verified copy path to carry the original producer identity.
    std::vector<uint8_t> copied(0x29c7c),copied_again(0x29c7c);
    const auto first_destination=reinterpret_cast<uintptr_t>(copied.data()),second_destination=reinterpret_cast<uintptr_t>(copied_again.data());
    auto transfer=E::get().begin_copy(first_destination,receipts[0].list);require(transfer && E::get().finish_copy(*transfer,true),"exact native copy transfers ordinary producer lineage");
    auto copied_views=actual;for(auto& entry:copied_views)entry.address=*E::address(first_destination,entry.slot);
    auto copied_pair=E::get().ordinary_pair(first_destination-0x160,copied_views,*E::get().revision(first_destination-0x160));
    require(copied_pair && E::get().current(*copied_pair) && copied_pair->producer_list==receipts[0].list && copied_pair->producer_revision==receipts[0].producer_revision && copied_pair->left.add_list!=copied_pair->producer_list,"copied render slots retain the separate native producer list and revision");
    for(unsigned n=0;n<2;++n){auto wrong=*copied_pair;if(n)++wrong.producer_revision;else ++wrong.producer_list;require(!E::get().current(wrong),"wrong copy origin cannot impersonate native ordinary provenance");}
    transfer=E::get().begin_copy(second_destination,first_destination);require(transfer && E::get().finish_copy(*transfer,true),"second native copy retains original lineage");
    for(auto& entry:copied_views)entry.address=*E::address(second_destination,entry.slot);
    copied_pair=E::get().ordinary_pair(second_destination-0x160,copied_views,*E::get().revision(second_destination-0x160));
    require(copied_pair && copied_pair->producer_list==receipts[0].list && copied_pair->producer_revision==receipts[0].producer_revision,"multi-copy lineage points to original producer, not intermediate storage");
    transfer=E::get().begin_copy(first_destination,second_destination);require(transfer && !E::get().finish_copy(*transfer,false) && !E::get().current(*copied_pair),"changed native copy bytes invalidate lineage on both endpoints");
    for(uint64_t id=102;id<106;++id){f.begin(id);f.invoke(c);}require(f.calls==4 && c.receipts().size()==4 && !E::get().current(*pair),"four-sample bound and later native job invalidate old receipts");
    Fixture blocked;C b;require(b.arm([&]{return reinterpret_cast<uintptr_t>(blocked.renderer.data());},{4,1,.064f}),"bank ownership negative arms");blocked.renderer[0x258]=1;blocked.begin(201);blocked.invoke(b);require(blocked.calls==0 && b.receipts().size()==1 && b.receipts()[0].refusal==2,"pre-existing ordinary bank is never stolen");blocked.renderer[0x258]=0;blocked.begin(202);blocked.invoke(b);require(blocked.calls==0,"ownership refusal stops future appends");
    Fixture warm;C w;require(w.arm([&]{return reinterpret_cast<uintptr_t>(warm.renderer.data());},{4,2,.064f}),"eligible cadence arms");warm.begin(301);warm.invoke(w);warm.invoke(w);require(!warm.calls,"same native job cannot count twice toward cadence");warm.begin(302);warm.invoke(w);require(warm.calls==1,"second distinct eligible job appends once");
    Fixture failure;C broken;require(broken.arm([&]{return reinterpret_cast<uintptr_t>(failure.renderer.data());},{4,1,.064f}),"callback failure arms");failure.begin(401);broken.on_main(failure.list.data(),failure.source.data(),&failure.frame,0,failure.args,[](void*,void*,const auto&)->int64_t{throw std::runtime_error("native fixture failure");});require(broken.receipts()[0].refusal==6,"callback failure does not strand an inflight provenance ticket");failure.begin(402);require(E::get().revision(reinterpret_cast<uintptr_t>(failure.list.data())-0x160).has_value(),"fresh main producer remains usable after refusal");
    Fixture requested;C halted;require(halted.arm([&]{return reinterpret_cast<uintptr_t>(requested.renderer.data());},{4,1,.064f}),"external render-thread stop fixture arms");std::thread stop_render([&]{halted.request_stop();});stop_render.join();requested.begin(451);requested.invoke(halted);require(!requested.calls && halted.receipts().empty(),"published render-thread refusal prevents a later experimental camera append");
    require(halted.restart() && !halted.statistics().stopped,"a consumer-requested stop (swap-chain reset) can be lifted on an armed control");requested.begin(452);requested.invoke(halted);require(requested.calls==1 && halted.receipts().size()==1,"the control appends again after the restart");
    {C never;require(!never.restart(),"an unarmed control cannot restart");}
    for(unsigned n=0;n<4;++n){C invalid;C::Config config;switch(n){case 0:config.samples=5;break;case 1:config.cadence=0;break;case 2:config.shift=0;break;case 3:config.shift=.2f;break;}require(!invalid.arm([]{return uintptr_t(1);},config),"invalid bounds never arm");}
    Fixture sequence;C contiguous;require(contiguous.arm([&]{return reinterpret_cast<uintptr_t>(sequence.renderer.data());},{4,3,.064f,true}),"bounded consecutive control arms");
    for(uint64_t id=501;id<=507;++id){sequence.begin(id);sequence.invoke(contiguous);sequence.invoke(contiguous);}
    const auto continuous=contiguous.receipts();require(sequence.calls==4 && continuous.size()==4,"consecutive mode retains four-call bound and ignores duplicate producer jobs");
    Fixture perf;C sustained;require(sustained.arm([&]{return reinterpret_cast<uintptr_t>(perf.renderer.data());},{4,2,.064f,false,true}),"performance mode arms");
    perf.begin(600);perf.invoke(sustained);require(!perf.calls,"performance mode starts disabled");sustained.performance_enabled(true);
    for(uint64_t id=601;id<=612;++id){perf.begin(id);perf.invoke(sustained);perf.invoke(sustained);}
    require(perf.calls==11 && sustained.receipts().empty() && sustained.statistics().appended==11,"continuous perf mode exceeds four, deduplicates jobs and stores counters only");
    sustained.performance_enabled(false);perf.begin(613);perf.invoke(sustained);require(perf.calls==11,"live off stops extra views");
    sustained.performance_enabled(true);perf.begin(614);perf.invoke(sustained);require(perf.calls==12,"live on resumes without stealing first-use ownership");
    perf.begin(616);sustained.on_main(perf.list.data(),perf.source.data(),&perf.frame,0,perf.args,[](void*,void*,const auto&)->int64_t{throw std::runtime_error("perf refusal");});
    const auto stopped_count=perf.calls;perf.begin(617);perf.invoke(sustained);require(sustained.statistics().stopped && sustained.statistics().refusals[6]==1 && perf.calls==stopped_count && sustained.receipts().empty(),"refusal stops perf permanently without growing receipts");
    for(unsigned i=0;i<4;++i)require(continuous[i].render_id==503+i,"consecutive mode warms up before four adjacent eligible jobs");
    NativeRenderProbe::Snapshot exported;exported.before.emplace_back();exported.before[0].projection[0]=1.25f;exported.before[0].inverse_projection[0]=.8f;std::ostringstream json;C::write_capture(json,exported,receipts[0]);
    require(json.str().find("\"projection\":[1.25,")!=std::string::npos && json.str().find("\"inverse_projection\":[0.8,")!=std::string::npos,"native eye projection/inverse are exported for independent SDK parity auditing");
    std::cout<<"Ordinary second-camera native tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}}
