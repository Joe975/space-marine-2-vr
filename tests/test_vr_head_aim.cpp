#include "VrHeadAim.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Matrix=vr_head_aim::Matrix;using Vector=vr_head_aim::Vector;
void require(bool ok,const std::string& why) {if(!ok) throw std::runtime_error{why};}
float angle(Vector a,Vector b) {return vr_upright::angle_degrees(a,b);}
Matrix yawed(float degrees,Vector position={}) {
    const float a=degrees/57.2957795f;
    Matrix m{std::cos(a),0,-std::sin(a),0, 0,1,0,0, std::sin(a),0,std::cos(a),0, 0,0,0,1};
    vr_arm::position(m,position);return m;
}
// The Marine's spine, neck, head and eyes as the game published them (build/arms_owned_rig_first.json,
// 13 Oct, an idle stance). An arm stands in for everything else on the chest.
struct Rig {
    std::vector<std::string> names{"CharacterWorldSpaceTM","CENTRE","BACKdown","BACKmiddle","BACKup","NECK","HEAD","eye_L","eye_R","ARM1L"};
    std::vector<int32_t> parents{-1,0,1,2,3,4,5,6,6,4};
    std::vector<int16_t> mapping{-1,0,1,20,2,21,3,22,23,120};
    std::vector<Matrix> locals{vr_camera::IDENTITY,
        {-0.0203028f,0.9997539f,0.0089454f,0,-0.1872806f,-0.0125918f,0.9822258f,0,0.9820967f,0.0182665f,0.1874901f,0,0.0514023f,1.4650235f,0.0868436f,1},
        {0.9993280f,0.0286324f,0.0228835f,0,-0.0282445f,0.9994548f,-0.0170982f,0,-0.0233606f,0.0164404f,0.9995919f,0,0.1441034f,0.0016555f,-0.0019002f,1},
        {0.9943694f,-0.0375003f,0.0991115f,0,0.0443198f,0.9967328f,-0.0675244f,0,-0.0962555f,0.0715368f,0.9927827f,0,0.1623970f,-0.0007509f,0.0029525f,1},
        {0.9994028f,-0.0275689f,0.0208314f,0,0.0278494f,0.9995236f,-0.0133002f,0,-0.0204548f,0.0138724f,0.9996945f,0,0.1358868f,-0.0023351f,0.0018197f,1},
        {0.9524223f,0.3047434f,0.0048294f,0,-0.3044550f,0.9505487f,0.0613534f,0,0.0141065f,-0.0599047f,0.9981044f,0,0.3229503f,0.0019215f,0.0000648f,1},
        {0.9939415f,-0.1021919f,-0.0404611f,0,0.1074913f,0.9806117f,0.1638486f,0,0.0229327f,-0.1672052f,0.9856554f,0,0.1320426f,0.0000181f,0.0000648f,1},
        {0.1934254f,0.9811150f,0,0,-0.9811150f,0.1934254f,0,0,0,0,1,0,0.0877701f,0.1003427f,0.0430107f,1},
        {0.1934254f,0.9811150f,0,0,-0.9811150f,0.1934254f,0,0,0,0,1,0,0.0877701f,0.1003427f,-0.0430202f,1},
        {1,0,0,0,0,1,0,0,0,0,1,0,.2f,.3f,.2f,1}};
};
std::vector<Matrix> posed(const Rig& rig,const std::vector<Matrix>& locals,const Matrix& root) {
    std::vector<Matrix> world;require(vr_arm::hierarchy(locals,rig.parents,root,world),"the rig evaluates");return world;
}
Vector face(const Rig& rig,const std::vector<Matrix>& locals,const Matrix& root) {return vr_arm::rotate(vr_head_aim::FACE,posed(rig,locals,root)[6]);}
const Vector AHEAD{0,0,1}; // A character's +z root axis is its forward (the trace's root_z_axis against body_yaw).
Vector at(float yaw,float pitch,float root_yaw=0) { // Left of the root's forward and up, degrees.
    const float y=(root_yaw+yaw)/57.2957795f,p=pitch/57.2957795f;
    return {std::cos(p)*std::sin(y),std::sin(p),std::cos(p)*std::cos(y)};
}

// FACE and the marine's axes: the eyes look straight ahead in the idle stance the rig was dumped in.
void face_axis_is_the_rigs() {
    Rig rig;const auto root=vr_camera::IDENTITY;
    require(angle(face(rig,rig.locals,root),AHEAD)<7,"the idle Marine looks ahead along FACE");
    const auto eye=vr_arm::rotate({1,0,0},rig.locals[7]);
    require(angle(eye,vr_head_aim::FACE)<.01f,"FACE is the eye bones' own axis");
    // Left: the rig's +x, with +z forward and y up.
    const auto world=posed(rig,rig.locals,root);
    require(vr_arm::position(world[7])[0]>vr_arm::position(world[8])[0],"eye_L is on the Marine's left, world +x when it faces +z");
}

void gaze_is_the_eyes_forward() {
    // A native camera (mirror) anchor and a game-style proper one both give compose's forward.
    for(const float sign:{1.f,-1.f}) for(const float heading:{0.f,35.f,-120.f}) {
        auto anchor=yawed(heading,{3,4,5});for(int j=0;j<3;++j) anchor[j]*=sign;
        for(const float head_yaw:{0.f,50.f}) for(const float head_pitch:{0.f,-25.f}) {
            auto head=vr_camera::multiply(vr_upright::rotation({1,0,0},head_pitch/57.2957795f),yawed(head_yaw));
            vr_arm::position(head,{.1f,.2f,.3f});
            vr_camera::Matrix world{},view{};
            require(vr_camera::compose(anchor,head,1,world,view),"compose");
            const auto gaze=vr_head_aim::gaze_world(anchor,head);
            require(gaze && angle(*gaze,{world[8],world[9],world[10]})<.02f,"gaze is the composed eye's forward");
        }
    }
    auto bad=vr_camera::IDENTITY;bad[0]=2;
    require(!vr_head_aim::gaze_world(vr_camera::IDENTITY,bad) && !vr_head_aim::gaze_world(bad,vr_camera::IDENTITY),"a broken matrix gives no gaze");
}

void aim_lands_on_the_gaze() {
    Rig rig;
    for(const float root_yaw:{0.f,-101.f,53.f}) {
        const auto root=yawed(root_yaw,{10,-706,3});
        for(const auto& [yaw,pitch]:std::array<std::array<float,2>,6>{{{13,-7},{-30,5},{0,0},{45,-20},{-60,15},{20,25}}}) {
            const auto gaze=at(yaw,pitch,root_yaw);
            const auto done=vr_head_aim::aim(rig.locals,rig.parents,rig.names,rig.mapping,root,gaze);
            require(done.has_value(),"a gaze inside the limits is aimed");
            require(!done->limited,"inside the limits");
            require(angle(face(rig,done->locals,root),gaze)<.1f,"the eyes end on the gaze");
            for(size_t i=0;i<rig.locals.size();++i) if(i!=5 && i!=6) require(done->locals[i]==rig.locals[i],"only NECK and HEAD change");
            require(done->locals[5]!=rig.locals[5] && done->locals[6]!=rig.locals[6],"the neck shares the turn");
            require(vr_camera::rigid(done->locals[5]) && vr_camera::rigid(done->locals[6]),"the bones stay rigid");
            for(const size_t i:{size_t{5},size_t{6}}) require(vr_arm::length(vr_arm::sub(vr_arm::position(done->locals[i]),vr_arm::position(rig.locals[i])))<1e-4f,"the bones turn in place");
        }
    }
    // The share: with none the neck stays and the head does it all.
    const auto root=vr_camera::IDENTITY;const auto gaze=at(40,0);
    vr_head_aim::Limits head_only;head_only.neck_share=0;
    const auto done=vr_head_aim::aim(rig.locals,rig.parents,rig.names,rig.mapping,root,gaze,head_only);
    require(done.has_value(),"aimed with the head alone");
    for(size_t k=0;k<16;++k) require(std::abs(done->locals[5][k]-rig.locals[5][k])<1e-4f,"neck_share 0 leaves the neck");
    require(angle(face(rig,done->locals,root),gaze)<.1f,"and the head still gets there");
    const auto half=vr_head_aim::aim(rig.locals,rig.parents,rig.names,rig.mapping,root,gaze);
    require(half && half->turned_degrees>30 && half->turned_degrees<60,"reports the turn from the animation");
}

void aim_stops_at_the_limits() {
    Rig rig;const auto root=vr_camera::IDENTITY;
    const auto chest=posed(rig,rig.locals,root)[4];
    const auto in_chest=[&](Vector world){return vr_arm::rotate(world,vr_camera::inverse(chest));};
    const auto yaw_of=[&](Vector v){const auto c=in_chest(v);return std::atan2(c[2],c[1])*57.2957795f;};
    const auto pitch_of=[&](Vector v){const auto c=in_chest(v);return std::asin(c[0]/vr_arm::length(c))*57.2957795f;};
    const vr_head_aim::Limits limits;
    for(const float side:{1.f,-1.f}) {
        // In the chest's axes: left is +z.
        const auto gaze=vr_arm::rotate({0,std::cos(120/57.2957795f),side*std::sin(120/57.2957795f)},chest);
        const auto done=vr_head_aim::aim(rig.locals,rig.parents,rig.names,rig.mapping,root,gaze);
        require(done && done->limited,"a gaze over the shoulder is limited");
        require(std::abs(yaw_of(face(rig,done->locals,root))-side*limits.yaw_degrees)<.2f,"the head stops at the yaw limit, on the side the player looked");
    }
    for(const auto& [up,limit]:std::array<std::array<float,2>,2>{{{80,35},{-80,-45}}}) {
        const auto gaze=vr_arm::rotate({std::sin(up/57.2957795f),std::cos(up/57.2957795f),0},chest);
        const auto done=vr_head_aim::aim(rig.locals,rig.parents,rig.names,rig.mapping,root,gaze);
        require(done && done->limited && std::abs(pitch_of(face(rig,done->locals,root))-limit)<.2f,"the head stops at the pitch limit");
    }
    const auto behind=vr_arm::rotate({0,-1,0},chest);
    const auto done=vr_head_aim::aim(rig.locals,rig.parents,rig.names,rig.mapping,root,behind);
    require(done && done->limited && vr_arm::dot(face(rig,done->locals,root),vr_arm::rotate({0,1,0},chest))>0,"a gaze straight behind never swings the head through the back");
}

void refused_rigs_change_nothing() {
    const auto root=vr_camera::IDENTITY;const auto gaze=at(30,0);
    const auto refused=[&](const Rig& rig,const char* why){require(!vr_head_aim::aim(rig.locals,rig.parents,rig.names,rig.mapping,root,gaze),why);};
    {Rig rig;rig.names[7]="eye";refused(rig,"no eye bones: the axes can't be trusted");}
    {Rig rig;std::swap(rig.locals[7][14],rig.locals[8][14]);refused(rig,"eyes on the wrong sides");}
    {Rig rig;rig.locals[7][13]=-.1f;refused(rig,"eyes behind the head");}
    {Rig rig;rig.mapping[5]=-1;refused(rig,"unpublished neck");}
    {Rig rig;rig.mapping[6]=-1;refused(rig,"unpublished head");}
    {Rig rig;rig.parents[6]=4;refused(rig,"head not on the neck");}
    {Rig rig;rig.names[9]="HEAD";refused(rig,"two heads");}
    Rig rig;
    require(!vr_head_aim::aim(rig.locals,rig.parents,rig.names,rig.mapping,root,{NAN,0,1}),"a broken gaze");
    require(!vr_head_aim::aim(rig.locals,rig.parents,rig.names,rig.mapping,root,{0,0,0}),"no gaze");
    vr_head_aim::Limits bad;bad.neck_share=2;
    require(!vr_head_aim::aim(rig.locals,rig.parents,rig.names,rig.mapping,root,gaze,bad),"bad limits");
    auto scaled=rig.locals;for(int j=0;j<3;++j) scaled[6][j]*=.001f;
    require(!vr_head_aim::aim(scaled,rig.parents,rig.names,rig.mapping,root,gaze),"a shrunken head (first person) is never aimed");
}
}

int main() {
    try {face_axis_is_the_rigs();gaze_is_the_eyes_forward();aim_lands_on_the_gaze();aim_stops_at_the_limits();refused_rigs_change_nothing();}
    catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
    std::cout<<"vr_head_aim tests passed\n";return 0;
}
