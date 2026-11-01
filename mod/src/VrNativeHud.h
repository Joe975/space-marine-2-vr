#pragma once
#include "VrCompactHud.h"
#include "SceneMode.h"

// Native status graphics are sampled directly; no inferred health or inventory.
struct VrNativeHud {
    using Values=vr_body::Parameters<8>;
    inline static constexpr std::array<Values::Parameter,8> PARAMETERS{{
        {"health_x",.045f,0,1},{"health_y",.035f,0,1},{"health_width",.23f,.01f,1},{"health_height",.085f,.01f,1},
        {"inventory_x",.40f,0,1},{"inventory_y",.84f,0,1},{"inventory_width",.22f,.01f,1},{"inventory_height",.13f,.01f,1}}};
    Values values=Values::defaults(PARAMETERS);
    bool valid()const {
        const auto& v=values.values;return values.in_range(PARAMETERS) && v[0]+v[2]<=1 && v[1]+v[3]<=1 && v[4]+v[6]<=1 && v[5]+v[7]<=1;
    }
    static std::optional<VrNativeHud> load(const std::filesystem::path& path){std::ifstream in{path};const auto v=Values::read(in,PARAMETERS);if(!v)return {};VrNativeHud hud{*v};return hud.valid()?std::optional{hud}:std::nullopt;}
    bool save(const std::filesystem::path& path)const{if(!path.is_absolute() || !valid())return false;std::ofstream out{path};return values.write(out,PARAMETERS);}
    // Eye render-target UV -> plane UV for a head-relative plane (metres): width x height,
    // centred centre_x right / centre_y up of straight ahead, at distance in front.
    static std::optional<std::array<float,4>> plane_transform(float width,float height,float centre_x,float centre_y,float distance,const vr_camera::Matrix& projection,const vr_camera::Matrix& eye){
        if(!vr_camera::rigid(eye) || !std::ranges::all_of(projection,[](auto v){return std::isfinite(v);}) || projection[0]<=0 || projection[5]<=0)return {};
        if(!(width>0) || !(height>0) || !(distance>0) || !std::isfinite(centre_x) || !std::isfinite(centre_y) || !std::isfinite(width) || !std::isfinite(height) || !std::isfinite(distance))return {};
        return std::array<float,4>{2*distance/(projection[0]*width),2*distance/(projection[5]*height),
            .5f+((-1-projection[8])*distance/projection[0]+eye[12]-centre_x)/width,
            .5f-((1-projection[9])*distance/projection[5]+eye[13]-centre_y)/height};
    }
    static std::optional<std::array<float,4>> transform(const VrCompactHud::Settings& placement,const vr_camera::Matrix& projection,const vr_camera::Matrix& eye){
        if(!placement.values.in_range(VrCompactHud::PARAMETERS))return {};
        const auto& p=placement.values.values;const float width=p[0];
        return plane_transform(width,width*VrCompactHud::HEIGHT/VrCompactHud::WIDTH,0,p[1],p[2],projection,eye);
    }
    bool operator==(const VrNativeHud&)const=default;
};

// The HUD is cut from the swap chain's back buffer. On a stereo frame the scene is redirected and
// the canvas is black behind the HUD; on a mono frame (the few native dispatches while owned
// stereo resumes after a gap) it holds the whole flat scene, which the HUD shader then shows as a
// see-through slice of the flat game (23 Oct playtest: "a flicker of a flat 2D screen").
// Only compose while stereo runs: the pair is at most two dispatches after the previous bridged
// pair (a resume after a gap is four or more), and the frame being presented is a tracked stereo
// frame at most two dispatches later.
struct VrNativeHudSource {
    static bool stereo(uint64_t pair_dispatch,uint64_t previous_bridged,std::optional<uint64_t> presented,bool presented_tracked){
        return presented && presented_tracked && previous_bridged && pair_dispatch>previous_bridged && pair_dispatch-previous_bridged<=2 &&
            *presented>=pair_dispatch && *presented-pair_dispatch<=2;
    }
};

// A refused eye capture with the HUD attached is usually transient: no free eye slot while the
// runtime still holds a lease (E_PENDING), or a packet that does not match its pair. One refusal
// used to turn the HUD off for the whole run (22 Oct playtest, 4 minutes in). Only a run of
// refusals with no composed frame between them does now, so a HUD that can never compose still
// stops costing eye pairs within about a second.
struct VrNativeHudRefusals {
    static constexpr uint32_t LIMIT=30;
    uint32_t consecutive{},total{};
    // False once the HUD should no longer be requested.
    bool record(bool composed){if(composed){consecutive=0;return true;}++total;return ++consecutive<LIMIT;}
};

// The game's own camera for one eye image: its world matrix (game rows right/up/forward) and its
// frustum tangents, from the untouched camera source the eye was prepared from.
struct VrHudCamera {
    vr_camera::Matrix world{vr_camera::IDENTITY};
    float left{},right{},bottom{},top{};
    float units_per_metre{1}; // The eye's game units per physical metre.
    bool valid() const {
        return std::ranges::all_of(world,[](float v){return std::isfinite(v);}) && std::isfinite(left) && std::isfinite(right) && std::isfinite(bottom) && std::isfinite(top) &&
            right>left && top>bottom && std::isfinite(units_per_metre) && units_per_metre>0;
    }
    bool operator==(const VrHudCamera&) const = default;
};

// Eye render-target UV -> canvas-crop UV for a plane that is not facing the eye: a 3x3 projective map
// (row-major, applied to (u,v,1), divided by the third component, which is positive in front of the eye),
// and the eye-UV box the plane covers (u0,v0,u1,v1; the whole image when part of it is behind the eye).
struct VrHudHomography { std::array<float,9> h{}; std::array<float,4> bounds{0,0,1,1}; bool empty() const {return bounds[2]<=bounds[0] || bounds[3]<=bounds[1];} };
namespace vr_hud_detail {
// General affine inverse (an eye's world can carry the diorama's scale); nullopt when singular.
inline std::optional<vr_camera::Matrix> affine_inverse(const vr_camera::Matrix& m){
    const float a=m[0],b=m[1],c=m[2],d=m[4],e=m[5],f=m[6],g=m[8],h=m[9],i=m[10];
    const float A=e*i-f*h,B=-(d*i-f*g),C=d*h-e*g,det=a*A+b*B+c*C;
    if(!std::isfinite(det) || std::abs(det)<1e-12f)return {};
    const float s=1/det;vr_camera::Matrix out=vr_camera::IDENTITY;
    out[0]=A*s;out[1]=-(b*i-c*h)*s;out[2]=(b*f-c*e)*s;
    out[4]=B*s;out[5]=(a*i-c*g)*s;out[6]=-(a*f-c*d)*s;
    out[8]=C*s;out[9]=-(a*h-b*g)*s;out[10]=(a*e-b*d)*s;
    for(int j=0;j<3;++j)out[12+j]=-(m[12]*out[j]+m[13]*out[4+j]+m[14]*out[8+j]);
    return out;
}
// Row vector (x,y,z,w) times a row-major 4x4.
inline std::array<float,4> times(const std::array<float,4>& p,const vr_camera::Matrix& m){
    std::array<float,4> out{};for(int j=0;j<4;++j)for(int k=0;k<4;++k)out[j]+=p[k]*m[k*4+j];return out;
}
// Eye UV -> crop UV for a flat rectangle in eye space (+z forward): its top-left corner origin (w=1)
// and its two edges across and down (w=0); crop (0,0) is the origin, (1,1) the far corner.
inline std::optional<VrHudHomography> plane_homography(const std::array<float,4>& origin,const std::array<float,4>& across,const std::array<float,4>& down,const vr_camera::Matrix& projection){
    if(!std::ranges::all_of(projection,[](auto v){return std::isfinite(v);}))return {};
    // Eye image UV in homogeneous form: U=(cx+cw)/2, V=(cw-cy)/2, W=cw.
    const auto uvw=[&](const std::array<float,4>& p){const auto c=times(p,projection);return std::array<float,3>{(c[0]+c[3])/2,(c[3]-c[1])/2,c[3]};};
    const auto s=uvw(across),tt=uvw(down),o=uvw(origin);
    // M maps crop (s,t,1) to eye (U,V,W); the shader needs its inverse.
    const std::array<float,9> m{s[0],tt[0],o[0], s[1],tt[1],o[1], s[2],tt[2],o[2]};
    const float det=m[0]*(m[4]*m[8]-m[5]*m[7])-m[1]*(m[3]*m[8]-m[5]*m[6])+m[2]*(m[3]*m[7]-m[4]*m[6]);
    if(!std::isfinite(det) || std::abs(det)<1e-20f)return {};
    const float r=1/det;VrHudHomography out;
    out.h={(m[4]*m[8]-m[5]*m[7])*r,-(m[1]*m[8]-m[2]*m[7])*r,(m[1]*m[5]-m[2]*m[4])*r,
           -(m[3]*m[8]-m[5]*m[6])*r,(m[0]*m[8]-m[2]*m[6])*r,-(m[0]*m[5]-m[2]*m[3])*r,
           (m[3]*m[7]-m[4]*m[6])*r,-(m[0]*m[7]-m[1]*m[6])*r,(m[0]*m[4]-m[1]*m[3])*r};
    if(!std::ranges::all_of(out.h,[](float x){return std::isfinite(x);}))return {};
    // The eye-UV box of the four corners when all are in front of the eye.
    float u0=1,v0=1,u1=0,v1=0;int behind=0;
    for(const auto [cs,ct]:{std::pair{0.f,0.f},{1.f,0.f},{0.f,1.f},{1.f,1.f}}){
        const float U=m[0]*cs+m[1]*ct+m[2],V=m[3]*cs+m[4]*ct+m[5],W=m[6]*cs+m[7]*ct+m[8];
        if(!(W>1e-6f)){++behind;continue;}
        u0=(std::min)(u0,U/W);u1=(std::max)(u1,U/W);v0=(std::min)(v0,V/W);v1=(std::max)(v1,V/W);
    }
    // All in front: their box. Partly behind: the whole image (the shader drops what is behind).
    out.bounds=behind==4?std::array<float,4>{0,0,0,0}:behind?std::array<float,4>{0,0,1,1}:std::array<float,4>{u0,v0,u1,v1};
    // Entirely off this eye (looking away from the plane): an empty box draws nothing.
    if(out.bounds[2]<=0 || out.bounds[3]<=0 || out.bounds[0]>=1 || out.bounds[1]>=1)out.bounds={0,0,0,0};
    return out;
}
}

// The native gameplay HUD canvas in the eyes: the focused interaction prompt ("hold X" marker,
// pickup name) and every world marker (interactables, objectives). Shown head-locked with the
// native camera's own angular mapping, so a native marker covers the same direction it does on
// the flat screen: when the native camera follows the gaze (default with -Controls) a marker sits
// over its object. Until 25 Oct only the centre crop (0.36,0.32 0.28x0.36) was shown, enough for
// the focused prompt, so markers anywhere else were never seen (Joe, 0.1.8: no interactable
// icons in the hangar). The whole canvas is the default now; the crop knobs still narrow it.
struct VrNativePrompt {
    using Values=vr_body::Parameters<7>;
    enum Index : size_t { ENABLED,X,Y,WIDTH,HEIGHT,DISTANCE,FOV };
    inline static constexpr std::array<Values::Parameter,7> PARAMETERS{{
        {"enabled",1,0,1},
        {"prompt_x",0,0,1},{"prompt_y",0,0,1},{"prompt_width",1,.02f,1},{"prompt_height",1,.02f,1},
        {"distance_metres",1.5f,.5f,5},
        {"native_horizontal_fov_degrees",110,60,150}}}; // config/first_person_wide_fov.json "fov".
    Values values=Values::defaults(PARAMETERS);
    static VrNativePrompt off() { VrNativePrompt p;p.values.values[ENABLED]=0;return p; }
    bool enabled() const { return values.values[ENABLED]>=.5f; }
    bool valid() const { const auto& v=values.values;return values.in_range(PARAMETERS) && (v[ENABLED]==0 || v[ENABLED]==1) && v[X]+v[WIDTH]<=1 && v[Y]+v[HEIGHT]<=1; }
    static std::optional<VrNativePrompt> load(const std::filesystem::path& path){std::ifstream in{path};const auto v=Values::read(in,PARAMETERS);if(!v)return {};VrNativePrompt p{*v};return p.valid()?std::optional{p}:std::nullopt;}
    bool save(const std::filesystem::path& path)const{if(!path.is_absolute() || !valid())return false;std::ofstream out{path};return values.write(out,PARAMETERS);}
    // The two halves of the crop, in the shape NativeHudRenderer draws (left/right regions).
    VrNativeHud crop() const {
        const auto& v=values.values;VrNativeHud c;
        c.values.values={v[X],v[Y],v[WIDTH]/2,v[HEIGHT],v[X]+v[WIDTH]/2,v[Y],v[WIDTH]/2,v[HEIGHT]};return c;
    }
    // Head-relative plane matching the native perspective: canvas UV maps linearly onto a
    // plane at the native FOV, so the crop keeps its true angular size and direction.
    struct Plane { float width{},height{},centre_x{},centre_y{},distance{}; };
    std::optional<Plane> plane(float canvas_aspect) const {
        if(!valid() || !(canvas_aspect>0) || !std::isfinite(canvas_aspect))return {};
        const auto& v=values.values;const float d=v[DISTANCE],h=std::tan(v[FOV]*.00872664626f),vt=h/canvas_aspect;
        return Plane{2*d*h*v[WIDTH],2*d*vt*v[HEIGHT],d*h*(2*(v[X]+v[WIDTH]/2)-1),-d*vt*(2*(v[Y]+v[HEIGHT]/2)-1),d};
    }
    std::optional<std::array<float,4>> transform(float canvas_aspect,const vr_camera::Matrix& projection,const vr_camera::Matrix& eye) const {
        const auto p=plane(canvas_aspect);if(!p)return {};
        return VrNativeHud::plane_transform(p->width,p->height,p->centre_x,p->centre_y,p->distance,projection,eye);
    }
    // The canvas locked to the game's camera instead of the head (30 Oct, Joe: "showing in world space
    // where the gun is aiming, rather than just attached to my face"). The crop lies on a plane
    // distance_metres in front of the game camera, spanning exactly the part of the game camera's
    // frustum it shows on the flat screen, so the crosshair sits on the game camera's aim line and each
    // world marker on the line to its object. eye_world: this eye's camera world matrix (game rows),
    // projection: its projection. The FOV knob is unused here: the game camera's own frustum is.
    std::optional<VrHudHomography> camera_locked(const VrHudCamera& camera,const vr_camera::Matrix& eye_world,const vr_camera::Matrix& projection) const {
        using vr_hud_detail::times;
        if(!valid() || !camera.valid() || !std::ranges::all_of(projection,[](auto v){return std::isfinite(v);}))return {};
        const auto view=vr_hud_detail::affine_inverse(eye_world);if(!view)return {};
        const auto native_to_eye=vr_camera::multiply(camera.world,*view);
        const auto& v=values.values;const float d=v[DISTANCE]*camera.units_per_metre;
        const float l=camera.left,w=camera.right-camera.left,t=camera.top,h=camera.top-camera.bottom;
        // In the game camera's frame (x right, y up, z forward): the crop's top-left corner and its two edges.
        const std::array<float,4> origin{d*(l+v[X]*w),d*(t-v[Y]*h),d,1},across{d*v[WIDTH]*w,0,0,0},down{0,-d*v[HEIGHT]*h,0,0};
        return vr_hud_detail::plane_homography(times(origin,native_to_eye),times(across,native_to_eye),times(down,native_to_eye),projection);
    }
    // Whether the canvas is camera_locked (above) or head-locked (transform): first person in plain play
    // only. The lock assumes the game camera faces where the player looks. In the hub (the Battle Barge,
    // 0x800) it did not until the gaze camera steered there too, so the locked canvas sat 100-174 degrees
    // from the gaze, and with it every prompt and pop-up: the battle simulator's accept pop-up was out of
    // sight (30 Oct 20:24 playtest, docs/VR_MENUS.md). Head-locked stays: the hub has no gun to aim, and a
    // pop-up stays in front of the player if the game camera lags or ignores the stick there.
    static bool camera_lock(bool first_person,const scene_mode::Scene& scene){return first_person && scene.gameplay();}
    bool operator==(const VrNativePrompt&)const=default;
};

// The right-hand corners of the native HUD canvas on a second low panel in front of you, under
// the health/inventory strip: top right (objective, squad, "Edit Squad"), bottom right (the
// mission card with "Reject Mission"). On the head-locked full canvas they sit about 45 degrees
// out at the edge of the lenses and move with the head, so they can never be looked at (Joe,
// 25 Oct). Left half of the panel = the first region, right half = the second.
struct VrNativeHudCorners {
    using Values=vr_body::Parameters<12>;
    enum Index : size_t { ENABLED,FIRST_X,FIRST_Y,FIRST_WIDTH,FIRST_HEIGHT,SECOND_X,SECOND_Y,SECOND_WIDTH,SECOND_HEIGHT,WIDTH,CENTRE_Y,DISTANCE };
    inline static constexpr std::array<Values::Parameter,12> PARAMETERS{{
        {"enabled",1,0,1},
        {"top_right_x",.70f,0,1},{"top_right_y",.03f,0,1},{"top_right_width",.28f,.01f,1},{"top_right_height",.27f,.01f,1},
        {"bottom_right_x",.72f,0,1},{"bottom_right_y",.58f,0,1},{"bottom_right_width",.28f,.01f,1},{"bottom_right_height",.40f,.01f,1},
        {"width_metres",.6f,.2f,1.2f},{"centre_y_metres",-.62f,-.9f,0},{"distance_metres",1.2f,.6f,3}}};
    // Each half of the panel is 1.5 wide to 1 high.
    inline static constexpr float ASPECT=3;
    Values values=Values::defaults(PARAMETERS);
    static VrNativeHudCorners off() { VrNativeHudCorners c;c.values.values[ENABLED]=0;return c; }
    bool enabled() const { return values.values[ENABLED]>=.5f; }
    bool valid() const { const auto& v=values.values;return values.in_range(PARAMETERS) && (v[ENABLED]==0 || v[ENABLED]==1) &&
        v[FIRST_X]+v[FIRST_WIDTH]<=1 && v[FIRST_Y]+v[FIRST_HEIGHT]<=1 && v[SECOND_X]+v[SECOND_WIDTH]<=1 && v[SECOND_Y]+v[SECOND_HEIGHT]<=1; }
    static std::optional<VrNativeHudCorners> load(const std::filesystem::path& path){std::ifstream in{path};const auto v=Values::read(in,PARAMETERS);if(!v)return {};VrNativeHudCorners c{*v};return c.valid()?std::optional{c}:std::nullopt;}
    bool save(const std::filesystem::path& path)const{if(!path.is_absolute() || !valid())return false;std::ofstream out{path};return values.write(out,PARAMETERS);}
    // The two regions in the shape NativeHudRenderer draws (left/right halves).
    VrNativeHud regions() const { const auto& v=values.values;VrNativeHud r;r.values.values={v[FIRST_X],v[FIRST_Y],v[FIRST_WIDTH],v[FIRST_HEIGHT],v[SECOND_X],v[SECOND_Y],v[SECOND_WIDTH],v[SECOND_HEIGHT]};return r; }
    std::optional<std::array<float,4>> transform(const vr_camera::Matrix& projection,const vr_camera::Matrix& eye) const {
        if(!valid())return {};const auto& v=values.values;
        return VrNativeHud::plane_transform(v[WIDTH],v[WIDTH]/ASPECT,0,v[CENTRE_Y],v[DISTANCE],projection,eye);
    }
    bool operator==(const VrNativeHudCorners&)const=default;
};

// Canvas rectangles (x,y,width,height) the full-canvas plane leaves out because a panel in front
// already shows them readable: health and inventory (the compact strip) and, when that panel is
// on, the right-hand corners. Otherwise each showed twice, once in front and once head-locked at
// the lens edge, coming and going as the game fades its HUD (27 Oct evening playtest). A marker
// crossing one of these rectangles is hidden there; everywhere else it keeps its native direction.
struct VrNativeHudExcluded {
    using Rects=std::array<std::array<float,4>,6>; // Unused entries are zero: they exclude nothing.
    // reticle, ammo: the crosshair patch and the column under it, when VrHudReticle draws them at the aim point.
    static Rects of(const VrNativeHud& strip,const VrNativeHudCorners& corners,const std::array<float,4>& reticle={},const std::array<float,4>& ammo={}) {
        const auto& s=strip.values.values;Rects r{};
        r[0]={s[0],s[1],s[2],s[3]};r[1]={s[4],s[5],s[6],s[7]};
        if(corners.enabled()){const auto c=corners.regions().values.values;r[2]={c[0],c[1],c[2],c[3]};r[3]={c[4],c[5],c[6],c[7]};}
        r[4]=reticle;r[5]=ammo;
        return r;
    }
    // The shader's rule, for tests: inside a rectangle, left/top edge included.
    static bool inside(const Rects& rects,float u,float v) {
        return std::ranges::any_of(rects,[&](const auto& e){return u>=e[0] && v>=e[1] && u<e[0]+e[2] && v<e[1]+e[3];});
    }
};
