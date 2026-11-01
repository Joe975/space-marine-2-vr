#include "GpuPresentMirror.h"

HRESULT GpuPresentMirror::submit(ID3D12CommandQueue* queue,const GpuInlineCopy::Sample& image,uint64_t tag,ID3D12Resource* buffer,uintptr_t expected_buffer){
    if(!queue || queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT || !tag || image.tag!=tag || !image.appended || !image.submitted || image.refusal!=GpuInlineCopy::Refusal::none ||
       !image.texture || !image.fence || !image.fence_value || image.fence->GetCompletedValue()==UINT64_MAX || image.queue!=reinterpret_cast<uintptr_t>(queue) || !buffer ||
       !expected_buffer || GpuObjectIdentity::get(buffer)!=expected_buffer || GpuObjectIdentity::get(buffer)==GpuObjectIdentity::get(image.texture.Get()) || m_work.size()>=4)return E_INVALIDARG;
    for(const auto& work:m_work)if(work.sample.tag==tag)return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ID3D12Device> device,owner;
    if(FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || device->GetNodeCount()!=1 || FAILED(buffer->GetDevice(IID_PPV_ARGS(&owner))) || GpuObjectIdentity::get(owner.Get())!=GpuObjectIdentity::get(device.Get()) ||
       FAILED(image.texture->GetDevice(IID_PPV_ARGS(&owner))) || GpuObjectIdentity::get(owner.Get())!=GpuObjectIdentity::get(device.Get()))return E_INVALIDARG;
    const auto source=image.texture->GetDesc(),dest=buffer->GetDesc();
    if(source.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || source.Width!=dest.Width || source.Height!=dest.Height || source.Format!=dest.Format || source.MipLevels!=1 || source.DepthOrArraySize!=1 || source.SampleDesc.Count!=1 ||
       dest.Dimension!=source.Dimension || dest.MipLevels!=1 || dest.DepthOrArraySize!=1 || dest.SampleDesc.Count!=1)return E_INVALIDARG;
    Work work;work.sample={tag,image.fence_value,1,GpuObjectIdentity::get(image.texture.Get()),expected_buffer,reinterpret_cast<uintptr_t>(queue)};
    work.queue=queue;work.source=image.texture;work.producer=image.fence;work.destination=buffer;
    auto hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&work.allocator));if(FAILED(hr))return hr;
    hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,work.allocator.Get(),nullptr,IID_PPV_ARGS(&work.commands));if(FAILED(hr))return hr;
    hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&work.fence));if(FAILED(hr))return hr;
    D3D12_RESOURCE_BARRIER transition{};transition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;transition.Transition={buffer,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST};
    work.commands->ResourceBarrier(1,&transition);work.commands->CopyResource(buffer,image.texture.Get());std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);work.commands->ResourceBarrier(1,&transition);
    hr=work.commands->Close();if(FAILED(hr))return hr;
    hr=queue->Wait(image.fence.Get(),image.fence_value);if(FAILED(hr))return hr;
    m_work.push_back(std::move(work));auto& pending=m_work.back();ID3D12CommandList* lists[]{pending.commands.Get()};queue->ExecuteCommandLists(1,lists);pending.sample.submitted=true;
    pending.sample.result=queue->Signal(pending.fence.Get(),1);return pending.sample.result;
}
void GpuPresentMirror::poll(){for(auto& work:m_work)if(work.sample.submitted && !work.sample.completed && SUCCEEDED(work.sample.result)){const auto value=work.fence->GetCompletedValue();if(value!=UINT64_MAX && value>=work.sample.fence_value)work.sample.completed=true;}}
std::optional<GpuPresentMirror::Sample> GpuPresentMirror::sample(uint64_t tag){poll();for(const auto& work:m_work)if(work.sample.tag==tag)return work.sample;return std::nullopt;}
bool GpuPresentMirror::reset(){poll();for(const auto& work:m_work)if(work.sample.submitted && !work.sample.completed)return false;m_work.clear();return true;}
GpuPresentMirror::~GpuPresentMirror(){if(!reset())for(auto& work:m_work)if(work.sample.submitted && !work.sample.completed){work.queue.Detach();work.source.Detach();work.destination.Detach();work.producer.Detach();work.fence.Detach();work.allocator.Detach();work.commands.Detach();}}
void GpuPresentMirror::write_sample(std::ostream& out,const Sample& s){out<<"{\"tag\":"<<s.tag<<",\"producer_fence\":"<<s.producer_fence<<",\"fence_value\":"<<s.fence_value<<",\"image\":"<<s.image<<",\"destination\":"<<s.destination<<",\"queue\":"<<s.queue<<",\"result\":"<<s.result<<",\"submitted\":"<<(s.submitted?"true":"false")<<",\"completed\":"<<(s.completed?"true":"false")<<'}';}
