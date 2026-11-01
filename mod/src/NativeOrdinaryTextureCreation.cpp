#include "NativeOrdinaryTextureCreation.h"
#include "NativePoolReleaseGuard.h"
#include "GameBuild.h"
#include "GpuObjectIdentity.h"
#include <algorithm>
#include <atomic>

namespace {
std::atomic<uint64_t> names{};
std::optional<size_t> active_row(const native_texture_contract::Snapshot& value){
    if(value.bank_variant<0 || value.bank_variant>=30)return {};
    const auto& a=value.targets[0];const auto& b=value.targets[1];
    if(a.variants>1 || b.variants!=a.variants || a.initialization_words.empty() || a.initialization_words.size()>30 || a.initialization_words!=b.initialization_words || (!a.variants && a.initialization_words.size()!=1))return {};
    const size_t row=a.variants?size_t(value.bank_variant):0;
    if(row>=a.initialization_words.size())return {};return row;
}
}
NativeOrdinaryTextureCreation& NativeOrdinaryTextureCreation::get(){static auto* value=new NativeOrdinaryTextureCreation;return *value;}
// TAA_PREV_FRAME_0/1, 1 Nov capture: render size, the same format as bloom, and unordered access too.
NativeOrdinaryTextureCreation& NativeOrdinaryTextureCreation::taa(){static auto* value=new NativeOrdinaryTextureCreation(Spec{{0x16,0x17},0x20000b3,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,"SM2VR_ORDINARY_RIGHT_TAA_"});return *value;}
bool NativeOrdinaryTextureCreation::matches(uint32_t timestamp,const std::array<std::array<uint8_t,16>,4>& code,const std::array<uint8_t,12>& retain){for(size_t i=0;i<code.size();++i)if(!game_build::code(timestamp,RVAS[i],code[i],BYTES[i]))return false;return game_build::code(timestamp,0x26e29d0,retain,RETAIN_BYTES);}
bool NativeOrdinaryTextureCreation::arm_in_game(HMODULE module,ID3D12Device* device){
    using namespace native_texture_contract;const auto base=reinterpret_cast<uintptr_t>(module);IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    std::array<std::array<uint8_t,16>,4> code{};std::array<uint8_t,12> retain{};
    if(!read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 || !read(base+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage<0x508cab0 || !game_build::supported(nt.FileHeader.TimeDateStamp))return false;
    for(size_t i=0;i<4;++i)if(!read(base+game_build::rva(RVAS[i]),code[i]))return false;
    if(!read(base+game_build::rva(0x26e29d0),retain) || !matches(nt.FileHeader.TimeDateStamp,code,retain))return false;
    std::array<uint8_t,native_pool_membership::RELEASE_BYTES.size()> release{};
    // The pool guard hooks this body (its first bytes become a jump) after checking the same digest.
    const auto release_address=reinterpret_cast<const void*>(base+game_build::rva(native_pool_membership::RELEASE_RVA));
    if(!read(reinterpret_cast<uintptr_t>(release_address),release) ||
        !NativePoolReleaseGuard::get().release_verified(native_pool_membership::matches(nt.FileHeader.TimeDateStamp,release),release_address))return false;
    using Create=uintptr_t(*)(uintptr_t,const Name*,uint64_t,uint32_t,uintptr_t);using Retain=int32_t(*)(uintptr_t);using Pre=void(*)(uintptr_t,uintptr_t,uintptr_t);using Init=void(*)(uintptr_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
    Api api;api.create=reinterpret_cast<Create>(base+game_build::rva(RVAS[0]));api.preinitialize=reinterpret_cast<Pre>(base+game_build::rva(RVAS[1]));api.initialize=reinterpret_cast<Init>(base+game_build::rva(RVAS[2]));api.retain=reinterpret_cast<Retain>(base+game_build::rva(0x26e29d0));
    char skip[4]{};api.skip_reference=GetEnvironmentVariableA("SM2VR_DIAG_NO_NATIVE_REFERENCE",skip,sizeof(skip))==1 && skip[0]=='1';
    api.vtable=base+game_build::rva(0x37a4b18);api.factory_method=base+game_build::rva(RVAS[3]);api.preinitialize_method=base+game_build::rva(RVAS[1]);api.initialize_method=base+game_build::rva(RVAS[2]);api.instruction_guards=true;
    return configure(device,base,std::move(api));
}
bool NativeOrdinaryTextureCreation::configure(ID3D12Device* device,uintptr_t base,Api api){
    std::lock_guard lock{m_mutex};if(m_armed || !device || !base || !api.create || !api.retain || !api.preinitialize || !api.initialize || !api.vtable || !api.factory_method || !api.preinitialize_method || !api.initialize_method || !(m_device_identity=GpuObjectIdentity::get(device)))return false;
    if(!GpuAllocationProbe::observes(device) && !m_allocations.install(device))return false;
    m_device=device;m_base=base;m_api=std::move(api);m_armed=true;return true;
}
bool NativeOrdinaryTextureCreation::arm_like(const NativeOrdinaryTextureCreation& other){
    std::scoped_lock lock{m_mutex,other.m_mutex};if(m_armed || !other.m_armed)return false;
    m_device=other.m_device;m_device_identity=other.m_device_identity;m_base=other.m_base;m_api=other.m_api;m_armed=true;return true;
}
bool NativeOrdinaryTextureCreation::contract(const Origin& origin,const native_texture_contract::Snapshot& value,const std::array<NativeRenderProbe::Output,9>& outputs)const{
    const auto selected_row=active_row(value);if(!selected_row)return false;
    if(!origin.dispatch || !origin.render_id || !origin.revision || origin.slot!=1 || !origin.entry || !origin.list || !origin.renderer || !value.complete || value.module!=m_base || !value.factory || value.factory_create!=m_api.factory_method || value.selected_id!=m_spec.ids[origin.counter&1u] || value.wrappers[1].vtable!=m_api.vtable || value.wrappers[1].methods[2]!=m_api.preinitialize_method || value.wrappers[1].methods[3]!=m_api.initialize_method)return false;
    for(size_t i=0;i<2;++i){const auto& target=value.targets[i];if(!target.readable || target.id!=m_spec.ids[i] || target.no_uav || target.wrappers.size()!=target.initialization_words.size() || target.definition[0]!=m_spec.definition)return false;
        for(const auto& row:target.initialization_words)if(!row[0] || row[0]>4096 || !row[1] || row[1]>2160 || row[2]!=1 || row[3]!=1 || row[4]!=38 || row[5]!=1)return false;}
    if(!value.wrappers[1].address || value.targets[origin.counter&1].wrappers[*selected_row]!=value.wrappers[1].address)return false;
    const auto& row=value.targets[0].initialization_words[*selected_row];const auto& observed=outputs[1];
    if(observed.status!=NativeRenderProbe::OutputStatus::observed || !observed.resource || observed.descriptors.views.empty())return false;
    for(const auto& view:observed.descriptors.views)if(!view.known || !view.identity || view.width!=row[0] || view.height!=row[1] || view.storage!=DXGI_FORMAT_R16G16B16A16_TYPELESS || view.format!=DXGI_FORMAT_R16G16B16A16_FLOAT || view.flags!=m_spec.flags || view.mips!=1 || view.depth_or_array!=1 || view.samples!=1 || view.planes!=1 || view.subresources!=1)return false;
    return true;
}
void NativeOrdinaryTextureCreation::consider(const Origin& origin,const native_texture_contract::Snapshot& value,const std::array<NativeRenderProbe::Output,9>& observed,const std::function<bool()>& current){
    std::unique_lock lock{m_mutex,std::try_to_lock};if(!lock || !m_armed || m_attempted)return;
    if(!current || !current()){m_refusal=Refusal::scope;return;}if(!contract(origin,value,observed)){m_refusal=Refusal::contract;return;}
    // A retired pair made for this layout becomes current again: no factory call, no new textures.
    // It passes the same checks as a fresh pair (reusable()), against this job's pool.
    const auto row=*active_row(value);
    for(auto it=m_retired.begin();it!=m_retired.end();++it){
        if(it->spent || !native_texture_contract::same_layout(value,it->contract))continue;
        Adoption adoption;Reuse reuse{Reuse::unusable};try{reuse=reusable(*it,origin,value,observed,current,row,adoption);}catch(...){reuse=Reuse::unusable;}
        if(reuse==Reuse::changed){m_refusal=Refusal::scope;return;} // Not attempted: the next job tries again.
        if(reuse==Reuse::unusable){it->spent=true;continue;}
        m_attempted=true;m_reused=true;++m_reuses;m_origin=origin;m_thread=GetCurrentThreadId();m_variant=uint32_t(value.bank_variant);m_row=uint32_t(row);m_refusal=Refusal::none;
        m_contract=std::move(adoption.contract);m_pool_source=std::move(adoption.source);m_first_use=0;
        m_textures=std::move(it->textures);for(size_t i=0;i<2;++i)m_textures[i].pool_membership=std::move(adoption.owned[i]);
        m_retired.erase(it);return; // Never current and retired at once.
    }
    if(!current()){m_refusal=Refusal::scope;return;}
    if(m_generation>=GENERATIONS){m_refusal=Refusal::generations;return;}
    ++m_generation;m_reused=false;m_attempted=true;m_origin=origin;m_thread=GetCurrentThreadId();m_variant=uint32_t(value.bank_variant);m_row=uint32_t(*active_row(value));m_refusal=Refusal::none;m_contract=value;
    try{
    m_pool_source=native_pool_membership::lookup(value.pool,value.wrappers[1].address,value.record_count);
    if(!m_pool_source.known || !m_pool_source.found || m_pool_source.record!=value.targets[origin.counter&1].record){m_refusal=Refusal::pool;return;}
    for(size_t i=0;i<2;++i){auto& texture=m_textures[i];const auto& row=value.targets[i].initialization_words[m_row];
        texture.name=m_spec.name+std::to_string(GetCurrentProcessId())+"_"+std::to_string(++names)+"_"+std::to_string(i);const Name name{texture.name.c_str(),uint32_t(texture.name.size()),0};
        uintptr_t factory{};
        if(!current() || (m_api.instruction_guards && (!native_texture_contract::read(m_base+game_build::rva(0x508caa8),factory) || factory!=value.factory))){m_refusal=Refusal::changed;return;}
        texture.native=m_api.create(value.factory,&name,value.targets[i].definition[0],1,0);
        if(!texture.native || texture.native>UINTPTR_MAX-0x280){m_refusal=Refusal::factory;return;}
        if(!current() || (m_api.instruction_guards && (!native_texture_contract::read(m_base+game_build::rva(0x508caa8),factory) || factory!=value.factory))){m_refusal=Refusal::changed;return;}
        for(const auto& borrowed:value.wrappers)if(borrowed.address==texture.native){m_refusal=Refusal::existing;return;}
        for(const auto& target:value.targets)if(std::ranges::find(target.wrappers,texture.native)!=target.wrappers.end()){m_refusal=Refusal::existing;return;}
        if(i && texture.native==m_textures[0].native){m_refusal=Refusal::existing;return;}
        uintptr_t vtable{};uint64_t flags{};
        if(!native_texture_contract::read(texture.native,vtable) || vtable!=m_api.vtable || !native_texture_contract::read(texture.native+0x98,flags) || flags!=value.targets[i].definition[0] || !native_texture_contract::read(texture.native+0x70,texture.refs_before) || texture.refs_before<0 || texture.refs_before==INT32_MAX){m_refusal=Refusal::factory;return;}
        if(m_api.skip_reference){texture.refs_after=texture.refs_before;texture.native_reference=true;}
        else{texture.refs_after=m_api.retain(texture.native+0x70);texture.native_reference=texture.refs_after==texture.refs_before+1;}
        int32_t actual_refs{};if(!texture.native_reference || !native_texture_contract::read(texture.native+0x70,actual_refs) || actual_refs!=texture.refs_after){m_refusal=Refusal::reference;return;}
        m_api.preinitialize(texture.native,0,0);m_api.initialize(texture.native,row[0],row[1],row[2],row[4],1,row[3]);
        uintptr_t raw{};if(!native_texture_contract::read(texture.native+0x98,flags) || flags!=(value.targets[i].definition[0]|(uint64_t(1)<<50)) || !native_texture_contract::read(texture.native+0x278,raw) || !raw){m_refusal=Refusal::initialize;return;}
        texture.initialized=true;
        for(const auto& source:observed)if(source.resource==raw){m_refusal=Refusal::existing;return;}
        // This pointer is from our fresh guarded factory/init result, whose native
        // reference we hold. It is never a borrowed cached source observation.
        texture.resource=reinterpret_cast<ID3D12Resource*>(raw);texture.identity=GpuObjectIdentity::get(texture.resource.Get());
        Microsoft::WRL::ComPtr<ID3D12Device> device;const auto desc=texture.resource->GetDesc();
        if(!texture.identity || FAILED(texture.resource->GetDevice(IID_PPV_ARGS(&device))) || GpuObjectIdentity::get(device.Get())!=m_device_identity || desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width!=row[0] || desc.Height!=row[1] || desc.Format!=DXGI_FORMAT_R16G16B16A16_TYPELESS || desc.Flags!=m_spec.flags || desc.DepthOrArraySize!=1 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1){m_refusal=Refusal::resource;return;}
        for(const auto& source:observed){if(source.resource==raw){m_refusal=Refusal::existing;return;}for(const auto& view:source.descriptors.views)if(view.identity==texture.identity){m_refusal=Refusal::existing;return;}}
        if(i && texture.identity==m_textures[0].identity){m_refusal=Refusal::existing;return;}
        texture.allocation=GpuAllocationProbe::read(texture.resource.Get());
        if(!texture.allocation || texture.allocation->range.kind!=GpuAllocationEvidence::committed || (texture.allocation->range.heap_flags&0x21) || texture.allocation->range.device!=m_device_identity || texture.allocation->range.resource!=texture.identity || texture.allocation->thread!=GetCurrentThreadId()){m_refusal=Refusal::allocation;return;}
        texture.pool_membership=native_pool_membership::lookup(value.pool,texture.native,value.record_count);
        if(!texture.pool_membership.known || texture.pool_membership.found){m_refusal=Refusal::pool;return;}
        if(!current()){m_refusal=Refusal::changed;return;}
    }
    if(!GpuAllocationEvidence::disjoint(m_textures[0].allocation->range,m_textures[1].allocation->range))m_refusal=Refusal::allocation;
    }catch(...){m_refusal=Refusal::exception;}
    // All fresh native references/COM leases, including partial failures, remain
    // pinned for this bounded process-lifetime experiment. Never retry or retire
    // resources against an unproven GPU completion. Creation grants no use by itself.
}
// Caller holds m_mutex; contract(origin,value,observed) passed. The checks a fresh pair passes after
// its factory call, on a retired pair against this job's pool: still our initialised wrappers with
// our resources and leases, the row's size and format, not this job's destination or a pool target's
// wrapper or output, absent from the game's pool, distinct and disjoint. changed: the pool or scope
// moved under us (not the pair's fault).
NativeOrdinaryTextureCreation::Reuse NativeOrdinaryTextureCreation::reusable(const Retired& pair,const Origin& origin,const native_texture_contract::Snapshot& value,const std::array<NativeRenderProbe::Output,9>& observed,const std::function<bool()>& current,size_t row,Adoption& adoption)const{
    const auto rebased=native_texture_contract::rebase_layout(value,pair.contract);if(!rebased)return Reuse::unusable;
    adoption.contract=*rebased;adoption.source=native_pool_membership::lookup(value.pool,value.wrappers[1].address,value.record_count);
    if(!adoption.source.known || !adoption.source.found || adoption.source.record!=value.targets[origin.counter&1].record)return Reuse::changed;
    const auto& words=value.targets[0].initialization_words[row];
    for(size_t i=0;i<2;++i){const auto& texture=pair.textures[i];
        if(!texture.native_reference || !texture.initialized || !texture.resource || !texture.identity || !texture.allocation || !texture.native || texture.native>UINTPTR_MAX-0x280)return Reuse::unusable;
        // The job's destination is the game's target, never ours. Its previous may be: the renderer's history
        // still names the last owned output after a stop, which the owned chain samples and the game releases
        // every job anyway (NativePoolReleaseGuard).
        if(value.wrappers[1].address==texture.native)return Reuse::unusable;
        for(const auto& target:value.targets)if(std::ranges::find(target.wrappers,texture.native)!=target.wrappers.end())return Reuse::unusable;
        uintptr_t vtable{},raw{};uint64_t flags{};int32_t refs{};
        if(!native_texture_contract::read(texture.native,vtable) || vtable!=m_api.vtable || !native_texture_contract::read(texture.native+0x98,flags) || flags!=(value.targets[i].definition[0]|(uint64_t(1)<<50)) ||
           !native_texture_contract::read(texture.native+0x70,refs) || refs<1 || !native_texture_contract::read(texture.native+0x278,raw) || raw!=reinterpret_cast<uintptr_t>(texture.resource.Get()))return Reuse::unusable;
        Microsoft::WRL::ComPtr<ID3D12Device> device;const auto desc=texture.resource->GetDesc();
        if(GpuObjectIdentity::get(texture.resource.Get())!=texture.identity || FAILED(texture.resource->GetDevice(IID_PPV_ARGS(&device))) || GpuObjectIdentity::get(device.Get())!=m_device_identity ||
           desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width!=words[0] || desc.Height!=words[1] || desc.Format!=DXGI_FORMAT_R16G16B16A16_TYPELESS || desc.Flags!=m_spec.flags || desc.DepthOrArraySize!=1 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1)return Reuse::unusable;
        // No other wrapper the job reads or writes is backed by our resource (outputs follow the job's wrappers).
        for(size_t k=0;k<observed.size();++k){if(k!=1 && value.wrappers[k].address==texture.native)continue;const auto& source=observed[k];
            if(source.resource==raw)return Reuse::unusable;for(const auto& view:source.descriptors.views)if(view.identity==texture.identity)return Reuse::unusable;}
        adoption.owned[i]=native_pool_membership::lookup(value.pool,texture.native,value.record_count);
        if(!adoption.owned[i].known)return Reuse::changed;
        if(adoption.owned[i].found)return Reuse::unusable; // The game's pool holds it: never select it.
    }
    const auto& a=pair.textures[0];const auto& b=pair.textures[1];
    if(a.native==b.native || a.resource==b.resource || a.identity==b.identity || !GpuAllocationEvidence::disjoint(a.allocation->range,b.allocation->range))return Reuse::unusable;
    return current()?Reuse::usable:Reuse::changed;
}
bool NativeOrdinaryTextureCreation::renew(const native_texture_contract::Snapshot& observed){
    std::unique_lock lock{m_mutex,std::try_to_lock};if(!lock || !m_armed || !m_attempted || m_refusal!=Refusal::none)return false;
    // This pair or a retired one made for the new layout: the next bootstrap makes it current, so the cap doesn't apply.
    bool reuse=native_texture_contract::same_layout(observed,m_contract);
    for(const auto& pair:m_retired)reuse=reuse || (!pair.spent && native_texture_contract::same_layout(observed,pair.contract));
    if(!reuse && m_generation>=GENERATIONS)return false;
    m_retired.push_back({std::move(m_textures),m_contract});m_textures={};m_attempted=false;m_reused=false;m_origin={};m_contract={};m_first_use=0;m_pool_source={};m_renewal_reuses=reuse;return true;
}
std::optional<NativeOrdinaryTextureCreation::OwnedPair> NativeOrdinaryTextureCreation::owned_pair()const{
    std::lock_guard lock{m_mutex};if(!m_armed || !m_attempted || m_refusal!=Refusal::none)return {};
    OwnedPair value;value.origin=m_origin;value.thread=m_thread;value.variant=m_variant;value.row=m_row;value.contract=m_contract;
    for(size_t i=0;i<2;++i){const auto& texture=m_textures[i];if(!texture.native_reference || !texture.initialized || !texture.resource || !texture.identity || !texture.allocation)return {};
        value.native[i]=texture.native;value.resource[i]=reinterpret_cast<uintptr_t>(texture.resource.Get());value.identity[i]=texture.identity;}
    return value;
}
void NativeOrdinaryTextureCreation::write(std::ostream& out,uint64_t dispatch)const{
    std::lock_guard lock{m_mutex};out<<"{\"model\":\""<<(m_first_use && m_first_use<=dispatch?"ordinary_native_texture_creation_v2":"ordinary_native_texture_creation_v1")<<"\",\"armed\":"<<(m_armed?"true":"false")<<",\"attempted\":"<<(m_attempted?"true":"false")<<",\"instruction_guards\":"<<(m_api.instruction_guards?"true":"false")<<",\"dispatch\":"<<dispatch<<",\"creation_dispatch\":"<<m_origin.dispatch<<",\"creation_thread\":"<<m_thread<<",\"creation_variant\":"<<m_variant<<",\"initializer_row\":"<<m_row<<",\"render_id\":"<<m_origin.render_id<<",\"revision\":"<<m_origin.revision<<",\"token\":"<<m_origin.token<<",\"counter\":"<<m_origin.counter<<",\"slot\":"<<m_origin.slot<<",\"entry\":"<<m_origin.entry<<",\"list\":"<<m_origin.list<<",\"renderer\":"<<m_origin.renderer<<",\"device_identity\":"<<m_device_identity<<",\"refusal\":"<<uint32_t(m_refusal)<<",\"creation_pair_verified\":"<<(m_attempted && m_refusal==Refusal::none?"true":"false")<<",\"used_in_native_pass\":"<<(m_first_use && m_first_use<=dispatch?"true":"false")<<",\"first_use_dispatch\":"<<m_first_use<<",\"native_gpu_histories_verified\":false,\"gpu_pair_verified\":false,\"runtime_submission\":false,\"retirement\":\"process_lifetime\",\"generation\":"<<m_generation<<",\"retired\":"<<m_retired.size()<<",\"reuses\":"<<m_reuses<<",\"reused\":"<<(m_reused?"true":"false")<<",\"textures\":[";
    for(size_t i=0;i<2;++i){if(i)out<<',';const auto& texture=m_textures[i];out<<"{\"name\":\""<<texture.name<<"\",\"native\":"<<texture.native<<",\"native_reference\":"<<(texture.native_reference?"true":"false")<<",\"refs_before\":"<<texture.refs_before<<",\"refs_after\":"<<texture.refs_after<<",\"initialized\":"<<(texture.initialized?"true":"false")<<",\"resource\":"<<reinterpret_cast<uintptr_t>(texture.resource.Get())<<",\"identity\":"<<texture.identity<<",\"resource_lease\":"<<(texture.resource?"true":"false")<<",\"allocation\":";
        if(!texture.allocation)out<<"null";else{const auto& a=*texture.allocation;out<<"{\"api\":"<<a.api<<",\"initial_state\":"<<a.initial_state<<",\"thread\":"<<a.thread<<",\"serial\":"<<a.range.serial<<",\"device\":"<<a.range.device<<",\"resource\":"<<a.range.resource<<",\"kind\":"<<a.range.kind<<",\"heap\":"<<a.range.heap<<",\"heap_flags\":"<<a.range.heap_flags<<",\"offset\":"<<a.range.offset<<",\"size\":"<<a.range.size<<",\"heap_size\":"<<a.range.heap_size<<",\"alignment\":"<<a.range.alignment<<",\"creation_begin_ns\":"<<a.creation_begin_ns<<",\"callsite\":"<<a.callsite<<",\"actual_heap_flags\":"<<a.actual_heap_flags<<",\"properties_known\":"<<(a.properties_known?"true":"false")<<",\"width\":"<<a.desc.Width<<",\"height\":"<<a.desc.Height<<",\"format\":"<<a.desc.Format<<",\"flags\":"<<a.desc.Flags<<",\"arrays\":"<<a.desc.DepthOrArraySize<<",\"mips\":"<<a.desc.MipLevels<<",\"samples\":"<<a.desc.SampleDesc.Count<<'}';}out<<'}';}
    out<<"],\"pool_membership\":{\"model\":\"native_pool_membership_v1\",\"instruction_guards\":"<<(m_api.instruction_guards?"true":"false")<<",\"release_rva\":"<<native_pool_membership::RELEASE_RVA<<",\"source\":";native_pool_membership::write(out,m_pool_source);out<<",\"owned\":[";
    for(size_t i=0;i<2;++i){if(i)out<<',';native_pool_membership::write(out,m_textures[i].pool_membership);}out<<"]}}";
}
