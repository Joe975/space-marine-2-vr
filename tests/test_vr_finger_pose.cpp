#include "VrFingerPose.h"
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error{why};}
vr_arm::Matrix closed_pose(float side) {
    auto m=vr_camera::IDENTITY;const float angle=side*1.2f;
    m[5]=m[10]=std::cos(angle);m[6]=std::sin(angle);m[9]=-std::sin(angle);m[12]=side*0.06f;
    return m;
}
}
int main() {
    try {
        for(size_t hand=0;hand<2;++hand) {
            std::vector<std::string> names{"BODY",hand==0?"HANDL":"HANDR","OTHER"};
            std::vector<int32_t> parents{-1,0,0};std::vector<vr_arm::Matrix> native(3,vr_camera::IDENTITY),bind=native;
            if(hand==1){names.push_back("HANDR_glove");parents.push_back(0);native.push_back(vr_camera::IDENTITY);bind.push_back(vr_camera::IDENTITY);}
            const float side=hand==0?1.0f:-1.0f;
            for(size_t branch=0;branch<(hand==1?2u:1u);++branch)for(size_t finger=1;finger<=5;++finger) {
                int32_t parent=branch?3:1;
                for(size_t segment=1;segment<=3;++segment) {
                    names.push_back(std::string{hand==0?"L":"R"}+std::to_string(finger)+"_fing"+std::to_string(segment)+(branch?"_glove":""));
                    parents.push_back(parent);parent=static_cast<int32_t>(names.size()-1);
                    native.push_back(closed_pose(side));auto open=vr_camera::IDENTITY;open[12]=side*0.061f;bind.push_back(open);
                }
            }
            const auto baseline=native;
            const auto open=vr_finger::solve(native,bind,parents,names,hand,{0,0,0,0,0});
            const auto fist=vr_finger::solve(native,bind,parents,names,hand,{1,1,1,1,1});
            const auto half=vr_finger::solve(native,bind,parents,names,hand,{0.5f,0.5f,0.5f,0.5f,0.5f});
            const auto point=vr_finger::solve(native,bind,parents,names,hand,{1,0,1,1,1});
            require(open && fist && half && point && fist->locals==native && native==baseline,"open/fist/half/point solve preserves input and exact native fist endpoint");
            require(open->bones==(hand==0?15u:30u) && open->max_open_angle_degrees>68,"both native and separate glove chains move with measurable open range");
            require(open->locals[0]==native[0] && open->locals[1]==native[1] && open->locals[2]==native[2],"finger solve never changes body/wrist/unrelated bones");
            for(size_t i=hand==0?3u:4u;i<native.size();++i) {
                require(vr_camera::rigid(half->locals[i]) && half->locals[i][5]>native[i][5] && half->locals[i][5]<open->locals[i][5],"half curl is continuous proper spherical interpolation");
                require(vr_arm::position(open->locals[i])==vr_arm::position(native[i]),"opening never copies bind translations or stretches finger lengths");
                const bool index=names[i].find("2_fing")!=std::string::npos;
                require(point->locals[i]==(index?open->locals[i]:native[i]),"index extension alone forms pointing pose including glove");
            }
            auto bad=names;bad.back()=bad[3];require(!vr_finger::solve(native,bind,parents,bad,hand,{0,0,0,0,0}),"missing/duplicate chain rejects complete finger batch");
            auto bad_parent=parents;bad_parent.back()=0;require(!vr_finger::solve(native,bind,bad_parent,names,hand,{0,0,0,0,0}),"wrong finger hierarchy rejects");
            require(!vr_finger::solve(native,bind,parents,names,hand,{0,NAN,0,0,0}) && !vr_finger::solve(native,bind,parents,names,hand,{0,1.1f,0,0,0}),"nonfinite/out-of-range curls rejected");
        }
        std::cout<<"open, fist, partial curl, pointing, mirrored hands, glove chains and native isolation passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
