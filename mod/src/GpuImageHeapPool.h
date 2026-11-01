#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

// A fresh placed resource for every image, backed by a bounded set of heaps.
// The resource owns a private COM lease: even a consumer holding only a raw
// resource ComPtr prevents that heap from being selected again. Callers must
// retain resources through GPU completion, just as for committed resources.
class GpuImageHeapPool {
    struct Heap {Microsoft::WRL::ComPtr<ID3D12Heap> object;uint64_t size{},alignment{};};
    struct Lease final : IUnknown {
        explicit Lease(std::shared_ptr<Heap> value):heap(std::move(value)){}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override {
            if(!out)return E_POINTER;*out=nullptr;
            if(iid!=__uuidof(IUnknown))return E_NOINTERFACE;
            *out=static_cast<IUnknown*>(this);AddRef();return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
        ULONG STDMETHODCALLTYPE Release()override{const auto remaining=--refs;if(!remaining)delete this;return remaining;}
        std::atomic<ULONG> refs{1};std::shared_ptr<Heap> heap;
    };
    inline static constexpr GUID KEY{0x724eaa04,0xa8e3,0x4729,{0xbb,0x39,0xcf,0x62,0xe1,0xb9,0x8b,0x17}};
public:
    static constexpr size_t CAPACITY=8;
    struct Allocation {Microsoft::WRL::ComPtr<ID3D12Resource> texture;uintptr_t heap{};bool reused{};};
    HRESULT allocate(ID3D12Device* device,const D3D12_RESOURCE_DESC& desc,Allocation& out) {
        out={};std::lock_guard lock{m_mutex};
        if(!device || (m_device && m_device.Get()!=device) || desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
           desc.Flags!=D3D12_RESOURCE_FLAG_NONE || !desc.Width || desc.Width>4096 || !desc.Height || desc.Height>2160 ||
           desc.MipLevels!=1 || desc.DepthOrArraySize!=1 || desc.SampleDesc.Count!=1 || desc.SampleDesc.Quality)return E_INVALIDARG;
        const auto info=device->GetResourceAllocationInfo(0,1,&desc);
        if(!info.SizeInBytes || info.SizeInBytes==UINT64_MAX || !info.Alignment)return E_INVALIDARG;
        std::shared_ptr<Heap> slot;std::shared_ptr<Heap>* replacement{};bool reused{};
        for(auto& entry:m_heaps)if(entry.use_count()==1) {
            if(entry->size>=info.SizeInBytes && entry->alignment>=info.Alignment){slot=entry;reused=true;break;}
            if(!replacement)replacement=&entry;
        }
        if(!slot) {
            // No unbounded heap growth when consumers keep older images. The
            // caller may fall back to a distinct committed image for this copy.
            if(!replacement && m_heaps.size()==CAPACITY)return E_PENDING;
            slot=std::make_shared<Heap>();D3D12_HEAP_DESC heap{};
            heap.SizeInBytes=info.SizeInBytes;heap.Alignment=info.Alignment;
            heap.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;heap.Properties.CreationNodeMask=heap.Properties.VisibleNodeMask=1;
            heap.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
            const auto hr=device->CreateHeap(&heap,IID_PPV_ARGS(&slot->object));if(FAILED(hr))return hr;
            slot->size=heap.SizeInBytes;slot->alignment=heap.Alignment;
            if(replacement)*replacement=slot;else m_heaps.push_back(slot);m_device=device;
        }
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        auto hr=device->CreatePlacedResource(slot->object.Get(),0,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&resource));
        if(FAILED(hr))return hr;
        Microsoft::WRL::ComPtr<IUnknown> lease;lease.Attach(new Lease(slot));
        hr=resource->SetPrivateDataInterface(KEY,lease.Get());if(FAILED(hr))return hr;
        out={std::move(resource),reinterpret_cast<uintptr_t>(slot->object.Get()),reused};return S_OK;
    }
    void reset(){std::lock_guard lock{m_mutex};m_heaps.clear();m_device.Reset();}
private:
    std::mutex m_mutex;std::vector<std::shared_ptr<Heap>> m_heaps;Microsoft::WRL::ComPtr<ID3D12Device> m_device;
};
