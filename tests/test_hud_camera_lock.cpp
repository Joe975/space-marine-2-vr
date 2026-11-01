#include "VrNativeHud.h"
#include "VrProjection.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error{why};}
bool near(float a,float b,float tolerance=1e-3f){return std::abs(a-b)<=tolerance;}
// Eye UV -> crop UV through the homography, as the shader does.
std::array<float,2> apply(const VrHudHomography& h,float u,float v){
    const auto& m=h.h;const float x=m[0]*u+m[1]*v+m[2],y=m[3]*u+m[4]*v+m[5],w=m[6]*u+m[7]*v+m[8];
    if(!(w>0))return {NAN,NAN};return {x/w,y/w};
}
// A game-convention world matrix (rows right, up, forward, translation) turned by yaw degrees to the
// right, at a position.
vr_camera::Matrix camera(float yaw_degrees,float x=0,float y=0,float z=0,float scale=1){
    const float a=yaw_degrees*.0174532925f,c=std::cos(a)*scale,s=std::sin(a)*scale;
    return {c,0,-s,0, 0,scale,0,0, s,0,c,0, x,y,z,1};
}
VrNativePrompt full_canvas(){VrNativePrompt p;auto& v=p.values.values;v[VrNativePrompt::X]=0;v[VrNativePrompt::Y]=0;v[VrNativePrompt::WIDTH]=1;v[VrNativePrompt::HEIGHT]=1;return p;}
}
int main(){
    try{
        const vr_projection::Tangents fov{-1,1,-.5f,.5f};vr_camera::Matrix projection{};require(vr_projection::build(fov,.1f,1000,projection),"projection");
        VrHudCamera game;game.left=-1;game.right=1;game.bottom=-.5f;game.top=.5f;game.units_per_metre=.9f;
        const auto prompt=full_canvas();
        // The eye is the game camera (gamepad first person, same frustum): the canvas fills the eye exactly.
        {const auto h=prompt.camera_locked(game,vr_camera::IDENTITY,projection);require(h.has_value(),"same camera: a map");
         for(const auto [u,v]:{std::pair{.5f,.5f},{.1f,.8f},{.9f,.2f}}){const auto c=apply(*h,u,v);require(near(c[0],u) && near(c[1],v),"same camera: eye UV is canvas UV");}
         require(near(h->bounds[0],0) && near(h->bounds[1],0) && near(h->bounds[2],1) && near(h->bounds[3],1),"same camera: the whole image");}
        // The head turned 30 degrees right of the game camera: the crosshair (canvas centre) stays on the
        // game camera's line, which is now left of the middle of the eye (tan(-30 deg) in NDC).
        {const auto h=prompt.camera_locked(game,camera(30),projection);require(h.has_value(),"turned head: a map");
         const float u=(1+std::tan(-30*.0174532925f))/2;const auto c=apply(*h,u,.5f);
         require(near(c[0],.5f) && near(c[1],.5f),"turned head: the crosshair stays where the game camera aims");
         const auto middle=apply(*h,.5f,.5f);require(middle[0]>.5f,"turned head: the middle of the eye looks right of the crosshair");}
        // Shoulder view: the eye 2 units behind the game camera, same heading. The aim line runs straight
        // away from the eye, so the crosshair stays in the middle; an off-centre point moves inward.
        {const float d=prompt.values.values[VrNativePrompt::DISTANCE]*game.units_per_metre;
         const auto h=prompt.camera_locked(game,camera(0,0,0,-2),projection);require(h.has_value(),"shoulder: a map");
         const auto centre=apply(*h,.5f,.5f);require(near(centre[0],.5f) && near(centre[1],.5f),"shoulder: the crosshair stays in the middle");
         // The canvas's right edge (x=d at depth d) is seen at x/z = d/(d+2).
         const float u=(1+d/(d+2))/2;const auto edge=apply(*h,u,.5f);require(near(edge[0],1),"shoulder: the canvas edge is closer to the middle from behind");}
        // A diorama eye carries scale: the general inverse still finds the line.
        {const auto h=prompt.camera_locked(game,camera(0,0,0,0,10),projection);require(h.has_value(),"scaled eye: a map");
         const auto centre=apply(*h,.5f,.5f);require(near(centre[0],.5f) && near(centre[1],.5f),"scaled eye: the crosshair in the middle");}
        // A cropped canvas: crop UV 0..1 spans only the crop, so the canvas centre is the crop centre.
        {auto crop=prompt;crop.values.values[VrNativePrompt::X]=.25f;crop.values.values[VrNativePrompt::WIDTH]=.5f;
         const auto h=crop.camera_locked(game,vr_camera::IDENTITY,projection);require(h.has_value(),"crop: a map");
         require(near(apply(*h,.5f,.5f)[0],.5f) && near(apply(*h,.25f,.5f)[0],0) && near(h->bounds[0],.25f) && near(h->bounds[2],.75f),"crop: its edges and box");}
        // Looking straight back: the canvas is behind the eye and draws nothing.
        {const auto h=prompt.camera_locked(game,camera(180),projection);require(h.has_value() && h->bounds==std::array<float,4>{0,0,0,0},"behind: an empty box");}
        // Invalid inputs refuse.
        {auto bad=game;bad.right=bad.left;require(!prompt.camera_locked(bad,vr_camera::IDENTITY,projection),"a degenerate frustum refuses");
         vr_camera::Matrix singular{};require(!prompt.camera_locked(game,singular,projection),"a singular eye refuses");}
        // Which scenes lock the canvas to the game camera (30 Oct 20:24 playtest: the game camera pointed 104
        // degrees from the gaze in the hub, and the battle simulator's accept pop-up went with the canvas out
        // of sight). First-person plain play only, though the gaze camera now steers in the hub as well.
        {const auto scene=[](uint32_t mask,bool scripted=false){return scene_mode::classify({true,mask,mask,scripted,false});};
         require(VrNativePrompt::camera_lock(true,scene(0x0)),"first-person gameplay: camera-locked");
         require(!VrNativePrompt::camera_lock(true,scene(0x800)),"the Battle Barge hub (0x800): head-locked");
         require(!VrNativePrompt::camera_lock(true,scene(0xc00)),"a hub load or front end (0xc00): head-locked");
         require(!VrNativePrompt::camera_lock(true,scene(0x108)),"photo mode: head-locked");
         require(!VrNativePrompt::camera_lock(true,scene(0x0,true)),"a cutscene: head-locked");
         require(!VrNativePrompt::camera_lock(true,scene(0x20)),"an unknown bit: head-locked");
         require(!VrNativePrompt::camera_lock(false,scene(0x0)),"third person: head-locked");
         require(!VrNativePrompt::camera_lock(true,scene_mode::classify({})),"unreadable state: head-locked");}
        std::cout<<"hud camera lock tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
