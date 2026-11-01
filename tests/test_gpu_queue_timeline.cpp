// GpuQueueTimeline on a real (WARP) D3D12 device: real vtable detours, unchanged results, the
// packed rows, and the watcher's GPU completion rows.
#include "GpuQueueTimeline.h"
#include <dxgi1_6.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D12 call failed: "+std::to_string(hr));}
struct Rows {
    std::mutex mutex;std::vector<std::tuple<std::string,double,double,double>> rows;
    void add(const char* e,double s,double d,double v){std::lock_guard lock{mutex};rows.emplace_back(e,s,d,v);}
    std::vector<std::tuple<std::string,double,double,double>> named(const std::string& e){std::lock_guard lock{mutex};std::vector<std::tuple<std::string,double,double,double>> out;for(auto& r:rows)if(std::get<0>(r)==e)out.push_back(r);return out;}
};
}
int main(){try{
    require(GpuQueueTimeline::unpack(GpuQueueTimeline::pack(3,17,0x1'0000'0005ull))==std::array<uint64_t,3>{3,17,5},"packing keeps queue, fence and the value's low 32 bits");
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> warp;check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    ComPtr<ID3D12Device> device;check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    D3D12_COMMAND_QUEUE_DESC direct{};direct.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    D3D12_COMMAND_QUEUE_DESC compute{};compute.Type=D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> queue,second;check(device->CreateCommandQueue(&direct,IID_PPV_ARGS(&queue)));check(device->CreateCommandQueue(&compute,IID_PPV_ARGS(&second)));
    ComPtr<ID3D12Fence> fence,gate;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
    Rows rows;auto& timeline=GpuQueueTimeline::get();
    require(!timeline.enabled(),"off until installed");
    timeline.executed(queue.Get(),2);require(rows.rows.empty(),"no rows before install");
    require(timeline.install(device.Get(),queue.Get(),[&](const char* e,double s,double d,double v){rows.add(e,s,d,v);}),"hooks install on the runtime's vtables");
    timeline.executed(queue.Get(),2);
    require(rows.named("queue_identity").size()==1 && std::get<3>(rows.named("queue_identity")[0])==1*16+D3D12_COMMAND_LIST_TYPE_DIRECT,"first queue: id 1, direct");
    require(std::get<3>(rows.named("queue_execute")[0])==1002,"execute row: queue 1, two lists");
    // The second queue waits (on the GPU) for a gate the CPU opens later, then signals the fence.
    check(second->Wait(gate.Get(),1));check(second->Signal(fence.Get(),7));
    Sleep(30);require(fence->GetCompletedValue()<7,"a GPU wait really holds the queue");
    require(rows.named("fence_completed").empty(),"nothing completes while the queue waits");
    check(gate->Signal(1));
    const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    check(fence->SetEventOnCompletion(7,event));require(WaitForSingleObject(event,2000)==WAIT_OBJECT_0,"original SetEventOnCompletion still works");CloseHandle(event);
    for(int i=0;i<100 && rows.named("fence_completed").size()<2;++i)Sleep(5);
    const auto wait=rows.named("queue_wait"),signal=rows.named("queue_signal"),done=rows.named("fence_completed");
    require(wait.size()==1 && GpuQueueTimeline::unpack(std::get<3>(wait[0]))==std::array<uint64_t,3>{2,1,1},"GPU wait: queue 2 on fence 1 (gate) value 1");
    require(signal.size()==1 && GpuQueueTimeline::unpack(std::get<3>(signal[0]))==std::array<uint64_t,3>{2,2,7},"queued signal: queue 2, fence 2, value 7");
    require(done.size()==2,"one completion for the queued signal and one for the awaited gate value");
    const auto reached=[&](double packed){for(auto& d:done)if(std::get<3>(d)==packed)return std::get<1>(d);return -1.0;};
    require(reached(std::get<3>(signal[0]))>=std::get<1>(signal[0]),"the watcher reports when the GPU reached the signal");
    require(reached(GpuQueueTimeline::pack(0,1,1))>=std::get<1>(wait[0]),"and when the awaited value arrived (queue 0)");
    require(rows.named("fence_cpu_signal").size()==1 && GpuQueueTimeline::unpack(std::get<3>(rows.named("fence_cpu_signal")[0]))==std::array<uint64_t,3>{0,1,1},"CPU signal of the gate");
    require(rows.named("fence_cpu_wait").size()==1 && GpuQueueTimeline::unpack(std::get<3>(rows.named("fence_cpu_wait")[0]))==std::array<uint64_t,3>{0,2,7},"CPU wait registration is traced; the watcher's own are not");
    require(rows.named("queue_identity").size()==2,"the compute queue got its own identity");
    require(timeline.pending()==0,"completed values release their fence");

    require(timeline.execute_hooked(),"ExecuteCommandLists is hooked when nothing else holds it");
    {
        ComPtr<ID3D12CommandAllocator> allocator;check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));check(list->Close());
        const auto executes=rows.named("queue_execute").size();
        ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
        check(queue->Signal(fence.Get(),9));const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(fence->SetEventOnCompletion(9,event));
        require(WaitForSingleObject(event,2000)==WAIT_OBJECT_0,"the real submission ran");CloseHandle(event);
        require(rows.named("queue_execute").size()==executes+1 && std::get<3>(rows.named("queue_execute").back())==1001,"a real ExecuteCommandLists is traced");
    }
    timeline.uninstall();require(!timeline.enabled(),"uninstall stops tracing");
    const auto before=rows.rows.size();check(queue->Signal(fence.Get(),8));require(rows.rows.size()==before,"no rows after uninstall");
    std::cout<<"gpu queue timeline: detours, packed rows and GPU completion pass\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
