#include "GpuCompositorOutput.h"

bool GpuCompositorOutput::configure(ID3D12Device* device,ID3D12Resource* shape,Open open,Clear clear){
    if(m_device || !device || !shape || !open || !clear)return false;
    auto desc=shape->GetDesc();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    desc.Alignment=0;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if(!GpuInlineCopy::display_backing(desc,heap,D3D12_HEAP_FLAG_ALLOW_DISPLAY))return false;
    Microsoft::WRL::ComPtr<ID3D12Device> owner;if(FAILED(shape->GetDevice(IID_PPV_ARGS(&owner))) || GpuObjectIdentity::get(owner.Get())!=GpuObjectIdentity::get(device))return false;
    if(!create(device,desc,heap))return false;
    m_device=device;m_desc=m_texture->GetDesc();m_open=std::move(open);m_clear=std::move(clear);return true;
}
bool GpuCompositorOutput::create(ID3D12Device* device,const D3D12_RESOURCE_DESC& desc,const D3D12_HEAP_PROPERTIES& heap){
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_ALLOW_DISPLAY,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&texture))))return false;
    D3D12_HEAP_PROPERTIES actual{};D3D12_HEAP_FLAGS flags{};if(FAILED(texture->GetHeapProperties(&actual,&flags)) || !GpuInlineCopy::display_backing(texture->GetDesc(),actual,flags))return false;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap;D3D12_DESCRIPTOR_HEAP_DESC h{};h.NumDescriptors=1;h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if(FAILED(device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&rtv_heap))))return false;
    m_heap=std::move(rtv_heap);m_rtv=m_heap->GetCPUDescriptorHandleForHeapStart();D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=desc.Format;view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;device->CreateRenderTargetView(texture.Get(),&view,m_rtv);
    m_texture=std::move(texture);m_state=D3D12_RESOURCE_STATE_COMMON;return true;
}
bool GpuCompositorOutput::renew(){
    if(!m_poisoned)return true;
    if(!m_device || s_active==this || m_owner.test_and_set(std::memory_order_acquire))return false;
    // The poisoned texture's GPU state is unknown and queued work may still use it: it and its RTV
    // heap are never reused and never released (at most a few per session, the cold-restart budget).
    auto retired_texture=m_texture.Detach();auto retired_heap=m_heap.Detach();(void)retired_texture;(void)retired_heap;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    auto desc=m_desc;desc.Alignment=0;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const bool created=GpuInlineCopy::display_backing(desc,heap,D3D12_HEAP_FLAG_ALLOW_DISPLAY) && create(m_device.Get(),desc,heap);
    if(created){m_poisoned=false;m_exposed=false;m_receipt={};++m_renewals;}
    m_owner.clear(std::memory_order_release);return created;
}
bool GpuCompositorOutput::begin(uint64_t tag,ID3D12Resource* source){
    if(s_active || m_owner.test_and_set(std::memory_order_acquire))return false;
    if(!m_texture || !tag || !source || m_poisoned){m_owner.clear(std::memory_order_release);return false;}
    const auto desc=source->GetDesc();Microsoft::WRL::ComPtr<ID3D12Device> device;
    if(desc.Dimension!=m_desc.Dimension || desc.Width!=m_desc.Width || desc.Height!=m_desc.Height || desc.Format!=m_desc.Format || desc.SampleDesc.Count!=1 || desc.MipLevels!=1 || desc.DepthOrArraySize!=1 ||
       FAILED(source->GetDevice(IID_PPV_ARGS(&device))) || GpuObjectIdentity::get(device.Get())!=GpuObjectIdentity::get(m_device.Get())){m_owner.clear(std::memory_order_release);return false;}
    m_source=source;m_commands.Reset();m_current_replaced=false;m_thread=GetCurrentThreadId();m_receipt={};m_receipt.tag=tag;m_receipt.active=true;m_receipt.private_backing=true;
    D3D12_HEAP_PROPERTIES source_heap{};D3D12_HEAP_FLAGS source_flags{};m_receipt.source_heap_known=SUCCEEDED(source->GetHeapProperties(&source_heap,&source_flags));m_receipt.source_heap_flags=source_flags;m_receipt.source_flags=desc.Flags;
    m_receipt.source=GpuObjectIdentity::get(source);m_receipt.target=GpuObjectIdentity::get(m_texture.Get());m_receipt.format=m_desc.Format;m_receipt.width=uint32_t(m_desc.Width);m_receipt.height=m_desc.Height;s_active=this;return true;
}
std::optional<RenderTargetProbe::Binding> GpuCompositorOutput::transform(ID3D12GraphicsCommandList* command,const RenderTargetProbe::Binding& native,std::span<const RenderTargetProbe::View> views){
    if(s_active!=this)return std::nullopt; // Unrelated threads never inspect or mutate this scope.
    if(GetCurrentThreadId()!=m_thread || !command || (m_commands && m_commands.Get()!=command)){m_receipt.refusal=Refusal::recording;return std::nullopt;}
    const auto generation=m_open(command);if(!generation || (m_commands && *generation!=m_receipt.generation)){m_receipt.refusal=Refusal::recording;return std::nullopt;}
    m_commands=command;m_receipt.generation=*generation;m_last_native=native;m_current_replaced=false;
    for(const auto& view:views)if(m_receipt.observed_count<m_receipt.observed.size())m_receipt.observed[m_receipt.observed_count++]={view.identity,view.width,view.height,view.format,view.known};
    if(native.count!=1 || views.size()!=1 || views[0].identity!=m_receipt.source)return std::nullopt;
    const auto& view=views[0];if(!view.known || view.format!=m_desc.Format || view.width!=m_desc.Width || view.height!=m_desc.Height || view.dimension!=D3D12_RTV_DIMENSION_TEXTURE2D || view.subresources!=1 || m_receipt.refusal!=Refusal::none){m_receipt.refusal=Refusal::description;return std::nullopt;}
    if(m_state==D3D12_RESOURCE_STATE_COMMON){D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={m_texture.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,m_state,D3D12_RESOURCE_STATE_RENDER_TARGET};command->ResourceBarrier(1,&barrier);m_state=D3D12_RESOURCE_STATE_RENDER_TARGET;m_clear(command,m_rtv);}
    auto replacement=native;replacement.handles[0]=m_rtv;m_current_replaced=m_exposed=true;++m_receipt.bindings;m_receipt.commands=reinterpret_cast<uintptr_t>(command);m_receipt.generation=*generation;return replacement;
}
GpuCompositorOutput::Receipt GpuCompositorOutput::end(uint64_t tag,bool safe,RenderTargetProbe& targets){
    if(s_active!=this){Receipt result;result.tag=tag;result.refusal=Refusal::scope;return result;}
    if(tag!=m_receipt.tag){auto result=m_receipt;result.refusal=Refusal::scope;return result;}
    const auto generation=m_commands?m_open(m_commands.Get()):std::nullopt;m_receipt.restore_required=m_current_replaced;
    // Copy refusal does not authorize leaving a native RTV binding redirected.
    // Restore CPU binding state whenever the exact recording is still valid;
    // unknown GPU resource state remains quarantined and never gains acceptance.
    const bool can_restore=generation && *generation==m_receipt.generation && GetCurrentThreadId()==m_thread;
    if(can_restore){
        if(m_current_replaced){targets.restore_binding(m_commands.Get(),m_last_native);++m_receipt.restores;
            if(s_clear_native.load(std::memory_order_relaxed))m_clear(m_commands.Get(),m_last_native.handles[0]);}
        m_receipt.restored=true;
    }
    if(!safe || !m_receipt.bindings || !generation || *generation!=m_receipt.generation || GetCurrentThreadId()!=m_thread || m_receipt.refusal!=Refusal::none){m_receipt.refusal=Refusal::unsafe_end;m_poisoned=true;}
    else{
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={m_texture.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,m_state,D3D12_RESOURCE_STATE_COMMON};m_commands->ResourceBarrier(1,&barrier);m_state=D3D12_RESOURCE_STATE_COMMON;
    }
    m_receipt.active=false;m_commands.Reset();m_source.Reset();const auto result=m_receipt;{std::lock_guard lock{m_snapshot_mutex};m_completed=result;}s_active=nullptr;m_owner.clear(std::memory_order_release);return result;
}
GpuCompositorOutput::~GpuCompositorOutput(){if(m_exposed){m_texture.Detach();m_heap.Detach();m_device.Detach();}}
void GpuCompositorOutput::write_receipt(std::ostream& out,const Receipt& r){out<<"{\"tag\":"<<r.tag<<",\"generation\":"<<r.generation<<",\"bindings\":"<<r.bindings<<",\"restores\":"<<r.restores<<",\"source\":"<<r.source<<",\"target\":"<<r.target<<",\"commands\":"<<r.commands<<",\"format\":"<<r.format<<",\"width\":"<<r.width<<",\"height\":"<<r.height<<",\"refusal\":"<<uint32_t(r.refusal)
    <<",\"active\":"<<(r.active?"true":"false")<<",\"restored\":"<<(r.restored?"true":"false")<<",\"restore_required\":"<<(r.restore_required?"true":"false")<<",\"private_backing\":"<<(r.private_backing?"true":"false")<<",\"source_flags\":"<<r.source_flags<<",\"source_heap_known\":"<<(r.source_heap_known?"true":"false")<<",\"source_heap_flags\":"<<r.source_heap_flags<<",\"observed_targets\":[";
    for(size_t i=0;i<r.observed_count;++i){if(i)out<<',';const auto& v=r.observed[i];out<<"{\"identity\":"<<v.identity<<",\"width\":"<<v.width<<",\"height\":"<<v.height<<",\"format\":"<<v.format<<",\"known\":"<<(v.known?"true":"false")<<'}';}out<<"]}";}
