#include "GpuTestHost.h"
#include "GpuSdkCapture.h"
#include "GpuCompletionProbe.h"

int main(int argc,char** argv){try{
    for(auto bits:{0x7c00,0xfc00,0x7c01,0xffff})require(GpuSdkCapture::nonfinite_half(uint16_t(bits)),"both signs of infinity and NaN remain distinguishable from decoded black");
    for(auto bits:{0,0x8000,0x7bff,0xfbff})require(!GpuSdkCapture::nonfinite_half(uint16_t(bits)),"finite zero and largest signed finite half remain finite");
    for(size_t c=0;c<3;++c){const uint32_t bits=c==2?uint32_t(31)<<27:uint32_t(31)<<(c*11+6);require(GpuSdkCapture::nonfinite_half(GpuSdkCapture::packed_half(bits,c)),"packed HDR exponent preserves nonfinite classification");}
    Host host;auto& capture=*new GpuSdkCapture;auto& coordinator=*new GpuCompletionProbe;
    coordinator.sdk_copies(&capture);require(coordinator.install(host.device.Get(),host.queue.Get(),[](uintptr_t){return uintptr_t{};}),"real command-list submission and reset hooks installed");
    const auto red=host.color(1,0,0,8,DXGI_FORMAT_R8G8B8A8_UNORM,4);
    host.begin();const auto state=D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto transition=barrier(red.Get(),D3D12_RESOURCE_STATE_COMMON,state);host.commands->ResourceBarrier(1,&transition);
    capture.enabled(false);
    capture.current_scope=[](){return std::optional<GpuSdkCapture::Scope>{{1,0}};};
    capture.request(1,0,2,reinterpret_cast<uintptr_t>(red.Get()));
    require(!capture.append(1,0,0,host.commands.Get(),red.Get(),state),"paused warmup rejects actual SDK copy");
    capture.enabled(true);
    transition=barrier(red.Get(),state,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
    require(capture.items().empty(),"paused warmup must not leave a deferred boundary request");
    std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&transition);
    require(!capture.append(0,0,0,host.commands.Get(),red.Get(),state),"zero job is rejected");
    require(!capture.append(1,2,0,host.commands.Get(),red.Get(),state),"invalid eye is rejected");
    require(!capture.append(1,0,0,host.commands.Get(),red.Get(),D3D12_RESOURCE_STATE_COMMON),"unrecognized tagged state is rejected");
    require(capture.append(1,0,0,host.commands.Get(),red.Get(),state),"actual SDK-boundary copy is appended");
    require(!capture.append(1,0,0,host.commands.Get(),red.Get(),state),"duplicate boundary is rejected");
    require(!capture.items()[0].submitted && capture.items()[0].fence->GetCompletedValue()==0,"copy cannot be read before submission");
    transition=barrier(red.Get(),state,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;ComPtr<ID3D12DescriptorHeap> heap;check(host.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
    auto rtv=heap->GetCPUDescriptorHandleForHeapStart();host.device->CreateRenderTargetView(red.Get(),nullptr,rtv);const float blue[]{0,0,1,1};host.commands->ClearRenderTargetView(rtv,blue,0,nullptr);
    std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&transition);
    require(capture.append(1,1,0,host.commands.Get(),red.Get(),state),"second eye captures later content from same transient source");
    check(host.commands->Close());ID3D12CommandList* lists[]{host.commands.Get()};host.queue->ExecuteCommandLists(1,lists);
    const auto items=capture.items();for(const auto& item:items)wait(item.fence.Get(),1);
    for(size_t eye=0;eye<2;++eye){const auto& item=items[eye];void* mapped{};D3D12_RANGE range{0,size_t(item.bytes)};check(item.buffer->Map(0,&range,&mapped));const auto* pixel=static_cast<const uint8_t*>(mapped)+item.footprint.Offset;
        require(pixel[0]==(eye?0:255) && pixel[1]==0 && pixel[2]==(eye?255:0),"GPU copy retains red before and blue after actual overwrite");D3D12_RANGE empty{};item.buffer->Unmap(0,&empty);}
    host.begin();require(capture.append(2,0,0,host.commands.Get(),red.Get(),state),"new job admitted");check(host.commands->Close());host.begin();require(capture.items().back().invalid,"actual native reset before submission invalidates pending snapshot");check(host.commands->Close());
    const auto hdr=host.color(2,.5f,.25f,8,DXGI_FORMAT_R16G16B16A16_FLOAT,4);
    const auto packed=host.color(2,.5f,.25f,960,DXGI_FORMAT_R11G11B10_FLOAT,4);
    capture.current_scope=[](){return std::optional<GpuSdkCapture::Scope>{{3,1}};};
    capture.request(3,0,2,reinterpret_cast<uintptr_t>(hdr.Get())); // Other eye must not consume this request.
    capture.request(3,1,2,reinterpret_cast<uintptr_t>(hdr.Get()));
    host.begin();transition=barrier(hdr.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
    const auto before=capture.items().size();transition=barrier(hdr.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,state);host.commands->ResourceBarrier(1,&transition);
    require(capture.items().size()==before+1,"matching live barrier captures exactly one requested eye");
    const auto hdrItem=capture.items().back();
    capture.request(3,1,4,reinterpret_cast<uintptr_t>(hdr.Get()));
    transition=barrier(red.Get(),state,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
    require(capture.items().size()==before+2 && capture.items().back().stage==4 && capture.items().back().state==state,"unchanged shader input captured using leased source and latest native state at unrelated live barrier");
    // A later post-process input was never tagged by SL. Lease it only at its
    // actual native transition, then capture while it stays shader-readable.
    transition=barrier(packed.Get(),D3D12_RESOURCE_STATE_COMMON,state);host.commands->ResourceBarrier(1,&transition);
    capture.request(3,1,6,reinterpret_cast<uintptr_t>(packed.Get()));
    transition=barrier(red.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,state);host.commands->ResourceBarrier(1,&transition);
    require(capture.items().size()==before+3 && capture.items().back().stage==6,"native-only input remains capturable without another transition of its own");
    capture.request(3,1,7,reinterpret_cast<uintptr_t>(packed.Get()));
    transition=barrier(red.Get(),state,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
    require(capture.items().size()==before+4 && capture.items().back().stage==7,"early scene boundary captures the same live native input independently");
    capture.request(3,1,10,reinterpret_cast<uintptr_t>(packed.Get()));
    transition=barrier(red.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,state);host.commands->ResourceBarrier(1,&transition);
    require(capture.items().size()==before+5 && capture.items().back().stage==10,"sharpen output boundary accepts independent completed native pixels");
    host.execute();wait(hdrItem.fence.Get(),1);void* raw{};D3D12_RANGE hdrRange{0,size_t(hdrItem.bytes)};check(hdrItem.buffer->Map(0,&hdrRange,&raw));
    const auto* half=static_cast<const uint16_t*>(raw);require(GpuTextureReadback::half(half[0])==2 && GpuTextureReadback::half(half[1])==.5f && GpuTextureReadback::half(half[2])==.25f,"HDR diagnostic retains unclamped actual FP16 values");D3D12_RANGE noWrites{};hdrItem.buffer->Unmap(0,&noWrites);
    const auto packedItem=capture.items().back();wait(packedItem.fence.Get(),1);D3D12_RANGE packedRange{0,size_t(packedItem.bytes)};check(packedItem.buffer->Map(0,&packedRange,&raw));uint32_t packedPixel{};std::memcpy(&packedPixel,static_cast<const uint8_t*>(raw)+packedItem.footprint.Offset,4);
    require(GpuSdkCapture::packed_hdr(packedPixel,0)==2 && GpuSdkCapture::packed_hdr(packedPixel,1)==.5f && GpuSdkCapture::packed_hdr(packedPixel,2)==.25f,"native packed HDR capture and decode retain actual R11G11B10 values");packedItem.buffer->Unmap(0,&noWrites);
    require(!capture.append(4,0,0,host.commands.Get(),red.Get(),state),"third live diagnostic job is refused");
    // sdk_request.txt (23 Oct): capture two more jobs at a held pose, once the earlier copies are saved.
    require(!capture.rearm(),"rearm waits while retained copies are not yet saved");
    for(const auto& item:capture.items())if(!item.invalid)wait(item.fence.Get(),1);
    const auto saved=std::filesystem::temp_directory_path()/("sm2vr-sdk-rearm-"+std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(saved);capture.save(saved);
    require(capture.rearm() && capture.items().empty(),"rearm clears saved copies");
    host.begin();require(capture.append(4,0,0,host.commands.Get(),red.Get(),state),"a rearmed capture admits a new job");
    host.execute();wait(capture.items().back().fence.Get(),1);std::filesystem::remove_all(saved);
    std::cout<<"SDK boundary capture: actual retained GPU pixels and pending-reset refusal PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
