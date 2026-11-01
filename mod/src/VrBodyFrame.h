#pragma once
#include "VrCameraPose.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

// Shared by the body-relative gestures (holsters, guard, grenade). Positions are in the
// head's yaw-only frame, standing-space metres, before world scale or rig snaps, so
// looking up/down, snap turns and room position never move a zone. VrSession body-scales
// the hands first (scaled_from_head), so zone constants are in Joe's metres for any player.
namespace vr_body {
struct Offset { float height{},lateral{},forward{}; }; // Up, towards the right, along gaze yaw.

// Unit gaze-yaw direction (x,z) of an OpenVR head matrix (gaze along -Z).
inline std::optional<std::array<float,2>> yaw_forward(const vr_camera::Matrix& head) {
    float fx=-head[8],fz=-head[10];
    // Looking steeply down at the hip (or up) flattens the gaze. The top of the head
    // then points forward (down) or backward (up), so take yaw from the up axis.
    if(std::hypot(fx,fz)<0.5f) { const float sign=-head[9]<0?1.0f:-1.0f;fx=sign*head[4];fz=sign*head[6]; }
    const float length=std::hypot(fx,fz);
    if(!(length>0.2f)) return std::nullopt;
    return std::array<float,2>{fx/length,fz/length};
}
// A world/standing-space vector expressed in the head-yaw frame.
inline Offset direction(const std::array<float,2>& forward,float dx,float dy,float dz) {
    return {dy,-dx*forward[1]+dz*forward[0],dx*forward[0]+dz*forward[1]};
}
inline std::optional<Offset> locate(const vr_camera::Matrix& head,const vr_camera::Matrix& hand) {
    const auto forward=yaw_forward(head);if(!forward) return std::nullopt;
    return direction(*forward,hand[12]-head[12],hand[13]-head[13],hand[14]-head[14]);
}
// The hand moved so its offset from the head is multiplied by factor (VrBodyScale): a
// shorter player's reach expressed in Joe's metres, so zones tuned to his body fit theirs.
// Orientation is unchanged.
inline vr_camera::Matrix scaled_from_head(const vr_camera::Matrix& head,vr_camera::Matrix hand,float factor) {
    for(int j=0;j<3;++j) hand[12+j]=head[12+j]+(hand[12+j]-head[12+j])*factor;
    return hand;
}
// The controller's pointing axis (-Z) in the head-yaw frame.
inline std::optional<Offset> pointing(const vr_camera::Matrix& head,const vr_camera::Matrix& hand) {
    const auto forward=yaw_forward(head);if(!forward) return std::nullopt;
    return direction(*forward,-hand[8],-hand[9],-hand[10]);
}

// Named, bounded, hot-reloadable float parameters ("name=value" lines), as the holster
// and swing configs use. Extra cross-parameter rules go in the owner's valid().
template<size_t N> struct Parameters {
    struct Parameter { const char* name; float initial,minimum,maximum; };
    std::array<float,N> values{};
    bool operator==(const Parameters&) const = default;
    static Parameters defaults(const std::array<Parameter,N>& table) { Parameters p;for(size_t i=0;i<N;++i) p.values[i]=table[i].initial;return p; }
    bool in_range(const std::array<Parameter,N>& table) const {
        for(size_t i=0;i<N;++i) if(!std::isfinite(values[i]) || values[i]<table[i].minimum || values[i]>table[i].maximum) return false;
        return true;
    }
    static std::optional<Parameters> read(std::istream& input,const std::array<Parameter,N>& table) {
        Parameters result=defaults(table);std::array<bool,N> seen{};std::string line;
        while(std::getline(input,line)) {
            const auto first=line.find_first_not_of(" \t\r");
            if(first==std::string::npos || line[first]=='#') continue;
            const auto split=line.find('=');if(split==std::string::npos) return {};
            size_t index=0;while(index<N && line.substr(0,split)!=table[index].name) ++index;
            if(index==N || seen[index]) return {};
            std::istringstream number{line.substr(split+1)};
            if(!(number>>result.values[index]) || !(number>>std::ws).eof()) return {};
            seen[index]=true;
        }
        if(!input.eof() || !std::ranges::all_of(seen,[](bool v){return v;}) || !result.in_range(table)) return {};
        return result;
    }
    bool write(std::ostream& out,const std::array<Parameter,N>& table) const {
        out<<std::setprecision(9);
        for(size_t i=0;i<N;++i) out<<table[i].name<<'='<<values[i]<<'\n';
        return bool(out);
    }
};
} // namespace vr_body
