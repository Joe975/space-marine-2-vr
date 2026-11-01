#include "synthetic_code.h"
#include "NativePipTonemap.h"
#include "game_build_ported.h"
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <vector>
#include <limits>
namespace {
void require(bool value,const char* why){if(!value) throw std::runtime_error(why);}
unsigned calls{};bool enabled{},throws{};float seen{};const void* seen_pointer{};std::array<uintptr_t,8> arguments{};std::array<uint8_t,2> flags{};
unsigned scene_calls{};int32_t scene_index{};std::array<uintptr_t,4> scene_arguments{};std::optional<int32_t> primary{0};bool nested{};
std::vector<int32_t> scene_indices;
unsigned outer_calls{};float seen_delta{};std::array<uintptr_t,7> outer_arguments{};uint8_t outer_skip{};uintptr_t allowed_entry{};
std::optional<NativePipTonemap::OuterGrading> outer_grading;std::optional<NativePipTonemap::OuterGrading> seen_outer_grading;
__declspec(noinline) void scene(uintptr_t a,uintptr_t b,uintptr_t c,const void* d,int32_t index) {
    ++scene_calls;scene_index=index;scene_indices.push_back(index);scene_arguments={a,b,c,reinterpret_cast<uintptr_t>(d)};
    seen_outer_grading.reset();if(d && reinterpret_cast<uintptr_t>(d)!=44) {NativePipTonemap::OuterGrading copy{};std::memcpy(copy.data(),d,copy.size());seen_outer_grading=copy;}
    volatile auto keep=scene_calls;(void)keep;
}
void invoke_scene(int32_t index) {void(*volatile fn)(uintptr_t,uintptr_t,uintptr_t,const void*,int32_t)=scene;fn(11,22,33,reinterpret_cast<void*>(44),index);}
__declspec(noinline) void core(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uint32_t f,int32_t g,const void* h,uintptr_t i) {
    ++calls;arguments={a,b,c,d,e,f,uintptr_t(g),i};seen_pointer=h;if(h){std::memcpy(&seen,h,4);std::memcpy(flags.data(),static_cast<const uint8_t*>(h)+4,2);}volatile auto keep=calls;(void)keep;
    if(nested) {
        nested=false;const bool previous=enabled;enabled=false;
        void(*volatile fn)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uint32_t,int32_t,const void*,uintptr_t)=core;
        fn(a,b,c,d,e,f,g,h,i);enabled=previous;
    }
    invoke_scene(g);
}
void invoke(const void* descriptor,int32_t index=7){void(*volatile fn)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uint32_t,int32_t,const void*,uintptr_t)=core;fn(1,2,3,4,5,6,index,descriptor,9);}
__declspec(noinline) void outer(uintptr_t a,uintptr_t b,float delta,uintptr_t entry,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h,const void* descriptor) {
    ++outer_calls;seen_delta=delta;outer_arguments={a,b,entry,e,f,g,h};outer_skip=static_cast<const uint8_t*>(descriptor)[5];
    invoke(descriptor,1);volatile auto keep=outer_calls;(void)keep;
}
void invoke_outer(uintptr_t entry,const void* descriptor) {
    void(*volatile fn)(uintptr_t,uintptr_t,float,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,const void*)=outer;
    fn(10,20,.03125f,entry,50,60,70,80,descriptor);
}
}
int main(){try {
    using Tone=NativePipTonemap;
    require(Tone::matches(0x6ac3e716,SYNTH(Tone::BYTES)) && !Tone::matches(0,SYNTH(Tone::BYTES)),"unknown native build rejected");
    auto changed=SYNTH(Tone::BYTES);changed[0]^=1;require(!Tone::matches(0x6ac3e716,changed),"changed HDR prologue rejected");
    require(Tone::matches(game_build_ported::EXE,game_build_ported::code(Tone::RVA)),"the 2026-10-21 HDR function matches");
    std::array<uint8_t,0xa60> entry{};std::array<uint8_t,0x148> renderer{};
    auto world=vr_camera::IDENTITY;world[10]=-1;const auto pointer=reinterpret_cast<uintptr_t>(entry.data());
    std::memcpy(renderer.data()+0x140,&pointer,8);uint64_t view_flags=0x40;std::memcpy(entry.data(),&view_flags,8);
    std::memcpy(entry.data()+0x34,world.data(),64);int32_t record=3;std::memcpy(entry.data()+0xa50,&record,4);
    const std::array<vr_camera::Matrix,1> poses{world};const auto address=reinterpret_cast<uintptr_t>(renderer.data());
    require(Tone::owned(address,3,poses) && !Tone::owned(address,2,poses) && !Tone::owned(1,3,poses),"only matching owned native entry accepted");
    world[12]=5;const std::array<vr_camera::Matrix,1> foreign{world};require(!Tone::owned(address,3,foreign),"foreign pose rejected");
    view_flags=0;std::memcpy(entry.data(),&view_flags,8);require(!Tone::owned(address,3,poses),"main camera cannot select PIP tonemapping");
    std::vector<uint8_t> list(0x2996c);const auto list_address=reinterpret_cast<uintptr_t>(list.data());
    const auto put=[&](size_t offset,const auto& value){std::memcpy(list.data()+offset,&value,sizeof(value));};
    put(0x29968,int32_t{2});put(0x168,uint64_t{0x3f7c013});put(0x168+0x188,int32_t{0});
    put(0x168+0xa60,uint64_t{0xe50043});put(0x168+0xa60+0x188,int32_t{1});
    require(Tone::primary_index(list_address)==0,"unique primary HDR index read from the live two-view list");
    put(0x168+0xa60+0x188,int32_t{0});require(!Tone::primary_index(list_address),"aliased main/PIP HDR indices rejected");
    put(0x168+0xa60+0x188,int32_t{1});put(0x29968,int32_t{3});
    require(Tone::primary_index(list_address)==0,"unrelated auxiliary entry does not hide the unique main and PIP views");
    put(0x168+2*0xa60,uint64_t{0x3f7c013});require(!Tone::primary_index(list_address),"additional main views make grading ownership ambiguous");
    put(0x168+2*0xa60,uint64_t{0xe50043});require(!Tone::primary_index(list_address),"additional PIP views make grading ownership ambiguous");
    put(0x29968,int32_t{65});require(!Tone::primary_index(list_address),"unbounded native view count rejected");
    put(0x29968,int32_t{2});put(0x168,uint64_t{0});require(!Tone::primary_index(list_address),"non-main view cannot supply scene grading");
    require(!Tone::primary_index(1) && !Tone::primary_index(UINTPTR_MAX-1),"unreadable/wrapped native list rejected");
    std::array<uint8_t,0x630> pipeline{};std::array<uint8_t,0xc8> component{};
    const auto component_pointer=reinterpret_cast<uintptr_t>(component.data());std::memcpy(pipeline.data()+0x628,&component_pointer,8);
    const uint32_t mask=2,mode=3;std::memcpy(component.data()+8,&mask,4);
    const std::array<float,4> values{.45f,1.f,1.6f,.436f};std::memcpy(component.data()+0x58,values.data(),16);std::memcpy(component.data()+0x68,&mode,4);
    const auto pipeline_before=pipeline;const auto component_before=component;
    outer_grading=Tone::outer_grading(reinterpret_cast<uintptr_t>(pipeline.data()),0,1.f,.0001f);
    require(outer_grading && pipeline==pipeline_before && component==component_before,"native main descriptor is reconstructed without changing pipeline/component source");
    uint32_t output_mode{};std::array<float,4> output{};std::memcpy(&output_mode,outer_grading->data(),4);std::memcpy(output.data(),outer_grading->data()+4,16);
    require(output_mode==3 && output==std::array<float,4>{.45f,.436f,1.f,1.6f} && (*outer_grading)[20]==0,"native mode, grading fields, ratio and final byte retain actual descriptor layout");
    require(!Tone::outer_grading(reinterpret_cast<uintptr_t>(pipeline.data()),1,1.f,.0001f) && !Tone::outer_grading(1,0,1.f,.0001f) &&
        !Tone::outer_grading(reinterpret_cast<uintptr_t>(pipeline.data()),0,0.f,.0001f),"disabled, unreadable and invalid-scale grading is refused");
    const float nan=std::numeric_limits<float>::quiet_NaN();std::memcpy(component.data()+0x58,&nan,4);
    require(!Tone::outer_grading(reinterpret_cast<uintptr_t>(pipeline.data()),0,1.f,.0001f),"nonfinite main grading is refused");component=component_before;
    auto& tone=Tone::get();require(tone.install(reinterpret_cast<void*>(&core),[]{if(throws) throw std::runtime_error("provider");return enabled;},reinterpret_cast<void*>(&scene),[]{return primary;},reinterpret_cast<void*>(&outer),[](uintptr_t entry){return entry==allowed_entry;},[](uintptr_t pipeline,int32_t index){require(pipeline==10 && index==0,"descriptor callback uses actual outer pipeline and unique main index");return outer_grading;}),"actual HDR, scene helper and outer effects detours installed");
    struct Descriptor{float exposure;uint8_t inherit,skip;uint8_t pad[2];} descriptor{2,1,1,{0x45,0x67}};
    invoke(&descriptor);require(calls==1 && seen_pointer==&descriptor && seen==2 && tone.applied()==0,"disabled/main call is unchanged");
    enabled=true;invoke(&descriptor);
    require(calls==2 && seen==0 && seen_pointer!=&descriptor && descriptor.exposure==2 && flags==std::array<uint8_t,2>{1,1},"scene branch uses a private descriptor and preserves PIP effect exclusions/source");
    require(arguments==std::array<uintptr_t,8>{1,2,3,4,5,6,7,9},"all nine native ABI arguments and original call count preserved");
    descriptor.skip=0;invoke(&descriptor);require(seen_pointer==&descriptor,"unexpected/main descriptor rejected");descriptor.skip=1;
    descriptor.exposure=-1;invoke(&descriptor);require(seen_pointer==&descriptor,"existing scene branch is not modified");
    descriptor.exposure=2;throws=true;invoke(&descriptor);require(calls==5 && seen_pointer==&descriptor && tone.applied()==1,"provider failure preserves original arguments exactly once");
    throws=false;invoke(&descriptor,1);
    require(scene_index==0 && arguments[6]==1 && tone.grading_applied()==1,"only scene helper reads main grading; HDR preparation retains PIP index");
    require(scene_arguments==std::array<uintptr_t,4>{11,22,33,44} && scene_calls==6,"helper original runs once with other four ABI arguments unchanged");
    invoke_scene(1);require(scene_index==1 && tone.grading_applied()==1,"grading override cannot escape owned HDR call scope");
    primary=1;invoke(&descriptor,1);require(scene_index==1 && tone.grading_applied()==1,"same index does not select main grading");
    primary=5;invoke(&descriptor,1);require(scene_index==1 && tone.grading_applied()==1,"invalid primary index is refused");
    primary.reset();invoke(&descriptor,1);require(scene_index==1,"missing primary evidence preserves helper index");
    primary=0;enabled=false;invoke(&descriptor,1);require(scene_index==1 && seen_pointer==&descriptor,"main/disabled HDR call never inherits owned grading scope");
    enabled=true;nested=true;const auto before=scene_indices.size();invoke(&descriptor,1);
    require(scene_indices.size()==before+2 && scene_indices[before]==1 && scene_indices[before+1]==0,"nested foreign HDR call clears the override and restores the outer owned scope");
    allowed_entry=reinterpret_cast<uintptr_t>(entry.data());view_flags=0x40;std::memcpy(entry.data(),&view_flags,8);
    const int32_t index=1;std::memcpy(entry.data()+0x188,&index,4);const auto source=descriptor;
    enabled=true;invoke_outer(allowed_entry,&descriptor);
    require(outer_calls==1 && seen_delta==.03125f && outer_arguments==std::array<uintptr_t,7>{10,20,allowed_entry,50,60,70,80},"outer native floating-point ABI and all other arguments are preserved exactly once");
    require(outer_skip==0 && seen==0 && flags==std::array<uint8_t,2>{1,0} && descriptor.skip==source.skip && descriptor.exposure==source.exposure && descriptor.inherit==source.inherit && tone.effects_applied()==1,
        "owned outer call privately enables native effects and inner scene tonemap while preserving the source descriptor");
    require(seen_outer_grading==outer_grading && tone.outer_grading_applied()==1 && scene_index==0 && arguments[6]==1 && arguments[7]==9,
        "only owned scene read/bind receives main outer grading; core PIP index and source descriptor argument remain unchanged");
    invoke_scene(1);require(scene_arguments[3]==44 && !seen_outer_grading && scene_index==1,"outer grading descriptor cannot escape its native owned scope");
    invoke(&descriptor,1);descriptor.skip=0;invoke(&descriptor,1);require(seen_pointer==&descriptor,"outer effects scope cannot escape into a later unrelated core call");descriptor.skip=1;
    view_flags=0;std::memcpy(entry.data(),&view_flags,8);invoke_outer(allowed_entry,&descriptor);
    require(outer_skip==1 && tone.effects_applied()==1,"main camera never enables PIP scene effects");
    view_flags=0x40;std::memcpy(entry.data(),&view_flags,8);const int32_t wrong_index=0;std::memcpy(entry.data()+0x188,&wrong_index,4);invoke_outer(allowed_entry,&descriptor);
    require(outer_skip==1 && tone.effects_applied()==1,"aliased primary HDR index refuses additional effects");
    std::memcpy(entry.data()+0x188,&index,4);allowed_entry=0;invoke_outer(reinterpret_cast<uintptr_t>(entry.data()),&descriptor);
    require(outer_skip==1 && tone.effects_applied()==1,"foreign physical entry cannot enable effects despite matching flags");
    std::cout<<"native PIP tonemap tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}}
