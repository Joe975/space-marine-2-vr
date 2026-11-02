#pragma once
#include "VrCameraPose.h"
#include <optional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <string_view>

// chase: the stock game camera, only on a -ChaseCam launch whose pack keeps the native camera files.
enum class VrViewMode { first_person, diorama, shoulder, chase };

struct VrViewSettings {
    VrViewMode mode{VrViewMode::first_person};
    float diorama_scale{10}, diorama_distance{0.5f}, diorama_height{0.6f}, diorama_pitch{0}, diorama_follow{0}, diorama_recenter{1.5f};
    float shoulder_distance{2}, shoulder_height{0.25f}, shoulder_side{0.4f}, shoulder_scale{1};
    float chase_scale{1};
    struct Parameter { const char* name; float VrViewSettings::*field; float initial,minimum,maximum; const char* unit; };
    inline static constexpr float DEFAULT_DIORAMA_SCALE=10;
    inline static constexpr std::array<Parameter,11> PARAMETERS{{
        {"diorama_scale",&VrViewSettings::diorama_scale,DEFAULT_DIORAMA_SCALE,2,40,"world scale multiplier"},
        {"diorama_distance",&VrViewSettings::diorama_distance,0.5f,0.25f,3,"physical metres"},
        {"diorama_height",&VrViewSettings::diorama_height,0.6f,0,2,"physical metres above native head camera"},
        // 0 since 27 Oct: any pitch tilts the whole table against the real horizon (Joe in the headset).
        {"diorama_pitch",&VrViewSettings::diorama_pitch,0,0,65,"degrees the table tilts toward you"},
        {"diorama_follow",&VrViewSettings::diorama_follow,0,0,5,"physical metres the Marine walks before the table slides back under you (0 off)"},
        // At the default scale: a fixed walk in the game world, so it stays the same part of the map at any scale.
        {"diorama_recenter",&VrViewSettings::diorama_recenter,1.5f,0,5,"physical metres at the default diorama_scale the Marine walks before a recenter (0 off)"},
        {"shoulder_distance",&VrViewSettings::shoulder_distance,2,0.25f,5,"metres at shoulder_scale 1 behind native head camera"},
        {"shoulder_height",&VrViewSettings::shoulder_height,0.25f,-0.5f,2,"metres at shoulder_scale 1"},
        {"shoulder_side",&VrViewSettings::shoulder_side,0.4f,-1.5f,1.5f,"metres right at shoulder_scale 1"},
        // Stops well short of the diorama's 40: the camera follows the Marine and doesn't stop at walls (Joe, 2 Nov).
        {"shoulder_scale",&VrViewSettings::shoulder_scale,1,0.5f,4,"world scale multiplier"},
        {"chase_scale",&VrViewSettings::chase_scale,1,0.5f,20,"world scale multiplier"}}};
    // Gamepad Back + D-pad (third person): one press changes the world scale by this factor.
    inline static constexpr float SCALE_STEP=1.41421356f;
    bool operator==(const VrViewSettings&) const = default;
    bool valid() const {
        if(mode<VrViewMode::first_person || mode>VrViewMode::chase) return false;
        for(const auto& p:PARAMETERS) if(!std::isfinite(this->*p.field) || this->*p.field<p.minimum || this->*p.field>p.maximum) return false;
        return true;
    }
    static const char* name(VrViewMode mode) {
        switch(mode) {case VrViewMode::first_person:return "first_person";case VrViewMode::diorama:return "diorama";case VrViewMode::shoulder:return "shoulder";case VrViewMode::chase:return "chase";default:return "invalid";}
    }
    // The camera files are installed before the game starts, so chase and the head-camera
    // modes never mix in one launch. Files and F8 cannot select chase.
    static bool allowed(VrViewMode mode,bool chase_launch) { return chase_launch==(mode==VrViewMode::chase); }
    static VrViewMode next(VrViewMode mode) {
        return mode==VrViewMode::first_person?VrViewMode::diorama:mode==VrViewMode::diorama?VrViewMode::shoulder:VrViewMode::first_person;
    }
    float scale(float life_size) const { return life_size*(mode==VrViewMode::diorama?diorama_scale:mode==VrViewMode::shoulder?shoulder_scale:mode==VrViewMode::chase?chase_scale:1); }
    // steps>0 makes the world bigger (a smaller multiplier), steps<0 smaller, within the mode's
    // range and rounded to 0.01 so the saved file stays readable. Third person only. False when
    // nothing changed.
    bool resize_world(int steps) {
        float VrViewSettings::* field=mode==VrViewMode::diorama?&VrViewSettings::diorama_scale:mode==VrViewMode::shoulder?&VrViewSettings::shoulder_scale:mode==VrViewMode::chase?&VrViewSettings::chase_scale:nullptr;
        if(!field || !steps) return false;
        const auto& p=*std::ranges::find(PARAMETERS,field,&Parameter::field);
        const float value=std::clamp(std::round(this->*field*std::pow(SCALE_STEP,float(-steps))*100)/100,p.minimum,p.maximum);
        if(value==this->*field) return false;
        this->*field=value;return true;
    }
    // A chase launch shows the file's knobs in chase mode (VrViewFile writes back the file's own
    // mode): a file never selects chase.
    VrViewSettings for_launch(bool chase_launch) const { auto result=*this;if(chase_launch) result.mode=VrViewMode::chase;return result; }
    static std::optional<VrViewSettings> read(std::istream& input) {
        VrViewSettings result;std::array<bool,PARAMETERS.size()> seen{};bool mode_seen{};std::string line;
        while(std::getline(input,line)) {
            const auto first=line.find_first_not_of(" \t\r");if(first==std::string::npos || line[first]=='#') continue;
            const auto split=line.find('=');if(split==std::string::npos) return {};
            const auto key=line.substr(0,split);std::istringstream value{line.substr(split+1)};
            if(key=="mode" && !mode_seen) {
                std::string word;if(!(value>>word) || !(value>>std::ws).eof()) return {};
                if(word=="first_person") result.mode=VrViewMode::first_person;
                else if(word=="diorama") result.mode=VrViewMode::diorama;
                else if(word=="shoulder") result.mode=VrViewMode::shoulder;
                else return {};
                mode_seen=true;continue;
            }
            size_t i=0;while(i<PARAMETERS.size() && key!=PARAMETERS[i].name) ++i;
            if(i==PARAMETERS.size() || seen[i] || !(value>>result.*PARAMETERS[i].field) || !(value>>std::ws).eof()) return {};
            seen[i]=true;
        }
        // shoulder_scale came after the first release: a player's saved file without it keeps its other knobs.
        for(size_t i=0;i<seen.size();++i) if(!seen[i] && PARAMETERS[i].field!=&VrViewSettings::shoulder_scale) return {};
        return input.eof() && mode_seen && result.valid()?std::optional{result}:std::nullopt;
    }
    static std::optional<VrViewSettings> load(const std::filesystem::path& path) { std::ifstream input{path};return read(input); }
    // The header and the shortest float that reads back exactly (0.6, not 0.600000024): a mode change
    // rewrites the file, and the repo's config/index_camera_modes.calibration must not churn (27 Oct).
    static constexpr const char* HEADER="# F8 cycles modes; F9 or gamepad Back/View recenters third person; Back held + D-pad up/down scales the world.";
    void write(std::ostream& output) const {
        output<<HEADER<<'\n'<<"mode="<<name(mode)<<'\n';
        for(const auto& p:PARAMETERS) {
            char text[32];const auto end=std::to_chars(text,text+sizeof text,this->*p.field).ptr;
            output<<p.name<<'='<<std::string_view{text,static_cast<size_t>(end-text)}<<'\n';
        }
    }
    bool save(const std::filesystem::path& path) const {
        if(!path.is_absolute() || !valid()) return false;
        std::ofstream output{path};write(output);
        output.flush();return bool(output);
    }
};

// The camera-mode file holds the knobs and the mode a launch starts in. Saves never write the live
// mode: F8 used to, so each launch started in whatever mode the last one ended in (27 Oct). The 1 s
// poll applies only what changed on disk since the mod last read or wrote it (a hand edit, a preset).
class VrViewFile {
public:
    VrViewSettings start(const std::optional<VrViewSettings>& on_disk,bool chase_launch) {
        m_last=on_disk;return (on_disk?*on_disk:VrViewSettings{}).for_launch(chase_launch);
    }
    std::optional<VrViewSettings> changed(const std::optional<VrViewSettings>& on_disk,bool chase_launch) {
        if(!on_disk || on_disk==m_last) return std::nullopt;
        m_last=on_disk;return on_disk->for_launch(chase_launch);
    }
    // The active knobs with the file's own mode, remembered so the poll does not apply it back.
    VrViewSettings to_save(const VrViewSettings& active) {
        auto file=active;file.mode=m_last?m_last->mode:VrViewMode::first_person;m_last=file;return file;
    }
private:
    std::optional<VrViewSettings> m_last;
};

// Owns the third-person world anchor, never the character. Values come from the exact
// issued eye so a live settings change cannot mix two configurations in one image.
class VrViewCamera {
public:
    struct Focus { uintptr_t actor{}; std::array<float,3> position{}; };
    struct Snapshot { bool anchored{}; uint64_t captures{},follows{},recenters{}; vr_camera::Matrix anchor{vr_camera::IDENTITY}; };
    vr_camera::Matrix camera(const vr_camera::Matrix& game,const VrViewSettings& settings,
        uint64_t reference,float eye_scale,std::optional<Focus> focus,bool scripted) {
        std::scoped_lock lock{m_mutex};
        vr_camera::Matrix basis{};
        if(settings.mode==VrViewMode::chase) {
            // The native camera is already the stock chase camera: level it and follow it every frame.
            if(!settings.valid() || !reference || scripted || !std::isfinite(eye_scale) || eye_scale<0.01f || eye_scale>100 ||
                !vr_camera::level_anchor(game,basis,false)) {m_state.anchored=false;return game;}
            if(!m_state.anchored || reference!=m_reference || settings!=m_settings) ++m_state.captures;
            m_state.anchor=basis;m_state.anchored=true;m_reference=reference;m_actor=0;m_settings=settings;
            return basis;
        }
        if(settings.mode==VrViewMode::first_person || !settings.valid() || !reference || scripted || !focus || !focus->actor ||
            !std::isfinite(eye_scale) || eye_scale<0.01f || eye_scale>100 || !vr_camera::level_anchor(game,basis,false)) {
            m_state.anchored=false;return game;
        }
        for(float p:focus->position) if(!std::isfinite(p) || std::abs(p)>1000000) {m_state.anchored=false;return game;}
        const auto life_size=eye_scale/(settings.mode==VrViewMode::diorama?settings.diorama_scale:settings.shoulder_scale);
        const bool discontinuity=m_state.anchored && std::hypot(focus->position[0]-m_position[0],focus->position[1]-m_position[1],focus->position[2]-m_position[2])>10*life_size;
        const bool changed=!m_state.anchored || reference!=m_reference || focus->actor!=m_actor || settings!=m_settings || discontinuity;
        m_position=focus->position;
        if(settings.mode==VrViewMode::shoulder || changed) {
            auto anchor=basis;
            const bool diorama=settings.mode==VrViewMode::diorama;
            const float distance=diorama?settings.diorama_distance:settings.shoulder_distance;
            const float height=diorama?settings.diorama_height:settings.shoulder_height;
            const float side=diorama?0:settings.shoulder_side;
            for(int j=0;j<3;++j) anchor[12+j]+=eye_scale*(side*basis[j]-distance*basis[8+j]);
            anchor[13]+=eye_scale*height;
            if(diorama) {
                const auto angle=settings.diorama_pitch*0.01745329252f,c=std::cos(angle),s=std::sin(angle);
                for(int j=0;j<3;++j) {anchor[4+j]=c*basis[4+j]+s*basis[8+j];anchor[8+j]=-s*basis[4+j]+c*basis[8+j];}
            }
            m_state.anchor=anchor;m_state.anchored=true;m_reference=reference;m_actor=focus->actor;m_settings=settings;
            if(changed) {++m_state.captures;m_anchored_at=focus->position;m_recenter_asked=false;}
        } else {
            const std::array<float,3> walked{focus->position[0]-m_anchored_at[0],focus->position[1]-m_anchored_at[1],focus->position[2]-m_anchored_at[2]};
            // The Marine walked off the table: ask for a recenter, once. It lands on a later frame
            // through the frame loop, as F9 and Back do, so it never splits an eye pair. Horizontal only:
            // a lift or a jump moves him up and down the same table.
            if(!m_recenter_asked && settings.diorama_recenter>0 &&
                std::hypot(walked[0],walked[2])>settings.diorama_recenter*VrViewSettings::DEFAULT_DIORAMA_SCALE*life_size) {
                m_recenter_asked=m_recenter_due=true;++m_state.recenters;
            }
            // The Marine walked diorama_follow physical metres across the table: slide the table back
            // under the player by that walk, without turning it (a recenter would turn it to his heading).
            if(settings.diorama_follow>0 && std::hypot(walked[0],walked[1],walked[2])>settings.diorama_follow*eye_scale) {
                for(int j=0;j<3;++j) m_state.anchor[12+j]+=walked[j];
                m_anchored_at=focus->position;++m_state.follows;
            }
        }
        return m_state.anchor;
    }
    Snapshot snapshot() const { std::scoped_lock lock{m_mutex};return m_state; }
    // The frame loop takes this and recenters. Dropped by reset(): a recenter that lands after a
    // switch to first person would take a seated eye-height reading.
    bool take_recenter() {std::scoped_lock lock{m_mutex};const bool result=m_recenter_due;m_recenter_due=false;return result;}
    void reset() {std::scoped_lock lock{m_mutex};m_state.anchored=false;m_recenter_due=false;}
private:
    mutable std::mutex m_mutex;
    Snapshot m_state;
    VrViewSettings m_settings;
    uint64_t m_reference{};
    uintptr_t m_actor{};
    std::array<float,3> m_position{},m_anchored_at{}; // focus last frame / where the diorama anchor last moved
    bool m_recenter_asked{},m_recenter_due{}; // Asked since the anchor last moved / not yet taken by the frame loop.
};

// Queue message edges so a short key down/up between Presents is never lost.
struct VrViewHotkeys {
    struct Actions { bool cycle{},recenter{}; };
    void press(bool foreground,bool cycle,bool repeat) {
        std::scoped_lock lock{m_mutex};
        if(foreground && !repeat) {if(cycle) m_pending.cycle=true;else m_pending.recenter=true;}
    }
    Actions take(bool foreground) {
        std::scoped_lock lock{m_mutex};const auto result=foreground?m_pending:Actions{};m_pending={};return result;
    }
    bool message(bool foreground,unsigned message,uintptr_t key,intptr_t detail) {
        // Win32 WM_KEYDOWN/UP, WM_KILLFOCUS and VK_F8/F9. Keep the event adapter
        // testable without the framework or a live window.
        if(message==0x8) {take(false);return false;}
        if((message==0x100 || message==0x101) && (key==0x77 || key==0x78)) {
            if(message==0x100) press(foreground,key==0x77,(detail&(1LL<<30))!=0);
            return true;
        }
        return false;
    }
private: std::mutex m_mutex;Actions m_pending;
};
