#include "OrdinaryCameraMath.h"
#include "fixtures/ordinary_camera_math_native.h"
#include <iostream>
#include <limits>
#include <stdexcept>
namespace math=ordinary_camera_math;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
math::Matrix matrix(const std::array<uint32_t,16>& words){math::Matrix out{};for(size_t i=0;i<16;++i)out[i]=std::bit_cast<float>(words[i]);return out;}
bool same(const math::Matrix& a,const math::Matrix& b,float tolerance=3e-4f){for(size_t i=0;i<16;++i)if(!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::abs(a[i]-b[i])>tolerance)return false;return true;}
math::Matrix captured(size_t i,size_t offset){return matrix(ordinary_math_fixture::CASES[i].matrices[math::Result::index(offset)]);}
math::Input simple(){math::Input in;in.world=in.view=vr_camera::IDENTITY;in.projection={2,0,0,0,0,3,0,0,0,0,-.0001f,1,0,0,.1f,0};in.width=1920;in.height=1080;in.auxiliary_scale=1;in.depth_remap=.25f;in.jitter={.25f,-.125f};in.jitter_index=19;return in;}
int main(){try{
    for(size_t i=1;i<4;++i){const auto& f=ordinary_math_fixture::CASES[i];const auto& prior=ordinary_math_fixture::CASES[i-1];auto in=simple();
        in.world=captured(i,0x34);in.view=captured(i,0x3dc);in.projection=captured(i,0x49c);for(size_t k=0;k<2;++k)in.jitter[k]=std::bit_cast<float>(f.jitter[k]);in.auxiliary_scale=f.auxiliary_scale;in.depth_remap=f.depth_remap;
        math::Previous p{captured(i-1,0x34),captured(i-1,0x3dc),captured(i-1,0x49c),captured(i-1,0x59c),captured(i-1,0x55c)};for(size_t k=0;k<3;++k)p.delta[k]=std::bit_cast<float>(prior.motion[k]);in.previous=p;
        auto result=math::build(in);require(result.has_value(),"shipping native reference accepts finite owned input");
        for(auto offset:math::Result::OFFSETS){const auto actual=captured(i,offset);const auto tolerance=(offset==0x9b4 || offset==0x9f4)?1e-3f:3e-4f;
            if(!same(result->at(offset),actual,tolerance)){float error{};for(size_t k=0;k<16;++k)error=std::max(error,std::abs(result->at(offset)[k]-actual[k]));std::cerr<<"native reference "<<i<<" offset 0x"<<std::hex<<offset<<std::dec<<" error "<<error<<'\n';throw std::runtime_error("reconstructed native packet differs from actual shipping Scene");}}
        for(size_t k=0;k<3;++k){require(std::abs(result->delta[k]-std::bit_cast<float>(f.motion[k]))<3e-4f,"actual native translation metadata");require(std::abs(result->angles[k]-std::bit_cast<float>(f.motion[k+3]))<1e-5f,"actual native angular metadata (stationary control)");}
        require(result->moved==bool(f.flags[0]) && result->cut==bool(f.flags[1]) && !result->reset,"actual native movement/cut flags");
    }
    auto in=simple();const auto first=math::build(in);require(first && first->reset && first->cut,"first own-eye frame is reset with coherent current-as-previous");require(first->jitter==in.jitter && first->jitter_index==19,"exact supplied native jitter metadata stays with the raster packet");require(same(first->at(0x9b4),vr_camera::IDENTITY,1e-6f),"first native shader motion is identity");
    in.previous=first->previous();in.world[12]=.25f;in.view[12]=-.25f;in.jitter={-.25f,.375f};const auto next=math::build(in);require(next && !next->reset && !next->cut && !next->moved,"small own-eye movement continues");
    require(std::abs(next->at(0x9b4)[3]-.5f)<1e-5f && std::abs(next->at(0x9f4)[3]+.5f)<1e-5f,"literal depth-free shader motion sign and transpose");
    require(next->at(0x934)[8]==-2*.25f/1920 && next->at(0x974)[8]==2*.25f/1920,"current jitter on prior unjittered combined and prior jitter on prior raster remain distinct");
    require(next->at(0x774)[12]!=next->at(0x7f4)[12],"current and previous auxiliary combined state are different inputs");
    in.world[12]=3;in.view[12]=-3;auto jump=math::build(in);require(jump && jump->moved && jump->cut,"native squared movement and delta-change thresholds preserved");
    in.cut=true;auto cut=math::build(in);require(cut && cut->reset && cut->cut && cut->delta==std::array<float,3>{} && same(cut->at(0x9b4),vr_camera::IDENTITY,1e-5f),"explicit cut resets own camera state coherently");
    for(size_t axis=0;axis<3;++axis){auto rotation=vr_camera::IDENTITY;size_t j=(axis+1)%3,k=(j+1)%3;const float angle=.4f;rotation[j*4+j]=rotation[k*4+k]=std::cos(angle);rotation[j*4+k]=std::sin(angle);rotation[k*4+j]=-std::sin(angle);auto result=math::angular(rotation);for(size_t n=0;n<3;++n)require(std::abs(result[n]-(n==axis?angle:0))<1e-6f,"analytic native Euler axis/sign convention");
        rotation[j*4+j]=rotation[k*4+k]=-1;rotation[j*4+k]=rotation[k*4+j]=0;result=math::angular(rotation);for(float v:result)require(std::isfinite(v),"180-degree quaternion trace branches stay finite");}
    auto singular=vr_camera::IDENTITY;singular[0]=singular[5]=0;require(!math::inverse(singular),"singular inverse refuses");
    for(unsigned mutation=0;mutation<11;++mutation){auto bad=simple();switch(mutation){case 0:bad.width=0;break;case 1:bad.height=32769;break;case 2:bad.jitter[0]=.6f;break;case 3:bad.jitter[1]=NAN;break;case 4:bad.projection={};break;case 5:bad.projection[8]=.1f;break;case 6:bad.world[12]=1;break;case 7:bad.auxiliary_scale=0;break;case 8:bad.depth_remap=2;break;case 9:bad.previous=first->previous();bad.previous->delta[0]=NAN;break;default:bad.view[0]=NAN;break;}require(!math::build(bad),"invalid or unsupported native packet inputs refuse");}
    bool refused{};try{first->at(0x5dc);}catch(const std::out_of_range&){refused=true;}require(refused,"unknown native field cannot be synthesized silently");
    std::cout<<"ordinary camera math: native reference and analytic controls PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
