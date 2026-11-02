#include "NativeRenderProbe.h"
#include "ForeignViewHold.h"
#include "GameBuild.h"
#include "NativeOrdinaryPacket.h"
#include "NativeSceneFaultEvidence.h"
#include "PerformanceTrace.h"
#include "NativeMemory.h"
#include "ScenePhaseTiming.h"
#include "SceneExecutionTiming.h"
#include "SceneQueryDependencyTiming.h"
#include "NativeObjectVisibilityProbe.h"
#include "NativeWorkerVisibilityProbe.h"
#include <MinHook.h>
#include <algorithm>
#include <cmath>

namespace {
template<class T> bool read(uintptr_t address,T& value) {return native_memory::read(address,value);}
}
NativeRenderProbe& NativeRenderProbe::get() {static auto* instance=new NativeRenderProbe;return *instance;}
bool NativeRenderProbe::matches(uint32_t stamp,std::span<const uint8_t> bytes) {
    return game_build::code(stamp,DISPATCH_RVA,bytes,PROLOGUE);
}
bool NativeRenderProbe::matches_scene(uint32_t stamp,std::span<const uint8_t> bytes) {
    return game_build::code(stamp,SCENE_RVA,bytes,SCENE_PROLOGUE);
}
bool NativeRenderProbe::install_in_game(HMODULE module,std::function<bool()> enabled,OutputObserver outputs) {
    const auto base=reinterpret_cast<uintptr_t>(module);IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};std::array<uint8_t,PROLOGUE.size()> bytes{};
    std::array<uint8_t,SCENE_PROLOGUE.size()> scene_bytes{};
    if(!read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 || !read(base+dos.e_lfanew,nt) ||
        nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || !game_build::supported(nt.FileHeader.TimeDateStamp) || nt.OptionalHeader.SizeOfImage<LIST_RVA+8 ||
        !read(base+game_build::rva(DISPATCH_RVA),bytes) || !matches(nt.FileHeader.TimeDateStamp,bytes) ||
        !read(base+game_build::rva(SCENE_RVA),scene_bytes) || !matches_scene(nt.FileHeader.TimeDateStamp,scene_bytes)) return false;
    return install(reinterpret_cast<void*>(base+game_build::rva(DISPATCH_RVA)),[base]{uintptr_t list{};read(base+game_build::rva(LIST_RVA),list);return list;},std::move(enabled),std::move(outputs));
}
bool NativeRenderProbe::attach_scene_hook(void* target) {
    std::lock_guard lock{m_install};if(!m_original || m_scene_target || !target) return false;
    m_scene_target=reinterpret_cast<uintptr_t>(target);return true;
}
bool NativeRenderProbe::install(void* target,std::function<uintptr_t()> list,std::function<bool()> enabled,OutputObserver outputs) {
    std::lock_guard lock{m_install};if(m_original || !target || !list || !enabled) return false;
    const auto initialized=MH_Initialize();if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED) return false;
    void* original{};if(MH_CreateHook(target,reinterpret_cast<void*>(&on_dispatch),&original)!=MH_OK) return false;
    m_list=std::move(list);m_enabled=std::move(enabled);m_outputs=std::move(outputs);m_original=reinterpret_cast<Dispatch>(original);
    if(MH_EnableHook(target)!=MH_OK) {m_original=nullptr;MH_RemoveHook(target);return false;}return true;
}
NativeRenderProbe::Output NativeRenderProbe::read_output(uintptr_t wrapper,const OutputObserver& observer) {
    Output value;if(!observer) return value;
    value.status=OutputStatus::unreadable;
    if(!wrapper || wrapper>UINTPTR_MAX-0x280 || !read(wrapper+0x278,value.resource) || !value.resource) return value;
    try {
        value.descriptors=observer(value.resource);
        uintptr_t after{};
        if(!read(wrapper+0x278,after) || after!=value.resource) value.status=OutputStatus::changed;
        else if(value.descriptors.views.size()>8) {value.descriptors.views.clear();value.status=OutputStatus::overflow;}
        else value.status=value.descriptors.views.empty()?OutputStatus::unobserved:OutputStatus::observed;
    } catch(...) {value.descriptors={};value.status=OutputStatus::unobserved;}
    return value;
}
std::optional<NativeRenderProbe::Scene::PersistentBank> NativeRenderProbe::read_history_bank(uintptr_t renderer,uint32_t slot) {
    constexpr size_t offset=0x200,stride=0x58;
    if(slot>1 || !renderer || renderer>UINTPTR_MAX-offset-size_t(slot)*stride-sizeof(Scene::PersistentBank::words))return {};
    Scene::PersistentBank value;value.address=renderer+offset+size_t(slot)*stride;std::array<uintptr_t,11> again{};
    if(!read(value.address,value.words) || !read(value.address,again) || value.words!=again)return {};
    return value;
}
std::optional<std::vector<NativeRenderProbe::View>> NativeRenderProbe::read_views(uintptr_t list) {
    if(!list || list>UINTPTR_MAX-0x2996c) return std::nullopt;
    int32_t count{};if(!read(list+0x29968,count) || count<1 || count>64) return std::nullopt;
    std::vector<View> result;
    // One system call per main/PIP view entry (bytes 0..0xa54: every field below), instead of ~45 field reads.
    static constexpr size_t ENTRY_BYTES=0xa54;
    thread_local native_memory::Block<ENTRY_BYTES> block;
    for(int32_t i=0;i<count;++i) {
        View value;value.slot=i;const auto entry=list+0x168+size_t(i)*0xa60;
        if(!block.read_from(entry)) return std::nullopt;
        block.get(0,value.flags);
        if(!value.pip() && !value.main()) continue;
        block.get(0xa50,value.record);block.get(0x34,value.world);block.get(0x3dc,value.view);block.get(0x45c,value.projection);
        block.get(0x160,value.rectangle);block.get(0x304,value.fov_h);block.get(0x308,value.fov_v);block.get(0x30c,value.aspect);
        if(!vr_camera::rigid(value.world,false) || !vr_camera::rigid(value.view,false) ||
            !std::ranges::all_of(value.projection,[](float x){return std::isfinite(x);}) || value.projection[0]<=0 || value.projection[5]<=0 ||
            !std::ranges::all_of(value.rectangle,[](float x){return std::isfinite(x);}) || value.rectangle[2]<=0 || value.rectangle[3]<=0 ||
            !std::isfinite(value.fov_h) || !std::isfinite(value.fov_v) || !std::isfinite(value.aspect) || value.aspect<=0) return std::nullopt;
        block.get(0x4dc,value.inverse_projection);block.get(0x55c,value.combined);block.get(0xc0,value.corners);block.get(0x32c,value.cull_radius);
        if(!std::isfinite(value.cull_radius) || !std::ranges::all_of(value.corners,[](float x){return std::isfinite(x);})) return std::nullopt;
        for(size_t plane=0;plane<6;++plane) for(size_t component=0;component<4;++component){
            block.get(0x194+plane*0x14+component*4,value.planes[plane*4+component]);
            if(!std::isfinite(value.planes[plane*4+component])) return std::nullopt;}
        block.get(0x340,value.cull_distance);block.get(0x344,value.cull_directions);block.get(0x374,value.cull_sides);
        block.get(0x3a4,value.cull_normal);block.get(0x3b0,value.cull_origin);
        if(value.main())value.packet=NativeCameraPacket::capture(entry);
        result.push_back(value);
    }
    return result.empty()?std::nullopt:std::optional{result};
}
// Raw entries, not read_views: that skips a view whose matrices it doesn't accept, and this must not
// miss the one frame that crashes. Flags alone in the common case, which is every dispatch.
bool NativeRenderProbe::remap_pip_hdr(uintptr_t list) {
    if(!list || list>UINTPTR_MAX-0x2996c) return false;
    int32_t count{};if(!read(list+0x29968,count) || count<3 || count>64) return false;
    const auto entry=[&](int32_t slot){return list+0x168+size_t(slot)*0xa60;};
    std::array<ForeignViewHold::View,16> views{};size_t n{};
    for(int32_t i=0;i<count && n<views.size();++i) {views[n].slot=i;if(!read(entry(i),views[n].flags)) return false;++n;}
    const std::span<const ForeignViewHold::View> seen{views.data(),n};
    if(!ForeignViewHold::behind_pair(seen)) return false;
    for(size_t i=0;i<n;++i) if(!read(entry(views[i].slot)+0x188,views[i].hdr)) return false;
    const auto slot=ForeignViewHold::hdr_remap(seen);if(!slot) return false;
    const int32_t index=ForeignViewHold::PIP_HDR_INDEX;SIZE_T written{};
    if(!WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(entry(*slot)+0x188),&index,sizeof(index),&written) || written!=sizeof(index)) return false;
    ForeignViewHold::get().remapped();return true;
}
void NativeRenderProbe::on_dispatch(void* renderer,uint32_t flags) {
    PerformanceTrace::Scope timing{"ordinary_dispatch_total_cpu"};
    struct Scope {
        uintptr_t previous{s_current_renderer};std::optional<NativeCameraEntries::Pair> entries{s_current_entries};
        Snapshot* snapshot{s_current_snapshot};
        std::optional<size_t> scene{s_current_scene};std::optional<NativeCameraEntries::Primary> primary{s_current_primary};
        Scope(uintptr_t value){s_current_renderer=value;s_current_entries.reset();s_current_primary.reset();s_current_scene.reset();s_current_snapshot=nullptr;}
        ~Scope(){s_current_renderer=previous;s_current_entries=entries;s_current_snapshot=snapshot;s_current_scene=scene;s_current_primary=primary;}
    } scope{reinterpret_cast<uintptr_t>(renderer)};
    auto& probe=get();std::optional<Snapshot> sample;
    try {
        if(probe.m_enabled()) {
            Snapshot value;value.sequence=++probe.m_sequence;value.renderer=reinterpret_cast<uintptr_t>(renderer);value.flags=flags;
            value.begin=std::chrono::steady_clock::now();value.list=probe.m_list();
            remap_pip_hdr(value.list);
            const auto revision=NativeCameraEntries::get().revision(value.list);
            value.camera_revision=revision;value.scene_observer=probe.scene_installed();value.scenes.reserve(MAX_SCENES);
            if(auto before=read_views(value.list)) {
                value.before=std::move(*before);std::vector<NativeCameraEntries::Entry> entries;
                for(const auto& view:value.before) entries.push_back({value.list+0x168+size_t(view.slot)*0xa60,view.slot,
                    view.pip()?NativeCameraEntries::Role::pip:view.main()?NativeCameraEntries::Role::main:NativeCameraEntries::Role::other,view.world});
                if(revision) {value.camera_entries=NativeCameraEntries::get().pair(value.list,entries,*revision);value.primary=NativeCameraEntries::get().primary(value.list,entries,*revision);value.ordinary_pair=NativeCameraEntries::get().ordinary_pair(value.list,entries,*revision);}
                NativeOrdinaryPacket::get().prepare(value);sample=std::move(value);
            }
        }
    } catch(...) {}
    if(sample) {s_current_entries=sample->camera_entries;s_current_primary=sample->primary;s_current_snapshot=&*sample;}
    PerformanceTrace::get().thread_role(PerformanceTrace::Role::render);
    // dispatch_thread_cpu: the render thread's own CPU inside the two-view dispatch (value = its wall time).
    {PerformanceTrace::Scope native_timing{"ordinary_dispatch_native_cpu"};PerformanceTrace::CpuScope native_cpu{"ordinary_dispatch_thread_cpu"};probe.m_original.load()(renderer,flags);}
    try {
        if(sample && sample->list==probe.m_list()) if(auto after=read_views(sample->list)) {
            sample->after=std::move(*after);sample->end=std::chrono::steady_clock::now();
            if(sample->camera_entries && !NativeCameraEntries::get().current(*sample->camera_entries)) sample->camera_entries.reset();
            if(sample->primary && !NativeCameraEntries::get().current(*sample->primary)) sample->primary.reset();
            if(sample->ordinary_pair && !NativeCameraEntries::get().current(*sample->ordinary_pair))sample->ordinary_pair.reset();
            if(sample->ordinary_pair)for(auto& scene:sample->scenes)
                if(scene.valid() && scene.slot<2 && scene.renderer==sample->renderer && scene.persistent_before)
                    scene.persistent_after=read_history_bank(scene.renderer,scene.slot);
            NativeOrdinaryPacket::get().complete(*sample);
            std::function<void(const Snapshot&)> complete;{std::lock_guard lock{probe.m_install};complete=probe.m_complete;}if(complete)complete(*sample);
            std::lock_guard lock{probe.m_mutex};probe.m_latest=std::move(sample);
        }
    } catch(...) {}
}
void NativeRenderProbe::observe_scene(void* renderer,uint32_t flags,int32_t native_slot,SceneRender original) {
    auto& probe=get();auto* sample=s_current_snapshot;
    const auto slot=uint32_t(native_slot);
    std::optional<size_t> index;vr_camera::Matrix world{};
    const auto address=reinterpret_cast<uintptr_t>(renderer);
    try {
        if(sample) {
            const auto ordinal=++sample->scene_calls;
            if(sample->scenes.size()>=MAX_SCENES) ++sample->scene_dropped;
            else {
                sample->scenes.emplace_back();index=sample->scenes.size()-1;auto& scene=sample->scenes[*index];
                scene.ordinal=ordinal;scene.dispatch_sequence=sample->sequence;scene.flags=flags;scene.slot=slot;scene.list=sample->list;scene.renderer=address;
                scene.camera_revision_before=NativeCameraEntries::get().revision(scene.list);
                int32_t count{};
                if(sample->renderer!=address) scene.refusal=SceneRefusal::renderer;
                else if(probe.m_list()!=scene.list) scene.refusal=SceneRefusal::list;
                else if(slot>=64) scene.refusal=SceneRefusal::slot;
                else if(!scene.list || scene.list>UINTPTR_MAX-0x29c7c || !address || address>UINTPTR_MAX-0x148 ||
                        !read(scene.list+0x29968,count)) scene.refusal=SceneRefusal::unreadable;
                else if(count<1 || count>64 || slot>=uint32_t(count)) scene.refusal=SceneRefusal::slot;
                else {
                    scene.entry=scene.list+0x168+size_t(slot)*0xa60;
                    if(read(scene.entry,scene.view_flags_before) && read(scene.entry+0x34,world) &&
                       read(scene.list+0x29c78,scene.render_counter_before) && read(address+0x100,scene.output_before)) {
                        scene.resource_before=read_output(scene.output_before,probe.m_outputs);
                        const auto view=std::ranges::find(sample->before,int32_t(slot),&View::slot);
                        bool same=view!=sample->before.end() && view->flags==scene.view_flags_before;
                        for(size_t i=0;i<world.size() && same;++i) same=std::isfinite(world[i]) && std::abs(world[i]-view->world[i])<=1e-4f;
                        scene.refusal=!same?SceneRefusal::view:scene.camera_revision_before!=sample->camera_revision?SceneRefusal::revision:SceneRefusal::none;
                        Scene::Temporal temporal;if(read(address+0x150,temporal.previous) && read(address+0x1a8,temporal.next))scene.temporal_before=temporal;
                        if(scene.valid() && sample->ordinary_pair && slot<2 && (scene.view_flags_before&0x50000044)==0)
                            scene.persistent_before=read_history_bank(address,slot);
                        if(scene.valid() && (scene.view_flags_before&0x50000044)==0)scene.packet_before=NativeCameraPacket::capture(scene.entry);
                    }
                }
            }
        }
    } catch(...) {if(sample && !index) ++sample->scene_dropped;}
    // Observation never retries, changes, or suppresses the engine's actual draw.
    {
        struct SceneScope {std::optional<size_t> previous{s_current_scene};SceneScope(std::optional<size_t> value){s_current_scene=value;}~SceneScope(){s_current_scene=previous;}} scene_scope{index};
        PerformanceTrace::Scope scene_timing{slot==0?"ordinary_scene_left_cpu":slot==1?"ordinary_scene_right_cpu":"native_scene_other_cpu"};
        PerformanceTrace::CpuScope scene_cpu{slot==0?"ordinary_scene_left_thread_cpu":slot==1?"ordinary_scene_right_thread_cpu":"native_scene_other_thread_cpu"};
        const bool detail=PerformanceTrace::get().probes() && index && sample->scenes[*index].valid() && !(sample->scenes[*index].view_flags_before&0x50000044);
        ScenePhaseTiming phases{slot,sample?sample->sequence:0,detail?PerformanceTrace::now_ms():0,detail};
        SceneExecutionTiming execution{slot,sample?sample->sequence:0,detail};
        SceneQueryDependencyTiming dependencies{slot,sample?sample->sequence:0,detail && SceneQueryDependencyTiming::enabled};
        NativeObjectVisibilityProbe::Scope visibility{NativeObjectVisibilityProbe::enabled()?current_scene():std::nullopt,NativeObjectVisibilityProbe::enabled()?current_ordinary_pair():std::nullopt};
        NativeObjectVisibilityCausalProbe::Scope causal{NativeObjectVisibilityCausalProbe::enabled()?current_scene():std::nullopt,NativeObjectVisibilityCausalProbe::enabled()?current_ordinary_pair():std::nullopt};
        NativeWorkerVisibilityProbe::Scope workers{NativeWorkerVisibilityProbe::enabled()?current_scene():std::nullopt,NativeWorkerVisibilityProbe::enabled()?current_ordinary_pair():std::nullopt};
        // Read by the level-load fault capture, if the game faults inside this scene.
        NativeSceneFaultEvidence::Scope fault_scene{reinterpret_cast<uintptr_t>(renderer),flags,uint32_t(native_slot),sample?sample->sequence:0};
        original(renderer,flags,native_slot);
        workers.finish();
        causal.finish();
        visibility.finish();
        if(detail && SceneQueryDependencyTiming::enabled)dependencies.finish(PerformanceTrace::now_ms());
        execution.finish();
        if(detail)phases.finish(PerformanceTrace::now_ms());
    }
    try {
        if(index) {
            auto& scene=sample->scenes[*index];
            if(scene.valid()) {
                vr_camera::Matrix after{};int32_t count{};
                scene.camera_revision_after=NativeCameraEntries::get().revision(scene.list);
                if(probe.m_list()!=scene.list) scene.refusal=SceneRefusal::list;
                else if(!read(scene.list+0x29968,count) || !read(scene.list+0x29c78,scene.render_counter_after) ||
                        !read(scene.entry,scene.view_flags_after) || !read(scene.entry+0x34,after) ||
                        !read(address+0x140,scene.active_entry_after) || !read(address+0x100,scene.output_after)) scene.refusal=SceneRefusal::unreadable;
                else if(count<1 || count>64 || slot>=uint32_t(count)) scene.refusal=SceneRefusal::slot;
                else if(scene.active_entry_after!=scene.entry) scene.refusal=SceneRefusal::entry;
                else if(scene.render_counter_before!=scene.render_counter_after) scene.refusal=SceneRefusal::render_counter;
                else if(scene.camera_revision_before!=scene.camera_revision_after) scene.refusal=SceneRefusal::revision;
                else {
                    bool same=scene.view_flags_before==scene.view_flags_after;
                    for(size_t i=0;i<after.size() && same;++i) same=std::isfinite(after[i]) && std::abs(after[i]-world[i])<=1e-4f;
                    if(!same) scene.refusal=SceneRefusal::view;
                }
                scene.resource_after=read_output(scene.output_after,probe.m_outputs);
                Scene::Temporal temporal;if(read(address+0x150,temporal.previous) && read(address+0x1a8,temporal.next))scene.temporal_after=temporal;
                if(scene.valid() && (scene.view_flags_after&0x50000044)==0)scene.packet_after=NativeCameraPacket::capture(scene.entry);
            }
            if(!scene.valid()) ++sample->scene_refused;
        }
    } catch(...) {if(index) {sample->scenes[*index].refusal=SceneRefusal::unreadable;++sample->scene_refused;}}
}
std::optional<NativeRenderProbe::Snapshot> NativeRenderProbe::latest() const {std::lock_guard lock{m_mutex};return m_latest;}
std::optional<NativeRenderProbe::Scene> NativeRenderProbe::current_scene() {
    if(!s_current_snapshot || !s_current_scene || *s_current_scene>=s_current_snapshot->scenes.size()) return std::nullopt;
    return s_current_snapshot->scenes[*s_current_scene];
}
