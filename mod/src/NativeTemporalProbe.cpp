#include "NativeTemporalProbe.h"
#include "NativeCallCensus.h"
#include "GameBuild.h"
#include "NativeOrdinaryTextureCreation.h"
#include "NativeOrdinaryTaa.h"
#include "NativeOrdinaryBloom.h"
#include "PerformanceTrace.h"
#include <MinHook.h>
#include <intrin.h>
#include <algorithm>

namespace {
template<class T>bool read(uintptr_t address,T& out){SIZE_T n{};return address && address<=UINTPTR_MAX-sizeof(T) && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&out,sizeof(T),&n) && n==sizeof(T);}
constexpr std::array<size_t,3> PREVIOUS{0x158,0x178,0x160},NEXT{0x1b0,0x1d0,0x1b8};
constexpr std::array<size_t,5> INPUTS{0x10,0x18,0x20,0x68,0x170};
}
NativeTemporalProbe& NativeTemporalProbe::get(){static auto* value=new NativeTemporalProbe;return *value;}
bool NativeTemporalProbe::matches(uint32_t stamp,std::span<const uint8_t> code,const std::array<std::array<uint8_t,5>,3>& calls){
    for(size_t i=0;i<calls.size();++i)if(!game_build::code(stamp,CALLS[i],calls[i],CALL_BYTES[i]))return false;
    return game_build::code(stamp,RVA,code,BYTES);
}
bool NativeTemporalProbe::install_in_game(HMODULE module,NativeRenderProbe::OutputObserver outputs){
    const auto base=reinterpret_cast<uintptr_t>(module);IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};std::array<uint8_t,BYTES.size()> code{};std::array<std::array<uint8_t,5>,3> calls{};
    if(!read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 || !read(base+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || !game_build::supported(nt.FileHeader.TimeDateStamp) || nt.OptionalHeader.SizeOfImage<RVA+code.size() || !read(base+game_build::rva(RVA),code))return false;
    for(size_t i=0;i<3;++i)if(!game_build::rva(CALLS[i]) || !read(base+game_build::rva(CALLS[i]),calls[i]))return false;
    if(!matches(nt.FileHeader.TimeDateStamp,code,calls))return false;
    char history[8]{};const bool global_history=GetEnvironmentVariableA("SM2VR_ORDINARY_GLOBAL_HISTORY",history,sizeof(history))==1 && history[0]=='1';
    return install(reinterpret_cast<void*>(base+game_build::rva(RVA)),[]()->std::optional<Scope>{const auto scene=NativeRenderProbe::current_scene();const auto pair=NativeRenderProbe::current_ordinary_pair();if(!scene || !pair)return {};return Scope{*scene,*pair};},std::move(outputs),base,0,global_history);
}
bool NativeTemporalProbe::install(void* target,Provider provider,NativeRenderProbe::OutputObserver outputs,uintptr_t base,uintptr_t contract_base,bool global_history){
    std::lock_guard lock{m_install};if(m_original || !target || !provider || !outputs)return false;const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
    void* original{};if(MH_CreateHook(target,reinterpret_cast<void*>(&on_native),&original)!=MH_OK)return false;
    m_provider=std::move(provider);m_outputs=std::move(outputs);m_base=base;m_contract_base=contract_base?contract_base:base;m_global_history=global_history;m_original=reinterpret_cast<Native>(original);
    if(MH_EnableHook(target)==MH_OK)return true;m_original=nullptr;MH_RemoveHook(target);return false;
}
std::optional<NativeTemporalProbe::State> NativeTemporalProbe::state(const Scope& scope,uintptr_t renderer,uintptr_t descriptors,uint8_t mode)const{
    const auto& s=scope.scene;const auto& pair=scope.pair;
    if(mode>2 || !s.valid() || !s.ordinal || !s.dispatch_sequence || s.slot>1 || renderer!=s.renderer || !renderer || renderer>UINTPTR_MAX-0x200 || descriptors!=renderer+0x10 || !NativeCameraEntries::get().current(pair))return {};
    const auto& eye=s.slot?pair.right:pair.left;
    if(eye.entry!=s.entry || eye.slot!=s.slot || eye.render_list!=s.list || s.camera_revision_before!=eye.revision || (s.view_flags_before&0x50000044) || !s.list || s.list>UINTPTR_MAX-0x29c7c)return {};
    State value;uint64_t flags{};
    if(!read(renderer+0x140,value.active) || value.active!=s.entry || !read(value.active,flags) || flags!=s.view_flags_before || !read(s.list+0x29c78,value.counter) || value.counter!=s.render_counter_before ||
        !read(renderer+PREVIOUS[mode],value.previous) || !read(renderer+NEXT[mode],value.next) || !read(renderer+0x150,value.previous_bank) || !read(renderer+0x1a8,value.next_bank))return {};
    for(size_t i=0;i<INPUTS.size();++i)if(!read(descriptors+INPUTS[i],value.inputs[i]))return {};
    State again;uint64_t after_flags{};
    if(!read(renderer+0x140,again.active) || !read(s.entry,after_flags) || flags!=after_flags || !read(s.list+0x29c78,again.counter) || !read(renderer+PREVIOUS[mode],again.previous) || !read(renderer+NEXT[mode],again.next) ||
        !read(renderer+0x150,again.previous_bank) || !read(renderer+0x1a8,again.next_bank))return {};
    for(size_t i=0;i<INPUTS.size();++i)if(!read(descriptors+INPUTS[i],again.inputs[i]))return {};
    if(value!=again || !NativeCameraEntries::get().current(pair))return {};
    return value;
}
std::array<NativeRenderProbe::Output,9> NativeTemporalProbe::outputs(const std::array<uintptr_t,7>& args,const State& state)const{
    std::array<NativeRenderProbe::Output,9> out;size_t i{};for(auto index:{1,2,3,5}){if(args[index])out[i]=NativeRenderProbe::read_output(args[index],m_outputs);++i;}
    for(auto wrapper:state.inputs){if(wrapper)out[i]=NativeRenderProbe::read_output(wrapper,m_outputs);++i;}return out;
}
native_texture_contract::Snapshot NativeTemporalProbe::texture_contract(const std::array<uintptr_t,7>& args,const State& state)const{
    std::array<uintptr_t,9> wrappers{};size_t i{};for(auto index:{1,2,3,5})wrappers[i++]=args[index];for(auto native:state.inputs)wrappers[i++]=native;
    return native_texture_contract::capture(m_contract_base,wrappers,uint8_t(args[6]),state.counter);
}
void NativeTemporalProbe::on_native(uintptr_t renderer,uintptr_t source,uintptr_t destination,uintptr_t previous,uintptr_t descriptors,uintptr_t extra,uint8_t mode){
    PerformanceTrace::Scope timing{"ordinary_temporal_total_cpu"};
    NativeCallCensus::get().count(NativeCallCensus::Hook::temporal);
    auto& self=get();std::optional<Event> event;
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    try{if(const auto scope=self.m_provider()){
        event.emplace();auto& e=*event;e.scope=*scope;e.thread=GetCurrentThreadId();e.mode=mode;e.caller=self.m_base && caller>=self.m_base?caller-self.m_base:caller;e.arguments={renderer,source,destination,previous,descriptors,extra,mode};
        if(mode>2)e.refusal=Refusal::mode;
        else if(self.m_base && !expected_callsite(mode,e.caller))e.refusal=Refusal::callsite;
        else if(!(e.before=self.state(*scope,renderer,descriptors,mode)))e.refusal=Refusal::scope;
        else if(!destination || e.before->previous!=previous || e.before->next!=destination)e.refusal=Refusal::bank;
        else {e.outputs_before=self.outputs(e.arguments,*e.before);e.texture_contract_before=self.texture_contract(e.arguments,*e.before);
            if(self.m_global_history)e.global_history_before=native_global_history::capture(self.m_contract_base,e.before->inputs[0],e.before->inputs[4],e.before->counter);
            auto& creation=mode==2?NativeOrdinaryTextureCreation::get():NativeOrdinaryTextureCreation::taa();
            if((mode==2 || (mode==0 && NativeOrdinaryTaa::get().enabled())) && scope->scene.slot==1 && creation.armed()){
                const auto& s=scope->scene;const auto& p=scope->pair.left;
                NativeOrdinaryTextureCreation::Origin origin{s.dispatch_sequence,p.render_id,p.revision,p.token,e.before->counter,s.slot,s.entry,s.list,s.renderer};
                creation.consider(origin,*e.texture_contract_before,e.outputs_before,[&]{return self.state(*scope,renderer,descriptors,mode)==e.before && self.texture_contract(e.arguments,*e.before)==e.texture_contract_before;});
            }}
    }}catch(...){if(event)event->refusal=Refusal::exception;std::lock_guard lock{self.m_mutex};++self.m_errors;}
    const std::array<uintptr_t,7> arguments{renderer,source,destination,previous,descriptors,extra,mode};
    const auto invoked_previous=mode==0?NativeOrdinaryTaa::get().previous(event?&*event:nullptr,arguments):NativeOrdinaryBloom::get().previous(event?&*event:nullptr,arguments);
    if(event){event->invoked_arguments=arguments;event->invoked_arguments[3]=invoked_previous;if(event->controlled && event->before)event->invoked_outputs_before=self.outputs(event->invoked_arguments,*event->before);}
    // Exactly one original call. Only the explicitly owned right-bloom control
    // may reset its predecessor; observed and invoked arguments stay distinct.
    {PerformanceTrace::Scope native_timing{"ordinary_temporal_native_cpu"};self.m_original.load()(renderer,source,destination,invoked_previous,descriptors,extra,mode);}
    if(event && event->controlled && mode==2)NativeOrdinaryTextureCreation::get().record_native_use(event->scope.scene.dispatch_sequence);
    try{if(event){auto& e=*event;if(e.refusal==Refusal::none){const auto scope=self.m_provider();e.after=self.state(e.scope,renderer,descriptors,mode);
            if(!scope || scope->scene.dispatch_sequence!=e.scope.scene.dispatch_sequence || scope->scene.ordinal!=e.scope.scene.ordinal || scope->scene.slot!=e.scope.scene.slot || scope->scene.entry!=e.scope.scene.entry ||
                scope->scene.list!=e.scope.scene.list || scope->scene.renderer!=e.scope.scene.renderer || scope->scene.camera_revision_before!=e.scope.scene.camera_revision_before || scope->pair.left.render_id!=e.scope.pair.left.render_id || scope->pair.left.revision!=e.scope.pair.left.revision ||
                !NativeCameraEntries::get().current(scope->pair) || !e.after || e.after!=e.before)e.refusal=Refusal::changed;
            else {e.outputs_after=self.outputs(e.arguments,*e.after);e.texture_contract_after=self.texture_contract(e.arguments,*e.after);for(size_t i=0;i<e.outputs_before.size();++i)if(e.outputs_before[i].resource!=e.outputs_after[i].resource)e.refusal=Refusal::resource_changed;}}
        if(e.after && self.m_global_history)e.global_history_after=native_global_history::capture(self.m_contract_base,e.after->inputs[0],e.after->inputs[4],e.after->counter);
        if(e.controlled && e.after)e.invoked_outputs_after=self.outputs(e.invoked_arguments,*e.after);
        NativeOrdinaryBloom::get().after_temporal(e);NativeOrdinaryTaa::get().after_temporal(e);self.store(std::move(e));}}
    catch(...){std::lock_guard lock{self.m_mutex};++self.m_errors;}
}
void NativeTemporalProbe::store(Event event){std::lock_guard lock{m_mutex};event.serial=++m_serial;if(event.refusal!=Refusal::none)++m_refused;if(m_events.size()==128){m_events.pop_front();++m_dropped;}m_events.push_back(std::move(event));}
void NativeTemporalProbe::write_global_history(std::ostream& out,const Sample& sample){
    out<<"{\"model\":\"ordinary_global_history_contract_v1\",\"read_only\":true,\"resource_leases\":false,\"writer_verified\":false,\"native_gpu_histories_verified\":false,\"gpu_pair_verified\":false,\"runtime_submission\":false,\"installed\":"<<(sample.installed?"true":"false")<<",\"refused\":"<<sample.refused<<",\"errors\":"<<sample.errors<<",\"dropped\":"<<sample.dropped<<",\"events\":[";
    for(size_t i=0;i<sample.events.size();++i){if(i)out<<',';const auto& e=sample.events[i];const auto& s=e.scope.scene;const auto& p=e.scope.pair.left;
        out<<"{\"dispatch\":"<<s.dispatch_sequence<<",\"render_id\":"<<p.render_id<<",\"revision\":"<<p.revision<<",\"token\":"<<p.token<<",\"thread\":"<<e.thread<<",\"ordinal\":"<<s.ordinal<<",\"slot\":"<<s.slot<<",\"entry\":"<<s.entry<<",\"list\":"<<s.list<<",\"renderer\":"<<s.renderer<<",\"mode\":"<<uint32_t(e.mode)<<",\"refusal\":"<<uint32_t(e.refusal)<<",\"counter\":"<<(e.before?e.before->counter:0);
        for(auto [label,snapshot]:{std::pair{"before",&e.global_history_before},std::pair{"after",&e.global_history_after}}){out<<",\""<<label<<"\":";if(*snapshot)native_global_history::write(out,**snapshot);else out<<"null";}out<<'}';
    }out<<"]}";
}
NativeTemporalProbe::Sample NativeTemporalProbe::sample(uint64_t dispatch)const{std::lock_guard lock{m_mutex};Sample value{m_original!=nullptr,m_refused,m_errors,m_dropped,{}};for(const auto& e:m_events)if(e.scope.scene.dispatch_sequence==dispatch)value.events.push_back(e);return value;}
void NativeTemporalProbe::write(std::ostream& out,const Sample& sample){
    const bool controlled=std::ranges::any_of(sample.events,[](const auto& event){return event.controlled;});
    out<<"{\"model\":\""<<(controlled?"ordinary_native_temporal_calls_v2":"ordinary_native_temporal_calls_v1")<<"\",\"cpu_only\":true,\"runtime_submission\":false,\"installed\":"<<(sample.installed?"true":"false")<<",\"refused\":"<<sample.refused<<",\"errors\":"<<sample.errors<<",\"dropped\":"<<sample.dropped<<",\"resource_leases\":false,\"native_gpu_histories_verified\":false,\"gpu_pair_verified\":false,\"output_labels\":[\"source\",\"destination\",\"previous\",\"extra\",\"descriptor+0x10\",\"descriptor+0x18\",\"descriptor+0x20\",\"descriptor+0x68\",\"descriptor+0x170\"],\"events\":[";
    const auto state=[&](const char* name,const std::optional<State>& s){out<<",\""<<name<<"\":";if(!s){out<<"null";return;}out<<"{\"counter\":"<<s->counter<<",\"active\":"<<s->active<<",\"previous\":"<<s->previous<<",\"next\":"<<s->next;
        for(auto [label,bank]:{std::pair{"previous_bank",s->previous_bank},std::pair{"next_bank",s->next_bank}}){out<<",\""<<label<<"\":[";for(size_t i=0;i<11;++i){if(i)out<<',';out<<bank[i];}out<<']';}out<<",\"descriptor_inputs\":[";for(size_t i=0;i<s->inputs.size();++i){if(i)out<<',';out<<s->inputs[i];}out<<"]}";};
    const auto outputs=[&](const char* label,const auto& values){out<<",\""<<label<<"\":[";for(size_t i=0;i<values.size();++i){if(i)out<<',';const auto& value=values[i];out<<"{\"resource\":"<<value.resource<<",\"status\":"<<uint32_t(value.status)<<",\"epoch\":"<<value.descriptors.epoch<<",\"serial\":"<<value.descriptors.serial<<",\"rtvs\":[";
            for(size_t j=0;j<value.descriptors.views.size();++j){if(j)out<<',';const auto& v=value.descriptors.views[j];out<<"{\"resource\":"<<v.resource<<",\"identity\":"<<v.identity<<",\"handle\":"<<v.handle<<",\"known\":"<<(v.known?"true":"false")<<",\"created\":"<<v.created<<",\"bound\":"<<v.bound<<",\"command_list\":"<<v.command_list<<",\"width\":"<<v.width<<",\"height\":"<<v.height<<",\"storage\":"<<v.storage<<",\"format\":"<<v.format<<",\"resource_dimension\":"<<v.resource_dimension<<",\"flags\":"<<v.flags<<",\"mips\":"<<v.mips<<",\"arrays\":"<<v.depth_or_array<<",\"samples\":"<<v.samples<<",\"planes\":"<<v.planes<<",\"subresources\":"<<v.subresources<<'}';}out<<"]}";}out<<']';};
    for(size_t i=0;i<sample.events.size();++i){if(i)out<<',';const auto& e=sample.events[i];const auto& s=e.scope.scene;const auto& p=e.scope.pair.left;
        out<<"{\"serial\":"<<e.serial<<",\"dispatch\":"<<s.dispatch_sequence<<",\"scene_ordinal\":"<<s.ordinal<<",\"slot\":"<<s.slot<<",\"entry\":"<<s.entry<<",\"list\":"<<s.list<<",\"renderer\":"<<s.renderer<<",\"render_id\":"<<p.render_id<<",\"revision\":"<<p.revision<<",\"token\":"<<p.token<<",\"thread\":"<<e.thread<<",\"mode\":"<<uint32_t(e.mode)<<",\"caller\":"<<e.caller<<",\"refusal\":"<<uint32_t(e.refusal)<<",\"arguments\":[";
        for(size_t k=0;k<7;++k){if(k)out<<',';out<<e.arguments[k];}out<<']';state("before",e.before);state("after",e.after);outputs("outputs_before",e.outputs_before);outputs("outputs_after",e.outputs_after);
        if(controlled){out<<",\"controlled\":"<<(e.controlled?"true":"false")<<",\"invoked_arguments\":[";for(size_t k=0;k<7;++k){if(k)out<<',';out<<e.invoked_arguments[k];}out<<']';outputs("invoked_outputs_before",e.controlled?e.invoked_outputs_before:e.outputs_before);outputs("invoked_outputs_after",e.controlled?e.invoked_outputs_after:e.outputs_after);}
        for(auto [label,snapshot]:{std::pair{"texture_contract_before",&e.texture_contract_before},std::pair{"texture_contract_after",&e.texture_contract_after}}){out<<",\""<<label<<"\":";if(*snapshot)native_texture_contract::write(out,**snapshot);else out<<"null";}out<<'}';}
    out<<"]}";
}
