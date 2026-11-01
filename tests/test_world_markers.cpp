#include "synthetic_code.h"
#include "EngineWorldMarkers.h"
#include "game_build_ported.h"
#include "VrWorldMarker.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
unsigned calls{};
void* seen_object{};
const float* seen_input{};
float* seen_output{};
__declspec(noinline) void native_view(void* object,const float* input,float* output) {
    ++calls;seen_object=object;seen_input=input;seen_output=output;
    uintptr_t camera{};
    if(object) std::memcpy(&camera,static_cast<const uint8_t*>(object)+0x20,sizeof(camera));
    if(!camera) return;
    const std::array<float,3> point{input[0],input[1],input[2]};
    for(int i=0;i<3;++i) output[i]=point[i]+float(i+10);
}
using Fn=void(*)(void*,const float*,float*);
__declspec(noinline) void invoke(Fn volatile fn,void* object,const float* input,float* output) { fn(object,input,output); }
__declspec(noinline) void native_other(void*,const float* input,float* output) {for(int i=0;i<3;++i) output[i]=input[i]+20;}
}
int main() {
    try {
        auto& probe=EngineWorldMarkers::get();
        const vr_projection::Tangents lens{-1.2f,0.8f,-0.7f,1.1f};
        vr_camera::Matrix projection{};require(vr_projection::build(lens,0.1f,1000,projection),"marker fixture eye projection builds");
        const std::array<float,4> rectangle{-0.15f,-0.08f,0.3f,0.16f};
        auto view=vr_camera::IDENTITY;view[12]=0.032f;view[13]=-1.7f;
        for(const auto& point:{vr_marker::Point{0,1.7f,2},vr_marker::Point{3,2,4},vr_marker::Point{-3,-2,4},vr_marker::Point{1,2,-4}}) {
            vr_marker::Point encoded{};
            require(vr_marker::reproject(point,view,lens,0.1f,rectangle,encoded),"finite world marker must reproject including offscreen/behind rays");
            const float x=point[0]+view[12],y=point[1]+view[13],z=point[2];
            const float native_x=2*(encoded[0]*0.1f/encoded[2]-rectangle[0])/rectangle[2]-1;
            const float native_y=2*(encoded[1]*0.1f/encoded[2]-rectangle[1])/rectangle[3]-1;
            require(std::abs(native_x-(x*projection[0]+z*projection[8])/z)<1e-5f &&
                std::abs(native_y-(y*projection[5]+z*projection[9])/z)<1e-5f,"native UI projection must match independently built D3D asymmetric eye projection on both axes");
            require(encoded[2]==z,"native marker front/back/depth rules must receive true eye-space depth");
        }
        vr_marker::Point sentinel{9,8,7};const auto before=sentinel;
        require(!vr_marker::reproject({0,0,0},view,lens,0.1f,rectangle,sentinel) && sentinel==before,"zero-depth marker must preserve caller output");
        require(!vr_marker::reproject({NAN,0,1},view,lens,0.1f,rectangle,sentinel) &&
            !vr_marker::reproject({0,0,1},view,lens,0,rectangle,sentinel),"invalid point/near plane must be refused");
        require(EngineWorldMarkers::matches_build(EngineCameraProbe::BUILD_TIMESTAMP,SYNTH(EngineWorldMarkers::PROLOGUE)),"verified marker function must match");
        auto bad=std::array<uint8_t,EngineWorldMarkers::PROLOGUE.size()>{};
        require(!EngineWorldMarkers::matches_build(0,SYNTH(EngineWorldMarkers::PROLOGUE)) && !EngineWorldMarkers::matches_build(EngineCameraProbe::BUILD_TIMESTAMP,bad),"different builds must be refused");
        require(EngineWorldMarkers::matches_build(game_build_ported::EXE,game_build_ported::code(EngineWorldMarkers::WORLD_VIEW_RVA)),"the 2026-10-21 marker function matches");
        require(probe.install(reinterpret_cast<void*>(&native_view),reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),"actual native detour must install");
        std::array<uint8_t,EngineCameraProbe::SOURCE_CAMERA_SIZE> camera{};
        std::array<uint8_t,0x28> object{};
        auto pointer=reinterpret_cast<uintptr_t>(camera.data());
        std::memcpy(object.data()+0x20,&pointer,sizeof(pointer));
        const auto saved_camera=camera;
        const auto saved_object=object;
        std::array<float,3> world{1,2,3};
        std::array<float,5> out{99,0,0,0,98};
        invoke(&native_view,object.data(),world.data(),out.data()+1);
        require(calls==1 && seen_object==object.data() && seen_input==world.data() && seen_output==out.data()+1,"native method must receive all original arguments exactly once");
        require(out==std::array<float,5>{99,11,13,15,98},"passive result and guard words must survive unchanged");
        require(camera==saved_camera && object==saved_object,"borrowed game/UI cameras must not be written");
        invoke(&native_view,object.data(),world.data(),world.data());
        const auto snapshot=probe.snapshot();
        require(world==std::array<float,3>{11,13,15} && snapshot.callers.back().world==std::array<float,3>{1,2,3},"aliased input must be captured before native write");
        object={};world={7,8,9};
        invoke(&native_view,object.data(),world.data(),world.data());
        require(calls==3 && world==std::array<float,3>{7,8,9},"missing UI camera must retain native no-op semantics");
        std::array<void*,9> bad_targets{};bad_targets.fill(reinterpret_cast<void*>(&native_view));bad_targets[0]=reinterpret_cast<void*>(&native_other);
        require(!probe.install_readers(bad_targets),"partial getter installation must fail when a later target is already owned");
        probe.enable(true);
        require(!probe.snapshot().enabled && !probe.snapshot().readers_installed,"failed getter installation must stay disabled despite enable requests");
        invoke(&native_other,nullptr,world.data(),out.data()+1);
        require(out==std::array<float,5>{99,27,28,29,98},"partially created getter hook must roll back while preserving unrelated native code");
        std::cout<<"world marker native forwarding passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
