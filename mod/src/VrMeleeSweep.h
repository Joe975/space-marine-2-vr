#pragma once
// Where the game's melee hit check sees the sword (docs/MELEE_SWEEP.md).
//
// A light attack deals damage through MeleeDetector: during the animation's damaging window it
// sweeps the GUN_R segment from last frame's position to this frame's, read from the evaluated
// skeleton (animINST+0xc0). The mod's arm driver puts GUN_R at the controller, so the sweep sees
// the sword at Joe's hand, which has stopped by then (the window opens a median 0.5 s after the
// swing commits): a still segment sweeps no area and hits nothing.
//
// This class decides which sword the detector sees instead; the sword drawn on screen stays at
// the hand. Mode::replay replays the path Joe's sword took around the swing commit, compressed
// into the start of the window and carried along with the character's root (the attack's lunge
// and turn). Mode::animation hands the detector the animation's own sword, as in the flat game.
// Mode::hand leaves the detector alone (the behaviour before 28 Oct).
//
// Transforms are GUN_R relative to the rig's root bone (product of the locals below the root),
// so world = relative * root world at the time the detector reads it. Thread safe: record() runs
// on the publication thread, swing() on the input thread, pose() on the detector's worker thread.
#include "VrCameraPose.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <array>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace vr_melee_sweep {
using Clock=std::chrono::steady_clock;
using Matrix=vr_camera::Matrix;
using std::chrono::milliseconds;

enum class Mode { hand, replay, animation };
inline std::optional<Mode> parse(std::string_view text) {
    while(!text.empty() && (text.back()=='\n' || text.back()=='\r' || text.back()==' ')) text.remove_suffix(1);
    if(text=="hand") return Mode::hand;
    if(text=="replay") return Mode::replay;
    if(text=="animation") return Mode::animation;
    return std::nullopt;
}
inline const char* name(Mode m) { return m==Mode::hand?"hand":m==Mode::replay?"replay":"animation"; }

// LEAD/TAIL: the replayed path runs from 250 ms before the swing commit (the stroke's start; Joe's
// strokes take a median 0.19 s from a slow hand to the commit) to 200 ms after (the follow-through).
// DURATION: the path is swept in the window's first 120 ms. MAX_AGE: a window more than 1.2 s after
// the last swing replays nothing (the native windows opened 0.02-0.85 s after the swing, 28 Oct).
// GAP: detector reads this far apart belong to different windows. KEEP: recorded history.
// LATEST: the newest sample must be this recent to stand for the hand's sword now (samples arrive about
// every 17 ms while the arm driver commits; when it stops, the native read is left alone).
inline constexpr auto LEAD=milliseconds(250),TAIL=milliseconds(200),DURATION=milliseconds(120),MAX_AGE=milliseconds(1200),
    GAP=milliseconds(150),KEEP=milliseconds(2000),LATEST=milliseconds(100);

// Linear blend of two rigid transforms, re-orthonormalised (x from the blend, z made orthogonal to
// x, y = z x x, keeping the inputs' handedness). Close samples at the publication rate only.
inline Matrix blend(const Matrix& a,const Matrix& b,float t) {
    Matrix out=vr_camera::IDENTITY;
    for(int i=0;i<16;++i) out[i]=a[i]+(b[i]-a[i])*t;
    auto row=[&](int r){return std::array<float,3>{out[r*4],out[r*4+1],out[r*4+2]};};
    auto set=[&](int r,std::array<float,3> v){for(int i=0;i<3;++i) out[r*4+i]=v[i];};
    auto dot=[](std::array<float,3> u,std::array<float,3> v){return u[0]*v[0]+u[1]*v[1]+u[2]*v[2];};
    auto unit=[&](std::array<float,3> v){const float l=std::sqrt(dot(v,v));if(l>1e-6f) for(auto& c:v) c/=l;return v;};
    auto cross=[](std::array<float,3> u,std::array<float,3> v){return std::array<float,3>{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};};
    const float sign=dot(cross(row(0),row(1)),row(2))<0?-1.f:1.f;
    auto x=unit(row(0));auto z=row(2);const float d=dot(z,x);for(int i=0;i<3;++i) z[i]-=d*x[i];z=unit(z);
    auto y=cross(z,x);for(auto& c:y) c*=sign;
    set(0,x);set(1,y);set(2,z);out[3]=out[7]=out[11]=0;out[15]=1;
    return out;
}

// A bone relative to its highest ancestor that has an evaluated node (mapped(i)): the product of
// its local matrices up to, not including, that ancestor (world = relative * ancestor world, the
// native evaluation C0[n] = D0[n] * C0[parent]). The rig's own root may have no evaluated node
// (live 29 Oct), so the anchor is the topmost one the game evaluates, nearest the root motion.
struct Chain { Matrix relative; size_t root; };
template<class Mapped> std::optional<Chain> below_root(std::span<const Matrix> locals,std::span<const int32_t> parents,size_t bone,Mapped mapped) {
    if(bone>=locals.size() || parents.size()!=locals.size()) return std::nullopt;
    std::vector<size_t> chain{bone};
    for(size_t steps=0;parents[chain.back()]>=0;++steps) {
        const auto p=parents[chain.back()];
        if(size_t(p)>=locals.size() || steps>=locals.size()) return std::nullopt;
        chain.push_back(size_t(p));
    }
    size_t top=chain.size();
    for(size_t k=chain.size();k-->1;) if(mapped(chain[k])) { top=k;break; }
    if(top==chain.size()) return std::nullopt;
    auto relative=locals[bone];
    for(size_t k=1;k<top;++k) relative=vr_camera::multiply(relative,locals[chain[k]]);
    return Chain{relative,chain[top]};
}
inline std::optional<Chain> below_root(std::span<const Matrix> locals,std::span<const int32_t> parents,size_t bone) {
    return below_root(locals,parents,bone,[](size_t){return true;});
}

class Sweep {
public:
    struct Snapshot { Mode mode{Mode::replay}; uint64_t windows{},replays{},animation{},no_swing{},reads{},samples{},refused{};
        float last_window_ms{},last_swing_age_ms{}; uint32_t last_path{}; };
    void set_mode(Mode m) { std::lock_guard lock{m_mutex};m_state.mode=m; }
    Mode mode() const { std::lock_guard lock{m_mutex};return m_state.mode; }
    // One publication: GUN_R relative to the root bone as drawn (final) and as animated (native).
    void record(Clock::time_point now,const Matrix& final,const Matrix& native) {
        std::lock_guard lock{m_mutex};
        if(!m_samples.empty() && now<=m_samples.back().time) return;
        m_samples.push_back({now,final,native});++m_state.samples;
        while(!m_samples.empty() && now-m_samples.front().time>KEEP) m_samples.pop_front();
    }
    // A physical swing committed (the native attack request went out).
    void swing(Clock::time_point now) { std::lock_guard lock{m_mutex};m_swing=now; }
    // The player's detector reads GUN_R now (detector identifies the window's owner). Returns the
    // relative transform the detector should see, or nothing to leave the native read alone.
    std::optional<Matrix> pose(uintptr_t detector,Clock::time_point now) {
        std::lock_guard lock{m_mutex};
        if(detector!=m_detector || !m_last_read || now-*m_last_read>GAP || now<*m_last_read) begin(detector,now);
        m_last_read=now;
        if(!m_window_mode) return std::nullopt;
        ++m_state.reads;
        m_state.last_window_ms=std::chrono::duration<float,std::milli>(now-m_window).count();
        if(*m_window_mode==Mode::animation) return fresh_locked(now)?std::optional{m_samples.back().native}:std::nullopt;
        const float t=std::clamp(std::chrono::duration<float>(now-m_window).count()/std::chrono::duration<float>(DURATION).count(),0.f,1.f);
        const float at=t*float(m_path.size()-1);
        const size_t i=std::min(size_t(at),m_path.size()-2);
        return blend(m_path[i],m_path[i+1],at-float(i));
    }
    // The world matrix the detector should see, from the native read of GUN_R (the hand's sword,
    // = latest recorded relative * root world): the chosen relative * inverse(latest relative) *
    // native. No second read of the evaluated skeleton is needed (the root bone's slot was refused
    // in the 29 Oct battle simulator). Nothing to leave the native read alone.
    std::optional<Matrix> world(uintptr_t detector,Clock::time_point now,const Matrix& native) {
        const auto relative=pose(detector,now);
        if(!relative) return std::nullopt;
        std::optional<Matrix> latest;
        {std::lock_guard lock{m_mutex};if(fresh_locked(now)) latest=invert(m_samples.back().final);if(!latest) ++m_state.refused;}
        if(!latest) return std::nullopt;
        return vr_camera::multiply(vr_camera::multiply(*relative,*latest),native);
    }
    Snapshot snapshot() const { std::lock_guard lock{m_mutex};return m_state; }
    // A newest sample no older than LATEST (one recorded after `now` counts as current).
    bool fresh_locked(Clock::time_point now) const { return !m_samples.empty() && (now<=m_samples.back().time || now-m_samples.back().time<=LATEST); }
    // Inverse of an affine row-vector matrix (rotation and scale in rows 0-2, translation in row 3).
    static std::optional<Matrix> invert(const Matrix& m) {
        const float a=m[0],b=m[1],c=m[2],d=m[4],e=m[5],f=m[6],g=m[8],h=m[9],i=m[10];
        const float A=e*i-f*h,B=-(d*i-f*g),C=d*h-e*g,det=a*A+b*B+c*C;
        if(!std::isfinite(det) || std::abs(det)<1e-8f) return std::nullopt;
        const float s=1/det;
        Matrix out=vr_camera::IDENTITY;
        out[0]=A*s;out[1]=-(b*i-c*h)*s;out[2]=(b*f-c*e)*s;
        out[4]=B*s;out[5]=(a*i-c*g)*s;out[6]=-(a*f-c*d)*s;
        out[8]=C*s;out[9]=-(a*h-b*g)*s;out[10]=(a*e-b*d)*s;
        for(int j=0;j<3;++j) out[12+j]=-(m[12]*out[j]+m[13]*out[4+j]+m[14]*out[8+j]);
        return out;
    }
private:
    struct Sample { Clock::time_point time; Matrix final,native; };
    void begin(uintptr_t detector,Clock::time_point now) {
        m_detector=detector;m_window=now;m_window_mode.reset();m_path.clear();++m_state.windows;m_state.last_window_ms=0;
        if(m_state.mode==Mode::hand) return;
        if(m_state.mode==Mode::animation) { m_window_mode=Mode::animation;++m_state.animation;return; }
        if(!m_swing || now<*m_swing || now-*m_swing>MAX_AGE) { ++m_state.no_swing;return; }
        m_state.last_swing_age_ms=std::chrono::duration<float,std::milli>(now-*m_swing).count();
        for(const auto& s:m_samples) if(s.time>=*m_swing-LEAD && s.time<=*m_swing+TAIL) m_path.push_back(s.final);
        m_state.last_path=uint32_t(m_path.size());
        if(m_path.size()<2) { m_path.clear();++m_state.no_swing;return; }
        m_window_mode=Mode::replay;++m_state.replays;
    }
    mutable std::mutex m_mutex;
    Snapshot m_state;
    std::deque<Sample> m_samples;
    std::optional<Clock::time_point> m_swing,m_last_read;
    uintptr_t m_detector{};
    Clock::time_point m_window{};
    std::optional<Mode> m_window_mode;
    std::vector<Matrix> m_path;
};
}
