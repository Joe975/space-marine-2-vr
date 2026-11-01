#include "synthetic_code.h"
#include "EngineScreenUi.h"
#include "game_build_ported.h"
#include "EngineCameraProbe.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
unsigned calls{};void* expected{};const int* expected_size{};const int* expected_origin{};
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
__declspec(noinline) void native_layout(void* object,const int* size,const int* origin) {
    require(object==expected && size==expected_size && origin==expected_origin,"forward exact native arguments");
    ++calls;
}
}
int main() {
    try {
        require(EngineScreenUi::matches_build(EngineCameraProbe::BUILD_TIMESTAMP,SYNTH(EngineScreenUi::PROLOGUE)),"known build");
        require(!EngineScreenUi::matches_build(0,SYNTH(EngineScreenUi::PROLOGUE)),"reject changed build");
        require(EngineScreenUi::matches_build(game_build_ported::EXE,game_build_ported::code(EngineScreenUi::LAYOUT_RVA)),"the 2026-10-21 layout function matches");
        auto bytes=SYNTH(EngineScreenUi::PROLOGUE);bytes[0]^=1;
        require(!EngineScreenUi::matches_build(EngineCameraProbe::BUILD_TIMESTAMP,bytes),"reject changed code");
        auto& probe=EngineScreenUi::get();require(probe.install(reinterpret_cast<void*>(&native_layout),0),"real detour installation");
        std::array<uint8_t,0x28> object{};object[1]=1;
        std::array<uintptr_t,0x200/8> vtable{};vtable[0x190/8]=0x1234;
        auto table=vtable.data();auto renderer=&table;std::memcpy(object.data()+0x18,&renderer,sizeof(renderer));
        std::array<int,4> rect{3840,2160,0,0};const auto before=object;
        expected=object.data();expected_size=rect.data();expected_origin=rect.data()+2;
        native_layout(expected,expected_size,expected_origin);
        auto snapshot=probe.snapshot();require(calls==1 && snapshot.calls==1 && snapshot.movies.size()==1,"capture once");
        require(snapshot.movies[0].size==std::array<int,2>{3840,2160} && snapshot.movies[0].origin==std::array<int,2>{0,0},"native size and origin ABI");
        require(snapshot.movies[0].viewport_function==0x1234 && snapshot.movies[0].flags[1]==1,"renderer and screen flags");
        require(object==before && rect==std::array<int,4>{3840,2160,0,0},"no native writes");
        expected_origin=expected_size;native_layout(expected,expected_size,expected_origin);
        require(calls==2 && probe.snapshot().movies[0].calls==2,"aliased input forwarded once");
        expected=nullptr;expected_size=nullptr;expected_origin=nullptr;native_layout(nullptr,nullptr,nullptr);
        require(calls==3 && probe.snapshot().calls==2,"invalid observation still forwards native call");
        std::cout<<"screen UI hook tests passed\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
