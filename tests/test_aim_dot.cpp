#include "VrAimDot.h"
#include "VrProjection.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error{why};}
bool near(float a,float b,float tolerance=1e-3f){return std::abs(a-b)<=tolerance;}
// A game-convention world matrix (rows right, up, forward, translation) turned by yaw degrees to the
// right, at a position, with a uniform scale (the diorama's).
vr_camera::Matrix camera(float yaw_degrees,float x=0,float y=0,float z=0,float scale=1){
    const float a=yaw_degrees*.0174532925f,c=std::cos(a)*scale,s=std::sin(a)*scale;
    return {c,0,-s,0, 0,scale,0,0, s,0,c,0, x,y,z,1};
}
// The dot's centre and angular width in normalised device coordinates.
struct Seen { float x,y,width; };
Seen seen(const std::array<float,16>& c){
    Seen s{};for(int k=0;k<4;++k){s.x+=c[k*4]/c[k*4+3]/4;s.y+=c[k*4+1]/c[k*4+3]/4;}
    s.width=c[4]/c[7]-c[0]/c[3];return s;
}
}
int main(int argc,char** argv){
    try{
        const vr_projection::Tangents fov{-1,1,-1,1};vr_camera::Matrix projection{};require(vr_projection::build(fov,.05f,1000,projection),"projection");
        require(!VrAimDot{}.enabled() && !VrAimDot{}.draw({0,0,10}),"the dot is off by default (the crosshair patch marks the aim)");
        VrAimDot dot;dot.values.values[VrAimDot::ENABLED]=1;const auto d=dot.draw({0,0,10});require(d && d->size_degrees==dot.values.values[VrAimDot::SIZE],"a draw for a finite point");
        const float tan_half=std::tan(d->size_degrees*.00872664626f);
        // Straight ahead: centred, and the angular size in NDC (tangent 1 is NDC 1 with this frustum).
        {const auto c=VrAimDot::clip(*d,vr_camera::IDENTITY,projection);require(c.has_value(),"ahead: drawn");const auto s=seen(*c);
         require(near(s.x,0) && near(s.y,0) && near(s.width,2*tan_half),"ahead: centred at its angular size");
         for(int k=0;k<4;++k)require((*c)[k*4+2]==0 && (*c)[k*4+3]>0,"ahead: z 0 (never depth-clipped), w positive");
         require((*c)[0]<(*c)[4] && (*c)[1]>(*c)[9],"corner order: top-left, top-right, bottom-right, bottom-left");}
        // Twice as far: the same angular size.
        {const auto far=dot.draw({0,0,20});const auto s=seen(*VrAimDot::clip(*far,vr_camera::IDENTITY,projection));require(near(s.width,2*tan_half),"farther: same angular size");}
        // Diorama side view: the aim runs along +z from the origin to (0,0,10); the eye stands 3 units to
        // the right of the hit point, scaled 9x, turned 90 degrees left to face it. The dot is in the middle of
        // the eye, on the hit point, not on the line near the gun.
        {const auto eye=camera(-90,3,0,10,9);const auto c=VrAimDot::clip(*d,eye,projection);require(c.has_value(),"side: drawn");const auto s=seen(*c);
         require(near(s.x,0) && near(s.y,0) && near(s.width,2*tan_half),"side: the dot sits on the hit point at its angular size");}
        // A point to the right and above: right of and above the middle.
        {const auto off=dot.draw({2,1,10});const auto s=seen(*VrAimDot::clip(*off,vr_camera::IDENTITY,projection));require(near(s.x,.2f) && near(s.y,.1f),"off-centre: where the point projects");}
        // Behind the eye: not drawn. Unusable inputs: not drawn.
        require(!VrAimDot::clip(*d,camera(180),projection),"behind: not drawn");
        vr_camera::Matrix singular{};require(!VrAimDot::clip(*d,singular,projection),"a singular eye: not drawn");
        require(!dot.draw({NAN,0,1}),"a non-finite point: no draw");
        // Pixel of the centre in a 200x100 image.
        {const auto p=VrAimDot::pixel(*d,vr_camera::IDENTITY,projection,200,100);require(p && near((*p)[0],100,.01f) && near((*p)[1],50,.01f),"pixel: the image centre");}
        // Modes and knobs.
        require(dot.shows(VrViewMode::diorama) && dot.shows(VrViewMode::first_person),"enabled: every view");
        {auto third=dot;third.values.values[VrAimDot::FIRST_PERSON]=0;require(!third.shows(VrViewMode::first_person) && third.shows(VrViewMode::shoulder),"first_person=0: third person only");}
        {auto off=dot;off.values.values[VrAimDot::ENABLED]=0;require(!off.shows(VrViewMode::diorama) && !off.draw({0,0,1}),"enabled=0: never");}
        {auto bad=dot;bad.values.values[VrAimDot::ENABLED]=.5f;require(!bad.valid(),"a switch must be 0 or 1");}
        // The shipped settings load and are the defaults.
        if(argc>1){const auto shipped=VrAimDot::load(argv[1]);require(shipped && *shipped==VrAimDot{},"config/index_aim_dot.calibration loads as the defaults");}
        if(argc>2){const auto shipped=VrHudReticle::load(argv[2]);require(shipped && *shipped==VrHudReticle{},"config/index_hud_reticle.calibration loads as the defaults");}
        // The game's crosshair patch (VrHudReticle).
        {const VrHudReticle reticle;const float aspect=16.f/9;
         require(reticle.shows(VrViewMode::first_person) && reticle.shows(VrViewMode::diorama) && reticle.shows(VrViewMode::chase),"the patch moves in every view by default (31 Oct: first person too)");
         {auto third=reticle;third.values.values[VrHudReticle::FIRST_PERSON]=0;require(!third.shows(VrViewMode::first_person) && third.shows(VrViewMode::diorama),"first_person=0: third person only");}
         // First person follows the canvas's rule: plain play moves it, the hub and the rest keep it on the canvas.
         {const auto scene=[](uint32_t mask){return scene_mode::classify({true,mask,mask});};
          require(reticle.shows(VrViewMode::first_person,scene(0)),"first person in plain play: at the aim point");
          require(!reticle.shows(VrViewMode::first_person,scene(0x800)),"first person in the hub: on the canvas, with the pop-ups around it");
          require(!reticle.shows(VrViewMode::first_person,scene(0x108)),"first person in photo mode: on the canvas");
          require(reticle.shows(VrViewMode::diorama,scene(0x800)),"third person: the scene rule does not apply");
          auto third=reticle;third.values.values[VrHudReticle::FIRST_PERSON]=0;require(!third.shows(VrViewMode::first_person,scene(0)),"first_person=0 holds in plain play");}
         const auto r=reticle.rect(aspect);require(r && near((*r)[3],.08f) && near((*r)[2]*aspect,.3f) && near((*r)[0]+(*r)[2]/2,.5f) && near((*r)[1]+(*r)[3]/2,.4225f),"rect: around the crosshair, wide enough for the heat bars beside it");
         // The magazine column (31 Oct 17:48). Game UI data, 1920x1080 design pixels from the crosshair: bar from
         // 77 down and 46 either side, counter 93 to 125 down. It is narrow and joins the band's lower edge.
         {const auto c=reticle.ammo_rect(aspect);require(c.has_value(),"ammo: a column by default");
          const float crosshair=reticle.values.values[VrHudReticle::CENTRE_Y];
          require(near((*c)[1],(*r)[1]+(*r)[3]) && (*c)[1]<=crosshair+77.f/1080 && (*c)[1]+(*c)[3]>=crosshair+125.f/1080,"ammo: from the band down past the counter");
          require((*c)[0]<=.5f-46.f/1920 && (*c)[0]+(*c)[2]>=.5f+46.f/1920 && (*c)[2]<(*r)[2]/2,"ammo: the bar's width, well under the band's");
          auto none=reticle;none.values.values[VrHudReticle::AMMO_BELOW]=0;require(!none.ammo_rect(aspect),"ammo_below=0: no column");
          auto tall=reticle;tall.values.values[VrHudReticle::AMMO_BELOW]=.3f;tall.values.values[VrHudReticle::CENTRE_Y]=.8f;tall.values.values[VrHudReticle::HEIGHT]=.02f;require(!tall.ammo_rect(aspect),"ammo: off the canvas refuses");
          const auto both=VrNativeHudExcluded::of(VrNativeHud{},VrNativeHudCorners::off(),*r,*c);
          require(VrNativeHudExcluded::inside(both,.5f,crosshair+.1f) && !VrNativeHudExcluded::inside(both,.6f,crosshair+.1f),"ammo: the canvas leaves out the column, not the sides under the band");}
         // Other UI beside the patch: a focused pickup hint is 220 by 80 design pixels, 125 above its object.
         {std::array<uint8_t,64*36> grid{};const auto clear=[&]{grid.fill(0);};
          const auto light=[&](float x0,float y0,float x1,float y1,uint8_t value=200){for(int row=0;row<36;++row)for(int column=0;column<64;++column){const float x=(column+.5f)*30,y=(row+.5f)*30;if(x>=x0 && x<x1 && y>=y0 && y<y1)grid[row*64+column]=value;}};
          const auto crowded=[&]{return reticle.crowded(grid,64,36,aspect);};
          const float cx=960,cy=456;
          require(!crowded(),"crowded: an empty canvas is not");
          light(cx-45,cy-45,cx+45,cy+45);light(cx-46,cy+77,cx+46,cy+125);require(!crowded(),"crowded: the crosshair, reload ring, magazine bar and counter are its own");
          clear();const auto hint=[&](float object_y){clear();light(cx-110,object_y-165,cx+110,object_y-85);};
          hint(540);require(crowded(),"crowded: a hint on an object at the gaze, over the band's top edge");
          hint(580);require(crowded(),"crowded: a hint wholly inside the band");
          hint(620);require(crowded(),"crowded: a hint across the band's lower edge, either side of the column");
          hint(500);require(crowded(),"crowded: a hint just above the band");
          hint(680);require(crowded(),"crowded: a hint just below the band");
          hint(900);require(!crowded(),"crowded: a hint well below the patch");
          clear();light(cx+100,cy-60,cx+130,cy-30);require(!crowded(),"crowded: one lit cell is not a hint");
          clear();light(cx-110,375,cx+110,455,20);require(!crowded(),"crowded: too dim to be a hint");
          require(!reticle.crowded(std::span<const uint8_t>{grid}.first(10),64,36,aspect),"crowded: a grid of the wrong size");}
         // The canvas stays whole for 300 ms after a crowded sample, never within 2 s of a trigger.
         {using Clock=VrHudReticle::Aside::Clock;const Clock::time_point t0{std::chrono::hours(1)};VrHudReticle::Aside aside;
          require(!aside.active(t0),"aside: off to begin with");
          aside.sample(true,t0);require(aside.active(t0+std::chrono::milliseconds(200)) && !aside.active(t0+std::chrono::milliseconds(400)),"aside: held over the 100 ms grid");
          aside.sample(true,t0);aside.sample(false,t0+std::chrono::milliseconds(100));require(aside.active(t0+std::chrono::milliseconds(200)),"aside: a clear sample does not end the hold");
          aside.fire(t0);aside.sample(true,t0+std::chrono::seconds(1));require(!aside.active(t0+std::chrono::seconds(1)),"aside: the crosshair's own firing lights those cells");
          aside.sample(true,t0+std::chrono::seconds(3));require(aside.active(t0+std::chrono::seconds(3)),"aside: back 2 s after the last trigger");
          auto off=reticle;off.values.values[VrHudReticle::STAND_ASIDE]=0;require(reticle.stands_aside() && !off.stands_aside(),"stand_aside knob");
          auto bad=reticle;bad.values.values[VrHudReticle::STAND_ASIDE]=.5f;require(!bad.valid(),"stand_aside is 0 or 1");}
         {auto square=reticle;square.values.values[VrHudReticle::WIDTH]=.08f;const auto s=square.rect(aspect);require(s && near((*s)[2]*aspect,(*s)[3]),"width 0.08 with height 0.08: square in pixels");}
         {auto edge=reticle;edge.values.values[VrHudReticle::WIDTH]=.8f;edge.values.values[VrHudReticle::CENTRE_X]=.2f;require(!edge.rect(.5f),"rect: off the canvas (a tall canvas) refuses");}
         // The game camera: 110 degrees across at 16:9 (the first-person pack), an eye with the same frustum.
         VrHudCamera game;game.left=-1.428f;game.right=1.428f;game.bottom=-.803f;game.top=.803f;
         vr_camera::Matrix same{};require(vr_projection::build({game.left,game.right,game.bottom,game.top},.05f,1000,same),"game projection");
         const auto apply=[](const VrHudHomography& h,float u,float v){const auto& m=h.h;const float x=m[0]*u+m[1]*v+m[2],y=m[3]*u+m[4]*v+m[5],w=m[6]*u+m[7]*v+m[8];return std::array<float,2>{x/w,y/w};};
         // The eye is the game camera and the hit point dead ahead: the patch lands exactly where the canvas
         // showed it, so moving it changes nothing in first person with a gamepad.
         {VrHudReticle middle;middle.values.values[VrHudReticle::CENTRE_Y]=.5f;const auto m=middle.rect(aspect);
          const auto h=middle.at({0,0,25},game,aspect,vr_camera::IDENTITY,same);require(h.has_value(),"same camera: a map");
          const auto centre=apply(*h,.5f,.5f),corner=apply(*h,(*m)[0],(*m)[1]),far=apply(*h,(*m)[0]+(*m)[2],(*m)[1]+(*m)[3]);
          require(near(centre[0],.5f) && near(centre[1],.5f) && near(corner[0],0) && near(corner[1],0) && near(far[0],1) && near(far[1],1),"same camera: the patch covers its own canvas rectangle");
          require(near(h->bounds[0],(*m)[0]) && near(h->bounds[2],(*m)[0]+(*m)[2]),"same camera: the box is the rectangle");}
         // A diorama eye side-on to the aim (as for the dot): the patch's centre is on the hit point.
         {const auto h=reticle.at({0,0,10},game,aspect,camera(-90,3,0,10,9),projection);require(h.has_value(),"side: a map");
          const auto centre=apply(*h,.5f,.5f);require(near(centre[0],.5f) && near(centre[1],.5f),"side: the crosshair sits on the hit point");}
         // Behind the eye: an empty map, so nothing is drawn and the crosshair still leaves the canvas.
         {const auto h=reticle.at({0,0,10},game,aspect,camera(180),projection);require(h && h->empty(),"behind: a map that draws nothing");}
         {vr_camera::Matrix singular{};require(!reticle.at({0,0,10},game,aspect,singular,projection),"a singular eye: no map, the crosshair stays on the canvas");}
         require(!reticle.at({0,0,10},game,aspect,vr_camera::IDENTITY,projection)->empty(),"ahead: a map that draws");
         // The default place is the measured one (31 Oct 16:04, unsteered first-person rays): the aim ray starts
         // at the game camera, 7.10 degrees above its axis on the 110-degree camera and 4.18 zoomed.
         {const auto aimed=[](float degrees,const VrHudCamera& c){return VrHudReticle::canvas_point({0,std::tan(degrees*.0174532925f)*30,30},c);};
          const float centre_y=reticle.values.values[VrHudReticle::CENTRE_Y];
          VrHudCamera zoomed=game;zoomed.left=-.839f;zoomed.right=.839f;zoomed.bottom=-.472f;zoomed.top=.472f;
          require(near((*aimed(7.1f,game))[1],centre_y) && near((*aimed(4.18f,zoomed))[1],centre_y),"the crosshair's canvas place is the default centre, at either field of view");
          const auto patch=VrNativeHudExcluded::of(VrNativeHud{},VrNativeHudCorners::off(),*r);
          require(VrNativeHudExcluded::inside(patch,.5f,(*aimed(7.1f,game))[1]),"the default patch holds the crosshair from the first pair");}
         // First person takes the knobs; a game-aimed sample from an animation must not drag the patch off the crosshair.
         {const std::array<float,2> sample{.52f,.47f};
          require(reticle.placed(VrViewMode::first_person,sample)==reticle && reticle.placed(VrViewMode::first_person,std::nullopt)==reticle,"placed: first person keeps the knobs");
          require(near(reticle.placed(VrViewMode::diorama,sample).values.values[VrHudReticle::CENTRE_Y],.47f) && reticle.placed(VrViewMode::diorama,std::nullopt)==reticle,"placed: third person follows the sample, else the knobs");}
         // Where the game's crosshair is on the canvas: the hit point through the game camera. Ahead is the
         // centre; a point 0.136 (tangent) above the camera's axis is at v 0.415 on this 16:9 frustum.
         {const auto ahead=VrHudReticle::canvas_point({0,0,30},game);require(ahead && near((*ahead)[0],.5f) && near((*ahead)[1],.5f),"canvas point: ahead is the centre");
          const auto above=VrHudReticle::canvas_point({0,.136f*30,30},game);require(above && near((*above)[0],.5f) && near((*above)[1],.5f-.136f/1.606f),"canvas point: above the axis is above the centre");
          const auto turned=VrHudReticle::canvas_point({0,0,30},VrHudCamera{camera(-20,0,0,0),game.left,game.right,game.bottom,game.top,1});
          require(turned && (*turned)[0]>.5f,"canvas point: a camera turned left sees the point right of centre");
          require(!VrHudReticle::canvas_point({0,0,-30},game),"canvas point: behind the camera, none");
          const auto moved=reticle.centred(*above);require(near(moved.values.values[VrHudReticle::CENTRE_Y],(*above)[1]) && moved.rect(aspect),"centred: the patch follows the crosshair");
          require(reticle.centred({.5f,.05f})==reticle,"centred: a point outside the knobs' range leaves the patch where it was");}
         require(!VrHudReticle::off().at({0,0,10},game,aspect,vr_camera::IDENTITY,projection) && !VrHudReticle::off().shows(VrViewMode::diorama),"off: never");
         // The canvas leaves the patch out, and only the patch.
         const auto excluded=VrNativeHudExcluded::of(VrNativeHud{},VrNativeHudCorners::off(),*r);
         require(VrNativeHudExcluded::inside(excluded,.5f,.4225f) && !VrNativeHudExcluded::inside(excluded,.5f,(*r)[1]-.01f),"the canvas leaves out the patch");
         require(!VrNativeHudExcluded::inside(VrNativeHudExcluded::of(VrNativeHud{},VrNativeHudCorners::off()),.5f,.4225f),"no patch: the crosshair stays on the canvas");}
        std::cout<<"aim dot tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
