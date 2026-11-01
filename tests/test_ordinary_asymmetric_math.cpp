#include "OrdinaryCameraMath.h"
#include "fixtures/ordinary_asymmetric_native_oracle.h"
#include <iostream>

namespace {
namespace math=ordinary_camera_math;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<size_t N>std::array<float,N> floats(const std::array<uint32_t,N>& words){std::array<float,N> out{};for(size_t i=0;i<N;++i)out[i]=std::bit_cast<float>(words[i]);return out;}
math::TrackedLens lens(const asymmetric_native_fixture::Case& c){const auto f=floats(c.fov);const auto clip=floats(c.clip);return {{f[0],f[1],f[2],f[3]},clip[0],clip[1]};}
math::TrackedInput input(size_t index){
    const auto& cases=asymmetric_native_fixture::CASES;const auto& c=cases[index];math::TrackedInput in;
    in.native.world=floats(c.world);in.native.view=floats(c.view);in.native.projection=floats(c.projection);
    in.native.width=c.width;in.native.height=c.height;in.native.auxiliary_scale=std::bit_cast<float>(c.scale);in.native.depth_remap=std::bit_cast<float>(c.depth);in.native.jitter=floats(c.jitter);in.native.jitter_index=uint32_t(index+1);in.lens=lens(c);
    if(c.previous>=0){const auto& p=cases[size_t(c.previous)];math::TrackedHistory prior;
        prior.camera={floats(p.world),floats(p.view),floats(p.projection),floats(p.expected[math::Result::index(0x59c)]),floats(p.expected[math::Result::index(0x55c)])};
        prior.lens=lens(p);prior.jitter=floats(p.jitter);prior.width=p.width;prior.height=p.height;in.previous=prior;}
    return in;
}
void native_oracle(){
    for(size_t i=0;i<asymmetric_native_fixture::CASES.size();++i){
        const auto in=input(i);require(!math::build(in.native),"existing symmetric desktop entry must continue refusing off-centre lenses");
        const auto result=math::build_tracked(in);require(bool(result),"explicit tracked lens and native-helper history must build");
        const auto& c=asymmetric_native_fixture::CASES[i];
        for(size_t block=0;block<math::Result::OFFSETS.size();++block){const auto offset=math::Result::OFFSETS[block];const auto expected=floats(c.expected[block]);
            const auto& actual=result->packet.at(offset);const float tolerance=(offset==0x9b4||offset==0x9f4)?1e-3f:3e-4f;
            for(size_t word=0;word<16;++word)if(!std::isfinite(actual[word]) || std::abs(actual[word]-expected[word])>tolerance){
                std::cerr<<"native helper oracle case "<<i<<" offset "<<std::hex<<offset<<std::dec<<" word "<<word<<" got "<<actual[word]<<" expected "<<expected[word]<<'\n';throw std::runtime_error("asymmetric packet differs from isolated shipping instructions");}}
        require(result->packet.reset==(c.previous<0) && result->packet.cut==(c.previous<0),"each eye has its own first reset and continuous predecessor");
        const auto history=result->history();require(history.lens==in.lens && history.width==in.native.width && history.height==in.native.height && history.jitter==in.native.jitter && history.camera.projection==in.native.projection,"returned typed history retains the exact current lens/raster input");
    }
}
void admission_and_resets(){
    const auto first=*math::build_tracked(input(0));
    for(unsigned mutation=0;mutation<18;++mutation){auto in=input(1);switch(mutation){
        case 0:in.lens.fov.left-=.01f;break;case 1:in.lens.near_z=.2f;break;case 2:in.lens.far_z=500;break;
        case 3:in.native.projection[8]+=.01f;break;case 4:in.native.projection[1]=.01f;break;case 5:in.native.previous=first.packet.previous();break;
        case 6:in.previous->lens.fov.right+=.01f;break;case 7:in.previous->camera.projection[9]+=.01f;break;
        case 8:in.previous->camera.combined[12]+=.01f;break;case 9:in.previous->camera.raster_combined[8]+=.01f;break;
        case 10:in.previous->camera.view[12]+=.01f;break;case 11:in.previous->jitter[0]=NAN;break;
        case 12:in.previous->width=0;break;case 13:in.native.width=32769;break;case 14:in.native.view[0]=NAN;break;
        case 15:in.previous->camera.delta[0]=NAN;break;case 16:in.lens.fov.bottom=0;break;case 17:in.lens.near_z=-1;break;
    }require(!math::build_tracked(in),"invalid lens, borrowed untyped history, inconsistent previous native packet or raster metadata must refuse");}
    auto changed=input(1);changed.lens.fov.left-=.1f;
    require(vr_projection::build(changed.lens.fov,changed.lens.near_z,changed.lens.far_z,changed.native.projection),"changed explicit lens builds");
    require(!math::build_tracked(changed),"lens change requires a deliberate temporal cut");changed.native.cut=true;
    auto reset=math::build_tracked(changed);require(reset && reset->packet.reset && reset->packet.delta==std::array<float,3>{},"explicit lens change cut resets own previous state");
    for(size_t i=0;i<16;++i)require(std::abs(reset->packet.at(0x9b4)[i]-vr_camera::IDENTITY[i])<1e-4f,"reset shader motion stays identity under an asymmetric lens");
    changed=input(1);++changed.native.width;require(!math::build_tracked(changed),"raster size change without temporal cut refuses");changed.native.cut=true;require(bool(math::build_tracked(changed)),"explicit resize cut accepts coherent regenerated current packet");
    auto ordinary=input(0);ordinary.lens.fov={-1,1,-1,1};require(vr_projection::build(ordinary.lens.fov,ordinary.lens.near_z,ordinary.lens.far_z,ordinary.native.projection),"symmetric explicit lens builds");
    require(math::build(ordinary.native) && math::build_tracked(ordinary),"explicit tracked API does not require artificial asymmetry while legacy desktop admission remains intact");
}
}
int main(){try{native_oracle();admission_and_resets();std::cout<<"32 asymmetric native-helper oracle cases x23 matrices and typed lens/history refusal/reset controls PASS; offline modeled schedule only\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
