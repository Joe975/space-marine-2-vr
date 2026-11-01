#include "EngineWeaponProbe.h"
#include "GameBuild.h"
#include "BoundedTable.h"
#include <MinHook.h>
#include <intrin.h>
#include <algorithm>
#include <cstring>

namespace {
thread_local EngineWeaponProbe::Sample* current_ray{};
thread_local EngineWeaponProbe::AimSample* current_aim{};
template<class T> bool read(uintptr_t address,T& value) {
    SIZE_T count{};return address && address<=UINTPTR_MAX-sizeof(T) &&
        ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(T),&count) && count==sizeof(T);
}
bool vector_valid(const std::array<float,3>& p) { return std::ranges::all_of(p,[](float v){return std::isfinite(v) && std::abs(v)<1000000;}); }
}
EngineWeaponProbe& EngineWeaponProbe::get() { static auto* value=new EngineWeaponProbe;return *value; }
bool EngineWeaponProbe::matches_build(uint32_t stamp,std::span<const uint8_t> bytes) {
    return game_build::code(stamp,RAY_RVA,bytes,PROLOGUE);
}
bool EngineWeaponProbe::install_in_game(HMODULE game) {
    if(installed()) return true;
    auto* base=reinterpret_cast<uint8_t*>(game);if(!base) return false;
    const auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto stamp=nt->FileHeader.TimeDateStamp;const auto ray=game_build::rva(stamp,RAY_RVA),aim=game_build::rva(stamp,AIM_RVA);
    if(!ray || !aim || nt->OptionalHeader.SizeOfImage<ray+PROLOGUE.size() || !matches_build(stamp,{base+ray,PROLOGUE.size()}) ||
        !game_build::code(stamp,AIM_RVA,std::span<const uint8_t>{base+aim,AIM_PROLOGUE.size()},AIM_PROLOGUE)) return false;
    if(!install(base+ray,reinterpret_cast<uintptr_t>(base),[]{return EngineBodyProbe::get().active_actor();})) return false;
    if(install_aim(base+aim)) return true;
    uninstall();return false;
}
bool EngineWeaponProbe::install(void* target,uintptr_t module,std::function<uintptr_t()> actor) {
    if(installed()) return true;
    if(!target || !actor || !EngineBodyProbe::get().installed()) return false;
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    if(MH_CreateHook(target,reinterpret_cast<void*>(&ray_hook),reinterpret_cast<void**>(&m_original))!=MH_OK) return false;
    m_target=target;m_module=module;m_active_actor=std::move(actor);
    EngineBodyProbe::get().set_world_observer(&world_observer);
    if(MH_EnableHook(target)!=MH_OK) { uninstall();return false; }
    m_installed=true;return true;
}
bool EngineWeaponProbe::install_aim(void* update) {
    if(m_aim_target) return true;
    if(!installed() || !update || MH_CreateHook(update,reinterpret_cast<void*>(&aim_hook),reinterpret_cast<void**>(&m_original_aim))!=MH_OK) return false;
    if(MH_EnableHook(update)!=MH_OK) {MH_RemoveHook(update);return false;}
    m_aim_target=update;return true;
}
void EngineWeaponProbe::uninstall() {
    m_enabled=false;m_driver.enable(false);EngineBodyProbe::get().set_world_observer(nullptr);
    if(m_target) { MH_DisableHook(m_target);MH_RemoveHook(m_target); }
    if(m_aim_target) { MH_DisableHook(m_aim_target);MH_RemoveHook(m_aim_target); }
    m_target=m_aim_target=nullptr;m_installed=false;
}
EngineWeaponProbe::Snapshot EngineWeaponProbe::snapshot() const { std::scoped_lock lock{m_mutex};return m_snapshot; }
std::optional<EngineWeaponProbe::OwnedTarget> EngineWeaponProbe::owned_target(std::chrono::steady_clock::time_point now) const {
    if(!m_enabled || !m_active_actor) return {};
    uintptr_t actor{};OwnedTarget target{};std::array<float,3> steered{};std::chrono::steady_clock::time_point received{},steered_received{};
    {std::scoped_lock lock{m_mutex};actor=m_ray_actor;target={m_ray_target,m_ray_steered};received=m_ray_received;steered=m_steered_target;steered_received=m_steered_received;}
    if(!actor || !VrWeaponAim::fresh(received,now) || m_active_actor()!=actor) return {};
    if(VrWeaponAim::fresh(steered_received,now)) return OwnedTarget{steered,true};
    return target;
}
void EngineWeaponProbe::ray_hook(void* object) {
    auto& self=get();
    if(!self.m_enabled) {self.m_original(object);return;}
    Sample sample{};sample.control=reinterpret_cast<uintptr_t>(object);
    uintptr_t wrapper{};
    if(read(sample.control+8,sample.scene) && sample.scene && read(sample.scene+0x20,sample.tracker) &&
        read(sample.scene+0x38,wrapper) && wrapper) read(wrapper,sample.scene_owner);
    sample.active_actor=self.m_active_actor();self.resolve_owner(sample);
    if(sample.owned) sample.driven=self.m_driver.ray_pose(sample.actor);
    read(sample.control+0x30,sample.max_distance);
    auto* previous=current_ray;current_ray=&sample;
    struct Restore { Sample* value;~Restore(){current_ray=value;} } restore{previous};
    self.m_original(object); // Original ray/collision work always runs exactly once.
    try {self.observe(sample);} catch(...) {}
}
void EngineWeaponProbe::resolve_owner(Sample& sample) const {
    uintptr_t root{};
    if(!sample.active_actor || !read(sample.active_actor+0x38,root) || !root) return;
    // Native TrackerSceneTransform::World (0x18e4e60) dereferences the weak SceneTransform
    // at tracker+0xd0 and copies its world matrix. Follow only that verified tracker type.
    std::array<uintptr_t,12> seen{};
    auto scene=sample.scene;
    for(uint32_t depth=0;depth<seen.size() && scene;++depth) {
        if(std::find(seen.begin(),seen.begin()+depth,scene)!=seen.begin()+depth) return;
        seen[depth]=scene;
        if(scene==root) {
            uintptr_t current_root{};
            if(read(sample.active_actor+0x38,current_root) && current_root==root) {
                sample.actor=sample.active_actor;sample.owned=true;sample.root_scene=root;sample.parent_depth=depth;
            }
            return;
        }
        uintptr_t tracker{},vtable{},method{},wrapper{},parent{};
        if(!read(scene+0x20,tracker) || !tracker || !read(tracker,vtable) || !vtable ||
            !read(vtable+0x48,method) || method<m_module || method-m_module!=game_build::rva(0x18e4e60) ||
            !read(tracker+0xd0,wrapper) || !wrapper || !read(wrapper,parent)) return;
        scene=parent;
    }
}
void EngineWeaponProbe::world_observer(uintptr_t caller,void* object,float* world,bool valid) {
    auto& self=get();const auto now=std::chrono::steady_clock::now();
    // CtrlTarget asks for its parent ray frame. Only this exact owned call is changed.
    if(auto* ray=current_ray;ray && ray->owned && ray->driven && reinterpret_cast<uintptr_t>(object)==ray->tracker && valid && self.m_active_actor()==ray->actor) {
        const auto current=self.m_driver.ray_pose(ray->actor,now);
        if(current && current->epoch==ray->driven->epoch && current->sequence==ray->driven->sequence) {
            std::memcpy(world,ray->driven->world.data(),sizeof(vr_camera::Matrix));ray->overridden=true;
        }
    }
    // Other target-sync consumers read the same target SceneTransform through aliases.
    // Do not touch its persistent cache, actor roots, gun locators or arbitrary cameras.
    // The getter runs for every entity on every thread: without a fresh owned ray (no gun aimed in the
    // last 100 ms) return before the driver's and this probe's locks (MOD_REVIEW_2026-10-29).
    const auto ray_ticks=self.m_ray_received_ticks.load(std::memory_order_relaxed);
    const bool ray_fresh=ray_ticks && VrWeaponAim::fresh(std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{ray_ticks}},now);
    if(!current_ray && valid && ray_fresh && self.m_driver.enabled()) {
        uintptr_t vtable{},method{},wrapper{},scene{},owned_scene{},owned_actor{};std::chrono::steady_clock::time_point received{};
        {std::scoped_lock lock{self.m_mutex};owned_scene=self.m_ray_scene;owned_actor=self.m_ray_actor;received=self.m_ray_received;}
        if(owned_scene && VrWeaponAim::fresh(received,now) && self.m_active_actor()==owned_actor &&
            read(reinterpret_cast<uintptr_t>(object),vtable) && read(vtable+0x48,method) && method>=self.m_module && method-self.m_module==game_build::rva(0x18e4e60) &&
            read(reinterpret_cast<uintptr_t>(object)+0xd0,wrapper) && wrapper && read(wrapper,scene) && scene==owned_scene) {
            Sample owner{};owner.scene=scene;owner.active_actor=owned_actor;self.resolve_owner(owner);
            if(const auto hit=self.m_driver.hit(owned_actor,now);hit && owner.owned) {
                auto target=hit->pose.world;for(int i=0;i<3;++i) target[12+i]=hit->target[i];
                std::memcpy(world,target.data(),sizeof(target));std::scoped_lock lock{self.m_mutex};++self.m_snapshot.target_overrides;
            }
        }
    }
    if(auto* aim=current_aim;aim && aim->read_count<aim->reads.size()) {
        auto& value=aim->reads[aim->read_count++];auto& self=get();
        value.tracker=reinterpret_cast<uintptr_t>(object);value.caller=caller>=self.m_module?caller-self.m_module:0;
        uintptr_t vtable{},method{};
        if(read(value.tracker,vtable) && read(vtable+0x48,method) && method>=self.m_module) value.method=method-self.m_module;
        uintptr_t wrapper{};if(value.method==game_build::rva(0x18e4e60) && read(value.tracker+0xd0,wrapper) && wrapper) read(wrapper,value.scene);
        value.valid=valid && read(reinterpret_cast<uintptr_t>(world),value.world) && vr_camera::rigid(value.world);
    }
    auto* sample=current_ray;if(!sample || reinterpret_cast<uintptr_t>(object)!=sample->tracker) return;
    sample->caller=caller>=self.m_module?caller-self.m_module:0;
    sample->valid=valid && read(reinterpret_cast<uintptr_t>(world),sample->world) && vr_camera::rigid(sample->world);
    read(sample->tracker,sample->tracker_words);
    uintptr_t method{};
    if(read(sample->tracker_words[0]+0x48,method) && method>=self.m_module) sample->world_method_rva=method-self.m_module;
    std::scoped_lock lock{self.m_mutex};++self.m_snapshot.worlds;
}
void EngineWeaponProbe::aim_hook(void* interface,void* arguments) {
    auto& self=get();
    if(!self.m_enabled) {self.m_original_aim(interface,arguments);return;}
    AimSample sample{};const auto part=reinterpret_cast<uintptr_t>(interface);
    sample.control=part>=0x20?part-0x20:0;sample.actor=self.m_active_actor();
    uintptr_t link{},entity{},root{},geometry{},owned_geometry{},properties{},geometry_properties{},input{};
    const bool owned=sample.control && sample.actor && read(sample.control+0x10,link) && link && read(link,sample.controller) && sample.controller &&
        read(sample.controller+8,entity) && entity && read(sample.actor+0x38,root) && entity==root &&
        read(sample.controller+0xe0,link) && link && read(link,geometry) && geometry &&
        read(sample.actor+0xd8,owned_geometry) && geometry==owned_geometry &&
        read(sample.actor+0x20,properties) && properties && read(geometry+0x58,geometry_properties) && geometry_properties==properties;
    if(owned) {
        sample.target_valid=read(reinterpret_cast<uintptr_t>(arguments),input) && read(input,sample.input_target) && vr_camera::rigid(sample.input_target);
        read(sample.control+0x64,sample.direction_only);
    }
    auto* previous=current_aim;current_aim=owned?&sample:nullptr;
    struct Restore {AimSample* value;~Restore(){current_aim=value;}} restore{previous};
    self.m_original_aim(interface,arguments); // Preserve native animation parameters and writes.
    sample.received=std::chrono::steady_clock::now();
    if(owned) read(sample.control+0xe0,sample.aimed);
    std::scoped_lock lock{self.m_mutex};++self.m_snapshot.aim_calls;
    if(owned) {++self.m_snapshot.aim_owned;sample.calls=self.m_snapshot.aim && self.m_snapshot.aim->control==sample.control?self.m_snapshot.aim->calls+1:1;self.m_snapshot.aim=sample;}
    else if(self.m_snapshot.aim && (self.m_snapshot.aim->control==sample.control || self.m_snapshot.aim->actor!=sample.actor)) self.m_snapshot.aim.reset();
}
void EngineWeaponProbe::observe(Sample& sample) {
    sample.received=std::chrono::steady_clock::now();
    const bool target_valid=read(sample.control+0x10c,sample.target) && vector_valid(sample.target);
    if(sample.overridden && target_valid && sample.driven && m_active_actor()==sample.actor) m_driver.hit(*sample.driven,sample.target,sample.received);
    std::scoped_lock lock{m_mutex};++m_snapshot.calls;
    if(!sample.scene || !sample.tracker || !sample.valid || !target_valid || !std::isfinite(sample.max_distance)) { ++m_snapshot.rejected;return; }
    if(sample.owned) {m_ray_scene=sample.scene;m_ray_actor=sample.actor;m_ray_target=sample.target;m_ray_steered=sample.overridden;m_ray_received=sample.received;m_ray_received_ticks.store(sample.received.time_since_epoch().count(),std::memory_order_relaxed);
        if(sample.overridden) {m_steered_target=sample.target;m_steered_received=sample.received;}}
    if(sample.overridden) ++m_snapshot.ray_overrides;
    // A full table gives the least recently seen row to the new control (BoundedTable.h).
    if(bounded_table::upsert(m_snapshot.targets,sample,[&](const Sample& row){return row.control==sample.control;},32)==bounded_table::Result::dropped) {
        ++m_snapshot.overflow;
        if(sample.owned) {
            const auto other=std::ranges::find(m_snapshot.targets,false,&Sample::owned);
            if(other!=m_snapshot.targets.end()) {sample.calls=1;*other=sample;}
        }
    }
}
