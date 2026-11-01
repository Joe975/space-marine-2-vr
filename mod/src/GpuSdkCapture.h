#pragma once
#include "GpuTextureReadback.h"
#include "GpuObjectIdentity.h"
#include <mutex>
#include <set>
#include <functional>
#include <optional>
#include <atomic>

// Bounded diagnostic at the actual SL API call. SL restores tagged resource
// states (ProgrammingGuide.md, resource tagging contract). Copies are recorded
// on that same native command list, before later passes can overwrite the image.
class GpuSdkCapture {
public:
    // Pause admission until the live headset has warmed up. Existing copies
    // still retire normally; this never changes their GPU lifetime.
    void enabled(bool value){m_enabled.store(value,std::memory_order_release);}
    using Scope=std::pair<uint64_t,uint32_t>;
    std::function<std::optional<Scope>()> current_scope;
    // Address is only matched against a live native barrier argument. Never
    // dereference the remembered wrapper address or retain it as a COM lease.
    void request(uint64_t dispatch,uint32_t eye,uint32_t stage,uintptr_t resource){
        if(!m_enabled.load(std::memory_order_acquire) || !dispatch || eye>1 || stage<2 || stage>10 || !resource)return;
        std::lock_guard lock{m_mutex};if(m_requests.size()<36)m_requests.push_back({dispatch,eye,stage,resource,GetCurrentThreadId()});
    }
    void barriers(ID3D12GraphicsCommandList* command,UINT count,const D3D12_RESOURCE_BARRIER* values){
        if(!m_enabled.load(std::memory_order_acquire) || s_copying || !values || count>4096)return;
        // Only already leased SDK/capture resources are tracked. No resource
        // pointer is promoted to a lease from remembered engine metadata.
        const auto scope=current_scope?current_scope():std::optional<Scope>{};
        if(scope){std::lock_guard lock{m_mutex};const bool admitted=std::ranges::any_of(m_items,[&](const auto& v){return v.dispatch==scope->first && !v.invalid;});
            if(admitted)for(UINT i=0;i<count;++i){const auto& b=values[i];if(b.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource || b.Flags!=D3D12_RESOURCE_BARRIER_FLAG_NONE || b.Transition.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)continue;
                if(std::ranges::any_of(m_observed,[&](const auto& v){return v.dispatch==scope->first && v.eye==scope->second && v.commands.Get()==command && v.source.Get()==b.Transition.pResource;}))continue;
                if(m_observed.size()>=128)break;const auto d=b.Transition.pResource->GetDesc();
                if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width<960 || d.Width>4096 || d.Height>2160 || d.MipLevels!=1 || d.DepthOrArraySize!=1 || d.SampleDesc.Count!=1 || (d.Format!=DXGI_FORMAT_R16G16B16A16_TYPELESS && d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT && d.Format!=DXGI_FORMAT_R11G11B10_FLOAT))continue;
                Item v;v.dispatch=scope->first;v.eye=scope->second;v.commands=command;v.source=b.Transition.pResource;v.current_state=b.Transition.StateBefore;m_observed.push_back(std::move(v));
            }
        }
        std::vector<Request> pending;{std::lock_guard lock{m_mutex};pending=m_requests;}
        for(const auto& request:pending)if(scope && request.dispatch==scope->first && request.eye==scope->second && request.thread==GetCurrentThreadId()){
            // A shader input can stay readable throughout the entire tone pass.
            // Capture it at the next live command-list boundary using the state
            // established by the SDK contract and subsequent native barriers.
            std::optional<Item> leased;
            {std::lock_guard lock{m_mutex};for(auto it=m_items.rbegin();it!=m_items.rend();++it)
                if(!it->invalid && it->state_known && it->dispatch==request.dispatch && it->eye==request.eye && it->commands.Get()==command && reinterpret_cast<uintptr_t>(it->source.Get())==request.resource){leased=*it;break;}
                if(!leased)for(auto it=m_observed.rbegin();it!=m_observed.rend();++it)if(!it->invalid && it->state_known && it->dispatch==request.dispatch && it->eye==request.eye && it->commands.Get()==command && reinterpret_cast<uintptr_t>(it->source.Get())==request.resource){leased=*it;break;}}
            if(leased && append(request.dispatch,request.eye,request.stage,command,leased->source.Get(),D3D12_RESOURCE_STATES(leased->current_state)))continue;
            for(UINT i=0;i<count;++i){const auto& b=values[i];if(b.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || b.Flags!=D3D12_RESOURCE_BARRIER_FLAG_NONE || b.Transition.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || reinterpret_cast<uintptr_t>(b.Transition.pResource)!=request.resource)continue;
                // Called before this real barrier, while StateBefore is exact.
                if(append(request.dispatch,request.eye,request.stage,command,b.Transition.pResource,b.Transition.StateBefore))break;
            }
        }
        std::lock_guard lock{m_mutex};const auto update=[&](auto& collection){for(auto& item:collection)for(UINT i=0;i<count;++i){const auto& b=values[i];
            if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource==item.source.Get()){
                item.state_known=b.Flags==D3D12_RESOURCE_BARRIER_FLAG_NONE && b.Transition.Subresource==D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                item.current_state=b.Transition.StateAfter;
            }else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING && (!b.Aliasing.pResourceBefore || b.Aliasing.pResourceBefore==item.source.Get() || b.Aliasing.pResourceAfter==item.source.Get()))item.state_known=false;
        }};update(m_items);update(m_observed);
    }
    struct Item {
        uint64_t dispatch{};uint32_t eye{},stage{},state{},current_state{};bool submitted{},invalid{},saved{},state_known{true};
        D3D12_RESOURCE_DESC desc{};D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
        Microsoft::WRL::ComPtr<ID3D12Resource> source,buffer;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    };
    bool append(uint64_t dispatch,uint32_t eye,uint32_t stage,ID3D12GraphicsCommandList* command,ID3D12Resource* resource,D3D12_RESOURCE_STATES state){
        if(!m_enabled.load(std::memory_order_acquire))return false;
        std::lock_guard lock{m_mutex};if(!dispatch || eye>1 || stage>10 || !command || !resource || m_items.size()>=44 || command->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
        std::set<uint64_t> jobs;for(const auto& item:m_items)if(!item.invalid)jobs.insert(item.dispatch);if(jobs.size()>=2 && !jobs.contains(dispatch))return false;
        for(const auto& item:m_items)if(item.dispatch==dispatch && item.eye==eye && item.stage==stage)return false;
        if(state!=D3D12_RESOURCE_STATE_UNORDERED_ACCESS && state!=D3D12_RESOURCE_STATE_RENDER_TARGET && state!=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE && state!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE && state!=(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))return false;
        Item item;item.dispatch=dispatch;item.eye=eye;item.stage=stage;item.state=item.current_state=state;item.desc=resource->GetDesc();const auto& d=item.desc;
        if(m_attempts.size()<64 && std::ranges::none_of(m_attempts,[&](const auto& a){return a.dispatch==dispatch && a.eye==eye && a.stage==stage;}))m_attempts.push_back({dispatch,eye,stage,uint32_t(d.Format),uint32_t(d.Width),d.Height,d.MipLevels,reinterpret_cast<uintptr_t>(resource)});
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || !d.Width || d.Width>4096 || !d.Height || d.Height>2160 || d.DepthOrArraySize!=1 || d.MipLevels!=1 || d.SampleDesc.Count!=1 ||
           (d.Format!=DXGI_FORMAT_R11G11B10_FLOAT && d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT && d.Format!=DXGI_FORMAT_R16G16B16A16_TYPELESS && d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && d.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS))return false;
        Microsoft::WRL::ComPtr<ID3D12Device> device,owner;if(FAILED(command->GetDevice(IID_PPV_ARGS(&device))) || FAILED(resource->GetDevice(IID_PPV_ARGS(&owner))) || GpuObjectIdentity::get(device.Get())!=GpuObjectIdentity::get(owner.Get()))return false;
        device->GetCopyableFootprints(&d,0,1,0,&item.footprint,nullptr,nullptr,&item.bytes);
        if(d.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS)item.footprint.Footprint.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        if(d.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS)item.footprint.Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=item.bytes;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&item.buffer))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&item.fence))))return false;
        item.source=resource;item.commands=command;
        struct CopyScope {bool before{s_copying};CopyScope(){s_copying=true;}~CopyScope(){s_copying=before;}} copying;
        // Publish leases before recording any GPU use.
        m_items.push_back(std::move(item));auto& stored=m_items.back();
        D3D12_RESOURCE_BARRIER transition{};transition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;transition.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,state,D3D12_RESOURCE_STATE_COPY_SOURCE};
        command->ResourceBarrier(1,&transition);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=resource;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=stored.buffer.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=stored.footprint;
        command->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);command->ResourceBarrier(1,&transition);return true;
    }
    void reset(ID3D12GraphicsCommandList* command){std::lock_guard lock{m_mutex};for(auto& item:m_items)if(item.commands.Get()==command && !item.submitted)item.invalid=true;for(auto& item:m_observed)if(item.commands.Get()==command)item.invalid=true;}
    // Called only AFTER the actual native ExecuteCommandLists.
    void submitted(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* lists){
        if(!queue || !lists || !count || count>4096)return;std::lock_guard lock{m_mutex};
        for(auto& item:m_items)if(!item.submitted && !item.invalid)for(UINT i=0;i<count;++i)if(GpuObjectIdentity::get(lists[i])==GpuObjectIdentity::get(item.commands.Get())){
            Microsoft::WRL::ComPtr<ID3D12Device> a,b;
            if(queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT || FAILED(queue->GetDevice(IID_PPV_ARGS(&a))) || FAILED(item.source->GetDevice(IID_PPV_ARGS(&b))) || GpuObjectIdentity::get(a.Get())!=GpuObjectIdentity::get(b.Get())){item.invalid=true;break;}
            item.submitted=true;if(FAILED(queue->Signal(item.fence.Get(),1)))item.invalid=true;break;
        }
    }
    std::vector<Item> items()const{std::lock_guard lock{m_mutex};return m_items;}
    // Starts a fresh two-job capture (the next two admitted dispatches) on request, e.g. at a held
    // head pose. Refused while any earlier copy is still pending: its readback keeps its leases.
    bool rearm(){
        std::lock_guard lock{m_mutex};
        if(std::ranges::any_of(m_items,[](const auto& item){return !item.invalid && !item.saved;}))return false;
        m_items.clear();m_observed.clear();m_requests.clear();m_attempts.clear();return true;
    }
    void save(const std::filesystem::path& directory){
        std::lock_guard lock{m_mutex};
        if(!m_requests.empty()){std::ofstream requests{directory/"sdk_requests.json"};requests<<'[';bool comma=false;for(const auto& r:m_requests){if(comma)requests<<',';comma=true;requests<<"{\"dispatch\":"<<r.dispatch<<",\"eye\":"<<r.eye<<",\"stage\":"<<r.stage<<",\"resource\":"<<r.resource<<'}';}requests<<']';}
        if(!m_attempts.empty()){std::ofstream attempts{directory/"sdk_attempts.json"};attempts<<'[';bool comma=false;for(const auto& a:m_attempts){if(comma)attempts<<',';comma=true;attempts<<"{\"dispatch\":"<<a.dispatch<<",\"eye\":"<<a.eye<<",\"stage\":"<<a.stage<<",\"resource\":"<<a.resource<<",\"format\":"<<a.format<<",\"width\":"<<a.width<<",\"height\":"<<a.height<<",\"mips\":"<<a.mips<<'}';}attempts<<']';}
        for(auto& item:m_items){if(item.saved || item.invalid || !item.submitted || item.fence->GetCompletedValue()!=1)continue;
            void* mapped{};D3D12_RANGE range{0,size_t(item.bytes)};if(FAILED(item.buffer->Map(0,&range,&mapped)))continue;
            const bool half=item.desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT || item.desc.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS;
            GpuTextureReadback::Pixels pixels{uint32_t(item.desc.Width),item.desc.Height,item.desc.Format,{}};pixels.rgb.resize(size_t(pixels.width)*pixels.height*3);
            double sum{};float maximum{};uint64_t clipped{},nonfinite{};
            for(uint32_t y=0;y<pixels.height;++y)for(uint32_t x=0;x<pixels.width;++x)for(size_t c=0;c<3;++c){const auto* p=static_cast<const uint8_t*>(mapped)+item.footprint.Offset+size_t(y)*item.footprint.Footprint.RowPitch+x*(half?8:4);float value;
                if(half){uint16_t v{};std::memcpy(&v,p+c*2,2);nonfinite+=nonfinite_half(v);value=GpuTextureReadback::half(v);}else if(item.desc.Format==DXGI_FORMAT_R11G11B10_FLOAT){uint32_t v{};std::memcpy(&v,p,4);nonfinite+=nonfinite_half(packed_half(v,c));value=packed_hdr(v,c);}else value=p[c]/255.f;
                sum+=value;maximum=std::max(maximum,value);clipped+=value>=1;pixels.rgb[(size_t(y)*pixels.width+x)*3+c]=uint8_t(std::clamp(value,0.f,1.f)*255+.5f);
            }
            D3D12_RANGE empty{};item.buffer->Unmap(0,&empty);
            const char* stages[]{"_input","_output","_tone_output","_post_output","_tone_scene","_tone_bloom","_tone_luminance","_early_scene","_colour_scene","_sharpen_input","_sharpen_output"};
            const auto stem="sdk_"+std::to_string(item.dispatch)+(item.eye?"_right":"_left")+stages[item.stage];
            if(!GpuTextureReadback::save(directory/(stem+".ppm"),pixels))continue;
            std::ofstream out{directory/(stem+".json")};out<<"{\"dispatch\":"<<item.dispatch<<",\"eye\":"<<item.eye<<",\"stage\":"<<item.stage<<",\"format\":"<<item.desc.Format<<",\"state\":"<<item.state<<",\"width\":"<<pixels.width<<",\"height\":"<<pixels.height<<",\"mean\":"<<sum/pixels.rgb.size()<<",\"max\":"<<maximum<<",\"fraction_at_least_one\":"<<double(clipped)/pixels.rgb.size()<<",\"nonfinite_fraction\":"<<double(nonfinite)/pixels.rgb.size()<<",\"gpu_completed\":true}";item.saved=bool(out);
        }
    }
    static bool nonfinite_half(uint16_t value){return (value&0x7c00)==0x7c00;}
    static uint16_t packed_half(uint32_t value,size_t channel){return uint16_t(channel==2?((value>>22)&1023)<<5:((value>>(channel*11))&2047)<<4);}
    static float packed_hdr(uint32_t value,size_t channel){return GpuTextureReadback::half(packed_half(value,channel));}
private:
    std::atomic_bool m_enabled{true};
    struct Request {uint64_t dispatch;uint32_t eye,stage;uintptr_t resource;DWORD thread;};
    struct Attempt {uint64_t dispatch;uint32_t eye,stage,format,width,height,mips;uintptr_t resource;};
    std::vector<Attempt> m_attempts;
    inline static thread_local bool s_copying{};
    std::vector<Request> m_requests;
    mutable std::mutex m_mutex;std::vector<Item> m_items,m_observed;
};
