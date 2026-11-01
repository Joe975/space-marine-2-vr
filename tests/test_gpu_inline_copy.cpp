#include <thread>
#include "GpuTestHost.h"
#include "GpuInlineCopy.h"
#include "GpuCompletionProbe.h"
#include "RenderTargetProbe.h"
#include "GpuTextureReadback.h"
#include "GpuCompositorOutput.h"
#include <d3dcompiler.h>
#include <MinHook.h>
#include <sstream>
#include <future>
#include <fstream>
#include "GpuApiForwardScope.h"
#include "GpuPresentMirror.h"

int main(int argc,char** argv){try{
    require(InlineCopyAllocation::parse("pooled")==InlineCopyAllocation::Mode::pooled && InlineCopyAllocation::parse("committed")==InlineCopyAllocation::Mode::committed,"exact allocation control vocabulary");
    for(const auto value:{"", "pooled ", "committed extra", "none", "1"})require(!InlineCopyAllocation::parse(value),"malformed allocation control cannot select a policy");
    InlineCopyAllocation policy;const auto now=InlineCopyAllocation::Clock::time_point{};
    require(policy.mode(now)==InlineCopyAllocation::Mode::pooled && !policy.control("relative.txt"),"pooled default and absolute control path contract");
    const auto control_path=std::filesystem::temp_directory_path()/("sm2vr-allocation-"+std::to_string(GetCurrentProcessId())+".txt");
    require(policy.control(control_path),"absolute allocation control");
    {std::ofstream file{control_path};file<<"committed\n";}
    require(policy.mode(now)==InlineCopyAllocation::Mode::committed,"committed control reaches the production policy");
    {std::ofstream file{control_path};file<<"pooled\r\n";}
    require(policy.mode(now+std::chrono::milliseconds(999))==InlineCopyAllocation::Mode::committed && policy.mode(now+std::chrono::seconds(1))==InlineCopyAllocation::Mode::pooled,"bounded poll preserves current policy until the next interval and accepts CRLF");
    {std::ofstream file{control_path};file<<"committed extra\n";}
    require(policy.mode(now+std::chrono::seconds(2))==InlineCopyAllocation::Mode::pooled,"partial control edits preserve the active policy");
    std::filesystem::remove(control_path);
    require(policy.mode(now+std::chrono::seconds(3))==InlineCopyAllocation::Mode::pooled,"missing control preserves the active policy");
    require(GpuSignatureProbe::get().install_device_creation(),"early native lifecycle observation");
    Host host;
    {
        GpuImageHeapPool pool;GpuImageHeapPool::Allocation images[GpuImageHeapPool::CAPACITY],extra;
        auto descriptor=host.color(0,0,0,8,DXGI_FORMAT_R8G8B8A8_UNORM,4)->GetDesc();descriptor.Flags=D3D12_RESOURCE_FLAG_NONE;
        for(auto& image:images)check(pool.allocate(host.device.Get(),descriptor,image));
        require(pool.allocate(host.device.Get(),descriptor,extra)==E_PENDING,"consumer-held images bound the reusable heap pool");
        auto external=images[0].texture;const auto first_heap=images[0].heap;images[0].texture.Reset();
        require(pool.allocate(host.device.Get(),descriptor,extra)==E_PENDING,"a bare external resource ComPtr keeps the private heap lease alive");
        external.Reset();check(pool.allocate(host.device.Get(),descriptor,extra));
        require(extra.reused && extra.heap==first_heap,"only the resource's final release returns a heap to the pool");
        extra.texture.Reset();descriptor.Width=1024;descriptor.Height=512;
        check(pool.allocate(host.device.Get(),descriptor,extra));
        require(!extra.reused && extra.heap!=first_heap,"a free undersized heap can be replaced after a resolution increase without expanding a full pool");
        pool.reset();require(extra.texture->GetDesc().Width==descriptor.Width,"an image retains its heap after pool reset");
    }
    auto& targets=RenderTargetProbe::get();auto& coordinator=*new GpuCompletionProbe;auto& copy=*new GpuInlineCopy;
    require(targets.install(host.device.Get()),"actual RTV callbacks install");
    coordinator.inline_copies(&copy);
    targets.observe_binding=[&](uintptr_t command,std::span<const uintptr_t> values,bool known){coordinator.bound(command,values,known);};
    require(coordinator.install(host.device.Get(),host.queue.Get(),[&](uintptr_t handle){return targets.canonical_resource(handle);},{},[&](uintptr_t command){return targets.observes_binding(command);}),"one real command-hook owner supplies inline callbacks");
    const auto texture=host.color(0,0,0,8,DXGI_FORMAT_R8G8B8A8_UNORM,4);ID3D12Resource* resources[]{texture.Get()};
    auto configure=[&]{require(copy.configure(host.device.Get(),host.queue.Get(),resources),"configure actual source and same-device direct queue");};configure();
    D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;desc.NumDescriptors=1;ComPtr<ID3D12DescriptorHeap> heap;check(host.device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&heap)));
    const auto handle=heap->GetCPUDescriptorHandleForHeapStart();host.device->CreateRenderTargetView(texture.Get(),nullptr,handle);
    const float red[]{1,0,0,1},blue[]{0,0,1,1};
    auto begin_frame=[&]{host.begin();auto transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);host.commands->OMSetRenderTargets(1,&handle,TRUE,nullptr);};
    auto end_frame=[&]{auto transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&transition);host.execute();};
    ComPtr<ID3D12CommandQueue> unrelated_queue;D3D12_COMMAND_QUEUE_DESC unrelated_desc{};unrelated_desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(host.device->CreateCommandQueue(&unrelated_desc,IID_PPV_ARGS(&unrelated_queue)));
    ComPtr<ID3D12CommandAllocator> unrelated_allocator;ComPtr<ID3D12GraphicsCommandList> unrelated_commands;
    check(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&unrelated_allocator)));
    check(host.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,unrelated_allocator.Get(),nullptr,IID_PPV_ARGS(&unrelated_commands)));check(unrelated_commands->Close());
    ID3D12CommandList* unrelated_lists[]{unrelated_commands.Get()};
    ComPtr<ID3D12Fence> unrelated_done;check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&unrelated_done)));
    begin_frame();require(copy.begin(1,texture.Get()),"first exact producing scope begins");
    unrelated_queue->ExecuteCommandLists(1,unrelated_lists);
    check(unrelated_queue->Signal(unrelated_done.Get(),1));wait(unrelated_done.Get(),1);
    host.commands->ClearRenderTargetView(handle,red,0,nullptr);
    unrelated_queue->ExecuteCommandLists(1,unrelated_lists);
    check(unrelated_queue->Signal(unrelated_done.Get(),2));wait(unrelated_done.Get(),2);
    const auto left=coordinator.finish_inline(1);require(left.appended && !left.completed && left.state==D3D12_RESOURCE_STATE_RENDER_TARGET,"first eye copied within actual open native recording");
    D3D12_HEAP_PROPERTIES destination_heap{};D3D12_HEAP_FLAGS destination_flags{};
    check(left.texture->GetHeapProperties(&destination_heap,&destination_flags));
    require(left.destination_heap && !left.heap_reused && destination_heap.Type==D3D12_HEAP_TYPE_DEFAULT,"first immutable copy is a distinct placed image in a newly leased default heap");
    require(copy.begin(2,texture.Get()),"second producing scope uses same shared source");host.commands->ClearRenderTargetView(handle,blue,0,nullptr);
    const auto right=coordinator.finish_inline(2);require(right.appended && left.generation==right.generation && left.commands==right.commands,"both copies precede single native batch submission");
    require(!copy.reset(),"unsubmitted recorded resources cannot retire");
    require(!copy.retire(1),"unsubmitted copy cannot release its producer lease");
    ComPtr<ID3D12Fence> gate;check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));check(host.queue->Wait(gate.Get(),1));
    auto transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&transition);check(host.commands->Close());
    ID3D12CommandList* lists[]{host.commands.Get()};{GpuApiForwardScope forwarding;host.queue->ExecuteCommandLists(1,lists);}
    const auto pending=copy.samples();require(pending.size()==2 && pending[0].submitted && pending[1].submitted && !pending[0].completed && !pending[1].completed,"actual queue gate blocks completion; CPU return is insufficient");
    require(!copy.reset() && !copy.retire(1),"queued incomplete copies cannot retire");check(gate->Signal(1));check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
    const auto completed=copy.samples();require(completed[0].completed && completed[1].completed && completed[0].submission==completed[1].submission,"both inline copies finish on the exact native batch fence");
    std::array<GpuTextureReadback::Source,2> readback{{{completed[0].texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE},{completed[1].texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE}}};std::vector<GpuTextureReadback::Pixels> pixels;check(GpuTextureReadback::capture(host.queue.Get(),readback,pixels));
    require(pixels.size()==2,"both retained images read back");for(size_t n=0;n<pixels[0].rgb.size();n+=3)require(pixels[0].rgb[n]==255 && pixels[0].rgb[n+1]==0 && pixels[0].rgb[n+2]==0 && pixels[1].rgb[n]==0 && pixels[1].rgb[n+1]==0 && pixels[1].rgb[n+2]==255,"pre-overwrite copies retain independent exact red/blue pixels");
    std::ostringstream exported;GpuInlineCopy::write_sample(exported,completed[0]);require(exported.str().find("\"completed\":true")!=std::string::npos,"completion evidence is exported");
    require(copy.retire(1) && copy.retire(2) && !copy.retire(1) && copy.samples().empty(),"completed copies retire exactly once without resetting source hooks");
    uintptr_t continued_heap{};
    for(uint64_t tag=100;tag<108;++tag){begin_frame();require(copy.begin(tag,texture.Get()),"retirement permits more than four sequential captures");host.commands->ClearRenderTargetView(handle,blue,0,nullptr);const auto image=coordinator.finish_inline(tag);require(image.appended,"continued copy appended");
        require(image.destination_heap && image.destination_heap!=left.destination_heap && image.destination_heap!=right.destination_heap,"held old images exclude their heaps from reuse");
        if(continued_heap)require(image.destination_heap==continued_heap && image.heap_reused,"completed and unleased continuation reuses its heap with a fresh image");continued_heap=image.destination_heap;
        end_frame();require(copy.retire(tag),"completed continuation retires");}
    std::vector<GpuInlineCopy::Sample> pinned;
    for(uint64_t tag=200;tag<210;++tag){begin_frame();require(copy.begin(tag,texture.Get()),"heap exhaustion control begins");host.commands->ClearRenderTargetView(handle,blue,0,nullptr);pinned.push_back(coordinator.finish_inline(tag));require(pinned.back().appended,"held images fall back to a fresh committed copy when the heap pool is full");end_frame();require(copy.retire(tag),"pinned completed image retires its producer");}
    require(std::ranges::any_of(pinned,[](const auto& image){return !image.destination_heap;}),"bounded pool exhaustion uses the original committed allocation path");
    const std::array<GpuTextureReadback::Source,1> fallback_source{{{pinned.back().texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE}}};
    std::vector<GpuTextureReadback::Pixels> fallback_pixels;check(GpuTextureReadback::capture(host.queue.Get(),fallback_source,fallback_pixels));
    for(size_t n=0;n<fallback_pixels[0].rgb.size();n+=3)require(fallback_pixels[0].rgb[n]==0 && fallback_pixels[0].rgb[n+2]==255,"committed fallback keeps exact copied pixels");
    check(GpuTextureReadback::capture(host.queue.Get(),readback,pixels));
    for(size_t n=0;n<pixels[0].rgb.size();n+=3)require(pixels[0].rgb[n]==255 && pixels[0].rgb[n+2]==0 && pixels[1].rgb[n]==0 && pixels[1].rgb[n+2]==255,"external completed leases preserve old pixels across capacity reuse");
    ComPtr<ID3D12InfoQueue> messages;
    if(SUCCEEDED(host.device.As(&messages)))for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){
        SIZE_T size{};check(messages->GetMessage(i,nullptr,&size));std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());check(messages->GetMessage(i,message,&size));
        require(message->Severity!=D3D12_MESSAGE_SEVERITY_ERROR && message->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,message->pDescription);
    }
    require(copy.reset(),"completed resources retire");configure();
    begin_frame();require(copy.begin(3,texture.Get()),"binding-only control begins");auto refused=coordinator.finish_inline(3);require(!refused.appended && refused.refusal==GpuInlineCopy::Refusal::no_writer,"binding alone cannot authorize copy");end_frame();
    begin_frame();require(copy.begin(4,texture.Get()),"alias control begins");D3D12_RESOURCE_BARRIER alias{};alias.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;{GpuApiForwardScope forwarding;host.commands->ResourceBarrier(1,&alias);}host.commands->ClearRenderTargetView(handle,red,0,nullptr);refused=coordinator.finish_inline(4);
    require(!refused.appended && refused.refusal==GpuInlineCopy::Refusal::hazard && (refused.hazards&GpuStateEvidence::aliasing) && refused.access.aliases==1 && refused.access.alias_null==1 && !refused.access.before_allocation,"actual global alias remains refused and exports exact null endpoints");end_frame();
    begin_frame();require(copy.begin(5,texture.Get()),"closed control begins");host.commands->ClearRenderTargetView(handle,blue,0,nullptr);transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&transition);check(host.commands->Close());refused=coordinator.finish_inline(5);
    require(!refused.appended && refused.refusal==GpuInlineCopy::Refusal::recording_changed,"closed command list cannot accept copy");host.queue->ExecuteCommandLists(1,lists);check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
    begin_frame();require(copy.begin(6,texture.Get()),"nested control begins");host.commands->ClearRenderTargetView(handle,red,0,nullptr);copy.invalidate_active(GpuInlineCopy::Refusal::busy);refused=coordinator.finish_inline(6);require(!refused.appended && refused.refusal==GpuInlineCopy::Refusal::busy,"nested provenance refuses append");end_frame();
    require(!copy.begin(7,texture.Get()),"bounded four-result capacity");require(copy.reset(),"refused controls hold no GPU work");configure();
    begin_frame();require(copy.begin(7,texture.Get()),"post-record reset control");host.commands->ClearRenderTargetView(handle,red,0,nullptr);copy.recording(host.commands.Get(),UINT64_MAX,true);refused=coordinator.finish_inline(7);require(!refused.appended && refused.refusal==GpuInlineCopy::Refusal::recording_changed,"generation replacement invalidates old producing scope");end_frame();
    require(copy.reset(),"final clean retirement");
    {   // 29 Oct: barriers on a command list never seen recording, on another render thread, refused the
        // left eye at startup and stopped stereo. Such a list refuses the eye only when it names the source.
        // A list the copy never saw begin recording (the harness's hooks register every list it creates),
        // so it is only a map key on this path, never dereferenced.
        auto* unseen=reinterpret_cast<ID3D12GraphicsCommandList*>(uintptr_t{0x7f00});
        D3D12_HEAP_PROPERTIES upload{};upload.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=256;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> unrelated_buffer;check(host.device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&unrelated_buffer)));
        const auto unrelated=barrier(unrelated_buffer.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        configure();const auto named=barrier(texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON); // after configure(): it makes the texture
        begin_frame();require(copy.begin(30,texture.Get()),"foreign-list control begins");host.commands->ClearRenderTargetView(handle,red,0,nullptr);
        std::thread([&]{copy.barriers(unseen,1,&unrelated);}).join();
        auto kept=coordinator.finish_inline(30);require(kept.appended && kept.refusal==GpuInlineCopy::Refusal::none && !kept.changed_site,"an unknown list on another thread that does not name the source leaves the eye alone");end_frame();
        begin_frame();require(copy.begin(31,texture.Get()),"named-source control begins");host.commands->ClearRenderTargetView(handle,red,0,nullptr);
        std::thread([&]{copy.barriers(unseen,1,&named);}).join();
        auto refused_named=coordinator.finish_inline(31);require(!refused_named.appended && refused_named.refusal==GpuInlineCopy::Refusal::recording_changed && refused_named.changed_site==4 && refused_named.changed_elsewhere,"an unknown list naming the source still refuses the eye");end_frame();
        begin_frame();require(copy.begin(32,texture.Get()),"same-thread control begins");host.commands->ClearRenderTargetView(handle,red,0,nullptr);copy.barriers(unseen,1,&unrelated);
        auto refused_own=coordinator.finish_inline(32);require(!refused_own.appended && refused_own.refusal==GpuInlineCopy::Refusal::recording_changed && refused_own.changed_site==4 && !refused_own.changed_elsewhere,"an unknown list on the eye's own thread still refuses it");end_frame();
        require(copy.reset(),"foreign-list controls retire");
    }
    {   // Command-list churn: a full recording table used to clear itself and refuse the active eye.
        // Closed lists nothing refers to are dropped instead; the eye keeps its copy.
        ComPtr<ID3D12CommandAllocator> churn_allocator;check(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&churn_allocator)));
        std::array<ComPtr<ID3D12GraphicsCommandList>,3> churn;
        configure();copy.recording_capacity(3);
        begin_frame();require(copy.begin(40,texture.Get()),"churn control begins");host.commands->ClearRenderTargetView(handle,red,0,nullptr);
        for(size_t i=0;i<churn.size();++i){
            check(host.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,churn_allocator.Get(),nullptr,IID_PPV_ARGS(&churn[i])));
            copy.recording(churn[i].Get(),1000+i,true);
            if(i+1<churn.size()){check(churn[i]->Close());copy.closed(churn[i].Get(),true);}
        }
        const auto kept=coordinator.finish_inline(40);
        require(kept.appended && kept.refusal==GpuInlineCopy::Refusal::none && !kept.changed_site,"a full recording table drops closed lists and keeps the active eye");end_frame();
        check(churn.back()->Close());copy.recording_capacity(2048);require(copy.reset(),"churn control retires");
    }
    for(bool wrong_queue:{false,true}){
        configure();begin_frame();require(copy.begin(20,texture.Get()),"premature writer submission control");host.commands->ClearRenderTargetView(handle,red,0,nullptr);
        // Exercise the submission observer without executing an open GPU list.
        copy.submitted(wrong_queue?unrelated_queue.Get():host.queue.Get(),1,lists);
        const auto premature=coordinator.finish_inline(20);require(!premature.appended && premature.refusal==GpuInlineCopy::Refusal::submission,"actual writer submitted before append is refused on either queue");end_frame();require(copy.reset(),"premature submission control retirement");
    }
    configure();const auto other=host.color(0,1,0,8,DXGI_FORMAT_R8G8B8A8_UNORM,4);const auto other_allocation=GpuAllocationProbe::read(other.Get());require(other_allocation && other_allocation->range.valid(),"actual unrelated allocation is observed before the native discard callback");
    begin_frame();require(copy.begin(10,texture.Get()),"extended writer diagnostic scope begins");host.commands->DiscardResource(other.Get(),nullptr);
    // Removing the live object's annotation after the callback must not erase
    // the POD already copied by that actual callback.
    check(other->SetPrivateData(GpuAllocationProbe::KEY,0,nullptr));host.commands->ClearRenderTargetView(handle,red,0,nullptr);const auto extended=coordinator.finish_inline(10);
    require(!extended.appended && (extended.hazards&GpuStateEvidence::extended_writer) && extended.access.first_extended==GpuTargetAccess::discard && extended.access.extended_resource==GpuObjectIdentity::get(other.Get()) && extended.access.extended_blocked[GpuTargetAccess::discard]==1 && extended.access.extended_allocation && extended.access.extended_allocation->range.resource==other_allocation->range.resource && extended.access.extended_callsite,"actual discard reports the blocked API, source identity, callsite and callback-time allocation independently of later metadata changes");end_frame();require(copy.reset(),"extended diagnostic control retirement");
    D3D12_HEAP_PROPERTIES display_heap{};display_heap.Type=D3D12_HEAP_TYPE_DEFAULT;display_heap.CreationNodeMask=display_heap.VisibleNodeMask=1;
    const auto hidden=CreateWindowExW(0,L"STATIC",L"SM2 inline copy fixture",WS_OVERLAPPEDWINDOW,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    require(hidden!=nullptr,"owned hidden swap-chain fixture window");
    ComPtr<IDXGIFactory4> display_factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&display_factory)));
    DXGI_SWAP_CHAIN_DESC1 swap_desc{};swap_desc.Width=8;swap_desc.Height=4;swap_desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;swap_desc.SampleDesc.Count=1;swap_desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;swap_desc.BufferCount=2;swap_desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> display_swap;check(display_factory->CreateSwapChainForHwnd(host.queue.Get(),hidden,&swap_desc,nullptr,nullptr,&display_swap));
    ComPtr<ID3D12Resource> display;check(display_swap->GetBuffer(0,IID_PPV_ARGS(&display)));const auto display_desc=display->GetDesc();
    D3D12_HEAP_PROPERTIES actual_heap{};D3D12_HEAP_FLAGS actual_flags{};check(display->GetHeapProperties(&actual_heap,&actual_flags));
    std::cout<<"GetBuffer: alignment="<<display_desc.Alignment<<" flags="<<display_desc.Flags<<" format="<<display_desc.Format<<" layout="<<display_desc.Layout<<" quality="<<display_desc.SampleDesc.Quality<<" heap="<<actual_flags<<" type="<<actual_heap.Type<<" cpu="<<actual_heap.CPUPageProperty<<" pool="<<actual_heap.MemoryPoolPreference<<" nodes="<<actual_heap.CreationNodeMask<<','<<actual_heap.VisibleNodeMask<<'\n';
    const bool private_display=GpuInlineCopy::display_backing(display_desc,actual_heap,actual_flags);
    require(!(actual_flags&D3D12_HEAP_FLAG_SHARED) || !private_display,"actual shared GetBuffer cannot be relabelled private");
    const auto private_flags=D3D12_HEAP_FLAGS(D3D12_HEAP_FLAG_ALLOW_DISPLAY|D3D12_HEAP_FLAG_DENY_BUFFERS|D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES);
    require(GpuInlineCopy::display_backing(display_desc,display_heap,private_flags),"CPU policy accepts restricted non-shared display backing and actual resolved 64 KiB alignment");
    for(unsigned n=0;n<9;++n){auto d=display_desc;auto h=display_heap;auto f=private_flags;switch(n){case 0:f=D3D12_HEAP_FLAG_NONE;break;case 1:f=D3D12_HEAP_FLAGS(f|D3D12_HEAP_FLAG_SHARED);break;case 2:h.Type=D3D12_HEAP_TYPE_UPLOAD;break;case 3:h.VisibleNodeMask=2;break;case 4:h.CreationNodeMask=2;break;case 5:d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;break;case 6:d.SampleDesc.Quality=1;break;case 7:d.MipLevels=2;break;case 8:d.Alignment=4194304;break;}
        require(!GpuInlineCopy::display_backing(d,h,f),"every private display backing premise is independently required");}
    resources[0]=display.Get();configure();host.device->CreateRenderTargetView(display.Get(),nullptr,handle);host.begin();transition=barrier(display.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
    require(copy.begin(8,display.Get()),"actual GetBuffer source begins");host.commands->ClearRenderTargetView(handle,red,0,nullptr);
    const auto retained=coordinator.finish_inline(8);require(retained.appended && retained.private_display==private_display && retained.heap_known && retained.heap_flags==actual_flags && !retained.hazards,"actual display descriptor and sharing flags survive the retained copy receipt");
    transition=barrier(display.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&transition);host.execute();require(copy.samples()[0].completed,"display inline copy completes");require(copy.reset(),"display retirement");
    configure();host.begin();transition=barrier(display.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);require(copy.begin(9,display.Get()),"display alias control begins");host.commands->ResourceBarrier(1,&alias);D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;host.commands->ResourceBarrier(1,&uav);host.commands->ClearRenderTargetView(handle,red,0,nullptr);
    const auto alias_result=coordinator.finish_inline(9);require(alias_result.appended==private_display && (private_display?(alias_result.excluded_aliases==1 && alias_result.excluded_uavs==1):(!alias_result.excluded_aliases && (alias_result.hazards&GpuStateEvidence::aliasing))),"only actual private backing can exclude unrelated aliases; actual shared backing refuses");
    transition=barrier(display.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&transition);host.execute();require(copy.reset(),"display alias control retirement");
    auto private_desc=display_desc;private_desc.Alignment=0;ComPtr<ID3D12Resource> private_output;
    check(host.device->CreateCommittedResource(&display_heap,D3D12_HEAP_FLAG_ALLOW_DISPLAY,&private_desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&private_output)));
    resources[0]=private_output.Get();configure();host.device->CreateRenderTargetView(private_output.Get(),nullptr,handle);host.begin();transition=barrier(private_output.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);require(copy.begin(11,private_output.Get()),"actual owned private display allocation begins");
    host.commands->ResourceBarrier(1,&alias);host.commands->ResourceBarrier(1,&uav);host.commands->ClearRenderTargetView(handle,red,0,nullptr);const auto private_retained=coordinator.finish_inline(11);
    require(private_retained.appended && private_retained.private_display && private_retained.excluded_aliases==1 && private_retained.excluded_uavs==1 && private_retained.access.alias_null==1,"actual non-shared committed display allocation proves the positive unrelated alias/UAV exclusion path");
    transition=barrier(private_output.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&transition);host.execute();require(copy.samples()[0].completed && copy.reset(),"owned private display copy completes and retires");
    GpuCompositorOutput compositor;std::vector<SIZE_T> clears;
    targets.transform_binding=[&](auto* command,const auto& binding,auto views){return compositor.transform(command,binding,views);};
    require(compositor.configure(host.device.Get(),texture.Get(),[&](auto* command){return coordinator.open_recording(command);},[&](auto* command,auto rtv){clears.push_back(rtv.ptr);coordinator.internal([&]{const float black[]{0,0,0,1};command->ClearRenderTargetView(rtv,black,0,nullptr);});}),"owned compositor output is allocated privately with matching actual RTV format");
    ID3D12Resource* output_sources[]{compositor.texture()};require(copy.configure(host.device.Get(),host.queue.Get(),output_sources),"inline copies observe the owned output rather than the shared native source");
    D3D12_ROOT_SIGNATURE_DESC root_desc{};root_desc.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;ComPtr<ID3DBlob> serialized,errors;check(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors));ComPtr<ID3D12RootSignature> root;check(host.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)));
    const char* vertex="float4 main(uint id:SV_VertexID):SV_Position{float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],0,1);}";
    const auto shader=[&](const char* text,const char* profile){ComPtr<ID3DBlob> code;check(D3DCompile(text,strlen(text),nullptr,nullptr,nullptr,"main",profile,0,0,&code,&errors));return code;};
    const auto vs=shader(vertex,"vs_5_0"),ps_red=shader("float4 main():SV_Target{return float4(1,0,0,1);}","ps_5_0"),ps_blue=shader("float4 main():SV_Target{return float4(0,0,1,1);}","ps_5_0");
    const auto pipeline=[&](ID3DBlob* ps){D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};p.SampleMask=UINT_MAX;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;p.SampleDesc.Count=1;ComPtr<ID3D12PipelineState> state;check(host.device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&state)));return state;};
    const auto red_pipeline=pipeline(ps_red.Get()),blue_pipeline=pipeline(ps_blue.Get());D3D12_RENDER_TARGET_VIEW_DESC native_rtv{};native_rtv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;native_rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;host.device->CreateRenderTargetView(texture.Get(),&native_rtv,handle);
    host.begin();transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
    GpuCompositorOutput::clear_native(true);
    require(compositor.begin(12,texture.Get()) && !compositor.begin(13,texture.Get()) && copy.begin(12,compositor.texture()),"one exact compositor scope begins; nested ownership is refused");
    auto unrelated=std::async(std::launch::async,[&]{RenderTargetProbe::Binding empty;return !compositor.transform(host.commands.Get(),empty,{}) && !compositor.begin(99,texture.Get()) && compositor.end(99,false,targets).refusal==GpuCompositorOutput::Refusal::scope;});require(unrelated.get(),"unrelated recording threads cannot enter, inspect or poison the native compositor scope");
    host.commands->OMSetRenderTargets(1,&handle,TRUE,nullptr);host.commands->SetGraphicsRootSignature(root.Get());host.commands->SetPipelineState(red_pipeline.Get());host.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0,0,8,4,0,1};const D3D12_RECT scissor{0,0,8,4};host.commands->RSSetViewports(1,&viewport);host.commands->RSSetScissorRects(1,&scissor);host.commands->DrawInstanced(3,1,0,0);
    const auto owned_copy=coordinator.finish_inline(12);const auto restored=compositor.end(12,owned_copy.appended,targets);GpuCompositorOutput::clear_native(false);
    require(clears.size()==2 && clears[1]==handle.ptr,"with clear_native the back buffer's own RTV is cleared at the end, after the private output's first clear");
    std::cout<<"Private compositor copy: ";GpuInlineCopy::write_sample(std::cout,owned_copy);std::cout<<"\nRedirect bindings="<<restored.bindings<<" refusal="<<uint32_t(restored.refusal)<<" generation="<<restored.generation<<" source="<<restored.source<<" target="<<restored.target<<'\n';
    require(owned_copy.appended && owned_copy.writes==1 && owned_copy.private_display,"actual original shader draws to the private target; owned clear is not counted as a native writer");
    require(restored.refusal==GpuCompositorOutput::Refusal::none && restored.restored && restored.restores==1 && restored.bindings==1 && restored.generation==owned_copy.generation && restored.target==owned_copy.identity,"actual native RTV binding is restored within the same open recording");
    host.commands->SetPipelineState(blue_pipeline.Get());host.commands->DrawInstanced(3,1,0,0);transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&transition);host.execute();
    const auto retained_output=copy.samples();require(retained_output.size()==1 && retained_output[0].completed,"private compositor draw and inline copy finish on the real queue fence");
    readback={{{retained_output[0].texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE},{texture.Get(),D3D12_RESOURCE_STATE_COMMON}}};check(GpuTextureReadback::capture(host.queue.Get(),readback,pixels));
    for(size_t n=0;n<pixels[0].rgb.size();n+=3)require(pixels[0].rgb[n]==255 && pixels[0].rgb[n+1]==0 && pixels[0].rgb[n+2]==0 && pixels[1].rgb[n]==0 && pixels[1].rgb[n+1]==0 && pixels[1].rgb[n+2]==255,"private red image survives while the subsequent native draw reaches the restored blue source");
    if(argc==2){const std::filesystem::path fixture{argv[1]};require(fixture.is_absolute(),"absolute fixture export");std::ofstream out{fixture};out<<"{\"model\":\"gpu_private_compositor_fixture_v1\",\"copy\":";GpuInlineCopy::write_sample(out,retained_output[0]);out<<",\"redirect\":";GpuCompositorOutput::write_receipt(out,restored);out<<",\"red_retained\":true,\"blue_original\":true,\"gpu_pair_verified\":false,\"runtime_submission\":false}\n";out.close();require(bool(out),"actual native GPU fixture exports its completed copy and restore receipt");}
    require(copy.reset(),"private compositor copy retirement");
    require(copy.configure(host.device.Get(),host.queue.Get(),output_sources),"new mirror control configures its producing source after retirement");
    host.device->CreateRenderTargetView(display.Get(),&native_rtv,handle);host.begin();transition=barrier(display.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
    require(compositor.begin(14,display.Get()) && copy.begin(14,compositor.texture()),"actual swap-chain mirror producing scope");
    host.commands->OMSetRenderTargets(1,&handle,TRUE,nullptr);host.commands->SetGraphicsRootSignature(root.Get());host.commands->SetPipelineState(red_pipeline.Get());host.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);host.commands->RSSetViewports(1,&viewport);host.commands->RSSetScissorRects(1,&scissor);host.commands->DrawInstanced(3,1,0,0);
    const auto mirror_copy=coordinator.finish_inline(14);const auto mirror_restore=compositor.end(14,mirror_copy.appended,targets);require(mirror_copy.appended && mirror_restore.restored,"private output retained and actual swap-chain RTV restored");
    host.commands->SetPipelineState(blue_pipeline.Get());host.commands->DrawInstanced(3,1,0,0);transition=barrier(display.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);host.commands->ResourceBarrier(1,&transition);check(host.commands->Close());
    ComPtr<ID3D12Fence> mirror_gate;check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&mirror_gate)));check(host.queue->Wait(mirror_gate.Get(),1));host.queue->ExecuteCommandLists(1,lists);
    const auto mirror_pending=copy.samples().front();require(mirror_pending.submitted && !mirror_pending.completed && mirror_pending.fence,"mirror receives the actual pending producer fence");
    GpuPresentMirror mirror;require(FAILED(mirror.submit(host.queue.Get(),mirror_pending,15,display.Get(),mirror_restore.source)) && FAILED(mirror.submit(host.queue.Get(),mirror_pending,14,display.Get(),mirror_restore.target)),"wrong producing tag or destination refuses before queue work");
    for(unsigned n=0;n<5;++n){auto invalid=mirror_pending;switch(n){case 0:invalid.fence.Reset();break;case 1:invalid.fence_value=0;break;case 2:invalid.submitted=false;break;case 3:invalid.refusal=GpuInlineCopy::Refusal::hazard;break;case 4:invalid.queue=0;break;}require(FAILED(mirror.submit(host.queue.Get(),invalid,14,display.Get(),mirror_restore.source)),"mirror independently requires the producer fence, submission, unrefused result and exact queue");}
    coordinator.internal([&]{check(mirror.submit(host.queue.Get(),mirror_pending,14,display.Get(),mirror_restore.source));});
    const auto queued_mirror=mirror.sample(14);require(queued_mirror && queued_mirror->submitted && !queued_mirror->completed && !mirror.reset(),"mirror owns its pending command/source/destination leases behind the blocked producer");
    check(mirror_gate->Signal(1));check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
    const auto completed_mirror=mirror.sample(14);require(completed_mirror && completed_mirror->completed && copy.samples().front().completed && completed_mirror->image==GpuObjectIdentity::get(mirror_pending.texture.Get()),"mirror completion requires its actual queue fence and joins the frozen image identity");
    const std::array<GpuTextureReadback::Source,1> mirror_readback{{{display.Get(),D3D12_RESOURCE_STATE_PRESENT}}};check(GpuTextureReadback::capture(host.queue.Get(),mirror_readback,pixels));
    for(size_t n=0;n<pixels[0].rgb.size();n+=3)require(pixels[0].rgb[n]==255 && pixels[0].rgb[n+1]==0 && pixels[0].rgb[n+2]==0,"desktop mirror replaces later blue pixels with the retained red output without touching native bindings");
    if(argc==2){std::ofstream out{std::filesystem::path{argv[1]}.replace_extension("mirror.json")};out<<"{\"model\":\"gpu_present_mirror_fixture_v1\",\"copy\":";GpuInlineCopy::write_sample(out,copy.samples().front());out<<",\"redirect\":";GpuCompositorOutput::write_receipt(out,mirror_restore);out<<",\"mirror\":";GpuPresentMirror::write_sample(out,*completed_mirror);out<<",\"retained_red_displayed\":true,\"blocked_completion_tested\":true,\"gpu_pair_verified\":false,\"runtime_submission\":false}\n";out.close();require(bool(out),"actual mirror exports GPU completion and exact image/destination receipt");}
    require(mirror.reset() && copy.reset(),"completed mirror and producer retire");
    host.device->CreateRenderTargetView(texture.Get(),&native_rtv,handle);require(compositor.begin(13,texture.Get()),"closed-list negative scope begins");RenderTargetProbe::Binding native_binding;native_binding.count=1;native_binding.handles[0]=handle;
    const auto source_views=targets.views(reinterpret_cast<uintptr_t>(texture.Get()));require(!source_views.empty() && !compositor.transform(host.commands.Get(),native_binding,source_views),"a closed native recording cannot redirect RTVs");require(compositor.end(13,false,targets).refusal==GpuCompositorOutput::Refusal::unsafe_end && !compositor.begin(14,texture.Get()),"unsafe scope poisons further use instead of releasing potentially queued output");
    GpuCompositorOutput failed_output;require(failed_output.configure(host.device.Get(),display.Get(),[&](auto* command){return coordinator.open_recording(command);},[&](auto* command,auto rtv){coordinator.internal([&]{const float black[]{0,0,0,1};command->ClearRenderTargetView(rtv,black,0,nullptr);});}),"copy refusal regression has its own private output");
    targets.transform_binding=[&](auto* command,const auto& binding,auto views){return failed_output.transform(command,binding,views);};
    ID3D12Resource* failed_sources[]{failed_output.texture()};require(copy.configure(host.device.Get(),host.queue.Get(),failed_sources),"failed-copy producing source");host.device->CreateRenderTargetView(display.Get(),&native_rtv,handle);host.begin();transition=barrier(display.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
    require(failed_output.begin(15,display.Get()) && copy.begin(15,failed_output.texture()),"failed-copy scope begins");host.commands->OMSetRenderTargets(1,&handle,TRUE,nullptr);host.commands->SetGraphicsRootSignature(root.Get());host.commands->SetPipelineState(red_pipeline.Get());host.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);host.commands->RSSetViewports(1,&viewport);host.commands->RSSetScissorRects(1,&scissor);host.commands->DrawInstanced(3,1,0,0);
    copy.invalidate_active(GpuInlineCopy::Refusal::busy);const auto refused_copy=coordinator.finish_inline(15);require(!refused_copy.appended,"injected provenance failure refuses copy");const auto failed_restore=failed_output.end(15,false,targets);
    require(failed_restore.refusal==GpuCompositorOutput::Refusal::unsafe_end && failed_restore.restored && failed_restore.restores==1 && !failed_output.begin(16,display.Get()),"copy refusal still restores exact native binding while poisoning private output reuse");
    host.commands->SetPipelineState(blue_pipeline.Get());host.commands->DrawInstanced(3,1,0,0);transition=barrier(display.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);host.commands->ResourceBarrier(1,&transition);host.execute();check(GpuTextureReadback::capture(host.queue.Get(),mirror_readback,pixels));
    for(size_t n=0;n<pixels[0].rgb.size();n+=3)require(pixels[0].rgb[n]==0 && pixels[0].rgb[n+1]==0 && pixels[0].rgb[n+2]==255,"later native draw reaches the original blue destination even when private copy is refused");require(copy.reset(),"refused copy has no pending copy leases");
    targets.transform_binding={};
    display.Reset();display_swap.Reset();DestroyWindow(hidden);
    MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();std::cout<<"GPU inline copy tests passed\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<'\n';return 1;}}
