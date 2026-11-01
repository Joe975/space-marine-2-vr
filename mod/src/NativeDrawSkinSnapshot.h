#pragma once
#include "NativeCommandBindingIdentity.h"
#include "GameBuild.h"
#include "NativeObjectConstantsSnapshot.h"
#include <cmath>

// Fixed read-only temporal receipt. Raw CPU cells do not authenticate GPU use.
namespace native_draw_skin {
enum Mask:uint32_t {not_requested=1,unqualified=2,unsupported=4,unreadable=8,bounds=16,changed=32,nonfinite=64};
struct Entry {uint16_t id{};bool active{};std::array<float,16> c0{};std::array<float,12> palette{},staging{};std::array<float,8> dual{},staging_dual{};};
struct Sample {
    uint32_t mask{not_requested},reads{},dirty{},instance_flags{};int32_t logical_count{},subset_count{};bool dual_enabled{},subset_enabled{};
    uintptr_t instance{},resource{},skin{},c0{},container{},matrix{},dq{},component{},record{},cpu_stream{};uint64_t row{};
    uint32_t matrix_offset{},dual_offset{};std::array<Entry,3> entries{{{114},{118},{120}}};
};
struct Evidence {Sample before,after;native_object_constants::Sample object_before,object_after;bool post_observed{};};
struct Failure{uint32_t mask;};
constexpr game_build::Code PUBLISHER=game_build::hashed(0x1e778f0,16,"84c9ead5e77673cbebd96f7ddc2083d6c4413c3adadf27bd5c47ef21f53de41d");
constexpr game_build::Code PRODUCER=game_build::hashed(0x1ed7ba0,16,"a47db1464e6379f0faf17e02be87c6facade726e6b4149bccf8a43bc4a0267e7");

template<class Reader> Sample capture(uintptr_t descriptor,const native_geometry_identity::Evidence& owner,const native_command_binding::Image& image,Reader&& reader)noexcept{
    native_command_binding::PreserveError preserve;Sample s;
    if(!native_command_binding::player(owner) || !owner.palette_read || !owner.palette_stable){s.mask=unqualified;return s;}
    if(!image.contains(image.base+game_build::rva(0x4d47f3c),4)){s.mask=unsupported;return s;}
    try{
        struct Cell{uintptr_t p{};size_t n{};std::array<uint8_t,512> bytes{};};
        std::array<Cell,96> cells{};size_t used{},bytes{};
        auto raw=[&](uintptr_t p,void* target,size_t n){if(s.reads>=256 || !n || n>512 || n>32768-bytes || p<0x10000 || p>0x800000000000ULL-n || p>UINTPTR_MAX-n)return false;++s.reads;bytes+=n;return reader(p,target,n);};
        auto read=[&](uintptr_t p,void* target,size_t n){if(used==cells.size() || !raw(p,target,n))throw Failure{unreadable};auto& c=cells[used++];c.p=p;c.n=n;std::memcpy(c.bytes.data(),target,n);};
        auto q=[&](uintptr_t p){uintptr_t v{};read(p,&v,8);return v;};
        auto ptr=[&](uintptr_t p){auto v=q(p);if(v<0x10000 || v>=0x800000000000ULL)throw Failure{bounds};return v;};
        auto i32=[&](uintptr_t p){int32_t v{};read(p,&v,4);return v;};
        auto u32=[&](uintptr_t p){return uint32_t(i32(p));};
        const auto same_owner=[&]{const auto current=native_geometry_identity::capture(descriptor,raw);return native_command_binding::same(owner,current) && current.palette_read && current.palette_stable && current.palette_pointer==owner.palette_pointer;};
        if(!same_owner())throw Failure{unqualified};
        std::array<uint8_t,16> code{};read(image.base+game_build::rva(0x1e778f0),code.data(),16);if(!game_build::code(0x1e778f0,code,PUBLISHER))throw Failure{unsupported};
        read(image.base+game_build::rva(0x1ed7ba0),code.data(),16);if(!game_build::code(0x1ed7ba0,code,PRODUCER))throw Failure{unsupported};
        s.instance=owner.instance;
        if(q(s.instance+0xd0)!=owner.palette_pointer)throw Failure{changed};
        s.resource=ptr(s.instance+0x250);s.skin=ptr(s.resource+0x28);s.logical_count=i32(s.skin+0x58);
        if(s.logical_count<=120 || s.logical_count>4096)throw Failure{bounds};
        s.dirty=u32(s.instance+0x2f0);s.instance_flags=u32(s.instance+0x30);s.dual_enabled=bool(s.instance_flags&0x200);
        s.c0=ptr(s.instance+0xc0);s.container=ptr(s.instance+0x210);s.matrix=ptr(s.container);
        if(i32(s.container+8)!=s.logical_count)throw Failure{bounds};
        if(s.dual_enabled){s.dq=ptr(s.container+0x10);if(i32(s.container+0x18)!=s.logical_count)throw Failure{bounds};}
        s.subset_enabled=i32(s.instance+0x240)!=0;s.subset_count=s.subset_enabled?i32(s.instance+0x248):0;
        std::array<uint16_t,256> subset{};
        if(s.subset_enabled){if(s.subset_count<0 || s.subset_count>256 || s.subset_count>s.logical_count)throw Failure{bounds};if(s.subset_count)read(ptr(s.instance+0x238),subset.data(),s.subset_count*2);}
        const auto alternate=q(s.instance+0x2c0);if(alternate && q(alternate))throw Failure{unsupported};
        const auto manager=ptr(s.instance+0x280);const auto index=i32(image.base+game_build::rva(0x4d47f3c)),count=i32(manager+0x718);
        if(index<0 || index>=count || count>4096)throw Failure{bounds};
        const auto table=ptr(manager+0x710);s.component=ptr(table+uint64_t(index)*16+8);
        const auto resources=ptr(s.component+0x82330);const auto resource_index=i32(s.resource+0x2c4);int16_t instance_index{};read(s.instance+0x42,&instance_index,2);
        if(resource_index<0 || resource_index>=4096 || instance_index<0 || instance_index>=4096)throw Failure{bounds};
        s.record=ptr(resources+uint64_t(resource_index)*8);const auto a=i32(s.record+0xc),b=i32(s.record+0x10),stride=i32(s.record+0x7c);int16_t enabled{};read(s.record+0xc0,&enabled,2);
        if(a<0 || a>4096 || b<0 || b>4096 || stride<=0 || stride>65536 || enabled<=0)throw Failure{bounds};
        s.row=uint64_t(a+b+instance_index)*stride;s.cpu_stream=ptr(s.record+0x30);s.matrix_offset=u32(s.record+0x40);s.dual_offset=u32(s.record+0x48);
        if((s.row+s.matrix_offset)*16+uint64_t(s.logical_count)*48>64*1024*1024ULL || (s.row+s.dual_offset)*16+uint64_t(s.logical_count)*32>64*1024*1024ULL)throw Failure{bounds};
        const auto floats=[&](uintptr_t p,auto& values){read(p,values.data(),sizeof(values));for(float v:values)if(!std::isfinite(v) || std::abs(v)>1000000)throw Failure{nonfinite};};
        for(auto& e:s.entries){e.active=!s.subset_enabled || std::find(subset.begin(),subset.begin()+s.subset_count,e.id)!=subset.begin()+s.subset_count;
            floats(s.c0+uint64_t(e.id)*64,e.c0);floats(s.matrix+uint64_t(e.id)*48,e.palette);floats(s.cpu_stream+(s.row+s.matrix_offset)*16+uint64_t(e.id)*48,e.staging);
            if(s.dual_enabled){floats(s.dq+uint64_t(e.id)*32,e.dual);floats(s.cpu_stream+(s.row+s.dual_offset)*16+uint64_t(e.id)*32,e.staging_dual);}}
        for(size_t j=0;j<used;++j){std::array<uint8_t,512> again{};const auto& c=cells[j];if(!raw(c.p,again.data(),c.n))throw Failure{unreadable};if(std::memcmp(again.data(),c.bytes.data(),c.n))throw Failure{changed};}
        if(!same_owner())throw Failure{changed};
        s.mask=0;return s;
    }catch(const Failure& f){s.mask=f.mask;return s;}catch(...){s.mask=unreadable;return s;}
}

inline void write_sample(std::ostream& out,const Sample& s){
    const auto old_precision=out.precision(9);struct Restore{std::ostream& out;std::streamsize precision;~Restore(){out.precision(precision);}} restore{out,old_precision};
    out<<"{\"mask\":"<<s.mask<<",\"reads\":"<<s.reads<<",\"dirty_flags\":"<<s.dirty<<",\"instance_flags\":"<<s.instance_flags<<",\"instance\":"<<s.instance<<",\"resource\":"<<s.resource<<",\"skin_resource\":"<<s.skin
       <<",\"logical_count\":"<<s.logical_count<<",\"subset_enabled\":"<<(s.subset_enabled?"true":"false")<<",\"subset_count\":"<<s.subset_count<<",\"dual_enabled\":"<<(s.dual_enabled?"true":"false")
       <<",\"container\":"<<s.container<<",\"c0\":"<<s.c0<<",\"matrix\":"<<s.matrix<<",\"dq\":"<<s.dq<<",\"component\":"<<s.component<<",\"record\":"<<s.record<<",\"cpu_stream\":"<<s.cpu_stream
       <<",\"row_float4\":"<<s.row<<",\"matrix_float4_offset\":"<<s.matrix_offset<<",\"dual_float4_offset\":"<<s.dual_offset<<",\"entries\":[";
    if(!s.mask)for(size_t i=0;i<s.entries.size();++i){if(i)out<<',';const auto& e=s.entries[i];out<<"{\"logical_id\":"<<e.id<<",\"active_subset\":"<<(e.active?"true":"false");
        const auto values=[&](const char* name,const auto& data){out<<",\""<<name<<"\":[";for(size_t j=0;j<data.size();++j){if(j)out<<',';out<<data[j];}out<<']';};
        values("c0_raw",e.c0);values("palette_transposed_3x4",e.palette);values("staging_transposed_3x4",e.staging);if(s.dual_enabled){values("dual_raw",e.dual);values("staging_dual_raw",e.staging_dual);}out<<'}';}
    out<<"],\"c0_validated\":false,\"native_table_capacity_verified\":false}";
}
inline void write(std::ostream& out,const Evidence& e){out<<"{\"before\":";write_sample(out,e.before);out<<",\"post_observed\":"<<(e.post_observed?"true":"false")<<",\"after\":";write_sample(out,e.after);out<<",\"object_constants\":{\"before\":";native_object_constants::write(out,e.object_before);out<<",\"after\":";native_object_constants::write(out,e.object_after);out<<"},\"atomic\":false,\"gpu_memory_read\":false,\"gpu_source_verified\":false,\"c0_evaluation_forced\":false,\"native_calls\":false}";}
}
