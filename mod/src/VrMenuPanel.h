#pragma once
#include "VrCameraPose.h"
#include "SceneMode.h"
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <algorithm>
#include <chrono>

struct VrMenuPanel {
    bool enabled{true};float width_degrees{90},distance_metres{2};
    // Runtime only (never saved): the level head pose the panel is fixed to in tracking space.
    // Without one the panel follows the head, as it did before 23 Oct.
    std::optional<vr_camera::Matrix> anchor;
    bool valid() const {return std::isfinite(width_degrees) && width_degrees>=40 && width_degrees<=100 &&
        std::isfinite(distance_metres) && distance_metres>=1 && distance_metres<=5;}
    // The pause menu (0x908) as the scene classifier sees it (SceneMode.h). 0x800 alone is NOT a menu:
    // the walkable Battle Barge publishes it the whole time (24 Oct evening: the whole barge went to the
    // panel); menus over the barge come through UI coverage (VrScreenMenu.h) instead.
    bool active(bool available,uint32_t published,uint32_t requested,bool scripted) const {
        return enabled && scene_mode::classify({available,published,requested,scripted}).pause_menu();
    }
    // Shader constants: three rows (u, v, w) of a homography from the eye image's uv (x, y, 1)
    // to the menu canvas; canvas uv = (u, v)/w, and w <= 0 means the ray misses the panel.
    // The full canvas lies on a plane distance_metres in front of the anchor (or the head).
    using Homography=std::array<float,12>;
    std::optional<Homography> transform(const vr_camera::Matrix& projection,const vr_camera::Matrix& eye,float aspect,
        const vr_camera::Matrix& head=vr_camera::IDENTITY) const {
        const auto& frame=anchor?*anchor:head;
        if(!valid() || !std::isfinite(aspect) || aspect<=0 || !vr_camera::rigid(eye) || !vr_camera::rigid(head) || !vr_camera::rigid(frame) ||
            !std::ranges::all_of(projection,[](float f){return std::isfinite(f);}) || projection[0]<=0 || projection[5]<=0) return std::nullopt;
        const float width=2*distance_metres*std::tan(width_degrees*3.14159265358979323846f/360),height=width/aspect;
        // Eye pose in the panel frame (row vectors: right, up, back, position).
        const auto q=vr_camera::multiply(vr_camera::multiply(eye,head),vr_camera::inverse(frame));
        const float c=-distance_metres-q[14];
        if(c>-.05f) return std::nullopt; // The eye must be in front of the plane.
        // Eye ray for image uv (x, y): x*ax + y*ay + a0, from the projection's tangents.
        std::array<std::array<float,3>,3> rays{};
        for(int k=0;k<3;++k) {
            rays[0][k]=2/projection[0]*q[k];
            rays[1][k]=-2/projection[5]*q[4+k];
            rays[2][k]=(-1-projection[8])/projection[0]*q[k]+(1-projection[9])/projection[5]*q[4+k]-q[8+k];
        }
        // Scaled by c so that w > 0 exactly when the hit is in front of the eye.
        Homography out{};
        for(int column=0;column<3;++column) {const auto& d=rays[column];
            out[column]=c*(.5f*d[2]+(q[12]*d[2]+c*d[0])/width);
            out[4+column]=c*(.5f*d[2]-(q[13]*d[2]+c*d[1])/height);
            out[8+column]=c*d[2];
        }
        if(!std::ranges::all_of(out,[](float f){return std::isfinite(f);})) return std::nullopt;
        return out;
    }
    bool save(const std::filesystem::path& path) const {
        if(!valid()) return false;std::ofstream out{path};
        out<<"version=1\nenabled="<<enabled<<"\nwidth_degrees="<<width_degrees<<"\ndistance_metres="<<distance_metres<<'\n';return bool(out);
    }
    static std::optional<VrMenuPanel> load(const std::filesystem::path& path) {
        std::ifstream in{path};if(!in) return std::nullopt;VrMenuPanel value;std::string line;unsigned seen{};
        try {while(std::getline(in,line)) {
            if(line.empty()) continue;const auto at=line.find('=');if(at==std::string::npos) return std::nullopt;
            const auto key=line.substr(0,at);size_t consumed{};const auto number=std::stof(line.substr(at+1),&consumed);
            const auto tail=line.substr(at+1+consumed);if(!tail.empty() && tail!="\r") return std::nullopt;
            unsigned bit{};
            if(key=="version" && number==1) bit=1;
            else if(key=="enabled" && (number==0 || number==1)) {bit=2;value.enabled=number!=0;}
            else if(key=="width_degrees") {bit=4;value.width_degrees=number;}
            else if(key=="distance_metres") {bit=8;value.distance_metres=number;}
            else return std::nullopt;
            if(seen&bit) return std::nullopt;seen|=bit;
        }} catch(...) {return std::nullopt;}
        return seen==15 && value.valid()?std::optional{value}:std::nullopt;
    }
    bool operator==(const VrMenuPanel&) const=default;
};

// Fixes the panel in the room: the first panel frame after a gap takes the level head pose
// (heading and position) as its anchor; later frames keep it until the panel goes away.
class VrMenuAnchor {
public:
    static constexpr auto GAP=std::chrono::milliseconds(500);
    VrMenuPanel apply(VrMenuPanel panel,const vr_camera::Matrix& head,std::chrono::steady_clock::time_point now) {
        if(!m_anchor || now<m_last || now-m_last>GAP) {vr_camera::Matrix level;m_anchor.reset();if(vr_camera::level_anchor(head,level)) m_anchor=level;}
        m_last=now;panel.anchor=m_anchor;return panel;
    }
private:
    std::optional<vr_camera::Matrix> m_anchor;std::chrono::steady_clock::time_point m_last{};
};
