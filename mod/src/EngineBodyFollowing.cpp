#include "EngineBodyFollowing.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <algorithm>
#include <cstring>

namespace {
constexpr game_build::Code YAW=game_build::hashed(0x185de20,16,"b277a5c032f506cff09d5fa5196861b1e4cfd52e7bbf934dcab8c0d4cea11da7");
constexpr game_build::Code CONTEXT_BYTES=game_build::hashed(0x2b21390,16,"1016957dd1bc437ff9a837290934d0c8531eee7d31b7603addf731db66056243");
template<class T> bool read(uintptr_t source,T& value) { SIZE_T count{}; return source && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(source),&value,sizeof(T),&count) && count==sizeof(T); }
}
EngineBodyFollowing& EngineBodyFollowing::get() { static auto* instance=new EngineBodyFollowing; return *instance; }
bool EngineBodyFollowing::matches_build(uint32_t stamp,std::span<const uint8_t> yaw,std::span<const uint8_t> context) {
    return game_build::code(stamp,SET_YAW_RVA,yaw,YAW) && game_build::code(stamp,CONTEXT_UPDATE_RVA,context,CONTEXT_BYTES);
}
bool EngineBodyFollowing::install_in_game(HMODULE game) {
    {char off[32]{};const auto n=GetEnvironmentVariableA("SM2VR_DIAG_FOLLOW_OFF",off,sizeof off);
     if(n && n<sizeof off){const std::string_view v{off,n};m_diag_no_yaw=v.find("yaw")!=std::string_view::npos;m_diag_no_room=v.find("room")!=std::string_view::npos;}}
    if (installed()) { return true; }
    const auto base=reinterpret_cast<uint8_t*>(game);
    if (!base) { return false; }
    const auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto stamp=nt->FileHeader.TimeDateStamp;const auto yaw=game_build::rva(stamp,SET_YAW_RVA),context=game_build::rva(stamp,CONTEXT_UPDATE_RVA);
    if (!yaw || !context || nt->OptionalHeader.SizeOfImage < context+CONTEXT_BYTES.size() ||
        !matches_build(stamp,{base+yaw,YAW.size()},{base+context,CONTEXT_BYTES.size()})) { return false; }
    EngineCinematicState::get().bind_in_game(game);
    return install(base+context,{reinterpret_cast<Api::Yaw>(base+yaw),[]{return EngineCinematicState::get().authority();}});
}
bool EngineBodyFollowing::install(void* context,Api api) {
    if (installed()) { return true; }
    if (!context || !api.set_yaw) { return false; }
    auto init=MH_Initialize(); if (init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) { return false; }
    m_api=api;
    if (MH_CreateHook(context,reinterpret_cast<void*>(&context_hook),reinterpret_cast<void**>(&m_original))!=MH_OK) { return false; }
    m_target=context;
    if (MH_EnableHook(context)!=MH_OK) { uninstall(); return false; }
    EngineBodyProbe::get().set_body_observer([this](const auto& row){body(row);});
    m_installed=true; return true;
}
void EngineBodyFollowing::uninstall() {
    enable(false); EngineBodyProbe::get().set_body_observer({});
    if (m_target) { MH_DisableHook(m_target); MH_RemoveHook(m_target); }
    m_target=nullptr; m_installed=false;
}
void EngineBodyFollowing::enable(bool value) { std::scoped_lock lock{m_mutex}; m_enabled=value; if (!value) { m_follow.identify(0); m_follow.allow(false); } }
void EngineBodyFollowing::allow(bool value) { std::scoped_lock lock{m_mutex}; m_follow.allow(value && m_enabled); }
void EngineBodyFollowing::dodge(bool pressed,VrBodyFollowing::Clock::time_point now) { std::scoped_lock lock{m_mutex};m_follow.dodge(pressed,now); }
void EngineBodyFollowing::melee(bool pressed,VrBodyFollowing::Clock::time_point now) { std::scoped_lock lock{m_mutex};m_follow.melee(pressed,now); }
void EngineBodyFollowing::combo(uint16_t buttons,VrBodyFollowing::Clock::time_point now) { std::scoped_lock lock{m_mutex};m_follow.combo(buttons,now); }
void EngineBodyFollowing::stick(float x,float y,VrBodyFollowing::Clock::time_point now) { std::scoped_lock lock{m_mutex};m_follow.stick(x,y,now); }
void EngineBodyFollowing::viewer_frame() { std::scoped_lock lock{m_mutex};m_follow.viewer_frame(); }
void EngineBodyFollowing::head_bone(uintptr_t actor,const VrBodyFollowing::Vector& position,VrBodyFollowing::Clock::time_point received) { std::scoped_lock lock{m_mutex};m_follow.head_bone(actor,position,received); }
void EngineBodyFollowing::character_motion(const vr_viewer::Motion::Sample& sample) { std::scoped_lock lock{m_mutex};m_follow.character_motion(sample); }
float EngineBodyFollowing::movement_heading_offset(VrBodyFollowing::Clock::time_point now) const { std::scoped_lock lock{m_mutex};return m_follow.movement_heading_offset(now); }
std::optional<float> EngineBodyFollowing::ground_speed(VrBodyFollowing::Clock::time_point now) const { if(!m_enabled) return {}; std::scoped_lock lock{m_mutex};return m_follow.ground_speed(now); }
bool EngineBodyFollowing::set_settings(const VrBodyFollowing::Settings& value) { std::scoped_lock lock{m_mutex}; return m_follow.set_settings(value); }
VrBodyFollowing::Settings EngineBodyFollowing::settings() const { std::scoped_lock lock{m_mutex}; return m_follow.settings(); }
EngineBodyFollowing::Snapshot EngineBodyFollowing::snapshot() const { std::scoped_lock lock{m_mutex}; return {m_follow.snapshot(),m_context_calls,m_matched_contexts,m_native_velocity,m_body_forward,m_facing_error_degrees}; }
vr_camera::Matrix EngineBodyFollowing::camera(const vr_camera::Matrix& game,const EngineCameraProbe::VrEye& eye) {
    if (!m_enabled || eye.synthetic || !eye.valid_at(std::chrono::steady_clock::now())) { return game; }
    const auto now=std::chrono::steady_clock::now();
    const auto actor=EngineBodyProbe::get().active_actor(now);
    const auto scripted=m_api.scripted?m_api.scripted():std::optional{false};
    std::scoped_lock lock{m_mutex}; m_follow.identify(actor);
    auto source=game;
    if(m_inject_takeover.load()) if(const auto root=m_follow.body_position(now))
        source=VrBodyFollowing::pulled_back(game,*root,eye.marine_units_per_metre(),std::chrono::duration<float>(now-m_inject_since).count());
    return m_follow.camera(source,VrBodyFollowing::marine_head(eye.relative_head,eye.fit_factor),eye.reference_epoch,eye.marine_units_per_metre(),now,scripted);
}
std::optional<VrBodyFollowing::Vector> EngineBodyFollowing::body_position(VrBodyFollowing::Clock::time_point now) const {
    if(!m_enabled) return {};
    std::scoped_lock lock{m_mutex};return m_follow.body_position(now);
}
void EngineBodyFollowing::inject_takeover(bool value) {
    std::scoped_lock lock{m_mutex};
    if(value && !m_inject_takeover.load()) m_inject_since=VrBodyFollowing::Clock::now();
    m_inject_takeover.store(value);
}
void EngineBodyFollowing::body(const EngineBodyProbe::Body& row) {
    if (!m_enabled) { return; }
    const auto actor=EngineBodyProbe::get().active_actor(row.received);
    uintptr_t context{},actor_entity{};
    if (!actor || actor!=row.actor || !read(row.body+0x4d8,context) || !read(row.actor+0x38,actor_entity) || actor_entity!=row.entity) { return; }
    std::optional<float> yaw;
    const auto scripted=m_api.scripted?m_api.scripted():std::optional{false};
    // The facing the game's own update left (its melee turns onto a target), before ours.
    VrBodyFollowing::Vector native_forward{};
    const bool native=read(row.entity+0xb8,native_forward) && std::hypot(native_forward[0],native_forward[2])>1e-3f;
    {
        std::scoped_lock lock{m_mutex}; m_follow.identify(actor);
        m_follow.body(row.actor,row.body,context,row.position,row.received);
        if(native) m_follow.native_facing(row.actor,std::atan2(native_forward[0],native_forward[2])*57.295779513f,row.received);
        m_follow.camera_authority(scripted);
        yaw=m_follow.yaw(row.actor,row.received);
    }
    // Runs after the original native body update on its simulation thread. Uses the regular
    // position-preserving setter, never the forced/teleport variant or render-thread writes.
    if (yaw && !m_diag_no_yaw) {
        m_api.set_yaw(reinterpret_cast<void*>(row.actor),*yaw);
        VrBodyFollowing::Vector forward{};
        if (read(row.entity+0xb8,forward)) {
            std::scoped_lock lock{m_mutex}; m_body_forward=forward;
            m_facing_error_degrees=std::remainder(std::atan2(forward[0],forward[2])*57.295779513f-*yaw,360.0f);
        }
    }
}
void EngineBodyFollowing::context_hook(void* context,void* parameters,float* output) {
    auto& self=get();
    // The game's update reads the physics velocity at +0xa0, last step's room velocity included;
    // it runs on its own movement only, and the field goes back as it was (VrBodyFollowing::room_feedback).
    auto* input=reinterpret_cast<float*>(static_cast<uint8_t*>(parameters)+0xa0);
    std::array<float,3> kept{};bool corrected{};
    if(self.m_enabled && output && !self.m_diag_no_room) try {
        std::scoped_lock lock{self.m_mutex};
        const auto current=self.m_follow.snapshot();
        if(reinterpret_cast<uintptr_t>(context)==current.context && reinterpret_cast<uintptr_t>(parameters)==current.body+0x4f0) {
            const auto room=self.m_follow.room_feedback(current.context,{input[0],input[1],input[2]});
            if(room[0]!=0 || room[2]!=0) {std::copy_n(input,3,kept.begin());input[0]-=room[0];input[2]-=room[2];corrected=true;}
        }
    } catch (...) {}
    self.m_original(context,parameters,output);
    if(corrected) std::copy_n(kept.begin(),3,input);
    if (!self.m_enabled || !output) { return; }
    try {
        std::scoped_lock lock{self.m_mutex}; ++self.m_context_calls;
        const auto current=self.m_follow.snapshot();
        if (reinterpret_cast<uintptr_t>(context)!=current.context || reinterpret_cast<uintptr_t>(parameters)!=current.body+0x4f0) { return; }
        ++self.m_matched_contexts;
        self.m_native_velocity={output[0],output[1],output[2]};
        float dt{};
        if (!read(reinterpret_cast<uintptr_t>(parameters)+0x88,dt)) { return; }
        if(self.m_diag_no_room) return;
        const auto room=self.m_follow.velocity(current.context,self.m_native_velocity,dt,std::chrono::steady_clock::now());
        for (int j:{0,2}) { output[j]+=room[j]; }
    } catch (...) {}
}

