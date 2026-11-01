#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include "GameBuild.h"

// The native GPU visibility shaders treat camera slot 0 as the only main view:
// any other HZB-enabled camera (our ordinary slot 1) goes down the shadow camera
// path, whose constants retail never fills. That dropped props from the right eye.
// The global switch EXE+0x4cb4003 (byte, native 1) disables HZB culling for the
// whole visibility pipeline; frustum/distance culling remains. The HZB rebuild
// loop doesn't read it. docs/HZB_STEREO_2026-11-01.md has the pipeline.
namespace native_hzb_culling {
constexpr uint32_t IMAGE_TIMESTAMP=0x6ac3e716;
constexpr uintptr_t SWITCH_RVA=0x4cb4003;
enum class Result { disabled,already_disabled,kept_native,unsupported_build,unexpected_value,write_failed };
inline const char* name(Result r){switch(r){case Result::disabled:return "disabled";case Result::already_disabled:return "already_disabled";
    case Result::kept_native:return "kept_native";case Result::unsupported_build:return "unsupported_build";case Result::unexpected_value:return "unexpected_value";default:return "write_failed";}}
// Writes 0 only on the supported build and only over a native boolean value.
inline Result apply(uint32_t timestamp,bool keep_native,const std::function<bool(uint8_t&)>& read,const std::function<bool(uint8_t)>& write){
    if(keep_native)return Result::kept_native;
    if(!game_build::supported(timestamp) || !game_build::rva(timestamp,SWITCH_RVA))return Result::unsupported_build;
    uint8_t value{};if(!read(value) || value>1)return Result::unexpected_value;
    if(value==0)return Result::already_disabled;
    uint8_t after{};if(!write(0) || !read(after) || after!=0)return Result::write_failed;
    return Result::disabled;
}

// The mod runs shared unless the launch says otherwise (-NativeHzb, or -HzbControl and hzb.txt, live).
// off: no occlusion culling at all. native: the game's culling, which
// loses right-eye props. left_only: slot 0 culls, slot 1 is frustum-only. shared: left_only, and an
// object the left eye could see but found hidden is skipped by the right eye too, and rocks and
// foliage test the left eye's HZB in both eyes. shared_objects: the same without rocks and foliage.
enum class Mode { off,native,left_only,shared_objects,shared };
constexpr Mode DEFAULT_MODE=Mode::shared;
inline const char* name(Mode m){switch(m){case Mode::off:return "off";case Mode::native:return "native";case Mode::left_only:return "left_only";case Mode::shared_objects:return "shared_objects";default:return "shared";}}
inline std::optional<Mode> parse_mode(std::string text){
    while(!text.empty() && (text.back()=='\r' || text.back()=='\n' || text.back()==' '))text.pop_back();
    if(text=="off")return Mode::off;if(text=="native")return Mode::native;if(text=="left_only")return Mode::left_only;if(text=="shared")return Mode::shared;if(text=="shared_objects")return Mode::shared_objects;return {};
}
constexpr bool switch_value(Mode m){return m!=Mode::off;}
constexpr bool second_eye_frustum_only(Mode m){return m!=Mode::off && m!=Mode::native;}
constexpr bool objects_inherit(Mode m){return m==Mode::shared || m==Mode::shared_objects;}
constexpr bool lwi_inherits(Mode m){return m==Mode::shared;}
// Camera mode and the tabletop view put the eyes far apart in world units, where what one eye can't see
// says little about the other. Sharing needs eyes close together; anything wider drops to left_only,
// which is always safe because the right eye draws everything in its own view. Play is about 0.09 units.
constexpr float MAX_SHARED_EYE_GAP=0.3f;
constexpr size_t ENTRY_POSITION=0x64;
constexpr bool eyes_close(const float* left,const float* right){
    float gap{};for(size_t axis=0;axis<3;++axis){const float d=left[axis]-right[axis];gap+=d*d;}
    return gap<=MAX_SHARED_EYE_GAP*MAX_SHARED_EYE_GAP;
}

constexpr uintptr_t VISIBILITY_RVA=0x1ee1130;       // the HZB rebuild and test pass
constexpr size_t ENTRY_FRUSTUM=0x190,OBJECT_BOUNDS=0x408;
constexpr uint64_t LEFT_EYE_BIT=1,RIGHT_EYE_BIT=2;
constexpr size_t LIST_ENTRIES=8,ENTRY_STRIDE=0xa60,ENTRY_SLOT=0x33,LIST_COUNT=0x29808;
constexpr uint32_t FLAG_MAIN=0x10,FLAG_NO_HZB=0x40;
constexpr uint32_t FLAGS_NOT_REBUILT=0x10080444;    // any of these: not in the rebuild loop
constexpr bool hzb_camera(uint32_t flags){return (flags&FLAGS_NOT_REBUILT)==0 && (flags&FLAG_MAIN)!=0;}
// True only for the two ordinary eyes: main cameras in slots 0 and 1.
constexpr bool stereo_pair(int32_t count,uint32_t left_flags,uint8_t left_slot,uint32_t right_flags,uint8_t right_slot){
    return count>=2 && count<=64 && left_slot==0 && right_slot==1 && hzb_camera(left_flags) && hzb_camera(right_flags);
}
// Only an object the right eye would draw and the left eye won't needs the frustum test. The GPU tests a
// tight rotated box and this one has a loose axis-aligned one, so a clear bit means hidden only for a box
// wholly inside the left eye's view. Anywhere else the bit may just mean off screen.
constexpr bool inherit_candidate(uint64_t mask){return (mask&(LEFT_EYE_BIT|RIGHT_EYE_BIT))==RIGHT_EYE_BIT;}
constexpr uint64_t inherit(uint64_t mask,bool inside_left_view){
    return inherit_candidate(mask) && inside_left_view ? mask&~RIGHT_EYE_BIT : mask;
}
// The LWI dispatch (0x1db8ef0, camera index in edi) forces OFF_GPU_HZB_CULLING unless the index is 0.
// Its shader tests the camera's own frustum, then one HZB with camera 0's matrix, and a box off the
// left eye's view skips that test, so index 1 can use the HZB variant as it stands. These 23 bytes
// hold the whole choice and nothing branches into them. The patch sets the key byte (rbp+0x10) to 1
// unless the index is 0 or 1 and the switch is on. Flags from the cmp before the site survive the mov.
constexpr uintptr_t LWI_KEY_RVA=0x1db9535;
constexpr size_t LWI_KEY_SIZE=23,LWI_SWITCH_END=0x12;
using LwiKey=std::array<uint8_t,LWI_KEY_SIZE>;
constexpr int32_t lwi_switch_disp(uintptr_t site,uintptr_t flag){return int32_t(flag-(site+LWI_SWITCH_END));}
constexpr LwiKey lwi_key_patch(int32_t disp){
    const auto d=static_cast<uint32_t>(disp);
    return {0xc6,0x45,0x10,0x01,                    // mov byte ptr [rbp+0x10],1
            0x75,0x11,                              // jne past the site
            0x83,0xff,0x01,                         // cmp edi,1
            0x77,0x0c,                              // ja past the site
            0x80,0x3d,uint8_t(d),uint8_t(d>>8),uint8_t(d>>16),uint8_t(d>>24),0x00,  // cmp byte ptr [switch],0
            0x0f,0x94,0x45,0x10,                    // sete byte ptr [rbp+0x10]
            0x90};
}
// Both tests clamp a box to the left eye's view, so a wall reaching past its right edge would be judged
// by its on-screen part alone and vanish from the right eye. So for the visibility work the left eye's
// matrices cover the right eye's outer edge as well: x in [-1,edge] maps to [-1,1]. Nothing has
// reprojected depth out there, so a box touching that part of the HZB stays visible. The left eye's
// view reaches 1.0; the right eye reaches further, by the lenses (the Index about 1.38) plus the gap
// between the eyes for near walls (WIDE_MARGIN). wide_edge() reads it from the two matrices, so another
// headset gets its own. Past MAX_WIDE_EDGE, or from a matrix it can't read, sharing stops: left_only.
constexpr float WIDE_MARGIN=.07f,MAX_WIDE_EDGE=3;
constexpr size_t ENTRY_VIEW_PROJECTION=0x59c,ENTRY_RELATIVE_VIEW_PROJECTION=0x61c,MATRIX_FLOATS=16;
constexpr uintptr_t LWI_PASS_RVA=0x1db9cb0;
// Row-vector matrices: clip x is column 0, clip w is column 3.
inline void widen_view(float* matrix,float edge){
    const float scale=2/(1+edge),shift=(1-edge)/(1+edge);
    for(size_t row=0;row<4;++row)matrix[row*4]=scale*matrix[row*4]+shift*matrix[row*4+3];
}
// Directions only, so eye positions don't enter: the right eye's two outer corners far away, in the left
// eye's clip x. A corner has x=w and y=+-w, so its direction is perpendicular to (x-w) and (y-+w).
// Returns 0 when sharing can't cover it.
inline float wide_edge(const float* left,const float* right){
    using Vector=std::array<float,3>;
    const auto column=[](const float* m,size_t c){return Vector{m[c],m[c+4],m[c+8]};};
    const auto dot=[](const Vector& a,const Vector& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    const auto rx=column(right,0),ry=column(right,1),rw=column(right,3),lx=column(left,0),lw=column(left,3);
    float edge{};
    for(const float y:{-1.f,1.f}){
        const Vector a{rx[0]-rw[0],rx[1]-rw[1],rx[2]-rw[2]},b{ry[0]-y*rw[0],ry[1]-y*rw[1],ry[2]-y*rw[2]};
        const Vector corner{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
        edge=std::max(edge,dot(corner,lx)/dot(corner,lw));
    }
    edge+=WIDE_MARGIN;
    return edge>=1 && edge<=MAX_WIDE_EDGE?edge:0;
}
// The left eye's two matrices, widened for one frame. The LWI pass ends it; a frame that never got there
// is ended at the next visibility pass, where a matrix still as widened goes back and one the game has
// rewritten since stays.
class WideView {
public:
    using Matrix=std::array<float,MATRIX_FLOATS>;
    void widen(uint8_t* entry,float edge){
        settle();m_entry=entry;
        for(size_t i=0;i<OFFSETS.size();++i){
            std::memcpy(m_saved[i].data(),entry+OFFSETS[i],sizeof(Matrix));
            m_wide[i]=m_saved[i];widen_view(m_wide[i].data(),edge);
            std::memcpy(entry+OFFSETS[i],m_wide[i].data(),sizeof(Matrix));
        }
    }
    void settle(){
        if(!m_entry)return;
        for(size_t i=0;i<OFFSETS.size();++i)if(!std::memcmp(m_entry+OFFSETS[i],m_wide[i].data(),sizeof(Matrix)))std::memcpy(m_entry+OFFSETS[i],m_saved[i].data(),sizeof(Matrix));
        m_entry=nullptr;
    }
private:
    static constexpr std::array<size_t,2> OFFSETS{ENTRY_VIEW_PROJECTION,ENTRY_RELATIVE_VIEW_PROJECTION};
    uint8_t* m_entry{};std::array<Matrix,2> m_saved{},m_wide{};
};
// The native frustum: int count, then 20-byte planes: normal, distance, a pad byte and for each axis the
// index into the six-float box (min, max) of the corner furthest along the normal. The opposite corner
// is the one nearest the plane, three indices away.
constexpr size_t PLANE_SIZE=20,PLANE_CORNER=0x11,MAX_PLANES=12,BOX_FLOATS=6;
constexpr uint8_t opposite_corner(uint8_t index){return index<3?index+3:index-3;}
inline bool wholly_inside(const uint8_t* frustum,const float* box){
    int32_t count{};std::memcpy(&count,frustum,sizeof(count));
    if(count<1 || count>MAX_PLANES)return false;
    for(int32_t i=0;i<count;++i){
        const auto* plane=frustum+sizeof(count)+size_t(i)*PLANE_SIZE;
        std::array<float,4> equation{};std::memcpy(equation.data(),plane,sizeof(equation));
        float distance=-equation[3];
        for(size_t axis=0;axis<3;++axis){
            const auto corner=plane[PLANE_CORNER+axis];if(corner>=BOX_FLOATS)return false;
            distance+=equation[axis]*box[opposite_corner(corner)];
        }
        if(!(distance>=0))return false;
    }
    return true;
}
// Takes slot 1 out of the rebuild loop and zeroes its VIS_HZB_RT_SIZE, for the native call only:
// the mod's own validators read this bit at other hooks.
class ScopedSecondEye {
public:
    explicit ScopedSecondEye(uint32_t* flags):m_flags{flags}{if(m_flags)*m_flags|=FLAG_NO_HZB;}
    ~ScopedSecondEye(){if(m_flags)*m_flags&=~FLAG_NO_HZB;}
    ScopedSecondEye(const ScopedSecondEye&)=delete;ScopedSecondEye& operator=(const ScopedSecondEye&)=delete;
private:
    uint32_t* m_flags;
};
}
