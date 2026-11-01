#include "GpuTestHost.h"
#include "GpuSignatureProbe.h"
#include "GpuAllocationProbe.h"
#include "GpuQueueProbe.h"
#include "RenderTargetProbe.h"
#include "GpuClosedListProbe.h"
#include <MinHook.h>

namespace {
class Witness final:public IUnknown {
public:
    explicit Witness(bool& destroyed):m_destroyed(destroyed) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {if(!out) return E_POINTER;*out=nullptr;if(iid!=__uuidof(IUnknown)) return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override {return ++m_refs;}
    ULONG STDMETHODCALLTYPE Release() override {const auto n=--m_refs;if(!n) delete this;return n;}
private:
    ~Witness() {m_destroyed=true;}std::atomic<ULONG> m_refs{1};bool& m_destroyed;
};
}
int main() {
    try {
        for(auto pip:{"","0","1","2","true"}) for(auto vr:{"","0","1","invalid"}) for(auto xr:{"","0","1"}) for(auto synthetic:{"","0","1"})
            require(GpuSignatureProbe::requested(pip,vr,xr,synthetic)==(std::string_view(pip)=="1" && (std::string_view(vr).empty() || std::string_view(vr)=="0") &&
                (std::string_view(xr).empty() || std::string_view(xr)=="0") && (std::string_view(synthetic).empty() || std::string_view(synthetic)=="0")),
                "early observation is gated by explicit desktop PIP and every VR/synthetic mode off");
        require(GpuSignatureProbe::requested("0","0","0","0","1") && !GpuSignatureProbe::requested("0","1","0","0","1") &&
                !GpuSignatureProbe::requested("0","0","0","0","true"),"ordinary desktop observation requests early metadata without enabling a runtime");
        for(auto pip:{"","0","1","invalid"}) for(auto vr:{"","0","1","invalid"}) for(auto xr:{"","0","1"})
            for(auto synthetic:{"","0","1"}) for(auto ordinary:{"","0","1"}) for(auto headset:{"1","invalid"}) {
                const auto off=[](std::string_view v){return v.empty() || v=="0";};
                const bool single_runtime=(std::string_view(vr)=="1" && off(xr)) || (std::string_view(xr)=="1" && off(vr));
                const bool expected=off(pip) && single_runtime && off(synthetic) && std::string_view(ordinary)=="1" && std::string_view(headset)=="1";
                require(GpuSignatureProbe::requested(pip,vr,xr,synthetic,ordinary,headset)==expected,
                    "single-frame headset observes creation early with exactly one runtime and no conflicting modes");
            }
        auto& early=GpuSignatureProbe::get();require(!early.status().early_installed && !early.status().implementations,"early fixture starts before device/signature observation");
        const auto native_validation=D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,__uuidof(ID3D12Device),nullptr);
        require(early.install_device_creation() && early.install_device_creation(),"actual device export hook installs idempotently before the real device");
        const auto hooked_validation=D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,__uuidof(ID3D12Device),nullptr);
        require(hooked_validation==native_validation && early.status().device_calls==1 && !early.status().implementations,"validation-only device creation preserves native HRESULT and cannot fabricate a device lease");
        Host host;const auto ready=early.status();
        require(ready.device_calls>=2 && ready.implementations,"actual WARP device return installs signature observation before its creator can use it");
        const auto main_queue=GpuQueueProbe::read(host.queue.Get());const auto device_queues=GpuQueueProbe::read_device(host.device.Get());
        std::cout<<"queue_fixture: known="<<bool(main_queue)<<" device_known="<<bool(device_queues)<<" early="<<(device_queues?device_queues->early_return:false)
            <<" hooks="<<(device_queues?device_queues->hooks:0)<<" calls="<<(device_queues?device_queues->calls[0]:0)<<'/'<<(device_queues?device_queues->calls[1]:0)
            <<" annotated="<<(device_queues?device_queues->annotated:0)<<" failed="<<(device_queues?device_queues->failed:0)<<" actual_node="<<host.queue->GetDesc().NodeMask<<'\n';
        require(main_queue && device_queues && device_queues->early_return && main_queue->device_serial==device_queues->serial && main_queue->api==1,
                "actual queue creation is observed before framework startup and belongs to its actual device");
        require(main_queue->requested_node==0 && main_queue->desc.NodeMask==host.queue->GetDesc().NodeMask,
                "requested default node and actual normalized native node mask are retained separately");
        for(auto type:{D3D12_COMMAND_LIST_TYPE_COMPUTE,D3D12_COMMAND_LIST_TYPE_COPY}) {
            D3D12_COMMAND_QUEUE_DESC queue_desc{};queue_desc.Type=type;ComPtr<ID3D12CommandQueue> queue;
            check(host.device->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&queue)));const auto receipt=GpuQueueProbe::read(queue.Get());
            require(receipt && receipt->desc.Type==type && receipt->identity!=main_queue->identity && receipt->serial!=main_queue->serial,
                    "compute and copy queue instances retain exact native type/identity/creation receipts");
            check(queue->SetPrivateData(GpuQueueProbe::QUEUE_KEY,sizeof(*main_queue),&*main_queue));
            require(!GpuQueueProbe::read(queue.Get()),"copied queue metadata cannot adopt another actual queue");
            bool queue_destroyed=false;auto* queue_witness=new Witness(queue_destroyed);
            check(queue->SetPrivateDataInterface(GpuQueueProbe::DEVICE_KEY,queue_witness));queue_witness->Release();queue.Reset();
            require(queue_destroyed,"queue creation observer retains no native queue lease");
        }
        ComPtr<ID3D12Device9> device9;
        if(SUCCEEDED(host.device.As(&device9))) {
            constexpr GUID creator{0x34b8e3b5,0x2b52,0x444d,{0x8b,0x19,0x61,0x98,0x71,0x6c,0x20,0x47}};
            D3D12_COMMAND_QUEUE_DESC queue_desc{};ComPtr<ID3D12CommandQueue> queue;
            check(device9->CreateCommandQueue1(&queue_desc,creator,IID_PPV_ARGS(&queue)));const auto receipt=GpuQueueProbe::read(queue.Get());
            require(receipt && receipt->api==2 && receipt->creator==creator,"actual Device9 queue creator ID and extended creation are preserved");
        }
        auto queue_device=*GpuQueueProbe::read_device(host.device.Get());
        require(queue_device.annotated>=3 && !queue_device.failed && queue_device.hooks==(queue_device.device9?3:1),"actual supported queue creators are hooked without observation errors");
        GpuQueueProbe::Device policy=queue_device;policy.method_count=1;policy.execute_methods[0]=123;
        require(GpuQueueProbe::all_observed_methods_match(policy,123),"matching actual observed queue implementation coverage");
        policy.execute_methods[0]=124;require(!GpuQueueProbe::all_observed_methods_match(policy,123),"different queue implementation cannot claim execution coverage");
        policy.execute_methods[0]=123;policy.early_return=false;require(!GpuQueueProbe::all_observed_methods_match(policy,123),"late-only device adoption cannot claim early queue coverage");
        D3D12_INDIRECT_ARGUMENT_DESC argument{};argument.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC desc{};desc.ByteStride=sizeof(D3D12_DISPATCH_ARGUMENTS);desc.NumArgumentDescs=1;desc.pArgumentDescs=&argument;
        ComPtr<ID3D12CommandSignature> signature;check(host.device->CreateCommandSignature(&desc,nullptr,IID_PPV_ARGS(&signature)));
        const auto before_late=GpuSignatureProbe::read(signature.Get());
        require(before_late && before_late->shape.action()==GpuIndirectEvidence::dispatch && before_late->device==GpuAllocationProbe::identity(host.device.Get()) &&
            before_late->signature==GpuAllocationProbe::identity(signature.Get()) && before_late->serial && before_late->thread && before_late->callsite,
            "signature inputs and canonical device/object identity are observed before late framework setup");
        auto resource=host.color(.1f,.2f,.3f);require(!GpuAllocationProbe::read(resource.Get()),"early signature observation does not invent resource allocation evidence");
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};rtv_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rtv_desc.NumDescriptors=1;ComPtr<ID3D12DescriptorHeap> rtv_heap;
        check(host.device->CreateDescriptorHeap(&rtv_desc,IID_PPV_ARGS(&rtv_heap)));const auto rtv_handle=rtv_heap->GetCPUDescriptorHandleForHeapStart();
        host.device->CreateRenderTargetView(resource.Get(),nullptr,rtv_handle);
        require(RenderTargetProbe::get().status().early_creation && RenderTargetProbe::get().canonical_resource(rtv_handle.ptr)==GpuObjectIdentity::get(resource.Get()),
            "actual device-return hook preserves RTV creation before framework startup");
        ComPtr<ID3D12CommandAllocator> copy_allocator;ComPtr<ID3D12GraphicsCommandList> copy_commands,other_commands;
        check(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,IID_PPV_ARGS(&copy_allocator)));
        check(host.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_COPY,copy_allocator.Get(),nullptr,IID_PPV_ARGS(&copy_commands)));
        check(copy_commands->Close());const auto closed=GpuClosedListProbe::read(copy_commands.Get());
        const auto born=GpuClosedListProbe::birth(copy_commands.Get());require(closed && born && born->api==1 && !born->initial_closed && born->type==D3D12_COMMAND_LIST_TYPE_COPY && born->identity==closed->identity,"actual base command creation has canonical object-owned initial-recording provenance");
        ComPtr<ID3D12Device4> command_device4;
        if(SUCCEEDED(host.device.As(&command_device4))) {
            ComPtr<ID3D12GraphicsCommandList> closed_at_creation;check(command_device4->CreateCommandList1(0,D3D12_COMMAND_LIST_TYPE_DIRECT,D3D12_COMMAND_LIST_FLAG_NONE,IID_PPV_ARGS(&closed_at_creation)));
            const auto extended_birth=GpuClosedListProbe::birth(closed_at_creation.Get());require(extended_birth && extended_birth->api==2 && extended_birth->initial_closed,"actual Device4 initial-closed contract is distinct from base initial recording");
            check(closed_at_creation->SetPrivateData(GpuClosedListProbe::BIRTH_KEY,sizeof(*born),&*born));require(!GpuClosedListProbe::birth(closed_at_creation.Get()),"copied birth receipt cannot adopt another native command object");
        }
        require(closed && closed->type==D3D12_COMMAND_LIST_TYPE_COPY && !GpuClosedListProbe::get().errors(),"actual pre-framework Close has an object-owned copy-list receipt");
        auto& late=*new GpuAllocationProbe;require(late.install(host.device.Get()),"late resource observer adopts existing signature hook without collision");
        auto later_resource=host.color(.3f,.2f,.1f);const auto created=GpuAllocationProbe::read(later_resource.Get());
        require(created && GpuClosedListProbe::precedes_copy(*closed,created->creation_begin_ns,created->range.device,D3D12_COMMAND_LIST_TYPE_COPY),"actual copy-list Close predates the beginning of actual target creation");
        for(UINT mutation=0;mutation<8;++mutation) {
            auto altered=*closed;auto creation=created->creation_begin_ns;auto owner=created->range.device;auto queue_type=D3D12_COMMAND_LIST_TYPE_COPY;
            if(mutation==0) altered.serial=0;if(mutation==1) altered.identity=0;if(mutation==2) altered.closed_ns=0;
            if(mutation==3) creation=altered.closed_ns;if(mutation==4) creation=altered.closed_ns-1;
            if(mutation==5) ++owner;if(mutation==6) altered.type=D3D12_COMMAND_LIST_TYPE_DIRECT;if(mutation==7) queue_type=D3D12_COMMAND_LIST_TYPE_DIRECT;
            require(!GpuClosedListProbe::precedes_copy(altered,creation,owner,queue_type),"missing, equal, later, foreign-device and non-copy evidence cannot exclude a list");
        }
        check(host.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_COPY,copy_allocator.Get(),nullptr,IID_PPV_ARGS(&other_commands)));
        check(other_commands->Close());check(other_commands->SetPrivateData(GpuClosedListProbe::KEY,sizeof(*closed),&*closed));
        require(!GpuClosedListProbe::read(other_commands.Get()),"another command object's Close receipt cannot establish identity");
        GpuClosedListProbe::get().invalidate(copy_commands.Get());require(!GpuClosedListProbe::read(copy_commands.Get()),"invalidation removes the actual Close receipt");
        const auto adopted=GpuAllocationProbe::read_signature(signature.Get());
        require(adopted && adopted->serial==before_late->serial && adopted->signature==before_late->signature && adopted->shape.action()==before_late->shape.action() &&
            early.status().implementations==ready.implementations,"late adoption preserves the original pre-framework creation receipt and deduplicates actual implementations");
        require(early.observe_device(host.device.Get()) && early.status().implementations==ready.implementations,"repeated actual device observation cannot double-hook its method");
        auto* entry=(*reinterpret_cast<void***>(host.device.Get()))[41];require(MH_DisableHook(entry)==MH_OK,"fixture temporarily removes only its own signature hook for native HRESULT oracle");
        const auto native_signature_validation=host.device->CreateCommandSignature(&desc,nullptr,__uuidof(ID3D12CommandSignature),nullptr);
        require(MH_EnableHook(entry)==MH_OK,"fixture restores signature observation before continuing");const auto before_count=early.status().signature_calls;
        const auto hooked_signature_validation=host.device->CreateCommandSignature(&desc,nullptr,__uuidof(ID3D12CommandSignature),nullptr);
        require(hooked_signature_validation==native_signature_validation && early.status().signature_calls==before_count,"validation-only signature call preserves native HRESULT and produces no metadata");
        ComPtr<ID3D12Device4> extended;
        if(SUCCEEDED(host.device.As(&extended))) {
            argument.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;desc.ByteStride=sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);ComPtr<ID3D12CommandSignature> indexed;
            check(extended->CreateCommandSignature(&desc,nullptr,IID_PPV_ARGS(&indexed)));const auto value=GpuSignatureProbe::read(indexed.Get());
            require(value && value->shape.action()==GpuIndirectEvidence::indexed && value->device==before_late->device,"actual extended device interface preserves inherited signature creation observation");
        }
        bool destroyed=false;auto* witness=new Witness(destroyed);constexpr GUID key{0xe57c1add,0xc6d4,0x4d21,{0xba,0xf2,0xd5,0xee,0x12,0x6f,0xa1,0x91}};
        check(signature->SetPrivateDataInterface(key,witness));witness->Release();signature.Reset();require(destroyed,"early observer retains no command-signature COM lease");
        const auto status=early.status();require(status.signature_calls>=2 && status.annotated>=2 && !status.failed,"early observer reports successful native creation without failures");
        auto& queues=GpuQueueProbe::get();require(!queues.errors(),"queue observation starts without sticky failures");
        check(host.device->SetPrivateData(GpuQueueProbe::DEVICE_KEY,0,nullptr));
        D3D12_COMMAND_QUEUE_DESC missing_owner_desc{};ComPtr<ID3D12CommandQueue> missing_owner_queue;
        check(host.device->CreateCommandQueue(&missing_owner_desc,IID_PPV_ARGS(&missing_owner_queue)));
        require(queues.errors()>0 && !GpuQueueProbe::read(missing_owner_queue.Get()),"a successful native creation with lost device provenance records a sticky observer failure");
        policy=queue_device;policy.method_count=1;policy.execute_methods[0]=123;
        require(!GpuQueueProbe::all_observed_methods_match(policy,123),"cached successful device metadata cannot hide a later queue observer failure");
        std::cout<<"early_signature_runtime_cases_exercised=device_return,pre_framework_creation,late_adoption,validation_hresult,interface_inheritance,retirement; implementations="<<status.implementations<<'\n';
        std::cout<<"GPU early signature probe tests passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
