#include "PathText.h"
#include "EngineArmDriver.h"
#include "NativePlayerPartVisibility.h"
#include <cstring>
#include <numeric>
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <iomanip>
namespace {
bool read(uintptr_t p,vr_arm::Matrix& m) { SIZE_T size{}; return p && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),m.data(),sizeof(m),&size) && size==sizeof(m); }
bool fresh(EngineArmDriver::Clock::time_point received,EngineArmDriver::Clock::time_point now) { return received!=EngineArmDriver::Clock::time_point{} && now>=received && now-received<=std::chrono::milliseconds(100); }
std::optional<vr_arm::Vector> bone(const std::vector<vr_arm::Matrix>& world,std::span<const std::string> names,const char* name) {
    const auto found=std::find(names.begin(),names.end(),name);
    if(found==names.end() || static_cast<size_t>(found-names.begin())>=world.size()) return std::nullopt;
    return vr_arm::position(world[static_cast<size_t>(found-names.begin())]);
}
// Native animated head/hands in world units for the playtest trace. Read-only.
// The traced body and weapon bone indices depend only on the rig's names: found once per layout, not
// with 31 name searches and a lowercased copy of every name on each publication (MOD_REVIEW_2026-10-29).
// weak_ptr: a layout freed and another allocated at its address is never mistaken for it.
struct TraceBones { std::weak_ptr<const EngineArmProbe::RigLayout> layout; std::vector<size_t> body,weapons; };
const TraceBones& trace_bones(const std::shared_ptr<const EngineArmProbe::RigLayout>& layout) {
    thread_local TraceBones cache;
    if(cache.layout.lock()!=layout) { cache.layout=layout;cache.body=vr_trace::body_bones(layout->names);cache.weapons=vr_trace::weapon_bones(layout->names); }
    return cache;
}
std::optional<vr_trace::Character> trace_character(const EngineArmProbe::Pose& pose,EngineArmDriver::Clock::time_point now) {
    auto locals=pose.layout->bind_locals;
    for(const auto& b:pose.bones) { if(b.rig_index>=locals.size()) return std::nullopt;locals[b.rig_index]=b.local; }
    std::vector<vr_arm::Matrix> world;
    if(!vr_arm::hierarchy(locals,pose.layout->parents,pose.world,world)) return std::nullopt;
    vr_trace::Character c;c.time=now;c.actor=pose.owner.actor;c.animset=pose.animset;c.root=pose.world;
    c.publication_instance=pose.owner.instance;c.publication_buffer=pose.buffer;c.publication_rig=pose.rig;c.publication_mapping=pose.mapping;
    if(NativePlayerPartVisibility::get().enabled()) {
        const auto state=NativePlayerPartVisibility::get().snapshot();vr_trace::PartVisibility part;
        part.requested=NativePlayerPartVisibility::name(state.requested);if(state.applied)part.applied=NativePlayerPartVisibility::name(*state.applied);
        part.status=state.status;part.control_status=state.control_status;part.revision=state.revision;part.updates=state.updates;part.writes=state.writes;
        part.restored=state.restored;part.refused=state.refused;part.lost_lease=state.lost_lease;part.conflicts=state.conflicts;part.failures=state.failures;part.rollback_failures=state.rollback_failures;
        part.instance=state.instance;part.resource=state.resource;
        for(const auto& change:state.changes)part.changes.push_back({change.id,change.before,change.after});c.part_visibility=std::move(part);
    }
    c.head=bone(world,pose.layout->names,"HEAD");c.neck=bone(world,pose.layout->names,"NECK");
    c.native_hands={bone(world,pose.layout->names,"HANDL"),bone(world,pose.layout->names,"HANDR")};
    const auto& names=pose.layout->names;const auto& parents=pose.layout->parents;const auto& traced=trace_bones(pose.layout);
    for(const auto i:traced.body) {
        vr_trace::WeaponBone b;b.name=names[i];b.parent=parents[i]>=0 && static_cast<size_t>(parents[i])<names.size()?names[parents[i]]:"";
        const auto mapped=std::find_if(pose.bones.begin(),pose.bones.end(),[&](const auto& bone){return bone.rig_index==i && bone.skin_index>=0 && bone.skin_index<4096;});
        if(mapped!=pose.bones.end()) {b.native=world[i];b.skin_index=mapped->skin_index;}
        c.body.push_back(std::move(b));
    }
    for(const auto i:traced.weapons) {
        vr_trace::WeaponBone w;w.name=names[i];w.parent=parents[i]>=0 && static_cast<size_t>(parents[i])<names.size()?names[parents[i]]:"";
        w.native=world[i];c.weapons.push_back(std::move(w));
    }
    return c;
}
}
EngineArmDriver& EngineArmDriver::get() { static auto* driver=new EngineArmDriver; return *driver; }
bool EngineArmDriver::Settings::valid() const {
    for (size_t i=0;i<2;++i) {
        for (float v:grips[i].metres) if (!std::isfinite(v) || std::abs(v)>0.25f) { return false; }
        for (float v:grips[i].degrees) if (!std::isfinite(v) || std::abs(v)>180) { return false; }
        if (!vr_arm::finite(elbow_poles[i]) || vr_arm::length(elbow_poles[i])<0.001f) { return false; }
        for (float v:elbow_poles[i]) if (std::abs(v)>2) { return false; }
    }
    return true;
}
std::optional<EngineArmDriver::Settings> EngineArmDriver::Settings::read(std::istream& input) {
    Settings settings; std::array<bool,6> seen{}; std::string line;
    const std::array<std::string,6> keys{"left_grip_metres","left_grip_degrees","left_elbow_pole","right_grip_metres","right_grip_degrees","right_elbow_pole"};
    while (std::getline(input,line)) {
        auto start=line.find_first_not_of(" \t\r"); if (start==std::string::npos || line[start]=='#') { continue; }
        auto split=line.find('='); if (split==std::string::npos) { return {}; }
        auto found=std::find(keys.begin(),keys.end(),line.substr(0,split)); if (found==keys.end()) { return {}; }
        auto index=static_cast<size_t>(found-keys.begin()); if (seen[index]) { return {}; } seen[index]=true;
        vr_arm::Vector value{}; std::istringstream numbers{line.substr(split+1)};
        if (!(numbers>>value[0]>>value[1]>>value[2]) || !(numbers>>std::ws).eof()) { return {}; }
        if (index%3==0) { settings.grips[index/3].metres=value; }
        else if (index%3==1) { settings.grips[index/3].degrees=value; }
        else { settings.elbow_poles[index/3]=value; }
    }
    return input.eof() && std::all_of(seen.begin(),seen.end(),[](bool value){return value;}) && settings.valid()?std::optional{settings}:std::nullopt;
}
std::optional<EngineArmDriver::Settings> EngineArmDriver::Settings::load(const std::filesystem::path& path) { std::ifstream input{path}; return read(input); }
bool EngineArmDriver::Settings::save(const std::filesystem::path& path) const {
    if (!path.is_absolute() || !valid()) { return false; }
    std::ofstream output{path,std::ios::trunc}; output<<std::setprecision(9);
    for (size_t hand=0;hand<2;++hand) {
        const auto name=hand==0?"left":"right";
        const auto write=[&](const char* field,vr_arm::Vector value){output<<name<<field<<'='<<value[0]<<' '<<value[1]<<' '<<value[2]<<'\n';};
        write("_grip_metres",grips[hand].metres); write("_grip_degrees",grips[hand].degrees); write("_elbow_pole",elbow_poles[hand]);
    }
    output.flush(); return bool(output);
}
void EngineArmDriver::configure(Api api) { std::scoped_lock lock{m_mutex}; m_api=std::move(api); m_targets.reset(); }
EngineArmDriver::Targets EngineArmDriver::diagnostic_targets(const EngineArmProbe::Pose& pose) {
    Targets targets; targets.actor=pose.owner.actor;targets.reference=1;targets.epoch=1;targets.captured=pose.received;
    if (!pose.layout || !pose.world_valid) { return targets; }
    auto locals=pose.layout->bind_locals;
    for (const auto& bone:pose.bones) {
        if (bone.rig_index>=locals.size()) { return targets; }
        locals[bone.rig_index]=bone.local;
    }
    std::vector<Matrix> world;
    if (!vr_arm::hierarchy(locals,pose.layout->parents,pose.world,world)) { return targets; }
    for (size_t hand=0;hand<2;++hand) {
        const auto chain=vr_arm::chain(pose.layout->names,pose.layout->parents,hand);
        if (!chain) { continue; }
        auto target=world[chain->wrist];target[13]+=0.05f;target[14]+=0.1f;
        targets.wrists[hand]=target;
    }
    return targets;
}
void EngineArmDriver::configure_native(bool diagnostic) {
    {
        // Bounded reads: an unset variable is nullopt; a value that does not fit is passed on
        // as an invalid marker so the strict parser refuses it rather than using a truncation.
        const auto env=[](const char* name)->std::optional<std::string> {
            char value[64]{};const DWORD length=GetEnvironmentVariableA(name,value,sizeof(value));
            if(length==0) return std::nullopt;
            if(length>=sizeof(value)) return std::string{"<too long>"};
            return std::string{value,length};
        };
        const auto config=vr_upright::parse_config(env("SM2VR_UPRIGHT_TORSO"),env("SM2VR_UPRIGHT_STRENGTH"),env("SM2VR_UPRIGHT_TWIST"));
        const bool accepted=config.error.empty() && set_upright_settings(config.settings);
        enable_upright(config.enabled && accepted);
        spdlog::info("UprightTorso: enabled={} strength={} twist_strength={} max_degrees={} deadband_degrees={} error='{}' (render bones only; camera and root unchanged)",
            config.enabled && accepted,config.settings.strength,config.settings.twist_strength,config.settings.max_degrees,config.settings.deadband_degrees,config.error);
        const auto placement=vr_body_placement::parse_config(env("SM2VR_BODY_RENDER_BACK_METRES"));
        set_body_placement(placement.settings);
        spdlog::info("BodyRenderPlacement: back_metres={} error='{}' (experimental CENTRE translation before IK; feet move, actor/Trajectory/camera unchanged)",
            placement.settings.back_metres,placement.error);
        set_body_placement_control({});
        wchar_t control[1024]{};const DWORD control_length=GetEnvironmentVariableW(L"SM2VR_BODY_RENDER_CONTROL",control,static_cast<DWORD>(std::size(control)));
        if(control_length) {
            const bool valid=control_length<std::size(control) && set_body_placement_control(std::filesystem::path{control});
            spdlog::info("BodyPlacementControl: configured={} path='{}' (absolute per-run path; invalid/missing reads retain last valid setting)",valid,
                control_length<std::size(control)?path_text(std::filesystem::path{control}):"<too long>");
        }
    }
    char invalidate[8]{};const auto invalidate_length=GetEnvironmentVariableA("SM2VR_INVALIDATE_ARM_POSE",invalidate,sizeof(invalidate));
    const bool invalidate_requested=invalidate_length==1&&invalidate[0]=='1';
    char resolve[8]{};const auto resolve_length=GetEnvironmentVariableA("SM2VR_RESOLVE_ARM_POSE",resolve,sizeof(resolve));
    const bool resolve_requested=resolve_length==1&&resolve[0]=='1';
    auto invalidation=(invalidate_requested||resolve_requested)?vr_arm_cache::bind(GetModuleHandleW(nullptr),resolve_requested):std::function<vr_arm_cache::Receipt(uintptr_t)>{};
    const bool invalidation_bound=bool(invalidation);
    enable_pose_invalidation(invalidate_requested);
    enable_pose_resolution(resolve_requested);
    configure({[](const auto& pose,auto now){return EngineArmProbe::get().current(pose,now);},
        [](uintptr_t instance){EnterCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(instance+0x288));},
        [](uintptr_t instance){LeaveCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(instance+0x288));},
        [](uintptr_t destination,const Matrix& matrix){SIZE_T count{}; return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(destination),matrix.data(),sizeof(matrix),&count) && count==sizeof(matrix);},{},std::move(invalidation),
        [](uintptr_t instance,uint32_t index,Matrix& matrix){
            if(index>=4096)return false;uintptr_t pointer{},recheck{};SIZE_T count{};int slots{};
            if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(instance+0xa8),&slots,sizeof(slots),&count)||count!=sizeof(slots)||slots<=0||slots>4096||index>=static_cast<uint32_t>(slots))return false;
            if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(instance+0xc0),&pointer,sizeof(pointer),&count)||count!=sizeof(pointer)||pointer<0x10000||pointer>=0x800000000000)return false;
            if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(pointer+size_t(index)*sizeof(matrix)),matrix.data(),sizeof(matrix),&count)||count!=sizeof(matrix))return false;
            return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(instance+0xc0),&recheck,sizeof(recheck),&count)&&count==sizeof(recheck)&&pointer==recheck;
        },
        [](uintptr_t destination,const void* bytes,size_t size){SIZE_T count{}; return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(destination),bytes,size,&count) && count==size;}});
    spdlog::info("ArmPoseInvalidation: requested={} bound={} (default-off native post-D0 invalidation; no forced evaluation)",invalidate_requested,invalidation_bound);
    spdlog::info("ArmPoseResolution: requested={} bound={} (default-off invalidate1 then flush1; pending animation callback refused)",resolve_requested,invalidation_bound);
    const auto part_bound=NativePlayerPartVisibility::get().configure_native(GetModuleHandleW(nullptr));
    spdlog::info("PlayerPartVisibility: diagnostic_control={} bound={} (first-person head policy hides head_01/helmet_01 when head hiding is on)",NativePlayerPartVisibility::get().snapshot().configured,part_bound);
    EngineArmProbe::get().set_pose_observer([diagnostic](const auto& pose){
        NativePlayerPartVisibility::get().apply(pose);
        if (diagnostic) {
            DWORD foreground{};GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
            get().allow(foreground==GetCurrentProcessId(),1);
            get().targets(diagnostic_targets(pose));
        }
        get().apply(pose);
    });
}
void EngineArmDriver::enable(bool value) { std::scoped_lock lock{m_mutex}; m_state.enabled=value; if (!value) { m_targets.reset(); } }
void EngineArmDriver::enable_fingers(bool value) { std::scoped_lock lock{m_mutex};m_state.fingers_enabled=value; }
void EngineArmDriver::enable_pose_invalidation(bool value) { std::scoped_lock lock{m_mutex};m_state.invalidate_pose_enabled=value; }
void EngineArmDriver::enable_pose_resolution(bool value) { std::scoped_lock lock{m_mutex};m_state.resolve_pose_enabled=value;if(value)m_state.invalidate_pose_enabled=true; }
void EngineArmDriver::hide_head(bool value) { std::scoped_lock lock{m_mutex};m_state.head_hidden=value; }
void EngineArmDriver::enable_upright(bool value) { std::scoped_lock lock{m_mutex};m_state.upright_enabled=value; }
bool EngineArmDriver::set_upright_settings(const vr_upright::Settings& value) { if(!value.valid()) return false;std::scoped_lock lock{m_mutex};m_upright=value;return true; }
vr_upright::Settings EngineArmDriver::upright_settings() const { std::scoped_lock lock{m_mutex};return m_upright; }
bool EngineArmDriver::set_body_placement(const vr_body_placement::Settings& value) {
    if(!value.valid()) return false;std::scoped_lock lock{m_mutex};m_body_placement=value;m_state.body_back_metres=value.back_metres;return true;
}
bool EngineArmDriver::set_body_placement_control(const std::filesystem::path& path) {
    if(!path.empty() && !path.is_absolute()) return false;
    std::scoped_lock lock{m_mutex};m_body_control_path=path;++m_body_control_generation;m_next_body_control_poll={};
    m_state.body_control_status=path.empty()?vr_body_placement::ControlStatus::Disabled:vr_body_placement::ControlStatus::Pending;
    m_body_control_enabled.store(!path.empty(),std::memory_order_relaxed);return true;
}
void EngineArmDriver::poll_body_placement_control(Clock::time_point now) {
    std::filesystem::path path;uint64_t generation{},sequence{};
    std::function<vr_body_placement::ControlRead(const std::filesystem::path&)> reader;
    {std::scoped_lock lock{m_mutex};
        if(m_body_control_path.empty() || now<m_next_body_control_poll)return;
        path=m_body_control_path;generation=m_body_control_generation;sequence=++m_body_control_poll_sequence;
        reader=m_api.read_body_control;m_next_body_control_poll=now+std::chrono::seconds{1};++m_state.body_control_polls;
    }
    // File I/O is outside both the driver mutex and native publication lock.
    const auto read=reader?reader(path):vr_body_placement::read_control(path);
    std::scoped_lock lock{m_mutex};
    // A later reservation supersedes this result even for the same path. File
    // reads can finish out of order; path generation alone cannot order them.
    if(generation!=m_body_control_generation || sequence!=m_body_control_poll_sequence){++m_state.body_control_superseded;return;}
    const auto previous=m_state.body_control_status;const auto before=m_body_placement.back_metres;
    m_state.body_control_status=read.status;
    if(read.settings) {
        if(previous!=vr_body_placement::ControlStatus::Valid || before!=read.settings->back_metres)++m_state.body_control_revision;
        m_body_placement=*read.settings;m_state.body_back_metres=m_body_placement.back_metres;
    } else ++m_state.body_control_rejected;
    if(previous!=read.status || before!=m_body_placement.back_metres)
        spdlog::info("BodyPlacementControl: path='{}' status={} configured_back_metres={} retained_last_valid={} revision={} polls={} rejected={}",
            path_text(path),vr_body_placement::CONTROL_STATUS[static_cast<size_t>(read.status)],m_body_placement.back_metres,!read.settings,
            m_state.body_control_revision,m_state.body_control_polls,m_state.body_control_rejected);
}
void EngineArmDriver::follow_weapon(bool value) {std::scoped_lock lock{m_mutex};m_weapon_enabled=value;m_weapon_hand.reset();m_weapon_relative.reset();m_weapon_grip.reset();m_weapon_seated=true;m_weapon_switch.reset();if(m_weapon_observer) m_weapon_observer({});}
void EngineArmDriver::weapon_switch(Clock::time_point now) {std::scoped_lock lock{m_mutex};m_weapon_switch.weapon_switch(now);m_support_model.reset();m_support_valid=false;++m_support_generation;}
void EngineArmDriver::enable_two_hand(bool value) {std::scoped_lock lock{m_mutex};m_two_hand_enabled=value;m_support_model.reset();m_support_valid=false;++m_support_generation;}
std::optional<vr_two_hand::Model> EngineArmDriver::support_model(Clock::time_point now) const {
    std::scoped_lock lock{m_mutex};return m_weapon_enabled && m_two_hand_enabled && m_support_valid && m_allowed && m_support_model && m_support_model->epoch==m_epoch && fresh(m_support_model->captured,now)?m_support_model:std::nullopt;
}
bool EngineArmDriver::weapon_enabled() const {std::scoped_lock lock{m_mutex};return m_weapon_enabled;}
bool EngineArmDriver::set_weapon_settings(const vr_weapon::Settings& settings) {
    if(!settings.valid()) return false;std::scoped_lock lock{m_mutex};m_weapon_settings=settings;m_support_model.reset();m_support_valid=false;++m_support_generation;if(m_weapon_observer) m_weapon_observer({});return true;
}
vr_weapon::Settings EngineArmDriver::weapon_settings() const {std::scoped_lock lock{m_mutex};return m_weapon_settings;}
void EngineArmDriver::set_weapon_observer(std::function<void(const VrWeaponAim::Pose&)> observer) {std::scoped_lock lock{m_mutex};m_weapon_observer=std::move(observer);}
void EngineArmDriver::allow(bool value,uint64_t epoch,bool head) { std::scoped_lock lock{m_mutex}; if (!value || epoch!=m_epoch) { m_targets.reset(); } m_allowed=value; m_epoch=epoch; m_head_allowed=head; }
void EngineArmDriver::targets(const Targets& value) {
    std::scoped_lock lock{m_mutex};
    if ((!m_state.enabled && !m_state.head_hidden) || !m_allowed || !value.actor || !value.reference || value.epoch!=m_epoch) { m_targets.reset(); return; }
    m_targets=value;
}
bool EngineArmDriver::set_settings(const Settings& settings) { if (!settings.valid()) { return false; } std::scoped_lock lock{m_mutex}; m_settings=settings;m_support_model.reset();m_support_valid=false;++m_support_generation; m_targets.reset(); return true; }
EngineArmDriver::Settings EngineArmDriver::settings() const { std::scoped_lock lock{m_mutex}; return m_settings; }
EngineArmDriver::Snapshot EngineArmDriver::snapshot() const {
    std::scoped_lock lock{m_mutex}; auto out=m_state; out.weapon_grip=m_weapon_grip; out.weapon_grip_switches=m_weapon_switch.switches(); out.weapon_left_refused=m_weapon_switch.refused(); out.weapon_primary_draws=m_weapon_switch.draws(); out.weapon_seated=m_weapon_seated; return out;
}
std::optional<EngineArmDriver::NativeHead> EngineArmDriver::native_head() const { std::scoped_lock lock{m_mutex}; return m_native_head; }
std::optional<EngineArmDriver::NativeMotion> EngineArmDriver::native_motion() const { std::scoped_lock lock{m_mutex}; return m_native_motion; }
void EngineArmDriver::set_left_handed(bool value) {std::scoped_lock lock{m_mutex};m_left_handed=m_state.left_handed=value;m_support_model.reset();m_support_valid=false;++m_support_generation;m_weapon_hand.reset();m_weapon_relative.reset();m_weapon_grip.reset();m_weapon_seated=true;m_weapon_switch.reset();}
bool EngineArmDriver::left_handed() const {std::scoped_lock lock{m_mutex};return m_left_handed;}
EngineArmDriver::Matrix EngineArmDriver::grip(size_t hand) const { std::scoped_lock lock{m_mutex};return grip_locked(hand); }
EngineArmDriver::Matrix EngineArmDriver::grip_locked(size_t hand) const {
    if (hand>1) { return vr_camera::IDENTITY; }
    auto out=vr_camera::IDENTITY;
    for (int i=0;i<3;++i) {
        const auto a=m_settings.grips[hand].degrees[i]*0.01745329252f; auto rotation=vr_camera::IDENTITY;
        const int j=(i+1)%3,k=(i+2)%3; rotation[j*4+j]=rotation[k*4+k]=std::cos(a); rotation[j*4+k]=std::sin(a); rotation[k*4+j]=-std::sin(a);
        out=vr_camera::multiply(out,rotation);
    }
    vr_arm::position(out,m_settings.grips[hand].metres); return out;
}
void EngineArmDriver::apply(const EngineArmProbe::Pose& pose) {
    if(m_body_control_enabled.load(std::memory_order_relaxed))poll_body_placement_control(Clock::now());
    // Native freshness is sampled after file I/O, never from a pre-read timestamp.
    apply_publication(pose,Clock::now());
}
void EngineArmDriver::apply(const EngineArmProbe::Pose& pose,Clock::time_point now) {
    if(m_body_control_enabled.load(std::memory_order_relaxed))poll_body_placement_control(now);
    apply_publication(pose,now); // Explicit deterministic clock, as in existing native fixtures.
}
void EngineArmDriver::apply_publication(const EngineArmProbe::Pose& pose,Clock::time_point now) {
    std::scoped_lock lock{m_mutex}; ++m_state.updates;m_state.body_applied_back_metres=0;m_state.invalidation={};
    VrWeaponAim::Pose weapon{};m_support_valid=false;m_state.two_hand_active=false;
    if(!m_targets || !m_targets->support.held || !m_targets->wrists[0] || !m_targets->wrists[1]){m_support_anchor.reset();m_support_anchor_key.reset();}
    std::optional<vr_two_hand::Model> support_model;bool two_hand_active=false;
    struct Notify { std::function<void(const VrWeaponAim::Pose&)>& callback;VrWeaponAim::Pose& pose;~Notify(){if(callback) callback(pose);} } notify{m_weapon_observer,weapon};
    std::optional<vr_trace::Character> trace;
    const char* stage="guard";const char* placement_refusal="";
    struct Trace { std::optional<vr_trace::Character>& value;Snapshot& state;const char*& stage;const char*& refusal;
        ~Trace(){if(value) {value->body_placement=vr_trace::BodyPlacement{state.body_back_metres,state.body_placement_updates,state.body_placement_rejected,state.body_placement_suspended,stage,refusal,
            state.body_applied_back_metres,state.body_control_revision,vr_body_placement::CONTROL_STATUS[static_cast<size_t>(state.body_control_status)]};
            if(state.invalidate_pose_enabled)value->arm_pose_invalidation=vr_trace::ArmPoseInvalidation{state.invalidation_calls,state.invalidation_refused,state.invalidation.called,state.invalidation.valid,state.invalidation.before,state.invalidation.after,state.invalidation.status,
                state.resolve_pose_enabled,state.invalidation.flush_called,state.invalidation.resolved,state.pose_recovery_failed,state.recovery_calls,state.recovery_failures,state.invalidation.world_error,state.invalidation.verified_hands};
            vr_trace::Recorder::get().character(std::move(*value));}}
    } trace_out{trace,m_state,stage,placement_refusal};
    const auto refuse_body=[&](vr_body_placement::Refusal why) {
        ++m_state.body_placement_rejected;const auto index=static_cast<size_t>(why);
        ++m_state.body_placement_refusals[index];placement_refusal=vr_body_placement::REFUSALS[index];
    };
    if(vr_trace::Recorder::get().enabled() && pose.layout && pose.world_valid) {
        trace=trace_character(pose,now);
        if(m_targets && m_targets->actor==pose.owner.actor) m_trace_units=m_targets->units_per_metre;
        if(trace) trace->units_per_metre=m_trace_units;
        if(trace && m_targets && m_targets->actor==pose.owner.actor) {
            for(size_t hand=0;hand<2;++hand) if(m_targets->wrists[hand]) trace->targets[hand]=vr_arm::position(*m_targets->wrists[hand]);
        }
    }
    if(pose.layout && pose.world_valid) {
        const auto& names=pose.layout->names;const auto& bind=pose.layout->bind_locals;
        const auto native=[&](const char* name)->std::optional<vr_arm::Vector>{
            const auto found=std::find(names.begin(),names.end(),name);
            if(found==names.end() || bind.size()!=names.size() || pose.layout->parents.size()!=names.size()) return std::nullopt;
            return vr_arm::chain_position(pose.layout->parents,pose.world,static_cast<size_t>(found-names.begin()),[&](size_t i)->const Matrix&{
                const auto published=std::find_if(pose.bones.begin(),pose.bones.end(),[&](const auto& b){return b.rig_index==i;});
                return published!=pose.bones.end()?published->local:bind[i];});
        };
        const auto head=native("HEAD");
        if(head) m_native_head=NativeHead{pose.owner.actor,*head,now};
        m_native_motion=NativeMotion{pose.owner.actor,{vr_arm::position(pose.world),head,native("HANDL"),native("HANDR")},now};
    }
    const bool shrink_head=m_state.head_hidden && m_head_allowed;
    if ((!m_state.enabled && !shrink_head) || !m_allowed || !m_targets || !fresh(m_targets->captured,now) || !fresh(pose.received,now) ||
        m_targets->epoch!=m_epoch || m_targets->actor!=pose.owner.actor || !pose.layout || !pose.world_valid ||
        !m_api.current || !m_api.enter || !m_api.leave || !m_api.write || !m_api.current(pose,now)) { ++m_state.rejected; return; }
    auto original=pose.layout->bind_locals,locals=original;
    stage="mapping";
    if (locals.size()!=pose.layout->parents.size() || locals.size()!=pose.layout->names.size()) { ++m_state.rejected; return; }
    std::vector<int16_t> mapping(locals.size(),-1);
    for (const auto& bone:pose.bones) {
        if (bone.rig_index>=locals.size() || bone.skin_index<0 || bone.skin_index>=4096 || mapping[bone.rig_index]!=-1) { ++m_state.rejected; return; }
        locals[bone.rig_index]=bone.local; mapping[bone.rig_index]=bone.skin_index;
    }
    original=locals; uint64_t solved_hands{},finger_hands{};std::array<bool,2> arm_solved{};
    bool body_placed=false;
    stage="body_placement";
    if(m_state.enabled && m_body_placement.back_metres>0) {
        // Sleeping/lost controllers still publish an actor target for head hiding.
        // Suspend optional body placement, preserving that established path, until
        // both tracked wrists return. Never retain a stale placement or wrist.
        if(!m_targets->wrists[0] || !m_targets->wrists[1]) ++m_state.body_placement_suspended;
        else {
            vr_body_placement::Refusal why{};
            const auto placed=vr_body_placement::place(locals,pose.layout->parents,pose.layout->names,mapping,pose.world,m_targets->units_per_metre,m_body_placement,&why);
            // Candidate failure rejects the entire batch: no partial body/arm/weapon placement.
            if(!placed) { refuse_body(why);return; }
            locals=placed->locals;body_placed=true;
        }
    }
    if(trace) { trace->lean_native=vr_upright::lean(locals,pose.layout->bind_locals,pose.layout->parents,pose.layout->names,pose.world);
        trace->twist_native=vr_upright::twist(locals,pose.layout->bind_locals,pose.layout->parents,pose.layout->names,pose.world); }
    bool upright_applied=false;
    stage="upright";
    if(m_state.enabled && m_state.upright_enabled) {
        // Before arm IK, so the arms reach from the upright shoulders. Only BACKdown changes.
        auto settings=m_upright;
        settings.twist_strength*=m_fire_hold.update(m_targets && m_targets->firearm_trigger,now);
        const auto upright=vr_upright::correct(locals,pose.layout->bind_locals,pose.layout->parents,pose.layout->names,pose.world,settings);
        bool mapped=upright.has_value();
        if(upright) for(size_t i=0;i<locals.size();++i) if(upright->locals[i]!=locals[i] && mapping[i]<0) mapped=false;
        if(mapped) { m_state.upright_before_degrees=upright->before_degrees;m_state.upright_after_degrees=upright->after_degrees;
            m_state.upright_twist_before_degrees=upright->twist_before_degrees;m_state.upright_twist_after_degrees=upright->twist_after_degrees;
            if(upright->corrected) { locals=upright->locals;upright_applied=true; } }
        else ++m_state.upright_rejected;
    }
    stage="arms";
    const auto arm_source=locals; // One native arm solve per candidate, not a second solve over stretched VR limbs.
    for (size_t hand=0;hand<2;++hand) if (m_state.enabled && m_targets->wrists[hand]) {
        const auto solve=[&](bool shield_twist){return vr_arm::solve_named(locals,pose.layout->parents,pose.layout->names,pose.world,hand,*m_targets->wrists[hand],vr_arm::rotate(m_settings.elbow_poles[hand],pose.world),true,shield_twist);};
        const auto unmapped=[&](const std::optional<vr_arm::Result>& s){for (size_t i=0;i<locals.size();++i) if (s->locals[i]!=locals[i] && mapping[i]<0) return true;return false;};
        auto solved=solve(true);
        if (!solved) { continue; }
        // The shield mount turns with the wrist (vr_arm::Companion::twist_only) only where the
        // animation publishes it; a rig that does not still gets its arm.
        if (unmapped(solved)) solved=solve(false);
        if (!solved) { continue; }
        bool mapped=!unmapped(solved);
        if (!mapped) { continue; }
        locals=solved->locals; m_state.errors[hand]=solved->target_error; ++solved_hands;
        arm_solved[hand]=true;
        if(m_state.fingers_enabled && m_targets->fingers[hand]) {
            const auto fingers=vr_finger::solve(locals,pose.layout->bind_locals,pose.layout->parents,pose.layout->names,hand,*m_targets->fingers[hand]);
            bool finger_mapped=fingers.has_value();
            if(fingers) for(size_t i=0;i<locals.size();++i) if(fingers->locals[i]!=locals[i] && mapping[i]<0) { finger_mapped=false; }
            if(finger_mapped) { locals=fingers->locals;++finger_hands;m_state.finger_open_angles[hand]=fingers->max_open_angle_degrees; }
            else { ++m_state.finger_rejected; }
        }
    }
    if(body_placed && solved_hands!=static_cast<uint64_t>(m_targets->wrists[0].has_value()+m_targets->wrists[1].has_value())) {
        refuse_body(vr_body_placement::Refusal::ArmSolve);return;
    }
    std::optional<vr_weapon::Attachment> attachment;
    stage="weapon";
    // Left-handed, weapons move to the other wrist; without a usable fit they stay off rather than right-handed.
    const auto swap=m_left_handed?vr_hand::Swap::make(grip_locked(0),grip_locked(1),m_targets->units_per_metre):std::nullopt;
    if(m_weapon_enabled && arm_solved[m_left_handed?0:1] && swap.has_value()==m_left_handed) {
        if(m_weapon_actor!=pose.owner.actor || m_weapon_layout!=pose.layout) {
            // Any rejected publication rebuilds the layout (EngineArmProbe::rig_layout) for the same
            // actor, who is still holding the same weapon: only a new actor holds the primary.
            if(m_weapon_actor!=pose.owner.actor) m_weapon_switch.new_actor();
            m_support_model.reset();++m_support_generation;
            m_weapon_actor=pose.owner.actor;m_weapon_layout=pose.layout;m_weapon_hand.reset();m_weapon_relative.reset();m_weapon_grip.reset();m_weapon_seated=true;
        }
        attachment=vr_weapon::retarget(original,locals,pose.layout->parents,pose.layout->names,pose.world,m_weapon_settings.matrix(),m_targets->units_per_metre,m_weapon_hand,m_weapon_relative,m_weapon_grip,&m_weapon_switch,now,m_weapon_seated,swap?&*swap:nullptr);
        if(attachment && m_two_hand_enabled && attachment->grip && vr_weapon::primary(*attachment->grip) && attachment->seated && arm_solved[0] && arm_solved[1]) {
            std::vector<Matrix> world;
            if(vr_arm::hierarchy(locals,pose.layout->parents,pose.world,world)) {
                const auto name=m_left_handed?"HANDL":"HANDR";
                const auto primary=std::find(pose.layout->names.begin(),pose.layout->names.end(),name);
                if(primary!=pose.layout->names.end()) {
                    if(attachment->grip!=m_weapon_grip || (m_support_model && (m_support_model->epoch!=m_targets->epoch || std::abs(m_support_model->units-m_targets->units_per_metre)>1e-5f))){m_support_model.reset();++m_support_generation;}
                    support_model=m_support_model;
                    if(!support_model)support_model=vr_two_hand::learn(original,pose.layout->parents,pose.layout->names,pose.world,*attachment,m_targets->units_per_metre,swap?&*swap:nullptr);
                    if(support_model) {
                        auto& model=*support_model;model.actor=pose.owner.actor;model.epoch=m_targets->epoch;model.generation=m_support_generation;model.captured=m_targets->captured;
                        model.tracking_units=m_targets->tracking_units_per_metre==0?model.units:m_targets->tracking_units_per_metre;
                        model.gun_to_primary=vr_camera::multiply(attachment->world,vr_camera::inverse(world[primary-pose.layout->names.begin()]));
                        model.drawn_to_primary=model.gun_to_primary;
                        if(m_targets->support.held && m_targets->support.epoch==model.epoch && m_targets->support.generation==model.generation) {
                            const std::array<uint64_t,3> key{model.epoch,model.generation,m_targets->support.engagement};
                            if(m_support_anchor_key!=key) {
                                m_support_anchor.reset();m_support_anchor_key.reset();
                                const auto main=vr_arm::position(*m_targets->wrists[model.dominant]);
                                const auto other=vr_arm::position(*m_targets->wrists[1-model.dominant]);
                                const auto native_support=vr_camera::multiply(model.support_to_gun,attachment->world);
                                const std::array<vr_arm::Vector,3> region{main,vr_arm::position(native_support),vr_arm::position(attachment->world)};
                                if(vr_arm::length(vr_arm::sub(other,main))>=.12f*model.tracking_units && vr_two_hand::region_distance(region,other)<=.12f*model.tracking_units) {
                                    m_support_anchor=vr_camera::multiply(*m_targets->wrists[1-model.dominant],vr_camera::inverse(attachment->world));m_support_anchor_key=key;
                                }
                            }
                            auto grabbed=model;if(m_support_anchor)grabbed.support_to_gun=*m_support_anchor;
                            const auto constrained=m_support_anchor?vr_two_hand::solve(grabbed,m_targets->wrists):std::nullopt;
                            const auto refuse=[&](size_t kind){++m_state.two_hand_refused;++m_state.two_hand_refusals[kind];};
                            const auto staged=[&]()->std::optional<std::vector<Matrix>> {
                                if(!constrained){refuse(0);return {};}
                                auto candidate=arm_source;
                                for(size_t h=0;h<2;++h) {
                                    auto solved=vr_arm::solve_named(candidate,pose.layout->parents,pose.layout->names,pose.world,h,constrained->wrists[h],vr_arm::rotate(m_settings.elbow_poles[h],pose.world),true,false);
                                    if(!solved || solved->target_error>.005f*model.units){refuse(1);return {};}
                                    for(size_t i=0;i<candidate.size();++i)if(solved->locals[i]!=candidate[i] && mapping[i]<0){refuse(2);return {};}
                                    candidate=std::move(solved->locals);
                                    // Finger curls are wrist-local; reapply them to this fresh arm candidate.
                                    if(m_state.fingers_enabled && m_targets->fingers[h]) {
                                        const auto fingers=vr_finger::solve(candidate,pose.layout->bind_locals,pose.layout->parents,pose.layout->names,h,*m_targets->fingers[h]);
                                        bool mapped=fingers.has_value();
                                        if(fingers)for(size_t i=0;i<candidate.size();++i)if(fingers->locals[i]!=candidate[i] && mapping[i]<0)mapped=false;
                                        if(mapped)candidate=fingers->locals;
                                    }
                                }
                                return candidate;
                            }();
                            std::vector<Matrix> constrained_world;
                            if(staged && vr_arm::hierarchy(*staged,pose.layout->parents,pose.world,constrained_world)) {
                                locals=*staged;
                                attachment->world=constrained->gun;
                                const auto animation=pose.layout->parents[attachment->index];
                                attachment->local=vr_camera::multiply(attachment->world,vr_camera::inverse(constrained_world[animation]));
                                model.drawn_to_primary=vr_camera::multiply(attachment->world,vr_camera::inverse(*m_targets->wrists[model.dominant]));
                                two_hand_active=true;
                            } else if(staged)refuse(3);
                            // A rejected constraint leaves the already solved, fresh one-hand
                            // candidate intact. Commit it normally so one bad frame cannot
                            // erase weapon ownership/contact and cancel a still-squeezed grip.
                        }
                    }
                }
            }
        }
        if(attachment && mapping[attachment->index]>=0 && (attachment->hand==1 || solved_hands==2)) locals[attachment->index]=attachment->local;else attachment.reset();
    }
    if(body_placed && m_weapon_enabled && !attachment) { refuse_body(vr_body_placement::Refusal::Attachment);return; }
    // After the gun: GUN_R is off its branch. With one wrist lost the sword stays on the right arm's animation.
    if(swap && arm_solved[0] && arm_solved[1]) {
        const auto sword=vr_hand::sword_to_left(locals,pose.layout->parents,pose.layout->names,pose.world,*swap);
        if(sword && mapping[sword->index]>=0) {locals[sword->index]=sword->local;++m_state.sword_moves;}
        else ++m_state.sword_refused;
    }
    // On the way into camera mode: the frozen head looks where the player looked. A refusal keeps
    // the animated head and everything else in this publication.
    std::optional<vr_head_aim::Result> head_aim;
    stage="head_aim";
    if(m_targets->gaze && m_state.enabled && !shrink_head) {
        head_aim=vr_head_aim::aim(locals,pose.layout->parents,pose.layout->names,mapping,pose.world,*m_targets->gaze);
        if(head_aim) locals=head_aim->locals; else ++m_state.head_aim_refused;
    }
    stage="final_hierarchy";
    if(trace && (solved_hands || attachment || upright_applied || body_placed)) {
        // As published after arm IK and the weapon retarget: shows which weapon bones
        // followed the tracked hand and which stayed where the animation left them.
        std::vector<Matrix> world;
        if(vr_arm::hierarchy(locals,pose.layout->parents,pose.world,world)) {
            trace->solved_hands={bone(world,pose.layout->names,"HANDL"),bone(world,pose.layout->names,"HANDR")};
            const auto indices=vr_trace::weapon_bones(pose.layout->names);
            for(size_t k=0;k<indices.size() && k<trace->weapons.size();++k) trace->weapons[k].final=world[indices[k]];
            const auto body=vr_trace::body_bones(pose.layout->names);
            for(size_t k=0;k<body.size() && k<trace->body.size();++k) if(trace->body[k].native) trace->body[k].final=world[body[k]];
            trace->head_final=bone(world,pose.layout->names,"HEAD");
            trace->lean_final=vr_upright::lean(locals,pose.layout->bind_locals,pose.layout->parents,pose.layout->names,pose.world);
            trace->twist_final=vr_upright::twist(locals,pose.layout->bind_locals,pose.layout->parents,pose.layout->names,pose.world);
        }
    }
    // The head visibility transform is deliberately nonrigid. Resolve expected
    // tracked hand worlds before it; the hands are outside the hidden subtree.
    // The native C0 evaluator accepts that affine head transform, whereas the
    // arm solver's hierarchy helper correctly requires rigid inputs.
    std::vector<Matrix> expected_world,original_world;
    const bool expected_world_valid=!m_state.resolve_pose_enabled||
        (vr_arm::hierarchy(locals,pose.layout->parents,pose.world,expected_world)&&
         vr_arm::hierarchy(original,pose.layout->parents,pose.world,original_world));
    bool head_applied=false;
    stage="head_visibility";
    if(shrink_head) {
        const auto hidden=vr_head::hide(locals,pose.layout->parents,pose.layout->names,mapping);
        bool mapped=hidden.has_value();
        if(hidden) for(size_t i=0;i<locals.size();++i) if((*hidden)[i]!=locals[i] && mapping[i]<0) mapped=false;
        if(mapped) { locals=*hidden;head_applied=true; } else ++m_state.head_rejected;
    }
    struct Patch { uintptr_t address; Matrix before,after; }; std::vector<Patch> patches;
    stage="patches";
    for (size_t i=0;i<locals.size();++i) if (locals[i]!=original[i]) {
        const auto offset=static_cast<size_t>(mapping[i])*sizeof(Matrix);
        if (mapping[i]<0 || pose.buffer>UINTPTR_MAX-offset-sizeof(Matrix)) { ++m_state.rejected; return; }
        patches.push_back({pose.buffer+offset,original[i],locals[i]});
    }
    if (patches.empty()) { return; }
    if(m_state.resolve_pose_enabled&&m_state.pose_recovery_failed) {++m_state.invalidation_refused;m_state.invalidation.status="recovery_poisoned";return;}
    if(m_state.invalidate_pose_enabled&&(!m_api.invalidate_pose||(m_state.resolve_pose_enabled&&!m_api.read_evaluated))) {++m_state.invalidation_refused;m_state.invalidation.status="api_unavailable";return;}
    if(!expected_world_valid) {++m_state.invalidation_refused;m_state.invalidation.status="expected_world_unavailable";return;}
    const auto verify_world=[&](const std::vector<Matrix>& expected,vr_arm_cache::Receipt& receipt) {
        const DWORD error=GetLastError();struct Preserve{DWORD error;~Preserve(){SetLastError(error);}} preserve{error};
        try {
            float error{};
            for(size_t hand=0;hand<2;++hand) {
                if(!m_targets||!m_targets->wrists[hand]||!m_state.enabled)continue;
                const auto found=std::find(pose.layout->names.begin(),pose.layout->names.end(),hand?"HANDR":"HANDL");
                if(found==pose.layout->names.end())return false;
                const size_t index=std::distance(pose.layout->names.begin(),found);Matrix actual{};
                if(mapping[index]<0||!m_api.read_evaluated(pose.owner.instance,static_cast<uint32_t>(mapping[index]),actual))return false;
                for(size_t k=0;k<16;++k) {if(!std::isfinite(actual[k]))return false;error=std::max(error,std::abs(actual[k]-expected[index][k]));}
                ++receipt.verified_hands;
            }
            receipt.world_error=error;return error<=.005f;
        }catch(...){return false;}
    };
    m_api.enter(pose.owner.instance);
    stage="locked_ownership";
    struct Leave { Api& api; uintptr_t instance; ~Leave(){api.leave(instance);} } leave{m_api,pose.owner.instance};
    if (!m_api.current(pose,now)) { ++m_state.rejected; return; }
    stage="original_bytes";
    // One read of the span the patched bones cover instead of one system call per bone (about 125 per
    // publication, inside the game's lock); bone by bone only if the span is not readable as a whole.
    const bool unchanged=[&]{
        const auto [low,high]=std::ranges::minmax(patches,{},&Patch::address);
        const auto span=high.address-low.address+sizeof(Matrix);
        thread_local std::vector<uint8_t> bytes;
        if (span<=size_t{4096}*sizeof(Matrix)) {
            bytes.resize(span);SIZE_T size{};
            if (ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(low.address),bytes.data(),span,&size) && size==span) {
                for (const auto& patch:patches) { Matrix actual{}; std::memcpy(actual.data(),bytes.data()+(patch.address-low.address),sizeof(Matrix)); if (actual!=patch.before) return false; }
                return true;
            }
        }
        for (const auto& patch:patches) { Matrix actual{}; if (!read(patch.address,actual) || actual!=patch.before) return false; }
        return true;
    }();
    if (!unchanged) { ++m_state.rejected; return; }
    size_t written{};
    stage="write";
    if (m_api.write_bytes) {
        // Adjacent slots go out as one write (about 125 bones a publication, inside the game's lock).
        std::vector<size_t> order(patches.size());std::iota(order.begin(),order.end(),size_t{0});
        std::ranges::sort(order,{},[&](size_t i){return patches[i].address;});
        thread_local std::vector<Matrix> run;
        const auto write_run=[&](size_t begin,size_t end,bool after) {
            run.clear();for (size_t k=begin;k<end;++k) run.push_back(after?patches[order[k]].after:patches[order[k]].before);
            return m_api.write_bytes(patches[order[begin]].address,run.data(),run.size()*sizeof(Matrix));
        };
        std::vector<std::pair<size_t,size_t>> runs;
        for (size_t begin=0;begin<order.size();) {
            size_t end=begin+1;
            while (end<order.size() && patches[order[end]].address==patches[order[end-1]].address+sizeof(Matrix)) ++end;
            runs.push_back({begin,end});begin=end;
        }
        for (size_t r=0;r<runs.size();++r) {
            if (!write_run(runs[r].first,runs[r].second,true)) {
                // Undo every run up to and including the failed one, still under the publication lock.
                for (size_t k=0;k<=r;++k) write_run(runs[k].first,runs[k].second,false);
                ++m_state.failures; return;
            }
            written+=runs[r].second-runs[r].first;
        }
    } else {
        for (const auto& patch:patches) {
            if (!m_api.write(patch.address,patch.after)) {
                // The next native publication also restores its animation; undo this partial
                // batch now while still holding the owned instance's publication lock.
                for (size_t i=0;i<=written;++i) { m_api.write(patches[i].address,patches[i].before); }
                ++m_state.failures; return;
            }
            ++written;
        }
    }
    if(m_state.invalidate_pose_enabled) {
        try {m_state.invalidation=m_api.invalidate_pose(pose.owner.instance);}
        catch(...) {m_state.invalidation.status="adapter_callback_exception";}
        m_state.invalidation_calls+=m_state.invalidation.called;
        if(m_state.resolve_pose_enabled&&m_state.invalidation.valid&&(!m_state.invalidation.flush_called||!m_state.invalidation.resolved)) {
            m_state.invalidation.valid=false;m_state.invalidation.status="world_resolution_receipt_missing";
        }
        if(m_state.resolve_pose_enabled&&m_state.invalidation.valid) {
            m_state.invalidation.valid=verify_world(expected_world,m_state.invalidation);
            if(!m_state.invalidation.valid)m_state.invalidation.status="evaluated_world_mismatch";
        }
        if(!m_state.invalidation.valid) {
            ++m_state.invalidation_refused;
            bool restored=true;for(const auto& patch:patches)restored=m_api.write(patch.address,patch.before)&&restored;
            if(m_state.resolve_pose_enabled) {
                // A failed post-evaluation check may have already changed C0. Rebuild
                // restored D0 before unlocking; never publish a partial VR result.
                bool recovered=false;
                const std::string_view reason=m_state.invalidation.status;
                const bool untouched=!m_state.invalidation.called&&!m_state.invalidation.flush_called&&
                    (reason=="animation_callback_pending"||reason=="world_evaluation_disabled"||reason=="resolve_before_unavailable"||reason=="resolve_api_unavailable");
                if(restored&&untouched)recovered=true;
                else if(restored)try {++m_state.recovery_calls;auto receipt=m_api.invalidate_pose(pose.owner.instance);recovered=receipt.valid&&receipt.flush_called&&receipt.resolved&&verify_world(original_world,receipt);}catch(...){}
                if(!recovered){++m_state.recovery_failures;m_state.pose_recovery_failed=true;}
            }
            ++m_state.failures;return;
        }
    }
    if(trace) trace->driven=solved_hands>0;
    m_state.bone_writes+=written; m_state.hands+=solved_hands;m_state.finger_hands+=finger_hands;if(head_applied) ++m_state.head_updates;if(upright_applied) ++m_state.upright_updates;
    if(body_placed) {++m_state.body_placement_updates;m_state.body_applied_back_metres=m_body_placement.back_metres;}
    if(head_aim) {++m_state.head_aims;m_state.head_aim_degrees=head_aim->turned_degrees;if(head_aim->limited) ++m_state.head_aim_limited;}
    if(attachment) {
        if(support_model){m_support_model=support_model;m_support_valid=true;m_state.two_hand_active=two_hand_active;if(two_hand_active)++m_state.two_hand_updates;}else m_support_model.reset();
        m_weapon_hand=attachment->hand;
        m_weapon_relative=attachment->relative;
        m_weapon_grip=attachment->grip;
        m_weapon_seated=attachment->seated;
        weapon={pose.owner.actor,m_targets->epoch,++m_weapon_sequence,m_targets->captured,attachment->world,attachment->hand};
    }
    if(m_sword_observer) {
        const auto& names=pose.layout->names;const auto found=std::find(names.begin(),names.end(),"GUN_R");
        const auto g=static_cast<size_t>(found-names.begin());
        if(found!=names.end() && mapping[g]>=0) {
            const auto evaluated=[&](size_t i){return mapping[i]>=0;};
            const auto drawn=vr_melee_sweep::below_root(locals,pose.layout->parents,g,evaluated),animated=vr_melee_sweep::below_root(original,pose.layout->parents,g,evaluated);
            if(drawn && animated && mapping[drawn->root]>=0) m_sword_observer({pose.owner.instance,mapping[g],mapping[drawn->root],drawn->relative,animated->relative,now});
        }
    }
    stage="committed";
}
void EngineArmDriver::set_sword_observer(std::function<void(const Sword&)> observer) { std::scoped_lock lock{m_mutex};m_sword_observer=std::move(observer); }
