#pragma once
#include <array>
#include <cmath>
#include <cstring>

// Model the shipping symmetric radius shortcut and its actual polygon-based
// direction calculation. This deliberately does not use production culling code.
inline void native_compact_frustum(uint8_t* camera){
    const auto get=[&]<class T>(size_t offset){T v{};std::memcpy(&v,camera+offset,sizeof(v));return v;};
    const auto put=[&](size_t offset,const auto& value){std::memcpy(camera+offset,&value,sizeof(value));};
    const auto world=get.operator()<std::array<float,16>>(0);
    const auto rect=get.operator()<std::array<float,4>>(0x12c);
    const auto corners=get.operator()<std::array<float,8>>(0x8c);
    const auto near_z=get.operator()<float>(0x80);
    const float radius=std::sqrt((rect[2]*rect[2]+rect[3]*rect[3])*.25f+near_z*near_z);
    std::array<float,12> directions{},sides{};
    std::array<float,3> normal{-world[8],-world[9],-world[10]},origin{world[12],world[13],world[14]};
    for(size_t c=0;c<4;++c){float length{};for(size_t a=0;a<3;++a){auto& v=directions[3*c+a];v=world[a]*corners[2*c]+world[4+a]*corners[2*c+1]+world[8+a]*near_z;length+=v*v;}
        length=std::sqrt(length);for(size_t a=0;a<3;++a)directions[3*c+a]/=length;}
    for(size_t c=0;c<4;++c)for(size_t axis=0;axis<3;++axis){const auto a=(axis+1)%3,b=(axis+2)%3,n=(c+1)%4;
        sides[c*3+axis]=directions[c*3+a]*directions[n*3+b]-directions[c*3+b]*directions[n*3+a];}
    const auto cap=(normal[0]*directions[0]+normal[1]*directions[1]+normal[2]*directions[2])*radius;
    put(0x2f8,radius);put(0x30c,cap);put(0x310,directions);put(0x340,sides);put(0x370,normal);put(0x37c,origin);
}
