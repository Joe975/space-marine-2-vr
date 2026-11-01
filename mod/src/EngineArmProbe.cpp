#include "EngineArmProbe.h"
#include "EngineBodyProbe.h"
#include "GameBuild.h"
#include "VrCameraPose.h"
#include "VrArmPose.h"
#include <MinHook.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr game_build::Code UPDATE=game_build::hashed(0xe46dc0,16,"69441f5a3bfc4c8393f44f32c5c9e01bf9da6f068720e55e5e4d34d2846eeef9");
constexpr game_build::Code PUBLISH=game_build::hashed(0xe31800,16,"d4253f492edc7f71e1e5e2e16dda926b893d82227e18b5dfc6a85cc2c793a8bf");
bool copy(uintptr_t source,void* output,size_t size) {
    SIZE_T count{};
    return source && size && source<=UINTPTR_MAX-size && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(source),output,size,&count) && count==size;
}
template<class T> bool read(uintptr_t source,size_t offset,T& value) { return source && source<=UINTPTR_MAX-offset && copy(source+offset,&value,sizeof(T)); }
bool fresh(EngineArmProbe::Clock::time_point received,EngineArmProbe::Clock::time_point now) { return now>=received && now-received<=std::chrono::milliseconds(100); }
bool same(const EngineArmProbe::Owner& a,const EngineArmProbe::Owner& b) {
    return a.actor==b.actor && a.entity==b.entity && a.controller==b.controller && a.wrapper==b.wrapper && a.network==b.network && a.definition==b.definition && a.instance==b.instance;
}
}
EngineArmProbe& EngineArmProbe::get() { static auto* probe=new EngineArmProbe; return *probe; }
bool EngineArmProbe::matches_build(uint32_t stamp,std::span<const uint8_t> update,std::span<const uint8_t> publish) {
    return game_build::code(stamp,UPDATE_RVA,update,UPDATE) && game_build::code(stamp,PUBLISH_RVA,publish,PUBLISH);
}
bool EngineArmProbe::install_in_game(HMODULE game) {
    if (installed()) { return true; }
    auto* base=reinterpret_cast<uint8_t*>(game); if (!base) { return false; }
    const auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto stamp=nt->FileHeader.TimeDateStamp;const auto update=game_build::rva(stamp,UPDATE_RVA),publish=game_build::rva(stamp,PUBLISH_RVA);
    if (!update || !publish || nt->OptionalHeader.SizeOfImage<update+UPDATE.size() ||
        !matches_build(stamp,{base+update,UPDATE.size()},{base+publish,PUBLISH.size()})) { return false; }
    return install(base+update,base+publish,[](auto now){return EngineBodyProbe::get().active_actor(now);},
        []{return EngineBodyProbe::get().active_actor();});
}
bool EngineArmProbe::install(void* update,void* publish,std::function<uintptr_t(Clock::time_point)> actor,std::function<uintptr_t()> actor_current) {
    if (installed()) { return true; }
    if (!update || !publish || !actor) { return false; }
    auto init=MH_Initialize(); if (init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) { return false; }
    m_actor=std::move(actor);
    m_actor_current=actor_current?std::move(actor_current):std::function<uintptr_t()>{[this]{return m_actor(Clock::now());}};
    if (MH_CreateHook(update,reinterpret_cast<void*>(&update_hook),reinterpret_cast<void**>(&m_update))!=MH_OK) { return false; }
    m_update_target=update;
    if (MH_CreateHook(publish,reinterpret_cast<void*>(&publish_hook),reinterpret_cast<void**>(&m_publish))!=MH_OK) { uninstall(); return false; }
    m_publish_target=publish;
    if (MH_EnableHook(update)!=MH_OK || MH_EnableHook(publish)!=MH_OK) { uninstall(); return false; }
    m_installed=true; return true;
}
void EngineArmProbe::enable(bool value) { std::scoped_lock lock{m_mutex}; m_enabled=value; if (!value) { m_owner.reset(); m_snapshot.pose.reset(); m_ambiguous_actor=0; } }
void EngineArmProbe::uninstall() {
    enable(false);
    for (auto target:{m_update_target,m_publish_target}) if (target) { MH_DisableHook(target); MH_RemoveHook(target); }
    m_update_target=m_publish_target=nullptr; m_installed=false;
}
std::optional<EngineArmProbe::Owner> EngineArmProbe::owner(uintptr_t controller,Clock::time_point now,std::optional<uintptr_t> actor_current) const {
    Owner row; row.actor=actor_current?*actor_current:m_actor(now); row.controller=controller; row.received=now;
    uintptr_t instance_link{},linked_instance{},owned_instance{},properties{},instance_properties{},entity{},definition_context{}; uint8_t ragdoll{},enabled{};
    if (!row.actor || !read(row.actor,0xd8,owned_instance) || !owned_instance ||
        !read(row.actor,0x20,properties) || !properties || !read(owned_instance,0x58,instance_properties) || instance_properties!=properties ||
        !read(controller,0xe0,instance_link) || !read(instance_link,0,linked_instance) || linked_instance!=owned_instance ||
        !read(row.actor,0x38,row.entity) || !read(controller,8,entity) || !row.entity || entity!=row.entity ||
        !read(controller,0x1e9,ragdoll) || ragdoll || !read(controller,0x1ea,enabled) || !enabled ||
        !read(controller,0xe8,row.wrapper) || !row.wrapper || !read(row.wrapper,0x2858,row.network) || !row.network ||
        !read(row.network,0,row.definition) || !row.definition || !read(row.wrapper,0,definition_context) ||
        !read(definition_context,8,entity) || entity!=row.definition ||
        !read(row.wrapper,0x10,row.instance) || row.instance!=owned_instance) { return std::nullopt; }
    return row;
}
EngineArmProbe::Snapshot EngineArmProbe::snapshot(Clock::time_point now) const {
    std::scoped_lock lock{m_mutex};return snapshot_locked(now);
}
EngineArmProbe::Snapshot EngineArmProbe::snapshot_current(TimeSource clock) const {
    std::lock_guard lock{m_mutex};
    // The current actor source owns its freshness clock. Read it before our
    // validation clock, keeping explicit snapshot(now)'s resolver unchanged.
    const auto actor=m_snapshot.pose && m_enabled?m_actor_current():uintptr_t{};
    return snapshot_locked(clock(),actor);
}
EngineArmProbe::Snapshot EngineArmProbe::snapshot_locked(Clock::time_point now,std::optional<uintptr_t> actor_current) const {
    auto result=m_snapshot;
    if (result.pose && (!fresh(result.pose->received,now) || !m_enabled || (actor_current?*actor_current:m_actor(now))!=result.pose->owner.actor)) { result.pose.reset(); }
    return result;
}
void EngineArmProbe::set_pose_observer(std::function<void(const Pose&)> observer) { std::scoped_lock lock{m_mutex}; m_pose_observer=std::move(observer); }
bool EngineArmProbe::current(const Pose& pose,Clock::time_point now) const {
    if (!m_enabled || !fresh(pose.received,now)) { return false; }
    const auto actual=owner(pose.owner.controller,now); uintptr_t buffer{};
    if (!actual || !same(*actual,pose.owner) || !read(actual->instance,0xd0,buffer) || buffer!=pose.buffer) { return false; }
    std::scoped_lock lock{m_mutex};
    return pose_current_locked(pose);
}
bool EngineArmProbe::current_current(const Pose& pose,TimeSource clock) const {
    if(!m_enabled)return false;
    // Do not pass a pre-Body-lock timestamp to the current actor resolver.
    const auto actual=owner(pose.owner.controller,{},m_actor_current());uintptr_t buffer{};
    if(!actual || !same(*actual,pose.owner) || !read(actual->instance,0xd0,buffer) || buffer!=pose.buffer)return false;
    return locked_clock_read(m_mutex,clock,[&](auto now){return fresh(pose.received,now) && pose_current_locked(pose);});
}
bool EngineArmProbe::pose_current_locked(const Pose& pose) const {
    return m_owner && same(*m_owner,pose.owner) && m_snapshot.pose && m_snapshot.pose->received==pose.received && m_ambiguous_actor!=pose.owner.actor;
}
std::shared_ptr<const EngineArmProbe::RigLayout> EngineArmProbe::rig_layout(uintptr_t rig,uint32_t count) {
    uintptr_t hierarchy{},parents{},names{},offsets{},text{},bind_attribute{},bind_buffer{},channels{},positions{},rotations{};
    uint32_t parent_count{},name_count{};
    if (!count || count>1024 || !read(rig,0x30,hierarchy) || !read(hierarchy,0,parent_count) || parent_count!=count ||
        !read(hierarchy,8,parents) || !read(rig,0x40,names) || !read(names,0,name_count) || name_count!=count ||
        !read(names,8,offsets) || !read(names,0x10,text) || !read(rig,0x48,bind_attribute) ||
        !read(bind_attribute,0x10,bind_buffer) || !read(bind_buffer,0x20,channels) ||
        !read(channels,0,positions) || !read(channels,8,rotations)) { return {}; }
    auto layout=std::make_shared<RigLayout>(); layout->parents.resize(count);
    std::vector<uint32_t> name_offsets(count);
    std::vector<std::array<float,4>> bind_positions(count),bind_rotations(count);
    if (!copy(parents,layout->parents.data(),count*4) || !copy(offsets,name_offsets.data(),count*4) ||
        !copy(positions,bind_positions.data(),count*16) || !copy(rotations,bind_rotations.data(),count*16)) { return {}; }
    for (uint32_t i=0;i<count;++i) {
        if (layout->parents[i]<-1 || layout->parents[i]>=static_cast<int32_t>(i) || name_offsets[i]>65536) { return {}; }
        std::string name;
        for (size_t j=0;j<128;++j) {
            char c{}; if (!read(text,static_cast<size_t>(name_offsets[i])+j,c)) { return {}; }
            if (!c) { break; }
            if (c<32 || c>126) { return {}; } name+=c;
        }
        if (name.empty() || name.size()==128 || std::find(layout->names.begin(),layout->names.end(),name)!=layout->names.end()) { return {}; }
        layout->names.push_back(std::move(name));
        auto q=bind_rotations[i]; const auto& p=bind_positions[i]; float norm{};
        for (float f:q) { norm+=f*f; } norm=std::sqrt(norm);
        if (!std::isfinite(norm) || std::abs(norm-1)>0.001f) { return {}; }
        for (auto& f:q) { f/=norm; }
        const auto x=q[0],y=q[1],z=q[2],w=q[3];
        Matrix matrix{1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w),0,
            2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w),0,
            2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y),0,p[0],p[1],p[2],1};
        if (!vr_camera::rigid(matrix)) { return {}; } layout->bind_locals.push_back(matrix);
    }
    return layout;
}
void EngineArmProbe::observe_controller(void* controller,Clock::time_point now) {
    if (!m_enabled) { return; }
    auto row=owner(reinterpret_cast<uintptr_t>(controller),now);
    std::scoped_lock lock{m_mutex}; ++m_snapshot.updates;
    // Unrelated NPC updates must not evict the player. Loss of the known player must.
    if (!row) {
        if (m_owner && (m_owner->controller==reinterpret_cast<uintptr_t>(controller) || m_actor(now)!=m_owner->actor)) { m_owner.reset(); m_snapshot.pose.reset(); }
        return;
    }
    ++m_snapshot.matched;
    if (row->actor==m_ambiguous_actor && fresh(m_ambiguous_received,now)) { return; }
    m_ambiguous_actor=0;
    if (m_owner && (row->controller!=m_owner->controller && row->actor==m_owner->actor && fresh(m_owner->received,now))) {
        // Multiple live controllers for one actor are ambiguous, not a nearest/last-writer choice.
        m_ambiguous_actor=row->actor; m_ambiguous_received=now;
        m_owner.reset(); m_snapshot.pose.reset(); ++m_snapshot.rejected; return;
    }
    if (!m_owner || !same(*row,*m_owner)) { m_snapshot.pose.reset(); }
    m_owner=row;
}
void EngineArmProbe::observe_publication(void* wrapper,uint64_t result,Clock::time_point now) {
    if (!m_enabled) { return; }
    std::unique_lock lock{m_mutex}; ++m_snapshot.publications;
    if (!m_owner || m_owner->wrapper!=reinterpret_cast<uintptr_t>(wrapper)) { return; }
    const auto verified=owner(m_owner->controller,now);
    if (!fresh(m_owner->received,now) || !verified || !same(*verified,*m_owner) || !(result&0xff)) { m_owner.reset(); m_snapshot.pose.reset(); ++m_snapshot.rejected; return; }
    Pose pose; pose.owner=*verified; pose.received=now;
    uintptr_t records{},map_object{},indices{}; int32_t active{}; uint32_t count{};
    if (!read(pose.owner.wrapper,0x2870,records) || !read(pose.owner.wrapper,0x2880,active) || active<0 || active>63 ||
        !read(records,static_cast<size_t>(active)*32+4,pose.animset) || !read(records,static_cast<size_t>(active)*32+8,map_object) ||
        !read(map_object,0,indices) || !read(map_object,8,count) || !count || count>1024 ||
        !read(pose.owner.instance,0xd0,pose.buffer) || !pose.buffer) { m_snapshot.pose.reset(); ++m_snapshot.rejected; return; }
    pose.mapping=indices;
    uintptr_t nodes{},root{},formats{},layout{},attributes{},rig_attribute{}; uint8_t stride{},first{};
    // Same animset-specific rig attribute selection used by native 0x2c93f20.
    if (read(pose.owner.definition,0xb0,nodes) && read(nodes,0,root) && read(root,0x90,formats) &&
        read(formats,4,stride) && read(formats,8,layout) && read(layout,0x15,first) &&
        read(root,0x48,attributes) && read(attributes,(static_cast<size_t>(first)+static_cast<size_t>(stride)*pose.animset)*16,rig_attribute)) {
        read(rig_attribute,0x10,pose.rig);
    }
    // Copy the exact map used by publication; don't infer live indices from asset strings.
    std::vector<int16_t> mapping(count);
    if (!copy(indices,mapping.data(),count*sizeof(int16_t))) { m_snapshot.pose.reset(); ++m_snapshot.rejected; return; }
    const auto highest=*std::max_element(mapping.begin(),mapping.end());
    if (highest<0 || highest>=4096 || std::any_of(mapping.begin(),mapping.end(),[](auto v){return v < -1;})) { m_snapshot.pose.reset(); ++m_snapshot.rejected; return; }
    std::vector<Matrix> locals(static_cast<size_t>(highest)+1);
    if (!copy(pose.buffer,locals.data(),locals.size()*sizeof(Matrix))) { m_snapshot.pose.reset(); ++m_snapshot.rejected; return; }
    for (uint32_t i=0;i<count;++i) if (mapping[i]>=0) {
        const auto& matrix=locals[mapping[i]];
        if (!std::all_of(matrix.begin(),matrix.end(),[](float v){return std::isfinite(v) && std::abs(v)<1000000;})) { m_snapshot.pose.reset(); ++m_snapshot.rejected; return; }
        pose.bones.push_back({i,mapping[i],matrix});
    }
    if (m_snapshot.pose && same(m_snapshot.pose->owner,pose.owner) && m_snapshot.pose->rig==pose.rig &&
        m_snapshot.pose->animset==pose.animset && m_snapshot.pose->layout && m_snapshot.pose->layout->names.size()==count) {
        pose.layout=m_snapshot.pose->layout;
    } else { pose.layout=rig_layout(pose.rig,count); }
    // The entity world, with a uniform scale divided out: some attack animations scale the
    // marine slightly (30 Oct, Ravener attacks), and refusing that world stood owned stereo
    // down for the whole attack (docs/RAVENER_ATTACK_2026-10-30.md).
    if (read(pose.owner.entity,0x98,pose.world)) {
        if (vr_camera::rigid(pose.world)) { pose.world_valid=true; }
        else if (const auto unscaled=vr_camera::unscaled(pose.world)) {
            pose.world=unscaled->matrix; pose.world_scale=unscaled->scale; pose.world_valid=true;
            ++m_snapshot.world_scaled; m_snapshot.last_world_scale=unscaled->scale;
        } else { ++m_snapshot.world_refused; m_snapshot.refused_world=pose.world; }
    }
    ++m_snapshot.captures; m_snapshot.pose=std::move(pose);
    auto observer=m_pose_observer;
    if (observer) { auto captured=*m_snapshot.pose; lock.unlock(); observer(captured); }
}
void EngineArmProbe::update_hook(void* controller,void* arguments) {
    auto& self=get();
    try { self.observe_controller(controller); } catch (...) {}
    self.m_update(controller,arguments);
    try { self.observe_controller(controller); } catch (...) {}
}
uint64_t EngineArmProbe::publish_hook(void* wrapper) {
    auto& self=get(); const auto result=self.m_publish(wrapper);
    const DWORD error=GetLastError();
    try { self.observe_publication(wrapper,result); } catch (...) {}
    SetLastError(error);
    return result;
}
