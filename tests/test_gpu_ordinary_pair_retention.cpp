#include "GpuTestHost.h"
#include "GpuOrdinaryPairRetention.h"
#include "OrdinaryRuntimeImages.h"
#include "PresentHandoff.h"
#include "OrdinaryEyeSnapshot.h"
#include "GpuCompletionProbe.h"
#include "GpuTextureReadback.h"
#include <d3dcompiler.h>
#include <fstream>
#include <string_view>

using Retention=GpuOrdinaryPairRetention;
std::function<void()> native_draw;uintptr_t native_wrapper{};unsigned native_calls{};
__declspec(noinline)void colour_post(uintptr_t,uintptr_t* out,uintptr_t,uintptr_t,uintptr_t,uint32_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,float){*out=native_wrapper;volatile auto used=*out;(void)used;}
__declspec(noinline)void colour_compositor(uintptr_t,uintptr_t,uint8_t){++native_calls;native_draw();volatile auto used=native_calls;(void)used;}
__declspec(noinline)void invoke_colour_post(NativeOrdinaryProbe::Post volatile fn,uintptr_t entry){uintptr_t out{};fn(0,&out,0,0,entry,0,0,0,0,0,0);volatile auto used=out;(void)used;}
__declspec(noinline)void invoke_colour_compositor(NativeOrdinaryProbe::Compositor volatile fn,uintptr_t state){fn(native_wrapper,state,0);volatile auto used=native_calls;(void)used;}
template<class T>void put(uintptr_t address,size_t offset,const T& value){std::memcpy(reinterpret_cast<void*>(address+offset),&value,sizeof(value));}
struct Camera {
    std::vector<uint8_t> list=std::vector<uint8_t>(0x2a000),renderer=std::vector<uint8_t>(0x600);
    ordinary_scene_scope::Scope scope;std::array<NativeRenderProbe::Scene,2> scenes;
    uintptr_t lp()const{return reinterpret_cast<uintptr_t>(list.data());}uintptr_t rp()const{return reinterpret_cast<uintptr_t>(renderer.data());}
    void frame(unsigned n){using E=NativeCameraEntries;E::get().enable();auto a=vr_camera::IDENTITY,b=a;b[12]=.064f;FrameSyncClock::Frame frame{n,81+n,0,0,false};
        auto ticket=E::get().begin(lp()+0x160,E::Role::main,frame);E::get().finish(*ticket,0,a);ticket=E::get().begin(lp()+0x160,E::Role::ordinary_right,frame);E::get().finish(*ticket,1,b);
        const auto revision=*E::get().revision(lp());const std::array<E::Entry,2> actual{{{lp()+0x168,0,E::Role::main,a},{lp()+0x168+0xa60,1,E::Role::main,b}}};scope.pair=*E::get().ordinary_pair(lp(),actual,revision);
        scope.scene={};auto& s=scope.scene;s.dispatch_sequence=77+n;s.renderer=rp();s.list=lp();s.render_counter_before=42+n;s.camera_revision_before=revision;s.view_flags_before=0x3f7c013;s.refusal=NativeRenderProbe::SceneRefusal::none;
        put(lp(),0x29968,int32_t(8));put(lp(),0x29c78,s.render_counter_before);for(size_t slot=0;slot<8;++slot)put(lp()+0x168+slot*0xa60,0,slot<2?s.view_flags_before:uint64_t(0x10000000));
        for(size_t slot=0;slot<2;++slot){bind(uint32_t(slot));scenes[slot]=scope.scene;scenes[slot].active_entry_after=scope.scene.entry;scenes[slot].view_flags_after=s.view_flags_before;scenes[slot].render_counter_after=s.render_counter_before;scenes[slot].camera_revision_after=revision;}
    }
    void bind(uint32_t slot){auto& s=scope.scene;s.slot=slot;s.ordinal=slot+1;s.entry=slot?scope.pair.right.entry:scope.pair.left.entry;put(rp(),0x140,s.entry);}
    Retention::Event event(uint32_t slot,NativeOrdinaryProbe::Stage stage,unsigned n){bind(slot);Retention::Event e;e.scope=scope;e.thread=GetCurrentThreadId();e.serial=n*6+slot*3+(stage==NativeOrdinaryProbe::Stage::compositor_before?2:3);
        e.boundary={stage,0x12340000,0,rp()+0x10};e.before=e.after=ordinary_scene_scope::inspect(scope);e.output.status=NativeRenderProbe::OutputStatus::observed;e.output.resource=0xdead0000;
        require(!e.before.mask,"actual native camera inventory authenticates fixture eye");return e;
    }
    NativeRenderProbe::Snapshot snapshot(unsigned n){NativeRenderProbe::Snapshot s;s.sequence=77+n;s.renderer=rp();s.list=lp();s.ordinary_pair=scope.pair;s.scene_calls=2;s.scenes.assign(scenes.begin(),scenes.end());return s;}
    NativeOrdinaryPacket::Receipt receipt(unsigned n){NativeOrdinaryPacket::Receipt r;r.dispatch=77+n;r.render_id=81+n;r.renderer=rp();r.context={lp(),42+n,8,4,1,1};r.reference=r.applied=r.accepted=r.source_unchanged=true;r.sdk=NativeOrdinaryPacket::Sdk{1,100+n,2+n};return r;}
};
int main(int argc,char** argv){try{
    const Retention::Status stopped{true,true,false,2,Retention::Refusal::changed};
    require(Retention::status_signature(stopped,true)!=Retention::status_signature(stopped,false),"reset must refresh disabled status even if an earlier refusal had already stopped the experiment");
    std::array<std::string_view,7> dependencies{"1","1","1","1","1","1","1"};
    require(Retention::requested("1",dependencies,true,false) && !Retention::requested("1",dependencies,false,false) && !Retention::requested("1",dependencies,true,true) && !Retention::requested("0",dependencies,true,false) && !Retention::requested("1",std::span{dependencies}.first(6),true,false),"bounded pair GPU path needs explicit desktop second-camera dependencies and excludes the sole-inline path");
    for(size_t i=0;i<dependencies.size();++i)for(const auto bad:{"","0","true"}){auto missing=dependencies;missing[i]=bad;require(!Retention::requested("1",missing,true,false),"every exact opt-in dependency is mandatory");}
    const std::string_view test=argc>1?argv[1]:"happy";require(!Retention::tag(0,0) && !Retention::tag(UINT64_MAX,0) && !Retention::tag(1,2) && Retention::tag(77,0)==154 && Retention::tag(77,1)==155,"eye tags preserve one dispatch identity with disjoint bounded slots");
    require(GpuSignatureProbe::get().install_device_creation(),"early GPU lifecycle hooks");Host host;auto& targets=RenderTargetProbe::get();auto& coordinator=*new GpuCompletionProbe;auto& copy=*new GpuInlineCopy;auto& retention=*new Retention;
    require(targets.install(host.device.Get()),"actual RTV callback observation");coordinator.inline_copies(&copy);targets.observe_binding=[&](uintptr_t command,std::span<const uintptr_t> resources,bool known){coordinator.bound(command,resources,known);};
    if(test=="preallocated")require((new GpuAllocationProbe)->install(host.device.Get()),"native texture creation owns allocation observation before completion startup");
    require(coordinator.install(host.device.Get(),host.queue.Get(),[&](uintptr_t handle){return targets.canonical_resource(handle);},{},[&](uintptr_t command){return targets.observes_binding(command);}),"one actual command-hook owner");
    require((coordinator.allocation_status().hook_mask&3)==3 && !GpuAllocationProbe::observed_status(nullptr).hook_mask,"completion reports the actual device-bound allocation observer even when another subsystem owns it");
    const auto hidden=CreateWindowExW(0,L"STATIC",L"SM2 ordinary pair GPU fixture",WS_OVERLAPPEDWINDOW,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);require(hidden!=nullptr,"owned hidden swapchain fixture");
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=8;sd.Height=4;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> initial;check(factory->CreateSwapChainForHwnd(host.queue.Get(),hidden,&sd,nullptr,nullptr,&initial));ComPtr<IDXGISwapChain3> swap;check(initial.As(&swap));ComPtr<ID3D12Resource> native;check(swap->GetBuffer(swap->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&native)));
    require(retention.configure(host.device.Get(),host.queue.Get(),swap.Get(),copy,targets,[&](auto* command){return coordinator.open_recording(command);},[&](auto* command,auto rtv){coordinator.internal([&]{const float black[]{0,0,0,1};command->ClearRenderTargetView(rtv,black,0,nullptr);});},[&](uint64_t tag){if(test=="finish_exception" || test=="abort_exception")throw std::runtime_error("injected finish failure");return coordinator.finish_inline(tag);}),"configure distinct owned private destinations from actual GetBuffer shape");
    targets.transform_binding=[&](auto* command,const auto& binding,auto views){return retention.transform(command,binding,views);};
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;ComPtr<ID3D12DescriptorHeap> heap;check(host.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));const auto handle=heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC native_view{};native_view.Format=sd.Format;native_view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;host.device->CreateRenderTargetView(native.Get(),&native_view,handle);
    ComPtr<ID3DBlob> serialized,errors;D3D12_ROOT_SIGNATURE_DESC rd{};rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors));ComPtr<ID3D12RootSignature> root;check(host.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)));
    const auto shader=[&](const char* source,const char* profile){ComPtr<ID3DBlob> code;check(D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"main",profile,0,0,&code,&errors));return code;};const auto vertex=shader("float4 main(uint id:SV_VertexID):SV_Position{float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],0,1);}","vs_5_0");
    const std::array<const char*,5> sources{"float4 main():SV_Target{return float4(1,0,0,1);}","float4 main():SV_Target{return float4(0,0,1,1);}","float4 main():SV_Target{return float4(0,1,0,1);}","float4 main():SV_Target{return float4(1,1,1,1);}","float4 main():SV_Target{return float4(1,0,1,1);}"};std::array<ComPtr<ID3D12PipelineState>,5> pipelines;
    for(size_t i=0;i<sources.size();++i){const auto ps=shader(sources[i],"ps_5_0");D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};p.SampleMask=UINT_MAX;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.RTVFormats[0]=sd.Format;p.SampleDesc.Count=1;check(host.device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&pipelines[i])));}
    const D3D12_VIEWPORT viewport{0,0,8,4,0,1};const D3D12_RECT scissor{0,0,8,4};
    const auto draw=[&](size_t colour){host.commands->SetGraphicsRootSignature(root.Get());host.commands->SetPipelineState(pipelines[colour].Get());host.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);host.commands->RSSetViewports(1,&viewport);host.commands->RSSetScissorRects(1,&scissor);host.commands->DrawInstanced(3,1,0,0);};
    Camera camera;camera.frame(0);auto before=camera.event(0,NativeOrdinaryProbe::Stage::compositor_before,0);
    const bool hooked=test=="hooked" || test=="hooked_capacity" || test=="hooked_exception" || test=="hooked_scope_lost";bool scoped=true,consumer_refused{};std::array<uint8_t,0x280> wrapper{};
    native_wrapper=reinterpret_cast<uintptr_t>(wrapper.data());put(native_wrapper,0x278,uintptr_t(0xdead0000));
    if(hooked){require(NativeOrdinaryPairColour::get().arm([&]()->std::optional<Retention::Scope>{return scoped?std::optional{camera.scope}:std::nullopt;},[](uintptr_t resource){RenderTargetProbe::View view;view.resource=resource;return RenderTargetProbe::Observation{1,1,{view}};},[&](const auto* event){if(!retention.consume(event,swap.Get()))consumer_refused=true;}),"actual pair observer arms synchronous GPU consumer before native hooks");
        require(NativeOrdinaryProbe::get().install(reinterpret_cast<void*>(colour_post),reinterpret_cast<void*>(colour_compositor),[]{return std::optional<NativeOrdinaryProbe::Scope>{};},[](uintptr_t){return RenderTargetProbe::Observation{};}),"real native post/compositor detours publish once");}
    if(test=="order" || test=="scope" || test=="source"){
        if(test=="order")before=camera.event(1,NativeOrdinaryProbe::Stage::compositor_before,0);
        if(test=="scope")before.thread++;
        require(!retention.begin(before,test=="source"?nullptr:swap.Get()) && copy.samples().empty(),"invalid right-first/scope/source refuses before GPU copy");std::cout<<"pair retention early refusal PASS\n";return 0;
    }
    if(test=="poisoned_output"){
        // 29 Oct: an eye whose copy is refused ends its output unsafely, which poisons it, and begin()
        // refuses it from then on. A cold restart that kept the poisoned output refused every later
        // eye (Refusal::redirect) until its budget ran out and stereo stopped for the session.
        GpuCompositorOutput output;
        require(output.configure(host.device.Get(),native.Get(),[&](auto* command){return coordinator.open_recording(command);},
            [&](auto* command,auto rtv){coordinator.internal([&]{const float black[]{0,0,0,1};command->ClearRenderTargetView(rtv,black,0,nullptr);});}),"the output configures");
        const auto first=GpuObjectIdentity::get(output.texture());
        require(output.renew() && output.renewals()==0,"an output that is not poisoned needs no renewal");
        require(output.begin(11,native.Get()),"a fresh output begins");
        const auto unsafe=output.end(11,false,targets);
        require(unsafe.refusal==GpuCompositorOutput::Refusal::unsafe_end && output.poisoned() && !output.begin(12,native.Get()),"an unsafe end poisons the output and begin refuses it");
        require(output.renew() && !output.poisoned() && output.renewals()==1 && output.texture() && GpuObjectIdentity::get(output.texture())!=first,"renew gives it a fresh texture");
        require(output.begin(13,native.Get()),"the renewed output begins again");
        output.end(13,false,targets);
        std::cout<<"poisoned output renew PASS\n";return 0;
    }
    if(test=="reset_kept" || test=="reset_forgotten"){
        // 31 Oct 19:00 (docs/FREEZE_REHOOK_2026-10-27.md): on_device_reset used to clear the RTV table.
        // reset_forgotten replays what that did, against the dumps of that run: the private output's
        // view (made once, at configure) binds as unknown, so the first eye has no writer (dump 52692:
        // copy_refusal=3 writes=0, redirect bindings=1). A cold restart renews the output, but any view
        // of the game's that was not made again taints the recording (dumps 52994, 53293, 53661:
        // copy_refusal=8 writes=1 hazards=32) and stereo stopped for the session. reset_kept is the fix.
        const bool forgotten=test=="reset_forgotten";
        const auto game=host.color(0,0,0,8,sd.Format,4);ComPtr<ID3D12DescriptorHeap> game_heap;check(host.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&game_heap)));
        const auto game_view=game_heap->GetCPUDescriptorHandleForHeapStart();host.device->CreateRenderTargetView(game.Get(),&native_view,game_view);
        // on_device_reset, then the game making its back-buffer view again, then the re-arm.
        retention.invalidate();require(copy.reset(),"the reset drains the copy");
        if(forgotten)targets.clear();else targets.device_reset();
        host.device->CreateRenderTargetView(native.Get(),&native_view,handle);
        require(retention.restart(swap.Get(),host.queue.Get()) && !retention.status().stopped,"the re-arm restarts the retention");
        // One left eye, after a draw on another of the game's targets in the same recording when asked.
        const auto eye=[&](unsigned n,bool game_draw){camera.frame(n);host.begin();
            if(game_draw){const auto ready=barrier(game.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&ready);host.commands->OMSetRenderTargets(1,&game_view,TRUE,nullptr);draw(0);}
            auto start=camera.event(0,NativeOrdinaryProbe::Stage::compositor_before,n);require(retention.consume(&start,swap.Get()),"the eye begins after the re-arm");
            host.commands->OMSetRenderTargets(1,&handle,TRUE,nullptr);draw(0);
            auto finish=camera.event(0,NativeOrdinaryProbe::Stage::compositor_after,n);const bool ended=retention.consume(&finish,swap.Get());check(host.commands->Close());return ended;};
        if(!forgotten){
            require(eye(1,true) && !retention.status().stopped,"the first eye after a reset keeps its copy, with a game target from before the reset in its recording");
            const auto sample=copy.samples().back();require(sample.appended && sample.refusal==GpuInlineCopy::Refusal::none && sample.writes==1 && !sample.hazards,"the copy is appended clean");
            std::cout<<"reset keeps render target views PASS\n";return 0;
        }
        require(!eye(1,false) && retention.status().stopped && retention.status().refusal==Retention::Refusal::copy,"forgotten: the private output's view is unknown, so the eye is refused");
        auto refused_eye=copy.samples().back();require(refused_eye.refusal==GpuInlineCopy::Refusal::no_writer && !refused_eye.writes && !refused_eye.appended,"forgotten: no writer, as in dump 52692");
        require(retention.first_detail().find("copy_refusal=3")!=std::string::npos,"forgotten: the detail line reads as at 19:00:32");
        // The cold restart: the poisoned output gets a fresh view, the game's own stays forgotten.
        require(copy.reset() && retention.restart(swap.Get(),host.queue.Get()) && !retention.status().stopped,"the cold restart renews the output");
        require(!eye(2,true) && retention.status().stopped && retention.first_detail().find("copy_refusal=8")!=std::string::npos,"forgotten: a game view that was not made again taints the recording");
        refused_eye=copy.samples().back();require(refused_eye.refusal==GpuInlineCopy::Refusal::hazard && refused_eye.writes==1 && refused_eye.hazards==GpuStateEvidence::implicit_binding,"forgotten: hazards=32 writes=1, as in dumps 52994 to 53661");
        std::cout<<"reset forgetting render target views refuses PASS\n";return 0;
    }
    host.begin();auto transition=barrier(native.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
    const bool happy=test=="happy" || test=="hooked" || test=="hooked_capacity" || test=="preallocated";const unsigned frames=test=="hooked_capacity"?4:happy?2:1;
    bool refused{};
    for(unsigned n=0;n<frames;++n){if(n)camera.frame(n);
        for(uint32_t slot=0;slot<2;++slot){before=camera.event(slot,NativeOrdinaryProbe::Stage::compositor_before,n);
            if(hooked){invoke_colour_post(colour_post,camera.scope.scene.entry);native_draw=[&]{require(retention.status().active==(n<2),"only admitted jobs redirect the actual native draw; later capacity jobs stay native");host.commands->OMSetRenderTargets(1,&handle,TRUE,nullptr);draw((n*2+slot)%4);if(test=="hooked_exception")throw std::runtime_error("native compositor failure");if(test=="hooked_scope_lost")scoped=false;};
                bool propagated{};const auto calls_before=native_calls;try{invoke_colour_compositor(colour_compositor,camera.rp()+0x10);}catch(const std::runtime_error& error){propagated=std::string_view(error.what())=="native compositor failure";}
                require(native_calls==calls_before+1 && !retention.status().active,"native compositor called once and consumer always exits before following native draw");
                if(test=="hooked_exception" || test=="hooked_scope_lost"){require(consumer_refused && (test!="hooked_exception" || propagated),"native exception/lost provider invalidates GPU retention with unchanged native exception");refused=true;}
                else require((n>=2 || !consumer_refused) && !propagated && !retention.status().stopped,"actual native callbacks retain admitted eyes and capacity never poisons completed jobs");
                draw(4);if(refused)break;continue;
            }
            require(retention.consume(&before,swap.Get()),"actual eye scope starts private compositor redirect through explicit pair consumer");host.commands->OMSetRenderTargets(1,&handle,TRUE,nullptr);
            if(test!="no_writer")draw(n*2+slot);
            auto after=camera.event(slot,NativeOrdinaryProbe::Stage::compositor_after,n);
            if(test=="changed")after.serial+=10;
            if(test=="refused_exit")after.refusal=NativeOrdinaryPairColour::Refusal::context;
            if(test=="duplicate"){require(!retention.begin(before,swap.Get()),"duplicate before boundary aborts active scope");refused=true;}
            else if(test=="abort" || test=="abort_exception"){retention.abort();refused=true;}
            else if(test=="scope_lost"){require(!retention.consume(nullptr,swap.Get()),"lost native scope invalidates current private consumer");refused=true;}
            else if(test=="device_reset"){retention.invalidate();require(retention.status().stopped && !retention.status().active,"device reset disables the bounded experiment and ends its current native binding");
                // 22 Oct: the reset must not end stereo for the process. The same swap chain (its buffers recreated at the same size) resumes; anything else is refused.
                // On a second control so this instance keeps its refused pair for the assertions below.
                {Retention again;GpuInlineCopy again_copy;require(!again.restart(swap.Get(),host.queue.Get()),"never configured: no restart");
                 require(again.configure(host.device.Get(),host.queue.Get(),swap.Get(),again_copy,targets,[&](auto* command){return coordinator.open_recording(command);},[&](auto* command,auto rtv){coordinator.internal([&]{const float black[]{0,0,0,1};command->ClearRenderTargetView(rtv,black,0,nullptr);});},[&](uint64_t tag){return again_copy.finish(tag);}),"second control configures on the same swap chain");
                 again.invalidate();require(again.status().stopped,"reset stops it");
                 require(!again.restart(nullptr,host.queue.Get()),"no swap chain: no restart");
                 require(again.restart(swap.Get(),host.queue.Get()) && !again.status().stopped && again.status().ready && again.status().pairs==0 && again.status().refusal==Retention::Refusal::none,"same swap chain after a reset: retention resumes with the refused pairs dropped");
                 // 27 Oct: Sm2Probe resets the inline copy before every restart (re-arm and cold restart), and
                 // that reset drops the copy's configuration. The restart must configure it again, or the
                 // next eye is refused with Refusal::copy and stereo stays stopped (08:51 and 09:39 runs).
                 again.invalidate();require(again_copy.reset() && !again_copy.configured(),"the consumer's reset leaves the copy unconfigured");
                 require(again.restart(swap.Get(),host.queue.Get()) && !again.status().stopped,"restart after the copy's reset");
                 auto first=camera.event(0,NativeOrdinaryProbe::Stage::compositor_before,0);
                 require(again.consume(&first,swap.Get()) && again.status().active && !again.status().stopped,"the first eye after a reset and restart begins its retained copy");
                 again.abort();require(!again.status().active,"the fixture ends that eye's redirect before the main control continues");
                 // The copy is configured again only on the queue the retention was configured with.
                 again.invalidate();require(again_copy.reset() && !again_copy.configured(),"reset again");
                 ComPtr<ID3D12CommandQueue> other_queue;D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;check(host.device->CreateCommandQueue(&qd,IID_PPV_ARGS(&other_queue)));
                 require(!again.restart(swap.Get(),nullptr) && !again.restart(swap.Get(),other_queue.Get()) && !again_copy.configured() && again.status().stopped,"no queue or another queue: the restart is refused and the copy stays unconfigured");
                 require(again.restart(swap.Get(),host.queue.Get()) && again_copy.configured() && !again.status().stopped,"the configured queue: the copy is configured again");}
                refused=true;}
            else{const bool ended=retention.consume(&after,swap.Get());if(test=="no_writer" || test=="changed" || test=="finish_exception" || test=="refused_exit"){require(!ended,"unsafe eye end must refuse");refused=true;}else{if(!ended){std::cerr<<"Rejected eye frame="<<n<<" slot="<<slot<<'\n';for(const auto& r:retention.samples()){Retention::write(std::cerr,r);std::cerr<<'\n';}}require(ended,"actual copy appends before native destination is restored");}}
            // Deliberately no OMSetRenderTargets: proves CPU native binding restoration.
            draw(4);
            if(refused)break;
        }
        if(refused)break;
        auto receipt=camera.receipt(n);if(test=="packet")receipt.sdk.reset();retention.complete(camera.snapshot(n),receipt);
    }
    const auto unsubmitted=retention.samples();require((test=="device_reset" || !unsubmitted.empty()) && std::ranges::none_of(unsubmitted,[](const auto& p){return p.gpu_retention_verified;}),"CPU return/native packet cannot prove GPU completion"); // device_reset: restart() dropped the refused pairs
    require(!retention.retire(77),"unsubmitted pair cannot retire");
    if(happy){require(unsubmitted.size()==2 && unsubmitted[0].cpu_verified && unsubmitted[1].cpu_verified,"two actual producing jobs remain distinct before one batch executes");const auto all=copy.samples();require(all.size()==4 && !copy.reset(),"four retained copies pin their unsubmitted source/recording leases");}
    transition=barrier(native.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);host.commands->ResourceBarrier(1,&transition);check(host.commands->Close());
    ComPtr<ID3D12Fence> gate;check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));check(host.queue->Wait(gate.Get(),1));ID3D12CommandList* lists[]{host.commands.Get()};host.queue->ExecuteCommandLists(1,lists);
    const auto pending=retention.samples();require(std::ranges::none_of(pending,[](const auto& p){return p.gpu_retention_verified;}),"real queue gate prevents readiness despite native submission returning");
    require(!retention.retire(77),"pending producer fences prevent pair retirement");
    std::unique_ptr<OrdinaryRuntimeImages> ordered_bridge;
    if(test=="happy"){
        require(pending[0].gpu_submission_verified && !pending[0].gpu_retention_verified,"typed submission proof is separate from native GPU completion");
        auto packet=camera.receipt(0);packet.tracked=NativeOrdinaryPacket::TrackedSource{};packet.left_patched=packet.patched=NativeCameraPacket{};
        const auto& origin=pending[0].origin;packet.tracked->producer_list=origin.pair.producer_list;packet.tracked->producer_revision=origin.pair.producer_revision;
        auto& prepared=packet.tracked->prepared;prepared.render_id=packet.render_id;prepared.game_token=origin.pair.left.token;
        for(size_t i=0;i<2;++i){auto& eye=prepared.eyes[i];eye.world=vr_camera::IDENTITY;eye.tracking.eye=int(i);eye.tracking.render_token=packet.render_id;eye.tracking.tracking_epoch=1;require(vr_projection::build({-1,1,-1,1},.1f,1000,eye.projection),"queue ordered bridge optics");}
        ordered_bridge=std::make_unique<OrdinaryRuntimeImages>();
        require(!ordered_bridge->capture(pending[0],packet,host.queue.Get(),nullptr,{},{},false,true),"isolated queue cannot consume unfinished producer work");
        require(!ordered_bridge->capture(pending[0],packet,host.queue.Get(),nullptr,{},{},true,true),"isolated queue cannot borrow same-queue ordering as cross-queue proof");
        require(!ordered_bridge->capture(pending[0],packet,host.queue.Get()),"baseline still refuses native copies whose fences are pending");
        for(unsigned fault=0;fault<7;++fault){auto bad=pending[0];auto foreign=packet;
            if(fault==0)bad.cpu_verified=false;
            if(fault==1)bad.gpu_submission_verified=false;
            if(fault==2)bad.eyes[1].copy->submitted=false;
            if(fault==3)++bad.eyes[1].copy->queue;
            if(fault==4)bad.eyes[1].copy->fence.Reset();
            if(fault==5)foreign.rollback=true;
            if(fault==6)++foreign.tracked->producer_revision;
            require(!ordered_bridge->capture(bad,foreign,host.queue.Get(),nullptr,{},{},true),"queue ordering never bypasses CPU, submission, exact queue/fence, rollback or producer guards");
        }
        require(ordered_bridge->capture(pending[0],packet,host.queue.Get(),nullptr,{},{},true),"CPU validated submitted native eyes enqueue on their exact queue without waiting for native fences");
        require(!ordered_bridge->take() && ordered_bridge->pending(),"queue ordered bridge cannot publish while the actual native GPU gate remains blocked");
    }
    if(happy)for(const auto& pair:pending)for(const auto& eye:pair.eyes)require(eye.copy && eye.copy->submitted && !eye.copy->completed,"both real producer fences are pending behind the actual queue gate");
    check(gate->Signal(1));check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);const auto completed=retention.samples();
    if(ordered_bridge){const auto image=ordered_bridge->take();require(image && image->dispatch==77 && image->images.left.ready() && image->images.right.ready(),"ordered bridge publishes only after both bridge fences, downstream of producer fences");
        const std::array<GpuTextureReadback::Source,2> sources{{{image->images.left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{image->images.right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
        std::vector<GpuTextureReadback::Pixels> colours;check(GpuTextureReadback::capture(host.queue.Get(),sources,colours));
        for(size_t i=0;i<colours[0].rgb.size();i+=3)require(colours[0].rgb[i]==255 && colours[0].rgb[i+1]==0 && colours[0].rgb[i+2]==0 && colours[1].rgb[i]==0 && colours[1].rgb[i+1]==0 && colours[1].rgb[i+2]==255,"actual pending-producer queue dependency preserves distinct red/blue eye pixels");
        require(!ordered_bridge->take(),"queue ordered bridge publishes each pair once");}
    if(ordered_bridge){ComPtr<ID3D12InfoQueue> debug;host.device.As(&debug);
        if(debug)for(UINT64 i=0;i<debug->GetNumStoredMessages();++i){SIZE_T bytes{};debug->GetMessage(i,nullptr,&bytes);std::vector<char> data(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());check(debug->GetMessage(i,message,&bytes));require(message->Severity!=D3D12_MESSAGE_SEVERITY_ERROR && message->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,"pending-producer queue ordering has no D3D12 debug errors");}}
    std::vector<GpuTextureReadback::Pixels> retained_pixels;
    if(happy){std::vector<GpuTextureReadback::Source> reads;
        for(size_t n=0;n<2;++n){const auto& pair=completed[n];require(pair.cpu_verified && pair.gpu_retention_verified && pair.same_submission && pair.refusal==Retention::Refusal::none,"accepted native job and both exact queue fences join retention completion");
            for(size_t slot=0;slot<2;++slot){const auto& e=pair.eyes[slot];require(e.tag==(77+n)*2+slot && e.copy->completed && e.redirect->restored && e.redirect->restores==1 && e.redirect->private_backing && !(e.copy->heap_flags&1),"exact eye tag, private lease and restored native binding retained");reads.push_back({e.copy->texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE});}}
        std::vector<GpuTextureReadback::Pixels> pixels;for(size_t pair=0;pair<2;++pair){std::vector<GpuTextureReadback::Pixels> two;check(GpuTextureReadback::capture(host.queue.Get(),std::span{reads}.subspan(pair*2,2),two));pixels.insert(pixels.end(),std::make_move_iterator(two.begin()),std::make_move_iterator(two.end()));}
        const std::array<std::array<uint8_t,3>,4> colours{{{255,0,0},{0,0,255},{0,255,0},{255,255,255}}};require(pixels.size()==4,"both bounded two-image readbacks retain all four real GPU images");
        for(size_t image=0;image<4;++image)for(size_t i=0;i<pixels[image].rgb.size();i+=3)for(size_t channel=0;channel<3;++channel)require(pixels[image].rgb[i+channel]==colours[image][channel],"left/right/two-job pixels remain independent after later native draws overwrite shared output");
        retained_pixels=std::move(pixels);
    }else{require(completed.size()==1 && completed[0].refusal!=Retention::Refusal::none && !completed[0].gpu_retention_verified,"refused scope/copy/CPU receipt cannot become publishable after GPU completion");
        const auto& redirect=completed[0].eyes[0].redirect;require(redirect && redirect->bindings==1 && redirect->restore_required && redirect->restored && redirect->restores==1,"negative control actually redirected and restored the native binding before its later draw");}
    const std::array<GpuTextureReadback::Source,1> native_read{{{native.Get(),D3D12_RESOURCE_STATE_PRESENT}}};std::vector<GpuTextureReadback::Pixels> pixels;check(GpuTextureReadback::capture(host.queue.Get(),native_read,pixels));for(size_t i=0;i<pixels[0].rgb.size();i+=3)require(pixels[0].rgb[i]==255 && pixels[0].rgb[i+1]==0 && pixels[0].rgb[i+2]==255,"later native magenta draw reaches original swapchain even when retained-copy path refuses");
    if(test=="hooked_capacity")require(native_calls==8 && completed.size()==2 && !retention.status().stopped,"four native camera jobs render once each while only the first two retain private eyes");
    if(happy && test!="hooked_capacity" && argc>2){std::ofstream out{argv[2]};out<<"{\"model\":\"ordinary_pair_gpu_retention_fixture_v1\",\"actual_game_run\":false,\"actual_gpu\":true,\"synthetic_native_camera\":true,\"native_colour_detours\":"<<(hooked?"true":"false")<<",\"blocked_completion_tested\":true,\"independent_rgba_pixels\":true,\"pairs\":[";for(size_t i=0;i<completed.size();++i){if(i)out<<',';Retention::write(out,completed[i]);}out<<"],\"readbacks\":[";
        const auto write_pixels=[&](const auto& image){out<<"{\"width\":"<<image.width<<",\"height\":"<<image.height<<",\"rgb\":[";for(size_t i=0;i<image.rgb.size();++i){if(i)out<<',';out<<unsigned(image.rgb[i]);}out<<"]}";};
        for(size_t i=0;i<retained_pixels.size();++i){if(i)out<<',';write_pixels(retained_pixels[i]);}out<<"],\"native_readback\":";write_pixels(pixels[0]);out<<",\"native_calls\":"<<native_calls<<",\"colour_scopes\":[";
        if(hooked)for(size_t i=0;i<completed.size();++i){if(i)out<<',';NativeOrdinaryPairColour::write(out,NativeOrdinaryPairColour::get().sample(completed[i].origin.scene.dispatch_sequence),8,4);}out<<"]}\n";require(bool(out),"actual GPU pixels and native callback receipts export for independent audit");}
    if(test=="happy"){
        auto packet=camera.receipt(0);packet.tracked=NativeOrdinaryPacket::TrackedSource{};packet.left_patched=packet.patched=NativeCameraPacket{};
        const auto& origin=completed[0].origin;packet.tracked->producer_list=origin.pair.producer_list;packet.tracked->producer_revision=origin.pair.producer_revision;
        auto& prepared=packet.tracked->prepared;prepared.render_id=packet.render_id;prepared.game_token=origin.pair.left.token;
        for(size_t i=0;i<2;++i){auto& eye=prepared.eyes[i];eye.world=vr_camera::IDENTITY;eye.tracking.eye=int(i);eye.tracking.render_token=packet.render_id;eye.tracking.tracking_epoch=1;
            require(vr_projection::build({-1,1,-1,1},.1f,1000,eye.projection),"runtime bridge fixture projection");}
        OrdinaryRuntimeImages bridge;auto foreign=packet;++foreign.tracked->producer_revision;require(!bridge.capture(completed[0],foreign,host.queue.Get()),"runtime bridge rejects another producer revision");
        auto alias=completed[0];alias.eyes[1].copy->texture=alias.eyes[0].copy->texture;require(!bridge.capture(alias,packet,host.queue.Get()),"runtime bridge rejects aliased eye images");
        require(bridge.refusals()[size_t(OrdinaryRuntimeImages::Refusal::state)]==1 && bridge.refusals()[size_t(OrdinaryRuntimeImages::Refusal::copy)]==1 && bridge.refusal_summary()=="state:1 copy:1",
            "runtime bridge counts each refusal under its reason");
        require(OrdinaryRuntimeImages{}.refusal_summary()=="none","runtime bridge with no refusals says none");
        {
            OrdinaryRuntimeImages isolated;
            require(!isolated.capture(completed[0],packet,host.queue.Get(),native.Get(),{},{},false,true),"live native HUD has no completed cross-queue source lease");
            ComPtr<ID3D12Fence> native_gate;check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&native_gate)));
            struct ReleaseGate{ID3D12Fence* fence;~ReleaseGate(){fence->Signal(1);}} release_native{native_gate.Get()};
            check(host.queue->Wait(native_gate.Get(),1));
            require(isolated.capture(completed[0],packet,host.queue.Get(),nullptr,{},{},false,true),"completed immutable eyes enter independent runtime queue while native queue is blocked");
            std::optional<VrSession::OrdinaryImages> image;
            for(unsigned n=0;n<2000 && !image;++n){image=isolated.take();if(!image)Sleep(1);}
            require(image && image->queue && image->queue.Get()!=host.queue.Get() && image->dispatch==77,"actual isolated bridge finishes despite blocked native queue and carries its producing queue");
            require(!isolated.take(),"isolated bridge publishes once");
            const std::array<GpuTextureReadback::Source,2> sources{{{image->images.left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{image->images.right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
            std::vector<GpuTextureReadback::Pixels> copied;check(GpuTextureReadback::capture(image->queue.Get(),sources,copied));
            require(copied[0].rgb==retained_pixels[0].rgb && copied[1].rgb==retained_pixels[1].rgb,"cross-queue completion preserves exact distinct producer pixels");
            check(native_gate->Signal(1));
            ComPtr<ID3D12Fence> runtime_gate;check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&runtime_gate)));
            ReleaseGate release_runtime{runtime_gate.Get()};check(image->queue->Wait(runtime_gate.Get(),1));
            host.begin();host.execute(); // Native work must progress while runtime work cannot.
            check(runtime_gate->Signal(1));
            require(isolated.reset(),"completed isolated bridge resets without abandoning pending copies");
            std::vector<GpuTextureReadback::Pixels> leased;check(GpuTextureReadback::capture(image->queue.Get(),sources,leased));
            require(leased[0].rgb==copied[0].rgb && leased[1].rgb==copied[1].rgb,"consumer queue and image leases survive bridge reset");
        }
        ComPtr<ID3D12Fence> bridge_gate;check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&bridge_gate)));check(host.queue->Wait(bridge_gate.Get(),1));
        require(bridge.capture(completed[0],packet,host.queue.Get()),"typed completed pair enters runtime image bridge");
        PresentHandoff handoff;std::optional<VrSession::OrdinaryImages> runtime;
        const auto drain=[&]{runtime=bridge.take();};
        handoff.visit(PresentHandoff::Point::present,drain);require(!runtime,"baseline handoff never takes unfinished bridge images");
        handoff.mode("opportunistic");handoff.visit(PresentHandoff::Point::post_frame,drain);require(!runtime && bridge.pending(),"extra handoff retains the pair behind its exact GPU fence without waiting");
        check(bridge_gate->Signal(1));host.begin();host.execute();
        handoff.visit(PresentHandoff::Point::post_present,drain);
        require(runtime && runtime->dispatch==77 && runtime->images.left.ready() && runtime->images.right.ready() && runtime->images.left.metadata.render_id==runtime->images.right.metadata.render_id,"same-frame post-Present handoff preserves same-job completed GPU leases");
        unsigned duplicate{};handoff.visit(PresentHandoff::Point::post_frame,[&]{if(bridge.take())++duplicate;});require(!duplicate && !bridge.pending(),"later extra callbacks cannot consume or submit a pair twice");
        const std::array<GpuTextureReadback::Source,2> runtime_sources{{{runtime->images.left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{runtime->images.right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
        std::vector<GpuTextureReadback::Pixels> runtime_pixels;check(GpuTextureReadback::capture(host.queue.Get(),runtime_sources,runtime_pixels));
        require(runtime_pixels[0].rgb==retained_pixels[0].rgb && runtime_pixels[1].rgb==retained_pixels[1].rgb,"runtime shader resources contain the original retained red/blue eye pixels");
        std::vector<GpuTextureReadback::Pixels> snapshot_pixels;
        check(OrdinaryEyeSnapshot::capture(*runtime,host.queue.Get(),snapshot_pixels));
        require(snapshot_pixels[0].rgb==retained_pixels[0].rgb && snapshot_pixels[1].rgb==retained_pixels[1].rgb,"on-demand snapshot reads the exact completed stereo leases");
        for(unsigned fault=0;fault<6;++fault){auto bad=*runtime;
            if(fault==0)++bad.images.right.metadata.render_id;
            if(fault==1)bad.images.right=bad.images.left;
            if(fault==2)bad.images.left.metadata.tracked_stereo=false;
            if(fault==3)++bad.images.right.metadata.tracking_epoch;
            if(fault==4)bad.images.right.fence.Reset();
            if(fault==5)bad.dispatch=0;
            require(OrdinaryEyeSnapshot::capture(bad,host.queue.Get(),snapshot_pixels)==E_INVALIDARG,"snapshot refuses mismatched or incomplete stereo images");
        }
        check(OrdinaryEyeSnapshot::capture(*runtime,host.queue.Get(),snapshot_pixels));
        require(snapshot_pixels[0].rgb==retained_pixels[0].rgb,"readback restores runtime shader resource state for a repeat capture");
        const auto snapshot_dir=std::filesystem::temp_directory_path()/("sm2vr-eye-snapshot-"+std::to_string(GetCurrentProcessId()));
        require(!std::filesystem::exists(snapshot_dir),"snapshot fixture owns a fresh output directory");
        check(OrdinaryEyeSnapshot::save(*runtime,host.queue.Get(),snapshot_dir,true));
        std::ifstream snapshot_meta{snapshot_dir/"pair.json"};std::string snapshot_text((std::istreambuf_iterator<char>(snapshot_meta)),{});snapshot_meta.close();
        require(snapshot_text.find("\"dispatch\":77")!=std::string::npos && snapshot_text.find("\"submitted\":true")!=std::string::npos &&
                std::filesystem::file_size(snapshot_dir/"left.ppm")>96 && std::filesystem::file_size(snapshot_dir/"right.ppm")>96,"snapshot saves both pixels and their producer metadata");
        require(OrdinaryEyeSnapshot::save(*runtime,host.queue.Get(),snapshot_dir,true)==E_INVALIDARG,"snapshot refuses to overwrite existing evidence");
        std::filesystem::remove_all(snapshot_dir);
        const auto joined_id="joined_"+std::to_string(GetCurrentProcessId());
        const auto joined_dir=std::filesystem::temp_directory_path()/joined_id;
        require(!std::filesystem::exists(joined_dir),"joined fixture owns a fresh output directory");
        OrdinaryJoinedCapture joined_gate;
        const auto joined_render=runtime->images.left.metadata.render_id;
        require(joined_gate.reserve(joined_id,runtime->dispatch,joined_render) && joined_gate.complete(joined_id,runtime->dispatch,joined_render),"completed native target is immutable before GPU capture");
        const auto target=joined_gate.claim(runtime->dispatch,joined_render,joined_render,true);require(bool(target),"current completed leases match the draw target");
        auto wrong_target=*target;++wrong_target.dispatch;
        require(OrdinaryEyeSnapshot::save(*runtime,host.queue.Get(),joined_dir,true,wrong_target)==E_INVALIDARG && !std::filesystem::exists(joined_dir),"a mismatched target refuses before GPU capture or file creation");
        check(OrdinaryEyeSnapshot::save(*runtime,host.queue.Get(),joined_dir,true,target));
        std::ifstream joined_meta{joined_dir/"pair.json"};std::string joined_text((std::istreambuf_iterator<char>(joined_meta)),{});joined_meta.close();
        require(joined_text.find("\"draw_dispatch\":77")!=std::string::npos && joined_text.find("\"verified\":true")!=std::string::npos,"real completed GPU images retain the exact draw-frame join");
        require(std::filesystem::file_size(joined_dir/"left.ppm")>96 && std::filesystem::file_size(joined_dir/"right.ppm")>96,"joined capture writes both actual GPU eyes");
        std::filesystem::remove_all(joined_dir);
        require(retention.retire(77) && !retention.retire(77),"completed first pair retires exactly once while second remains owned");
        for(unsigned n=2;n<7;++n){camera.frame(n);host.begin();auto transition=barrier(native.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
            for(uint32_t slot=0;slot<2;++slot){auto event=camera.event(slot,NativeOrdinaryProbe::Stage::compositor_before,n);require(retention.consume(&event,swap.Get()),"freed output slot accepts another native pair");host.commands->OMSetRenderTargets(1,&handle,TRUE,nullptr);draw(slot);event=camera.event(slot,NativeOrdinaryProbe::Stage::compositor_after,n);require(retention.consume(&event,swap.Get()),"reused output restores native binding");}
            retention.complete(camera.snapshot(n),camera.receipt(n));transition=barrier(native.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);host.commands->ResourceBarrier(1,&transition);host.execute();
            const auto latest=retention.samples();require(latest.size()==2 && latest.back().gpu_retention_verified,"continuous pair joins exact GPU completion");require(retention.retire(77+n),"new pair retires after consumer takes its lease");
        }
        std::array<GpuTextureReadback::Source,2> old{{{completed[0].eyes[0].copy->texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE},{completed[0].eyes[1].copy->texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE}}};
        std::vector<GpuTextureReadback::Pixels> preserved;check(GpuTextureReadback::capture(host.queue.Get(),old,preserved));require(preserved[0].rgb==retained_pixels[0].rgb && preserved[1].rgb==retained_pixels[1].rgb,"consumer-held retired pair stays immutable through five subsequent pairs");
        require(retention.retire(78) && retention.samples().empty(),"second output slot retires independently");
    }
    targets.transform_binding={};DestroyWindow(hidden);std::cout<<"ordinary pair GPU retention: "<<test<<" PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
