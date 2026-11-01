#include "GpuTestHost.h"
#include "ScenePhaseTiming.h"
#include "SceneExecutionTiming.h"
#include "SceneQueryDependencyTiming.h"
#include "PresentHandoff.h"
#include <vector>
#include <fstream>

int main(){try{
    struct Event{std::string name;double start,duration,id;};std::vector<Event> events;
    const auto observe=[&](const char* name,double start,double duration,double id){events.push_back({name,start,duration,id});};
    {
        using Q=SceneQueryDependencyTiming;
        Q::Snapshot snapshot; snapshot.manager=33;snapshot.count=snapshot.captured=2;snapshot.keys[0]=0x101;snapshot.keys[1]=0x200;snapshot.valid=true;
        Q timing{0,111,true,observe};Q::fence(100,3);
        Q::consume(33,0x200,0,0,7,101); // Before this Scene's collector.
        Q::collect_begin(snapshot,102);Q::collect_end(103);
        Q::consume(34,0x200,0,0,8,103.1);Q::consume(33,0x200,1,0,9,103.2); // Foreign manager/column.
        {Q disabled{1,112,false,observe};Q::consume(33,0x200,0,0,10,103.3);}
        Q::consume(33,0x200,0,0,0x1e208be,103.5);Q::consume(33,0x100,1,4,11,104);
        timing.finish(110);const auto size=events.size();timing.finish(111);require(size==events.size(),"dependency scope emits once");
        const auto find=[&](const char* name)->const Event&{const auto it=std::ranges::find(events,std::string{name},&Event::name);require(it!=events.end(),"dependency event exists");return *it;};
        require(find("scene_left_dependency_matched_reads").id==2 && find("scene_left_dependency_reads_before").id==1,"dependency joins exact published object AND column on the same manager; nested disabled scope masks parent");
        require(find("scene_left_dependency_publish_to_use_cpu").duration==.5 && find("scene_left_dependency_publish_to_use_cpu").id==111 && find("scene_left_dependency_consumer_rva").id==0x1e208be,"consumer latency and RVA belong to this actual publish and Scene");
        require(find("scene_left_dependency_gc_pending").id==3 && find("scene_left_dependency_consumer_result").id==0,"pending retirement and zero visibility results are preserved as evidence");
    }
    events.clear();{
        using Q=SceneQueryDependencyTiming;Q timing{1,113,true,observe};Q::Snapshot bad;bad.manager=33;bad.valid=true;bad.captured=65;bad.count=65;
        Q::fence(1,0);Q::collect_begin(bad,2);Q::collect_end(3);Q::consume(33,0,0,1,4,4);timing.finish(5);
        require(std::ranges::none_of(events,[](const auto& e){return e.name=="scene_right_dependency_publish_to_use_cpu";}),"invalid/truncated key bounds cannot manufacture dependency proof");
    }
    events.clear();
    {
        ScenePhaseTiming left{0,77,100,true,observe};
        ScenePhaseTiming::wait(0,true,103);ScenePhaseTiming::wait(0,false,104);
        {
            ScenePhaseTiming nested{1,78,104,true,observe};
            ScenePhaseTiming::wait(0,true,105);ScenePhaseTiming::wait(0,false,106);
            ScenePhaseTiming::wait(1,true,107);ScenePhaseTiming::wait(1,false,108);
            ScenePhaseTiming::wait(2,true,109);ScenePhaseTiming::wait(2,false,110);nested.finish(115);nested.finish(116);
        }
        ScenePhaseTiming::wait(1,true,116);ScenePhaseTiming::wait(1,false,118);
        ScenePhaseTiming::wait(2,true,120);ScenePhaseTiming::wait(2,false,121);left.finish(125);
    }
    std::vector<double> left;unsigned completions{};
    for(const auto& event:events){if(event.id==77 && event.duration)left.push_back(event.duration);if(event.name.ends_with("phase_complete"))++completions;}
    require(left==std::vector<double>{3,12,2,4} && completions==2,"phase spans partition real boundaries and nested scopes restore their parent; finish emits once");
    events.clear();{
        ScenePhaseTiming invalid{0,79,0,true,observe};ScenePhaseTiming::wait(1,true,4);invalid.finish(10);
    }
    require(events.size()==1 && events[0].name=="scene_left_unclassified_tail_cpu","missing/out-of-order boundaries never claim a complete named partition");
    events.clear();{ScenePhaseTiming disabled{0,80,0,false,observe};ScenePhaseTiming::wait(0,true,3);disabled.finish(4);}
    require(events.empty(),"disabled probes do not publish events");
    const auto cpu_probe=[&](unsigned fault){
        events.clear();unsigned read{};
        SceneExecutionTiming timing{0,91,fault!=5,[&]()->std::optional<SceneExecutionTiming::Counter>{
            if(fault==4)return {};return SceneExecutionTiming::Counter{read++?uint64_t(fault==3?10:30000):10000,DWORD(fault==2 && read>1?8:7)};
        },observe};
        if(fault!=1)SceneExecutionTiming::third_wait_end(100);
        SceneExecutionTiming::sdk_entry(112);
        if(fault==6)SceneExecutionTiming::sdk_entry(113);
        timing.finish();timing.finish();
    };
    cpu_probe(0);require(events.size()==2 && events[0].duration==12 && events[1].duration==2 && events[1].id==91,"thread CPU delta differs from elapsed wall time and publishes once");
    for(unsigned fault=1;fault<=6;++fault){cpu_probe(fault);require(events.empty(),"incomplete/foreign-thread/reversed/unavailable/disabled/duplicate SDK counters never fabricate running time");}
    PresentHandoff handoff;unsigned calls{};
    require(!handoff.visit(PresentHandoff::Point::post_present,[&]{++calls;},7),"extra callbacks cannot establish a foreign Present owner");
    require(handoff.visit(PresentHandoff::Point::present,[&]{++calls;},7) && calls==1,"baseline Present callback executes");
    require(!handoff.visit(PresentHandoff::Point::post_frame,[&]{++calls;},7),"baseline has no extra handoff");
    require(handoff.mode("opportunistic") && !handoff.mode("opportunistic "),"only exact control vocabulary changes mode");
    require(!handoff.visit(PresentHandoff::Point::post_present,[&]{++calls;},8),"extra handoff refuses another thread");
    handoff.visit(PresentHandoff::Point::post_frame,[&]{++calls;require(!handoff.visit(PresentHandoff::Point::post_present,[&]{++calls;},7),"reentrant runtime callbacks cannot submit twice");},7);
    require(calls==2,"post-frame mode executes exactly once without nested submission");
    try{handoff.visit(PresentHandoff::Point::post_present,[]{throw std::runtime_error("fixture");},7);}catch(...){}
    require(handoff.visit(PresentHandoff::Point::post_present,[&]{++calls;},7),"exception restores the callback guard");
    handoff.reset();require(!handoff.visit(PresentHandoff::Point::post_frame,[&]{++calls;},7),"device reset invalidates callback owner until next Present");
    require(handoff.mode("queued") && handoff.queued() && !handoff.extra() && handoff.code()==2,"queue ordered mode is independent of extra callback polling");
    require(!handoff.capture_here(false,false) && handoff.capture_here(true,false) && handoff.capture_here(false,true),"queued mode defers new captures to native CPU completion while retaining the original HUD capture boundary");
    require(!handoff.visit(PresentHandoff::Point::dispatch_complete,[&]{++calls;},7),"dispatch completion cannot establish a Present owner");
    handoff.visit(PresentHandoff::Point::present,[]{},7);
    require(!handoff.visit(PresentHandoff::Point::dispatch_complete,[&]{++calls;},8),"native completion on another thread cannot touch bridge or runtime state");
    bool captured{},submitted{};
    require(handoff.visit(PresentHandoff::Point::dispatch_complete,[&]{captured=true;handoff.visit(PresentHandoff::Point::present,[&]{submitted=true;},7);},7) && captured && !submitted,"owned post-dispatch enqueue cannot reenter Present submission");
    handoff.mode("present");require(handoff.capture_here(false,false) && !handoff.visit(PresentHandoff::Point::dispatch_complete,[]{},7),"baseline neither defers capture nor adds native completion work");
    require(handoff.mode("isolated") && handoff.isolated() && !handoff.queued() && !handoff.extra() && handoff.code()==3 && handoff.capture_here(false,false),"isolated runtime queue keeps baseline capture and callback timing");
    require(!handoff.visit(PresentHandoff::Point::dispatch_complete,[]{},7),"isolated queue mode adds no early callback work");
    const auto converted=GpuTimestampSpan::convert({1000,1200},1000,1500,10000,1000);
    require(converted && converted->start_ms==9500 && converted->duration_ms==200,"calibration handles GPU samples before its anchor without unsigned wrap");
    require(!GpuTimestampSpan::convert({1200,1000},1000,1500,10000,1000) && !GpuTimestampSpan::convert({0,0},1000,0,0,1000) &&
        !GpuTimestampSpan::convert({1,2},0,0,0,1000) && !GpuTimestampSpan::convert({1,2},1000,0,0,0),"invalid GPU intervals and clock frequencies cannot create measurements");

    Host host;GpuTimestampSpan span;require(!span.configure(nullptr,host.queue.Get()) && span.configure(host.device.Get(),host.queue.Get()),"GPU query storage checks inputs and configures on the actual device/queue");
    auto source=host.color(1,0,0),destination=host.color(0,0,1);
    ComPtr<ID3D12InfoQueue> debug;host.device.As(&debug);if(debug)debug->ClearStoredMessages();
    for(unsigned n=0;n<3;++n){
        host.begin();span.begin(host.commands.Get());
        D3D12_RESOURCE_BARRIER before[]{barrier(source.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE),barrier(destination.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST)};
        host.commands->ResourceBarrier(2,before);host.commands->CopyResource(destination.Get(),source.Get());
        for(auto& value:before)std::swap(value.Transition.StateBefore,value.Transition.StateAfter);host.commands->ResourceBarrier(2,before);
        span.end(host.commands.Get());host.execute();const auto measured=span.read(host.queue.Get());
        require(measured && measured->start_ms>0 && measured->duration_ms>=0 && measured->duration_ms<1000,"actual completed GPU timestamps resolve and storage safely reuses after the same queue fence");
    }
    if(debug)for(UINT64 i=0;i<debug->GetNumStoredMessages();++i){SIZE_T bytes{};debug->GetMessage(i,nullptr,&bytes);std::vector<char> data(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());check(debug->GetMessage(i,message,&bytes));require(message->Severity!=D3D12_MESSAGE_SEVERITY_ERROR && message->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,"timestamp recording/resolution/reuse must have no D3D12 debug errors");}

    const auto output=std::filesystem::temp_directory_path()/("sm2vr_probe_"+std::to_string(GetCurrentProcessId())+".csv");
    auto& trace=PerformanceTrace::get();require(!trace.probes() && trace.open(output,{},1000,true) && trace.probes(),"detailed probes require an explicitly enabled profiling session");
    {
        EyeTextureCapture capture;ComPtr<ID3D12Fence> gate;check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));check(host.queue->Wait(gate.Get(),1));
        require(capture.capture(host.device.Get(),host.queue.Get(),source.Get(),meta(1,0,100))==S_OK,"instrumented copy enqueues behind deterministic GPU gate");
        capture.poll();trace.flush();
        {std::ifstream file{output};std::string text{std::istreambuf_iterator<char>{file},{}};require(text.find(",bridge_left_gpu,")==std::string::npos,"pending query buffers are never mapped/published before copy fence completion");}
        check(gate->Signal(1));const auto image=capture.latest(0);wait(image->fence.Get(),image->fence_value);capture.poll();host.pixels(*image,255,0,0);
        trace.flush();std::ifstream file{output};std::string text{std::istreambuf_iterator<char>{file},{}};
        require(text.find(",bridge_left_gpu,")!=std::string::npos && text.find(",bridge_left_completion_poll_delay_cpu,")!=std::string::npos,"production capture publishes GPU duration and completion observation only after completion while preserving pixels");
        const auto once=text;capture.poll();trace.flush();std::ifstream again{output};text.assign(std::istreambuf_iterator<char>{again},{});require(text==once,"a completed copy's query is published exactly once");
    }
    std::cout<<"performance probes PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
