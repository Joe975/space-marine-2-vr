#include "GpuTestHost.h"
#include "GpuAllocationProbe.h"

namespace {
class Witness final:public IUnknown {
public:
    explicit Witness(bool& destroyed):m_destroyed(destroyed) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {if(!out) return E_POINTER;*out=nullptr;if(iid!=__uuidof(IUnknown)) return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override {return ++m_refs;}
    ULONG STDMETHODCALLTYPE Release() override {const auto n=--m_refs;if(!n) delete this;return n;}
private:
    ~Witness() {m_destroyed=true;}
    std::atomic<ULONG> m_refs{1};bool& m_destroyed;
};
}
int main() {
    try {
        Host host;auto before=host.color(.1f,.2f,.3f);auto& probe=*new GpuAllocationProbe;
        D3D12_INDIRECT_ARGUMENT_DESC indirect_argument{};indirect_argument.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC indirect_desc{};indirect_desc.ByteStride=sizeof(D3D12_DISPATCH_ARGUMENTS);indirect_desc.NumArgumentDescs=1;indirect_desc.pArgumentDescs=&indirect_argument;
        ComPtr<ID3D12CommandSignature> old_signature;check(host.device->CreateCommandSignature(&indirect_desc,nullptr,IID_PPV_ARGS(&old_signature)));
        const auto before_desc=before->GetDesc();const auto before_info=host.device->GetResourceAllocationInfo(0,1,&before_desc);
        D3D12_HEAP_DESC old_heap_desc{};old_heap_desc.SizeInBytes=before_info.SizeInBytes*4;old_heap_desc.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;
        old_heap_desc.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;ComPtr<ID3D12Heap> old_heap;
        check(host.device->CreateHeap(&old_heap_desc,IID_PPV_ARGS(&old_heap)));
        const auto native_validation=host.device->CreatePlacedResource(old_heap.Get(),0,&before_desc,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),nullptr);
        require(probe.install(host.device.Get()),"real resource creation hooks install");
        require(!GpuAllocationProbe::read_signature(old_signature.Get()),"pre-observation signature cannot be inferred from its later use/name/PSO");
        ComPtr<ID3D12CommandSignature> dispatch_signature;check(host.device->CreateCommandSignature(&indirect_desc,nullptr,IID_PPV_ARGS(&dispatch_signature)));
        const auto dispatch_shape=GpuAllocationProbe::read_signature(dispatch_signature.Get());
        require(dispatch_shape && dispatch_shape->shape.action()==GpuIndirectEvidence::dispatch && dispatch_shape->serial && dispatch_shape->thread && dispatch_shape->callsite && !dispatch_shape->root &&
            dispatch_shape->signature==GpuAllocationProbe::identity(dispatch_signature.Get()) && (probe.status().hook_mask&32),"actual signature creation records object-owned action and canonical identity");
        indirect_argument.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;indirect_desc.ByteStride=sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        ComPtr<ID3D12CommandSignature> indexed_signature;check(host.device->CreateCommandSignature(&indirect_desc,nullptr,IID_PPV_ARGS(&indexed_signature)));
        const auto indexed_shape=GpuAllocationProbe::read_signature(indexed_signature.Get());
        require(indexed_shape && indexed_shape->shape.action()==GpuIndirectEvidence::indexed,"real indexed signature is distinguished from compute dispatch");
        for(UINT mutation=0;mutation<7;++mutation) {
            auto corrupt=*dispatch_shape;
            if(mutation==0) corrupt.signature=indexed_shape->signature;
            if(mutation==1) corrupt.device+=1;
            if(mutation==2) corrupt.magic=0;
            if(mutation==3) corrupt.version=2;
            if(mutation==4) corrupt.serial=0;
            if(mutation==5) corrupt.shape.count=33;
            if(mutation==6) corrupt.shape.stride=4;
            check(dispatch_signature->SetPrivateData(GpuAllocationProbe::SIGNATURE_KEY,sizeof(corrupt),&corrupt));
            require(!GpuAllocationProbe::read_signature(dispatch_signature.Get()),"invalid object/owner/shape metadata never certifies an indirect action");
        }
        check(dispatch_signature->SetPrivateData(GpuAllocationProbe::SIGNATURE_KEY,sizeof(*dispatch_shape),&*dispatch_shape));
        require(!GpuAllocationProbe::read(before.Get()),"creation before observation cannot be invented from a later descriptor");
        auto committed=host.color(.2f,.3f,.4f);const auto owned=GpuAllocationProbe::read(committed.Get());
        require(owned && owned->range.kind==GpuAllocationEvidence::committed && owned->range.valid() && owned->api==0 && owned->callsite && owned->thread,
            "actual committed creation receives object-owned descriptor/range/callsite evidence");
        std::cout<<"actual_heap_properties known="<<owned->properties_known<<" type="<<owned->properties.Type<<" cpu="<<owned->properties.CPUPageProperty
            <<" pool="<<owned->properties.MemoryPoolPreference<<" creation="<<owned->properties.CreationNodeMask<<" visible="<<owned->properties.VisibleNodeMask
            <<" creation_flags="<<owned->range.heap_flags<<" actual_flags="<<owned->actual_heap_flags<<std::endl;
        require(owned->properties_known && owned->properties.Type==D3D12_HEAP_TYPE_DEFAULT && owned->access().private_render_target(),
            "actual GetHeapProperties verifies private GPU target access restrictions");
        const auto desc=committed->GetDesc();const auto info=host.device->GetResourceAllocationInfo(0,1,&desc);
        D3D12_HEAP_DESC heap_desc{};heap_desc.SizeInBytes=info.SizeInBytes*4;heap_desc.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;
        heap_desc.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
        ComPtr<ID3D12Heap> heap;check(host.device->CreateHeap(&heap_desc,IID_PPV_ARGS(&heap)));
        const auto place=[&](uint64_t offset) {ComPtr<ID3D12Resource> result;check(host.device->CreatePlacedResource(heap.Get(),offset,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&result)));return result;};
        auto first=place(0),adjacent=place(info.SizeInBytes),overlap=place(0);
        const auto before_validation=probe.status().annotated;
        const auto hooked_validation=host.device->CreatePlacedResource(old_heap.Get(),0,&before_desc,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),nullptr);
        std::cout<<"validation_only_native_hresult="<<std::hex<<uint32_t(native_validation)<<" hooked_hresult="<<uint32_t(hooked_validation)<<std::dec<<'\n';
        require(hooked_validation==native_validation &&
            probe.status().annotated==before_validation,"validation-only creation preserves its real HRESULT and cannot fabricate a resource receipt");
        const auto a=GpuAllocationProbe::read(first.Get()),b=GpuAllocationProbe::read(adjacent.Get()),c=GpuAllocationProbe::read(overlap.Get());
        require(a && b && c && a->api==1 && a->range.heap==b->range.heap && a->range.offset==0 && b->range.offset==info.SizeInBytes &&
            a->range.size==info.SizeInBytes && a->range.heap_size==heap_desc.SizeInBytes && a->range.heap_serial,"actual placed API records real heap origin, offset and allocation size");
        ComPtr<ID3D12Resource> unknown_heap;
        check(host.device->CreatePlacedResource(old_heap.Get(),0,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&unknown_heap)));
        const auto unobserved=GpuAllocationProbe::read(unknown_heap.Get());
        require(unobserved && !unobserved->range.heap_serial && !GpuAllocationEvidence::disjoint(unobserved->range,a->range),"pre-observation heap origin cannot masquerade as private backing");
        require(GpuAllocationEvidence::disjoint(a->range,b->range) && !GpuAllocationEvidence::disjoint(a->range,c->range),"real adjacent/overlapping resources match CPU interval policy");
        require(GpuAllocationEvidence::disjoint(owned->range,a->range),"separate actual committed and placed allocations are disjoint");
        ComPtr<ID3D12Device4> extended;
        if(SUCCEEDED(host.device.As(&extended))) {
            D3D12_HEAP_PROPERTIES properties{};properties.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> resource;
            check(extended->CreateCommittedResource1(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,nullptr,IID_PPV_ARGS(&resource)));
            const auto value=GpuAllocationProbe::read(resource.Get());
            require(value && value->api==2 && value->range.kind==GpuAllocationEvidence::committed && (probe.status().hook_mask&4),"extended committed API is actually observed");
            std::cout<<"extended_committed_runtime_case_exercised=true\n";
            ComPtr<ID3D12Heap> extended_heap;check(extended->CreateHeap1(&heap_desc,nullptr,IID_PPV_ARGS(&extended_heap)));
            ComPtr<ID3D12Resource> extended_placed;check(host.device->CreatePlacedResource(extended_heap.Get(),0,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&extended_placed)));
            const auto extended_range=GpuAllocationProbe::read(extended_placed.Get());
            require(extended_range && extended_range->range.heap_serial && probe.status().heap_calls[1],"extended heap creation establishes actual heap-origin evidence");
        } else std::cout<<"extended_committed_runtime_case_exercised=false (interface unavailable)\n";
        auto corrupt=*a;corrupt.range.resource=b->range.resource;check(first->SetPrivateData(GpuAllocationProbe::KEY,sizeof(corrupt),&corrupt));
        require(!GpuAllocationProbe::read(first.Get()),"another resource's metadata cannot establish ownership");
        corrupt=*a;corrupt.desc.Width+=1;check(first->SetPrivateData(GpuAllocationProbe::KEY,sizeof(corrupt),&corrupt));
        require(!GpuAllocationProbe::read(first.Get()),"descriptor mismatch refuses even with valid address/range");
        for(UINT mutation=0;mutation<4;++mutation) {
            corrupt=*a;
            if(mutation==0) corrupt.magic=0;
            if(mutation==1) corrupt.version=3;
            if(mutation==2) corrupt.api=0;
            if(mutation==3) corrupt.range.device+=1;
            check(first->SetPrivateData(GpuAllocationProbe::KEY,sizeof(corrupt),&corrupt));
            require(!GpuAllocationProbe::read(first.Get()),"malformed or contradictory creation metadata refuses");
        }
        for(UINT mutation=0;mutation<6;++mutation) {
            corrupt=*owned;
            if(mutation==0) corrupt.properties_known=false;
            if(mutation==1) corrupt.properties.Type=D3D12_HEAP_TYPE_UPLOAD;
            if(mutation==2) corrupt.properties.CPUPageProperty=D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
            if(mutation==3) corrupt.properties.MemoryPoolPreference=D3D12_MEMORY_POOL_L0;
            if(mutation==4) corrupt.properties.VisibleNodeMask=2;
            if(mutation==5) corrupt.actual_heap_flags=D3D12_HEAP_FLAG_SHARED;
            check(committed->SetPrivateData(GpuAllocationProbe::KEY,sizeof(corrupt),&corrupt));
            require(!GpuAllocationProbe::read(committed.Get()),"contradictory actual heap properties cannot establish exclusions");
        }
        check(committed->SetPrivateData(GpuAllocationProbe::KEY,sizeof(*owned),&*owned));
        check(first->SetPrivateData(GpuAllocationProbe::KEY,0,nullptr));require(!GpuAllocationProbe::read(first.Get()),"removed metadata cannot leave a stale global-address receipt");
        bool destroyed=false;auto retired=place(info.SizeInBytes*2);auto* witness=new Witness(destroyed);
        constexpr GUID witness_key{0xf2d85c4e,0x94d2,0x492e,{0xaa,0x33,0xb2,0x11,0xc9,0x10,0x07,0xa2}};
        check(retired->SetPrivateDataInterface(witness_key,witness));witness->Release();retired.Reset();
        require(destroyed,"observer metadata does not retain a retired resource or its private-data witness");
        bool signature_destroyed=false;auto* signature_witness=new Witness(signature_destroyed);
        check(dispatch_signature->SetPrivateDataInterface(witness_key,signature_witness));signature_witness->Release();dispatch_signature.Reset();
        require(signature_destroyed,"signature evidence does not add a COM lease or pin a retired signature");
        require(probe.status().signature_calls>=2 && probe.status().signatures_annotated>=2,"actual signature creation observations are counted");
        std::cout<<"signature_runtime_cases_exercised=dispatch,indexed,unknown_origin,metadata_rejection,object_retirement\n";
        heap.Reset();require(GpuAllocationProbe::read(adjacent.Get())->range.heap==b->range.heap,"placed resource itself retains its actual heap after the external heap lease ends");
        const auto status=probe.status();require(status.annotated>=6 && !status.failed,"creation observation exports annotation success/failure counts");
        std::cout<<"allocation_runtime_cases_exercised=committed,placed,adjacent,overlap,object_retirement\nGPU allocation probe tests passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
