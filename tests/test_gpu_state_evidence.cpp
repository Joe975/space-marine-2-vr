#define CINTERFACE
#include "GpuStateEvidence.h"
#include "GpuAllocationEvidence.h"
#include "GpuTargetAccess.h"
#include "GpuIndirectEvidence.h"
#include "GpuCommandCoverage.h"
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <utility>

// Verify the slot against the installed SDK's generated C COM ABI, not memory.
static_assert(offsetof(ID3D12GraphicsCommandList7Vtbl,Barrier)==80*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,Dispatch)==14*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,ExecuteIndirect)==59*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,ExecuteBundle)==27*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,ClearUnorderedAccessViewUint)==49*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,ClearUnorderedAccessViewFloat)==50*sizeof(void*));
static_assert(offsetof(ID3D12DeviceVtbl,CreateCommittedResource)==27*sizeof(void*));
static_assert(offsetof(ID3D12DeviceVtbl,CreatePlacedResource)==29*sizeof(void*));
static_assert(offsetof(ID3D12Device4Vtbl,CreateCommittedResource1)==53*sizeof(void*));
static_assert(offsetof(ID3D12DeviceVtbl,CreateHeap)==28*sizeof(void*));
static_assert(offsetof(ID3D12Device4Vtbl,CreateHeap1)==54*sizeof(void*));
static_assert(offsetof(ID3D12DeviceVtbl,CreateCommandSignature)==41*sizeof(void*));
static_assert(offsetof(ID3D12DeviceVtbl,CreateCommandQueue)==8*sizeof(void*));
static_assert(offsetof(ID3D12Device9Vtbl,CreateCommandQueue1)==75*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,ClearState)==11*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandList4Vtbl,BeginRenderPass)==68*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandList4Vtbl,EndRenderPass)==69*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,DiscardResource)==51*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,ClearDepthStencilView)==47*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,CopyTiles)==18*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandList1Vtbl,ResolveSubresourceRegion)==64*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandList4Vtbl,InitializeMetaCommand)==70*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandList4Vtbl,ExecuteMetaCommand)==71*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandList4Vtbl,DispatchRays)==76*sizeof(void*));
static_assert(offsetof(ID3D12GraphicsCommandList6Vtbl,DispatchMesh)==79*sizeof(void*));

namespace {
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
ID3D12Resource* resource(uintptr_t id) {return reinterpret_cast<ID3D12Resource*>(id);}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* target) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={target,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET};return b;
}
}
int main() {
    try {
        std::array<void*,60> methods{};GpuCommandCoverage::Methods installed{};
        for(size_t i=0;i<installed.size();++i) methods[GpuCommandCoverage::slots[i]]=installed[i]=reinterpret_cast<void*>(uintptr_t(i+1));
        require(GpuCommandCoverage::matches(installed,methods.data()),"all actual base implementations match");
        for(size_t i=0;i<installed.size();++i) {
            auto changed=methods;changed[GpuCommandCoverage::slots[i]]=reinterpret_cast<void*>(uintptr_t(999));
            require(!GpuCommandCoverage::matches(installed,changed.data()),"any different lifecycle/writer/barrier implementation refuses coverage");
            auto absent=installed;absent[i]=nullptr;
            require(!GpuCommandCoverage::matches(absent,methods.data()),"missing implementation cannot claim coverage");
        }
        require(!GpuCommandCoverage::matches(installed,nullptr),"missing native vtable refuses coverage");
        using State=GpuStateEvidence;const auto target=resource(0x1000);auto full=transition(target);
        using Indirect=GpuIndirectEvidence;Indirect indirect;indirect.count=1;indirect.stride=sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        indirect.arguments[0].Type=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;require(indirect.action()==Indirect::indexed,"actual indexed action type classifies independently of PSO/bindings");
        for(auto pair:{std::pair{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW,Indirect::draw},std::pair{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH,Indirect::dispatch},
                       std::pair{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS,Indirect::rays},std::pair{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH,Indirect::mesh}}) {
            indirect.arguments[0].Type=pair.first;indirect.stride=256;require(indirect.action()==pair.second,"each supported actual action is classified");
        }
        const auto valid_indirect=indirect;
        for(UINT n:{0u,33u}) {indirect.count=n;require(indirect.action()==Indirect::unknown,"missing/excessive signature arguments refuse");}indirect=valid_indirect;
        indirect.stride=0;require(indirect.action()==Indirect::unknown,"zero signature stride refuses");indirect.stride=13;
        require(indirect.action()==Indirect::unknown,"unaligned stride refuses");indirect.stride=4;
        require(indirect.action()==Indirect::unknown,"truncated action bytes refuse");indirect=valid_indirect;
        indirect.count=2;indirect.arguments[0].Type=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;indirect.arguments[1].Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        require(indirect.action()==Indirect::unknown,"multiple actions cannot masquerade as a single dispatch");
        indirect.arguments[0].Type=D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;indirect.arguments[0].Constant.Num32BitValuesToSet=2;
        indirect.stride=20;require(indirect.action()==Indirect::dispatch,"root updates followed by one action preserve action classification");
        indirect.stride=16;require(indirect.action()==Indirect::unknown,"root-update bytes count toward required stride");
        indirect.arguments[0].Constant.Num32BitValuesToSet=UINT32_MAX;require(indirect.action()==Indirect::unknown,"root argument size overflow cannot produce a small valid stride");
        using Allocation=GpuAllocationEvidence;
        Allocation a{Allocation::placed,0,1,10,20,30,0,65536,262144,65536,1},b=a;
        require(a.valid(),"observed aligned bounded placement is valid");
        b.serial=2;b.resource=21;b.offset=65536;
        require(Allocation::disjoint(a,b) && Allocation::disjoint(b,a),"adjacent allocations are physically disjoint");
        b.offset=0;require(!Allocation::disjoint(a,b),"different resource pointers at overlapping placement are not disjoint");
        b.heap=31;require(Allocation::disjoint(a,b),"different known private heaps are disjoint");
        b.heap_serial=0;require(!Allocation::disjoint(a,b),"separate heap identity with unobserved origin cannot exclude externally opened backing");b.heap_serial=2;
        b.heap_flags=1;require(!Allocation::disjoint(a,b),"shared heaps may have separately opened identities");b.heap_flags=32;
        require(!Allocation::disjoint(a,b),"cross-adapter shared heaps also refuse identity-only disjointness");b=a;b.resource=21;
        for(uint32_t kind:{0u,3u}) {b.kind=static_cast<Allocation::Kind>(kind);require(!b.valid() && !Allocation::disjoint(a,b),"unknown allocation kinds cannot prove exclusion");}b=a;
        b.offset=UINT64_MAX-65535;require(!b.valid(),"placement end overflow refuses");b=a;b.offset=1;require(!b.valid(),"misaligned placement refuses");
        b=a;b.size=0;require(!b.valid(),"empty size refuses");b.size=UINT64_MAX;require(!b.valid(),"allocation failure sentinel refuses");
        b=a;b.alignment=3;require(!b.valid(),"invalid alignment refuses");b=a;b.heap_size=1;require(!b.valid(),"out-of-heap placement refuses");
        b=a;b.serial=0;require(!b.valid(),"missing observed creation identity refuses");b=a;b.resource=0;require(!b.valid(),"missing object identity refuses");
        b=a;b.device=11;require(!Allocation::disjoint(a,b),"different devices are not evidence of independent backing");
        b=a;b.kind=Allocation::committed;b.heap=0;b.heap_serial=0;b.heap_size=b.size;b.resource=21;
        require(b.valid() && Allocation::disjoint(a,b),"observed private committed allocation is distinct");
        b.resource=a.resource;require(!Allocation::disjoint(a,b),"same object cannot be relabelled as disjoint");
        GpuTargetAccess access;access.allocation=b;access.allocation.resource=21;access.properties_known=true;
        access.properties.Type=D3D12_HEAP_TYPE_DEFAULT;access.properties.CreationNodeMask=access.properties.VisibleNodeMask=1;
        access.desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;access.desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        access.desc.Width=1280;access.desc.Height=720;access.desc.MipLevels=access.desc.DepthOrArraySize=access.desc.SampleDesc.Count=1;
        require(access.private_render_target() && access.exclude_compute() && access.exclude_alias(0,0,true) && access.exclude_alias(90,91,true),
            "actual private committed GPU target excludes unrelated/wildcard aliasing and compute access");
        require(!access.exclude_alias(21,0,true) && !access.exclude_alias(0,21,true) && !access.exclude_alias(0,0,false),
            "target canonical names and unresolved identities never inherit exclusions");
        access.actual_heap_flags=0x84;require(access.private_render_target(),"runtime-added restrictive implicit heap flags are compatible with the observed RT creation");
        for(bool known:{false,true}) for(bool bound:{false,true}) for(bool covered:{false,true})
            require(access.exclude_indirect(known,bound,covered)==(known && !bound && covered),"indirect exclusion requires complete observed off-target RTVs and matching actual implementation");
        for(bool known:{false,true}) for(bool bound:{false,true}) for(bool covered:{false,true}) for(UINT maximum:{0u,1u}) {
            require(access.indirect(Indirect::unknown,known,bound,covered,maximum)==GpuTargetAccess::refuse,"unknown signature is never inferred from current bindings");
            require(access.indirect(Indirect::dispatch,known,bound,covered,maximum)==GpuTargetAccess::compute_excluded &&
                access.indirect(Indirect::rays,known,bound,covered,maximum)==GpuTargetAccess::compute_excluded,"observed indirect compute cannot access private non-UAV backing");
            const auto expected=!known || !covered?GpuTargetAccess::refuse:(!bound || !maximum?GpuTargetAccess::raster_excluded:GpuTargetAccess::raster_possible);
            for(auto action:{Indirect::draw,Indirect::indexed,Indirect::mesh}) require(access.indirect(action,known,bound,covered,maximum)==expected,
                "indirect raster decisions preserve unknown bindings/coverage and distinguish possible target writes from exclusions");
        }
        for(auto kind:{GpuTargetAccess::discard,GpuTargetAccess::resolve_region,GpuTargetAccess::copy_tiles}) {
            require(access.exclude_extended(kind,99,false,false,false) && !access.exclude_extended(kind,21,true,false,true) && !access.exclude_extended(kind,0,true,false,true),
                "explicit extended operations exclude only another canonical resource with private backing proof");
        }
        require(access.exclude_extended(GpuTargetAccess::depth,0,false,true,false) && access.exclude_extended(GpuTargetAccess::dispatch_rays,0,false,true,false),
            "non-DS/non-UAV private render target cannot receive depth clears or ray shader writes");
        for(bool known:{false,true}) for(bool bound:{false,true}) for(bool covered:{false,true})
            require(access.exclude_extended(GpuTargetAccess::dispatch_mesh,0,known,bound,covered)==(known && !bound && covered),"mesh dispatch requires complete off-target raster bindings");
        for(auto kind:{GpuTargetAccess::initialize_meta,GpuTargetAccess::execute_meta,GpuTargetAccess::extended_count})
            require(!access.exclude_extended(kind,99,true,false,true),"opaque and unknown extended operations always refuse");
        for(unsigned mutation=0;mutation<19;++mutation) {
            auto unsafe=access;
            switch(mutation) {
                case 0:unsafe.properties_known=false;break;
                case 1:unsafe.allocation.kind=Allocation::unknown;break;
                case 2:unsafe.allocation.serial=0;break;
                case 3:unsafe.allocation.heap_flags=1;break;
                case 4:unsafe.actual_heap_flags=32;break;
                case 5:unsafe.properties.Type=D3D12_HEAP_TYPE_CUSTOM;break;
                case 6:unsafe.properties.Type=D3D12_HEAP_TYPE_UPLOAD;break;
                case 7:unsafe.properties.CPUPageProperty=D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;break;
                case 8:unsafe.properties.MemoryPoolPreference=D3D12_MEMORY_POOL_L0;break;
                case 9:unsafe.properties.CreationNodeMask=0;break;
                case 10:unsafe.properties.VisibleNodeMask=2;break;
                case 11:unsafe.desc.Flags|=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;break;
                case 12:unsafe.desc.Flags|=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;break;
                case 13:unsafe.desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;break;
                case 14:unsafe.desc.MipLevels=2;break;
                case 15:unsafe.desc.DepthOrArraySize=2;break;
                case 16:unsafe.desc.SampleDesc.Count=2;break;
                case 17:unsafe.desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;break;
                case 18:unsafe.desc.SampleDesc.Quality=1;break;
            }
            require(!unsafe.private_render_target() && !unsafe.exclude_compute() && !unsafe.exclude_alias(0,0,true),
                "missing/contradictory/CPU/shared/UAV/non-singleton facts cannot exclude potential access");
            require(!unsafe.exclude_indirect(true,false,true),"RTV exclusion alone cannot exclude shader writes to unknown/shared/UAV backing");
            require(unsafe.indirect(Indirect::dispatch,true,false,true,1)==GpuTargetAccess::refuse &&
                unsafe.indirect(Indirect::draw,true,true,true,1)==GpuTargetAccess::refuse,"all indirect decisions require actual private target proof");
            for(uint32_t kind=0;kind<GpuTargetAccess::extended_count;++kind)
                require(!unsafe.exclude_extended(static_cast<GpuTargetAccess::Extended>(kind),99,true,false,true),"all extended exclusions require complete actual private target proof");
        }
        bool forwarding=false;
        try {GpuWriterForwardScope outer{forwarding};require(forwarding,"outer forwarding scope is observable");
            {GpuWriterForwardScope inner{forwarding};require(forwarding,"nested native forwarding remains classified as nested");}
            require(forwarding,"nested return cannot prematurely end outer forwarding");throw 1;
        } catch(int) {}
        require(!forwarding,"exception unwinding restores writer classification for the next real call");
        State state;state.raster_write(false);require(!state.seen,"multiple subresources cannot inherit raster state");
        state.raster_write(true);require(state.seen && state.raster && !state.unknown && state.end==D3D12_RESOURCE_STATE_RENDER_TARGET,"actual single-subresource raster write establishes RT");
        auto split=full;split.Flags=D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
        auto counts=State::legacy(&state,target,true,1,&split);state.raster_write(true);
        require(counts.transitions==1 && state.unknown && !state.raster,"draw cannot repair a split transition");
        State::legacy(&state,target,true,1,&full);require(!state.unknown && state.legacy_seen,"full transition still repairs ordinary split uncertainty");
        auto alternate=full;alternate.Transition.pResource=resource(0x7000);state={};
        counts=State::legacy(&state,target,true,1,&alternate);require(!counts.transitions,"unverified alternate pointer is not presumed to name the target");
        counts=State::legacy(&state,target,true,1,&alternate,{},[](ID3D12Resource* value){return value==resource(0x7000);});
        require(counts.transitions==1 && state.legacy_seen && !state.unknown,"caller-proved canonical identity matches an alternate native resource interface");
        alternate.Transition.pResource=nullptr;state={};counts=State::legacy(&state,target,true,1,&alternate);
        require(counts.malformed==1 && state.hazards==State::malformed,"missing transition resource cannot silently become an unrelated transition");
        auto sub=full;sub.Transition.Subresource=0;state={};State::legacy(&state,target,false,1,&sub);
        require(state.unknown,"partial transition cannot establish all-subresource state");
        State::legacy(&state,target,true,1,&sub);require(!state.unknown,"subresource zero covers the verified singleton");
        for(const auto before:{target,resource(0x2000),static_cast<ID3D12Resource*>(nullptr)})
            for(const auto after:{target,resource(0x3000),static_cast<ID3D12Resource*>(nullptr)}) {
                auto alias=full;alias.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;alias.Aliasing={before,after};
                state={};state.raster_write(true);counts=State::legacy(&state,target,true,1,&alias);
                State::legacy(&state,target,true,1,&full);state.raster_write(true);
                require(counts.aliases==1 && state.unknown && state.hazards==State::aliasing && !state.raster,
                    "all named/wildcard aliases remain unsafe without heap-overlap proof; later draws/transitions cannot erase them");
            }
        auto uav=full;uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        for(const auto named:{target,static_cast<ID3D12Resource*>(nullptr)}) {
            state={};uav.UAV.pResource=named;counts=State::legacy(&state,target,true,1,&uav);
            require(counts.uavs==1 && state.unknown && state.hazards==State::uav,"target/global UAV ordering cannot certify an unobserved writer");
        }
        state={};uav.UAV.pResource=resource(0x2000);counts=State::legacy(&state,target,true,1,&uav);
        require(!counts.uavs && !state.seen,"a named other UAV is not target state evidence");
        for(const auto type:{D3D12_BARRIER_TYPE_GLOBAL,D3D12_BARRIER_TYPE_TEXTURE,D3D12_BARRIER_TYPE_BUFFER}) {
            D3D12_BARRIER_GROUP group{};group.Type=type;state={};state.raster_write(true);
            State::enhanced_barriers(&state,1,&group);State::legacy(&state,target,true,1,&full);state.raster_write(true);
            require(state.unknown && state.hazards==State::enhanced && !state.raster,"enhanced layout/access uncertainty is sticky regardless of group kind");
        }
        state={};State::enhanced_barriers(&state,0,nullptr);State::legacy(&state,target,true,0,nullptr);
        require(!state.seen,"empty barrier calls do not taint a recording");
        for(const auto count:{1u,4097u}) {
            state={};counts=State::legacy(&state,target,true,count,nullptr);require(counts.malformed==1 && state.hazards==State::malformed,"missing/oversized legacy descriptions refuse before dereferencing");
            state={};State::enhanced_barriers(&state,count,nullptr);require(state.hazards==(State::enhanced|State::malformed),"missing/oversized enhanced descriptions remain unsupported");
        }
        state={};auto invalid=full;invalid.Type=static_cast<D3D12_RESOURCE_BARRIER_TYPE>(99);State::legacy(&state,target,true,1,&invalid);
        require(state.hazards==State::malformed,"unknown legacy descriptor type refuses");
        auto alias=full;alias.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;alias.Aliasing={nullptr,nullptr};
        counts=State::legacy(nullptr,target,true,1,&alias);require(counts.aliases==1,"untracked hazards remain countable without a recording");
        state={};counts=State::legacy(&state,target,true,1,&alias,[](const auto&){return true;});state.raster_write(true);
        require(counts.aliases==1 && counts.excluded_aliases==1 && !state.hazards && state.raster,"proven exclusions conserve observed barriers without poisoning raster state");
        alias.Flags=D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;state={};counts=State::legacy(&state,target,true,1,&alias,[](const auto&){return true;});
        require(!counts.excluded_aliases && state.hazards==State::aliasing,"malformed alias flags cannot inherit even an affirmative exclusion");alias.Flags=D3D12_RESOURCE_BARRIER_FLAG_NONE;
        state={};state.raster_write(true);require(!state.unknown && !state.hazards,"Reset starts a new recording; old hazard flags cannot leak or masquerade as new evidence");
        for(bool known:{false,true}) for(bool supported:{false,true}) for(bool hooked:{false,true})
            require(State::coverage(known,supported,hooked)==(known && (!supported || hooked)),"unknown capabilities or an unsupported observation path cannot pass coverage");
        for(const auto kind:{State::dispatch,State::indirect,State::bundle,State::clear_uav_uint,State::clear_uav_float}) {
            state={};state.raster_write(true);state.unmodeled_writer(kind);
            State::legacy(&state,target,true,1,&full);state.raster_write(true);
            require(state.hazards==State::unsupported_writer && state.writer_kinds==kind && state.unknown && !state.raster,
                "each unmodeled base writer taints a recording despite a later full transition and draw");
            state={};state.raster_write(true);State::legacy(&state,target,true,1,&full);state.unmodeled_writer(kind);
            require(state.hazards==State::unsupported_writer && state.unknown,"a later unmodeled writer invalidates previously known end state");
        }
        for(uint32_t installed=0;installed<=63;++installed)
            require(State::base_writer_coverage(installed)==(installed==31),"every base writer hook must actually install; partial/unknown masks cannot pass");
        state={};state.unmodeled_writer(State::dispatch);state.unmodeled_writer(State::bundle);
        require(state.writer_kinds==(State::dispatch|State::bundle),"writer diagnostics retain all observed kinds");
        state={};state.raster_write(true);require(!state.hazards && !state.writer_kinds,"a new recording resets writer diagnostics");
        for(uint32_t hazards:{0u,1u,2u,4u,8u,15u,16u,31u}) {
            require(State::epoch_hazards(true,true,hazards)==0,"known observed-queue recording can be re-evaluated after Reset");
            require(State::epoch_hazards(true,false,hazards)==hazards && State::epoch_hazards(false,true,hazards)==hazards &&
                State::epoch_hazards(false,false,hazards)==hazards,"unattributed or foreign-queue hazard survives later known direct recordings until target invalidation");
        }
        std::cout<<"GPU state evidence CPU tests passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
