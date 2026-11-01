#pragma once
#include "VrNativeHud.h"
#include "VrViewMode.h"
#include <chrono>
#include <span>

// The aim dot (docs/THIRD_PERSON.md "Aim dot"): a small round marker drawn into both eyes at the
// point the game's own aim ray hits (CtrlTarget's published target, EngineWeaponProbe). It sits on
// what the gun is aimed at from wherever the eye is, so it works in diorama, shoulder and chase, where
// the game's crosshair (a mark on the aim line in front of the game camera) can be far off or edge-on.
// It faces the eye and keeps one angular size at any distance. No depth test: it shows through what
// stands between the eye and the hit point, like a marker. config/index_aim_dot.calibration.
struct VrAimDot {
    using Values=vr_body::Parameters<4>;
    enum Index : size_t { ENABLED,SIZE,OPACITY,FIRST_PERSON };
    inline static constexpr std::array<Values::Parameter,4> PARAMETERS{{
        {"enabled",0,0,1},            // Off since the game's own crosshair moves to the aim point (VrHudReticle, below).
        {"size_degrees",.7f,.2f,4},   // Angular diameter.
        {"opacity",.9f,.1f,1},
        {"first_person",1,0,1}}};     // Also in first person (with motion controls: where the controller gun's shots land).
    // Warm red centre; the outer ring (from RING of the radius) is dark so it reads on bright and dark scenes.
    static constexpr std::array<float,3> DOT_COLOUR{1,.2f,.1f};
    static constexpr float RING=.6f;
    Values values=Values::defaults(PARAMETERS);
    bool valid() const { const auto& v=values.values;return values.in_range(PARAMETERS) && (v[ENABLED]==0 || v[ENABLED]==1) && (v[FIRST_PERSON]==0 || v[FIRST_PERSON]==1); }
    bool enabled() const { return values.values[ENABLED]>=.5f; }
    bool shows(VrViewMode mode) const { return valid() && enabled() && (mode!=VrViewMode::first_person || values.values[FIRST_PERSON]>=.5f); }
    static std::optional<VrAimDot> load(const std::filesystem::path& path){std::ifstream in{path};const auto v=Values::read(in,PARAMETERS);if(!v)return {};VrAimDot d{*v};return d.valid()?std::optional{d}:std::nullopt;}
    bool operator==(const VrAimDot&) const = default;

    // One pair's dot: the game-world hit point and its look. Both eyes of a pair carry the same one.
    struct Draw {
        std::array<float,3> point{};
        float size_degrees{};
        std::array<float,4> colour{};
        bool operator==(const Draw&) const = default;
    };
    std::optional<Draw> draw(const std::array<float,3>& point) const {
        if(!valid() || !enabled() || !std::ranges::all_of(point,[](float v){return std::isfinite(v);}))return {};
        const auto& v=values.values;return Draw{point,v[SIZE],{DOT_COLOUR[0],DOT_COLOUR[1],DOT_COLOUR[2],v[OPACITY]}};
    }

    // Clip-space corners (x,y,z,w; top-left, top-right, bottom-right, bottom-left as the eye sees them)
    // of the dot's square in one eye. eye_world: the eye camera's world matrix (game rows, may carry the
    // diorama's scale), projection: its game-style (+z forward) projection. nullopt when the point is
    // behind the eye or the inputs are unusable.
    static std::optional<std::array<float,16>> clip(const Draw& d,const vr_camera::Matrix& eye_world,const vr_camera::Matrix& projection){
        using vr_hud_detail::times;
        if(!std::ranges::all_of(projection,[](float v){return std::isfinite(v);}) || !(d.size_degrees>0))return {};
        const auto view=vr_hud_detail::affine_inverse(eye_world);if(!view)return {};
        const auto e=times({d.point[0],d.point[1],d.point[2],1},*view);
        const float distance=std::sqrt(e[0]*e[0]+e[1]*e[1]+e[2]*e[2]);
        if(!std::isfinite(distance) || !(e[2]>1e-4f*distance))return {};
        // Half-width at the point for the angular size, on a square square-on to the eye's view direction.
        const float half=distance*std::tan(d.size_degrees*.00872664626f);
        std::array<float,16> out{};
        for(size_t c=0;c<4;++c){
            const float x=e[0]+(c==0 || c==3?-half:half),y=e[1]+(c<2?half:-half);
            const auto p=times({x,y,e[2],1},projection);
            if(!(p[3]>0))return {};
            out[c*4]=p[0];out[c*4+1]=p[1];out[c*4+2]=0;out[c*4+3]=p[3]; // z=0: never clipped by depth.
            for(int j=0;j<4;++j)if(!std::isfinite(out[c*4+j]))return {};
        }
        return out;
    }
    // Where the dot's centre lands in a width x height eye image (snapshot evidence).
    static std::optional<std::array<float,2>> pixel(const Draw& d,const vr_camera::Matrix& eye_world,const vr_camera::Matrix& projection,float width,float height){
        const auto c=clip(d,eye_world,projection);if(!c)return {};
        float x=0,y=0;for(int k=0;k<4;++k){x+=(*c)[k*4]/(*c)[k*4+3]/4;y+=(*c)[k*4+1]/(*c)[k*4+3]/4;}
        return std::array<float,2>{(x+1)/2*width,(1-y)/2*height};
    }
};

// The game's own crosshair moved to the aim point (docs/THIRD_PERSON.md "Crosshair at the aim point"):
// a patch of the native HUD canvas around the crosshair, with whatever the game draws on and beside it
// (reload, the plasma heat bars either side, hit markers), drawn facing the eye at the aim ray's hit point with the angular size
// it has on the game camera, and left out of the canvas. In first person too since the 31 Oct 16:04
// playtest: the canvas there follows the game camera, which follows the head, while the gun aims
// anywhere. config/index_hud_reticle.calibration.
struct VrHudReticle {
    using Values=vr_body::Parameters<9>;
    enum Index : size_t { ENABLED,HEIGHT,WIDTH,CENTRE_X,CENTRE_Y,FIRST_PERSON,AMMO_WIDTH,AMMO_BELOW,STAND_ASIDE };
    inline static constexpr std::array<Values::Parameter,9> PARAMETERS{{
        {"enabled",1,0,1},
        {"height",.08f,.02f,.4f},       // Patch height as a fraction of the canvas height.
        {"width",.3f,.02f,.8f},         // Patch width in the same units (canvas heights), so 0.08 is square: wide
                                        // enough for the plasma heat bars beside the crosshair (Joe, 30 Oct: 0.08 left them out).
        // The crosshair's fixed place on the canvas, as measured (docs/THIRD_PERSON.md): the game camera looks
        // 7.1 degrees below the aim, 4.2 when zoomed, and v stays 0.4225 at either field of view.
        {"centre_x",.5f,.2f,.8f},{"centre_y",.4225f,.2f,.8f},
        {"first_person",1,0,1},
        // The magazine count, bar and reload bullets, in a narrow column under the band, in canvas heights:
        // its width, and how far below the crosshair it ends. Narrow because the canvas centre, where a looked-at
        // object's marker sits, is in the way of anything wider. 0 below: no column.
        {"ammo_width",.1f,.02f,.8f},{"ammo_below",.125f,0,.4f},
        {"stand_aside",1,0,1}}}; // Leave the canvas whole while other UI is beside the patch (crowded).
    Values values=Values::defaults(PARAMETERS);
    static VrHudReticle off() { VrHudReticle r;r.values.values[ENABLED]=0;return r; }
    bool valid() const { const auto& v=values.values;return values.in_range(PARAMETERS) && (v[ENABLED]==0 || v[ENABLED]==1) && (v[FIRST_PERSON]==0 || v[FIRST_PERSON]==1) && (v[STAND_ASIDE]==0 || v[STAND_ASIDE]==1); }
    bool enabled() const { return values.values[ENABLED]>=.5f; }
    bool stands_aside() const { return values.values[STAND_ASIDE]>=.5f; }
    bool shows(VrViewMode mode) const { return valid() && enabled() && (mode!=VrViewMode::first_person || values.values[FIRST_PERSON]>=.5f); }
    // This pair. First person follows the canvas's own rule (VrNativePrompt::camera_lock): in the hub the
    // ray still runs but the canvas is head-locked and no gun is drawn, so the patch would carry whatever
    // the canvas shows there off to wherever the ray happens to point.
    bool shows(VrViewMode mode,const scene_mode::Scene& scene) const { return shows(mode) && (mode!=VrViewMode::first_person || VrNativePrompt::camera_lock(true,scene)); }
    static std::optional<VrHudReticle> load(const std::filesystem::path& path){std::ifstream in{path};const auto v=Values::read(in,PARAMETERS);if(!v)return {};VrHudReticle r{*v};return r.valid()?std::optional{r}:std::nullopt;}
    bool operator==(const VrHudReticle&) const = default;
    // The patch on the canvas (x,y,width,height in canvas UV) for a canvas width/height; nullopt off the canvas.
    std::optional<std::array<float,4>> rect(float canvas_aspect) const {
        if(!valid() || !(canvas_aspect>0) || !std::isfinite(canvas_aspect))return {};
        const auto& v=values.values;const float h=v[HEIGHT],w=v[WIDTH]/canvas_aspect,x=v[CENTRE_X]-w/2,y=v[CENTRE_Y]-h/2;
        if(x<0 || y<0 || x+w>1 || y+h>1)return {};
        return std::array<float,4>{x,y,w,h};
    }
    // The column under the patch, from its lower edge down; nullopt when off, or off the canvas.
    std::optional<std::array<float,4>> ammo_rect(float canvas_aspect) const {
        const auto band=rect(canvas_aspect);if(!band)return {};
        const auto& v=values.values;const float top=(*band)[1]+(*band)[3],bottom=v[CENTRE_Y]+v[AMMO_BELOW],w=v[AMMO_WIDTH]/canvas_aspect,x=v[CENTRE_X]-w/2;
        if(!(bottom>top) || x<0 || x+w>1 || bottom>1)return {};
        return std::array<float,4>{x,top,w,bottom-top};
    }
    // Whether other UI is lit right beside the patch, from the coverage grid (UiCoverageProbe, cells of
    // brightness, rows first). The canvas and the patch are one image, so a rectangle left out of the canvas
    // takes whatever else is in it: a focused pickup hint sits 85 to 165 design pixels above its object,
    // which is on the band when the object is near the gaze (31 Oct 17:49). Watched: the band and WATCH
    // above and below it, without the column the crosshair's own widgets use from the band down.
    static constexpr float WATCH=.05f;
    static constexpr uint8_t LIT=64;
    static constexpr size_t LIT_CELLS=2;
    bool crowded(std::span<const uint8_t> cells,size_t columns,size_t rows,float canvas_aspect) const {
        const auto band=rect(canvas_aspect);if(!band || !columns || cells.size()!=columns*rows)return false;
        const float w=values.values[AMMO_WIDTH]/canvas_aspect,inner=values.values[CENTRE_X]-w/2;
        const float left=(*band)[0],right=left+(*band)[2],top=(*band)[1],bottom=top+(*band)[3];
        size_t lit{};
        for(size_t row=0;row<rows;++row)for(size_t column=0;column<columns;++column){
            const float u=(column+.5f)/columns,v=(row+.5f)/rows;
            if(cells[row*columns+column]<LIT || u<left || u>=right || v<top-WATCH || v>=bottom+WATCH)continue;
            if(v>=top && u>=inner && u<inner+w)continue;
            ++lit;
        }
        return lit>=LIT_CELLS;
    }
    // Keeps the canvas whole for HOLD after a crowded sample (the grid comes every 100 ms), except for FIRE_HOLD
    // after a trigger: the crosshair's own spread, hit markers and heat bars then light the same cells.
    struct Aside {
        using Clock=std::chrono::steady_clock;
        static constexpr auto HOLD=std::chrono::milliseconds(300);
        static constexpr auto FIRE_HOLD=std::chrono::seconds(2);
        Clock::time_point until{},fired{};
        void fire(Clock::time_point now){fired=now;}
        void sample(bool crowded,Clock::time_point now){if(crowded)until=now+HOLD;}
        bool active(Clock::time_point now) const {return now<until && (fired==Clock::time_point{} || now-fired>=FIRE_HOLD);}
    };
    // Where a game-world point shows on the canvas (UV) through the game camera; nullopt behind it.
    // The game draws its crosshair where its aim ray points, which is not the canvas centre: its camera
    // looks a few degrees below the aim (diorama test room, 30 Oct: the crosshair at v 0.415). So the
    // patch is centred on the hit point as the game camera sees it, while the game's own ray aims.
    static std::optional<std::array<float,2>> canvas_point(const std::array<float,3>& point,const VrHudCamera& camera){
        using vr_hud_detail::times;
        if(!camera.valid() || !std::ranges::all_of(point,[](float v){return std::isfinite(v);}))return {};
        const auto view=vr_hud_detail::affine_inverse(camera.world);if(!view)return {};
        const auto c=times({point[0],point[1],point[2],1},*view);if(!(c[2]>1e-6f))return {};
        const std::array<float,2> uv{(c[0]/c[2]-camera.left)/(camera.right-camera.left),(camera.top-c[1]/c[2])/(camera.top-camera.bottom)};
        if(!std::isfinite(uv[0]) || !std::isfinite(uv[1]))return {};
        return uv;
    }
    // A copy centred on a canvas point, when that is within the knobs' range; otherwise unchanged.
    VrHudReticle centred(const std::array<float,2>& uv) const {
        auto r=*this;const auto& x=PARAMETERS[CENTRE_X];const auto& y=PARAMETERS[CENTRE_Y];
        if(uv[0]>=x.minimum && uv[0]<=x.maximum && uv[1]>=y.minimum && uv[1]<=y.maximum){r.values.values[CENTRE_X]=uv[0];r.values.values[CENTRE_Y]=uv[1];}
        return r;
    }
    // The patch's place on the canvas this pair. Third person: the latest sample from the game's aim
    // ray. First person: the knobs. Samples there are rare once the controller gun steers the ray, and
    // the last one before it does is often from an animation (31 Oct: 3 to 18 degrees from the usual
    // 7.1), which would leave the patch off the crosshair until the next.
    VrHudReticle placed(VrViewMode mode,const std::optional<std::array<float,2>>& sampled) const { return mode!=VrViewMode::first_person && sampled?centred(*sampled):*this; }
    // The same rectangle in the shape NativeHudRenderer draws (left/right halves).
    static VrNativeHud crop(const std::array<float,4>& r){VrNativeHud c;c.values.values={r[0],r[1],r[2]/2,r[3],r[0]+r[2]/2,r[1],r[2]/2,r[3]};return c;}
    // Eye UV -> patch UV for the patch drawn at point (game world), square-on to the eye's view direction,
    // spanning the angles it spans on the game camera (camera: its frustum tangents). nullopt when the
    // inputs are unusable. A point behind the eye gives an empty map: nothing to draw, but the crosshair
    // still leaves the canvas, or it snaps to the head whenever the gun points past you (31 Oct: 5% of
    // the frames with the gun steering).
    std::optional<VrHudHomography> at(const std::array<float,3>& point,const VrHudCamera& camera,float canvas_aspect,const vr_camera::Matrix& eye_world,const vr_camera::Matrix& projection) const {
        const auto r=rect(canvas_aspect);if(!r)return {};
        return at(*r,point,camera,eye_world,projection);
    }
    // Any canvas rectangle, placed as it sits around the crosshair, with the crosshair on the point.
    std::optional<VrHudHomography> at(const std::array<float,4>& r,const std::array<float,3>& point,const VrHudCamera& camera,const vr_camera::Matrix& eye_world,const vr_camera::Matrix& projection) const {
        using vr_hud_detail::times;
        if(!valid() || !enabled() || !camera.valid() || !std::ranges::all_of(point,[](float v){return std::isfinite(v);}))return {};
        const auto view=vr_hud_detail::affine_inverse(eye_world);if(!view)return {};
        const auto e=times({point[0],point[1],point[2],1},*view);
        const float distance=std::sqrt(e[0]*e[0]+e[1]*e[1]+e[2]*e[2]);
        if(!std::isfinite(distance))return {};
        if(!(e[2]>1e-4f*distance))return VrHudHomography{{},{0,0,0,0}};
        const float across=distance*(camera.right-camera.left),down=distance*(camera.top-camera.bottom);
        const auto& v=values.values;
        return vr_hud_detail::plane_homography({e[0]+(r[0]-v[CENTRE_X])*across,e[1]-(r[1]-v[CENTRE_Y])*down,e[2],1},{r[2]*across,0,0,0},{0,-r[3]*down,0,0},projection);
    }
};
