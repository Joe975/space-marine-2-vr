#pragma once
#include "VrArmPose.h"
#include <charconv>
#include <string>
#include <filesystem>
#include <fstream>

// Experimental whole-body placement, before arm IK. CENTRE owns the visible body;
// its sibling Trajectory owns native camera/contact targets. Never move the actor,
// camera or tracked wrist targets to make the armour clear the headset.
namespace vr_body_placement {
enum class Refusal : size_t { None, Settings, Layout, Topology, Mapping, Hierarchy, Direction, Output, ArmSolve, Attachment, Count };
inline constexpr std::array<const char*,static_cast<size_t>(Refusal::Count)> REFUSALS{
    "", "settings", "layout", "topology", "mapping", "hierarchy", "direction", "output", "arm_solve", "weapon_attachment"};
struct Settings {
    float back_metres{}; // Disabled by default. Physical metres, along level actor forward.
    bool valid() const { return std::isfinite(back_metres) && back_metres>=0 && back_metres<=.4f; }
    bool operator==(const Settings&) const = default;
};
struct Config { Settings settings; std::string error; };
enum class ControlStatus : size_t { Disabled, Pending, Valid, Unavailable, Oversized, Invalid };
inline constexpr std::array<const char*,6> CONTROL_STATUS{"disabled","pending","valid","unavailable","oversized","invalid"};
inline Config parse_config(const std::optional<std::string>& value) {
    Config result;
    if(!value) return result;
    float metres{};const auto* first=value->data();const auto* last=first+value->size();
    const auto [end,code]=std::from_chars(first,last,metres);
    if(value->empty() || code!=std::errc{} || end!=last || !Settings{metres}.valid()) {
        result.error="SM2VR_BODY_RENDER_BACK_METRES must be a number from 0 to 0.4";
        return result;
    }
    result.settings.back_metres=metres;return result;
}
struct ControlRead { std::optional<Settings> settings; ControlStatus status{ControlStatus::Unavailable}; };
// One scalar in physical metres; only one optional LF/CRLF is stripped. Never
// accept a truncation, extra line/key, whitespace prefix, NaN or partial number.
inline ControlRead read_control(const std::filesystem::path& path) {
    std::ifstream input{path,std::ios::binary};if(!input) return {};
    char buffer[64]{};input.read(buffer,sizeof(buffer));const auto count=input.gcount();
    if(count==sizeof(buffer)) return {{},ControlStatus::Oversized};
    if(input.bad()) return {};
    std::string text{buffer,static_cast<size_t>(count)};
    if(text.ends_with("\r\n")) text.resize(text.size()-2);else if(text.ends_with('\n'))text.pop_back();
    const auto config=parse_config(text);
    return config.error.empty()?ControlRead{config.settings,ControlStatus::Valid}:ControlRead{{},ControlStatus::Invalid};
}
struct Result { std::vector<vr_arm::Matrix> locals; size_t centre{}; vr_arm::Vector world_offset{}; };
inline std::optional<Result> place(std::span<const vr_arm::Matrix> locals,std::span<const int32_t> parents,
    std::span<const std::string> names,std::span<const int16_t> mapping,const vr_arm::Matrix& root,
    float units_per_metre,const Settings& settings,Refusal* refusal=nullptr) {
    if(refusal) *refusal=Refusal::None;
    const auto reject=[&](Refusal why)->std::optional<Result> { if(refusal)*refusal=why;return {}; };
    if(!settings.valid() || settings.back_metres<=0 || !std::isfinite(units_per_metre) || units_per_metre<.01f || units_per_metre>100) return reject(Refusal::Settings);
    if(names.empty() || names.size()!=locals.size() || names.size()!=parents.size() || names.size()!=mapping.size()) return reject(Refusal::Layout);
    // Refuse unknown root arrangements rather than moving only part of the body.
    if(names[0]!="CharacterWorldSpaceTM" || parents[0]!=-1 || std::count(names.begin(),names.end(),"CENTRE")!=1 ||
       std::count(names.begin(),names.end(),"Trajectory")!=1) return reject(Refusal::Topology);
    const size_t centre=std::find(names.begin(),names.end(),"CENTRE")-names.begin();
    const size_t trajectory=std::find(names.begin(),names.end(),"Trajectory")-names.begin();
    if(parents[centre]!=0 || parents[trajectory]!=0) return reject(Refusal::Topology);
    if(mapping[centre]<0 || mapping[centre]>=4096 || std::count(mapping.begin(),mapping.end(),mapping[centre])!=1) return reject(Refusal::Mapping);
    for(size_t i=1;i<parents.size();++i) {
        if(parents[i]<0 || parents[i]>=static_cast<int32_t>(i)) return reject(Refusal::Topology);
        if(parents[i]==0 && i!=centre && i!=trajectory) return reject(Refusal::Topology);
    }
    std::vector<vr_arm::Matrix> world;
    if(!vr_arm::hierarchy(locals,parents,root,world)) return reject(Refusal::Hierarchy);
    vr_arm::Vector forward{root[8],0,root[10]};const float length=vr_arm::length(forward);
    if(!(length>1e-4f)) return reject(Refusal::Direction);
    Result result;result.locals.assign(locals.begin(),locals.end());result.centre=centre;
    result.world_offset=vr_arm::scale(forward,-settings.back_metres*units_per_metre/length);
    // Translation only: do not round-trip native axes through parent * inverse.
    // Accepted native matrices are approximately orthonormal; that round trip
    // squares their small error and can fail the unchanged 1e-4 rigidity guard.
    const auto local_offset=vr_arm::rotate(result.world_offset,vr_camera::inverse(world[0]));
    vr_arm::position(result.locals[centre],vr_arm::add(vr_arm::position(locals[centre]),local_offset));
    if(!vr_camera::rigid(result.locals[centre])) return reject(Refusal::Output);
    return result;
}
} // namespace vr_body_placement
