#pragma once
#include "VrBodyFrame.h"

// Left support hand measured in the right controller's pointing frame, in physical
// metres. This frame follows a held rifle through head movement and snap turns.
class VrSupportGrip {
public:
    using Values=vr_body::Parameters<4>;
    enum Index { MIN_FORWARD,MAX_FORWARD,RADIUS,RELEASE_MARGIN };
    inline static constexpr std::array<Values::Parameter,4> PARAMETERS{{
        {"min_forward_metres",.10f,0,.4f},{"max_forward_metres",.65f,.2f,1},
        {"radius_metres",.22f,.05f,.4f},{"release_margin_metres",.06f,0,.15f}}};
    struct Settings {
        Values values=Values::defaults(PARAMETERS);
        bool valid() const {return values.in_range(PARAMETERS) && values.values[MIN_FORWARD]<values.values[MAX_FORWARD];}
        bool operator==(const Settings&) const=default;
        static std::optional<Settings> load(const std::filesystem::path& path) {
            std::ifstream in{path};auto p=Values::read(in,PARAMETERS);if(!p)return {};Settings s{*p};return s.valid()?std::optional{s}:std::nullopt;
        }
        bool save(const std::filesystem::path& path) const {if(!path.is_absolute() || !valid())return false;std::ofstream out{path};return values.write(out,PARAMETERS);}
    };
    struct Snapshot {bool held{},in_zone{};float forward{},radial{};uint64_t engagements{};}; // engagements: support starts, for the log; survives reset().
    bool set_settings(const Settings& s) {if(!s.valid())return false;m_settings=s;reset();return true;}
    Settings settings() const {return m_settings;}
    Snapshot snapshot() const {auto s=m_state;s.engagements=m_engagements;return s;}
    void reset(){m_state={};m_released=false;}
    bool contains(const std::optional<vr_camera::Matrix>& right,const std::optional<vr_camera::Matrix>& left) const {
        if(!right || !left || !vr_camera::rigid(*right) || !vr_camera::rigid(*left)) return false;
        const auto relative=vr_camera::multiply(*left,vr_camera::inverse(*right));
        const auto& v=m_settings.values.values;
        const float margin=m_state.held?v[RELEASE_MARGIN]:0;
        return -relative[14]>=v[MIN_FORWARD]-margin && -relative[14]<=v[MAX_FORWARD]+margin &&
            std::hypot(relative[12],relative[13])<=v[RADIUS]+margin;
    }
    bool update(const std::optional<vr_camera::Matrix>& right,const std::optional<vr_camera::Matrix>& left,float grip,bool allowed) {
        if(!allowed || !right || !left || !vr_camera::rigid(*right) || !vr_camera::rigid(*left) || !std::isfinite(grip)) {reset();return false;}
        if(grip<=.35f) {m_released=true;m_state.held=false;}
        const auto relative=vr_camera::multiply(*left,vr_camera::inverse(*right));
        m_state.forward=-relative[14];m_state.radial=std::hypot(relative[12],relative[13]);
        m_state.in_zone=contains(right,left);
        if(!m_state.in_zone) m_state.held=false;
        else if(m_released && grip>=.65f) {if(!m_state.held)++m_engagements;m_state.held=true;}
        return m_state.held;
    }
private:
    Settings m_settings;Snapshot m_state;bool m_released{};uint64_t m_engagements{};
};
