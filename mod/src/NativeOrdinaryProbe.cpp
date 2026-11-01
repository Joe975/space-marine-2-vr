#include "NativeOrdinaryProbe.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <algorithm>

namespace {
template<class T> bool read(uintptr_t address,T& value) {SIZE_T bytes{};return address && address<=UINTPTR_MAX-sizeof(T) &&
    ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(value),&bytes) && bytes==sizeof(value);}
}
NativeOrdinaryProbe& NativeOrdinaryProbe::get(){static auto* value=new NativeOrdinaryProbe;return *value;}
bool NativeOrdinaryProbe::matches(uint32_t stamp,std::span<const uint8_t> post,std::span<const uint8_t> compositor) {
    return game_build::code(stamp,POST_RVA,post,POST_BYTES) && game_build::code(stamp,COMPOSITOR_RVA,compositor,COMPOSITOR_BYTES);
}
bool NativeOrdinaryProbe::guarded_in_game(HMODULE module) {
    const auto base=reinterpret_cast<uintptr_t>(module);IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    std::array<uint8_t,POST_BYTES.size()> post{};std::array<uint8_t,COMPOSITOR_BYTES.size()> compositor{};
    if(!read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 || !read(base+dos.e_lfanew,nt) ||
        nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || !game_build::supported(nt.FileHeader.TimeDateStamp) || nt.OptionalHeader.SizeOfImage<POST_RVA+post.size() ||
        !read(base+game_build::rva(POST_RVA),post) || !read(base+game_build::rva(COMPOSITOR_RVA),compositor) || !matches(nt.FileHeader.TimeDateStamp,post,compositor)) return false;
    return true;
}
bool NativeOrdinaryProbe::attach_raw(RawObserver observer){std::lock_guard lock{m_install};if(m_post || m_compositor || m_raw || !observer)return false;m_raw=std::move(observer);return true;}
bool NativeOrdinaryProbe::install_in_game(HMODULE module,NativeRenderProbe::OutputObserver outputs,Boundary boundary) {
    if(!guarded_in_game(module))return false;const auto base=reinterpret_cast<uintptr_t>(module);
    return install(reinterpret_cast<void*>(base+game_build::rva(POST_RVA)),reinterpret_cast<void*>(base+game_build::rva(COMPOSITOR_RVA)),[]()->std::optional<Scope>{
        const auto scene=NativeRenderProbe::current_scene();const auto primary=NativeRenderProbe::current_primary();
        if(!scene || !primary) return std::nullopt;return Scope{*scene,*primary};
    },std::move(outputs),std::move(boundary));
}
void NativeOrdinaryProbe::observe(Stage stage,uintptr_t wrapper,uintptr_t entry,uintptr_t renderer_state){
    record(stage,wrapper,entry,renderer_state);
    if(m_raw)try{m_raw(RawBoundary{stage,wrapper,entry,renderer_state});}catch(...){++m_raw_errors;}
}
bool NativeOrdinaryProbe::install(void* post,void* compositor,Provider provider,NativeRenderProbe::OutputObserver outputs,Boundary boundary) {
    std::lock_guard lock{m_install};if(m_post || m_compositor || !post || !compositor || !provider || !outputs) return false;
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    void* original_post{};void* original_compositor{};
    if(MH_CreateHook(post,reinterpret_cast<void*>(&on_post),&original_post)!=MH_OK) return false;
    if(MH_CreateHook(compositor,reinterpret_cast<void*>(&on_compositor),&original_compositor)!=MH_OK) {MH_RemoveHook(post);return false;}
    m_provider=std::move(provider);m_outputs=std::move(outputs);m_boundary=std::move(boundary);
    m_post=reinterpret_cast<Post>(original_post);m_compositor=reinterpret_cast<Compositor>(original_compositor);
    if(MH_EnableHook(post)==MH_OK && MH_EnableHook(compositor)==MH_OK) return true;
    MH_DisableHook(post);MH_DisableHook(compositor);MH_RemoveHook(post);MH_RemoveHook(compositor);m_post=nullptr;m_compositor=nullptr;return false;
}
void NativeOrdinaryProbe::record(Stage stage,uintptr_t wrapper,uintptr_t entry,uintptr_t renderer_state) {
    try {
        const auto scope=m_provider();if(!scope) return;
        const auto& scene=scope->scene;const auto& primary=scope->primary;uintptr_t active{};uint64_t flags{};uint32_t counter{};
        const bool valid=scene.valid() && scene.ordinal && scene.dispatch_sequence && scene.renderer && scene.renderer<=UINTPTR_MAX-0x148 &&
            primary.entry==scene.entry && primary.slot==scene.slot && primary.render_list==scene.list &&
            scene.camera_revision_before==primary.revision && NativeCameraEntries::get().current(primary) &&
            (scene.view_flags_before&0x50000044)==0 && (scene.view_flags_before&0x20000) &&
            (stage!=Stage::post || entry==scene.entry) && (stage==Stage::post || renderer_state==scene.renderer+0x10) &&
            read(scene.renderer+0x140,active) && active==scene.entry && read(active,flags) && flags==scene.view_flags_before &&
            scene.list<=UINTPTR_MAX-0x29c7c && read(scene.list+0x29c78,counter) && counter==scene.render_counter_before;
        if(!valid){std::lock_guard lock{m_mutex};++m_refused;return;}
        Event event;event.stage=stage;event.scope=*scope;event.wrapper=wrapper;event.output=NativeRenderProbe::read_output(wrapper,m_outputs);
        {std::lock_guard lock{m_mutex};event.serial=++m_serial;
        if(m_events.size()==128){m_events.pop_front();++m_dropped;}m_events.push_back(event);}
        if(m_boundary)m_boundary(event); // Outside observer lock; still inside the verified native Scene scope.
    }catch(...){std::lock_guard lock{m_mutex};++m_errors;}
}
void NativeOrdinaryProbe::on_post(uintptr_t a,uintptr_t* b,uintptr_t c,uintptr_t d,uintptr_t entry,uint32_t f,uintptr_t g,uintptr_t h,uintptr_t i,uintptr_t j,float delta) {
    auto& self=get();self.m_post.load()(a,b,c,d,entry,f,g,h,i,j,delta);
    uintptr_t wrapper{};read(reinterpret_cast<uintptr_t>(b),wrapper);self.observe(Stage::post,wrapper,entry,0);
}
void NativeOrdinaryProbe::on_compositor(uintptr_t colour,uintptr_t renderer_state,uint8_t flag) {
    auto& self=get();self.observe(Stage::compositor_before,colour,0,renderer_state);
    // No fabricated exit event and no catch/retry/translation of the native
    // exception. Nonallocating unwind cleanup only invalidates optional consumers.
    struct Unwind {NativeOrdinaryProbe& probe;uintptr_t colour,state;bool returned{};
        ~Unwind(){if(!returned)probe.abort_raw(colour,state);}} unwind{self,colour,renderer_state};
    self.m_compositor.load()(colour,renderer_state,flag);
    unwind.returned=true;
    self.observe(Stage::compositor_after,colour,0,renderer_state);
}
void NativeOrdinaryProbe::abort_raw(uintptr_t wrapper,uintptr_t renderer_state)noexcept{
    if(m_raw)try{m_raw(RawBoundary{Stage::compositor_before,wrapper,0,renderer_state,true});}catch(...){++m_raw_errors;}
}
NativeOrdinaryProbe::Sample NativeOrdinaryProbe::sample(uint64_t sequence) const {
    std::lock_guard lock{m_mutex};Sample value{m_refused,m_errors,m_dropped,{}};
    for(const auto& event:m_events) if(event.scope.scene.dispatch_sequence==sequence) value.events.push_back(event);return value;
}
void NativeOrdinaryProbe::write_capture(std::ostream& out,const NativeRenderProbe::Snapshot& dispatch,const Sample& sample,uint32_t width,uint32_t height) {
    out<<"{\"model\":\"ordinary_colour_boundaries_v1\",\"cpu_only\":true,\"pip_enabled\":false,\"gpu_pair_verified\":false,\"runtime_submission\":false"
        <<",\"display_width\":"<<width<<",\"display_height\":"<<height<<",\"dispatch\":"<<dispatch.sequence<<",\"list\":"<<dispatch.list<<",\"renderer\":"<<dispatch.renderer
        <<",\"scene_calls\":"<<dispatch.scene_calls<<",\"scene_refused\":"<<dispatch.scene_refused<<",\"scene_dropped\":"<<dispatch.scene_dropped
        <<",\"refused\":"<<sample.refused<<",\"errors\":"<<sample.errors<<",\"ring_evictions\":"<<sample.dropped<<",\"primary\":";
    if(dispatch.primary){const auto& p=*dispatch.primary;out<<"{\"add_list\":"<<p.add_list<<",\"render_list\":"<<p.render_list<<",\"entry\":"<<p.entry<<",\"slot\":"<<p.slot
        <<",\"revision\":"<<p.revision<<",\"render_id\":"<<p.render_id<<",\"token\":"<<p.token<<'}';}else out<<"null";
    out<<",\"scenes\":[";
    for(size_t n=0;n<dispatch.scenes.size();++n){if(n)out<<',';const auto& s=dispatch.scenes[n];
        out<<"{\"ordinal\":"<<s.ordinal<<",\"slot\":"<<s.slot<<",\"entry\":"<<s.entry<<",\"active_entry_after\":"<<s.active_entry_after
            <<",\"flags\":"<<s.view_flags_before<<",\"flags_after\":"<<s.view_flags_after<<",\"refusal\":"<<uint32_t(s.refusal)
            <<",\"counter_before\":"<<s.render_counter_before<<",\"counter_after\":"<<s.render_counter_after<<",\"revision_before\":";
        if(s.camera_revision_before)out<<*s.camera_revision_before;else out<<"null";
        out<<",\"revision_after\":";if(s.camera_revision_after)out<<*s.camera_revision_after;else out<<"null";out<<'}';}
    out<<"],\"views\":[";
    for(size_t n=0;n<dispatch.before.size();++n){if(n)out<<',';const auto& v=dispatch.before[n];out<<"{\"slot\":"<<v.slot<<",\"flags\":"<<v.flags<<",\"world\":[";
        for(size_t k=0;k<16;++k){if(k)out<<',';out<<v.world[k];}out<<"]}";}
    out<<"],\"views_after\":[";
    for(size_t n=0;n<dispatch.after.size();++n){if(n)out<<',';const auto& v=dispatch.after[n];out<<"{\"slot\":"<<v.slot<<",\"flags\":"<<v.flags<<",\"world\":[";
        for(size_t k=0;k<16;++k){if(k)out<<',';out<<v.world[k];}out<<"]}";}
    out<<"],\"events\":[";
    for(size_t n=0;n<sample.events.size();++n){if(n)out<<',';const auto& e=sample.events[n];const auto& s=e.scope.scene;const auto& p=e.scope.primary;
        out<<"{\"serial\":"<<e.serial<<",\"stage\":"<<uint32_t(e.stage)<<",\"dispatch\":"<<s.dispatch_sequence<<",\"ordinal\":"<<s.ordinal<<",\"slot\":"<<s.slot
            <<",\"entry\":"<<s.entry<<",\"render_id\":"<<p.render_id<<",\"token\":"<<p.token<<",\"revision\":"<<p.revision<<",\"wrapper\":"<<e.wrapper
            <<",\"resource\":"<<e.output.resource<<",\"status\":"<<uint32_t(e.output.status)<<",\"scene_begin_epoch\":"<<s.resource_before.descriptors.epoch
            <<",\"scene_begin_serial\":"<<s.resource_before.descriptors.serial<<",\"epoch\":"<<e.output.descriptors.epoch<<",\"serial_at_capture\":"<<e.output.descriptors.serial
            <<",\"lease_verified\":false,\"final_writer_verified\":false,\"rtvs\":[";
        for(size_t k=0;k<e.output.descriptors.views.size();++k){if(k)out<<',';const auto& v=e.output.descriptors.views[k];out<<"{\"resource\":"<<v.resource<<",\"identity\":"<<v.identity
            <<",\"handle\":"<<v.handle<<",\"known\":"<<(v.known?"true":"false")<<",\"created\":"<<v.created<<",\"bound\":"<<v.bound<<",\"command_list\":"<<v.command_list
            <<",\"width\":"<<v.width<<",\"height\":"<<v.height<<",\"storage\":"<<v.storage<<",\"format\":"<<v.format<<",\"dimension\":"<<v.dimension
            <<",\"flags\":"<<v.flags<<",\"subresources\":"<<v.subresources<<'}';}out<<"]}";}
    out<<"]}\n";
}
