#pragma once
#include "NativeCommandBindingIdentity.h"
#include "GameBuild.h"
#include "NativeObjectConstantGetterProbe.h"

// CPU counterpart of the object's actual +110 CB record, not a shader-use proof.
namespace native_object_constants {
enum Mask:uint32_t {not_requested=1,unqualified=2,unsupported=4,unreadable=8,bounds=16,changed=32,gpu_base_unmatched=64,getter_unavailable=128};
struct Sample {
    uint32_t mask{not_requested},reads{},offset{},encoded_count{},chunk_base{};uint64_t gpu_base{};
    uintptr_t device{},stream{},table{},chunk{},cpu_base{},cpu_address{};int32_t threads{},frame{},chunks{},current_chunk{},selected_chunk{-1},capacity_float4{};
    uint8_t producer_thread{};int16_t native_instance_index{};std::array<uint32_t,36> words{};
    NativeObjectConstantGetterProbe::Locator getter;
};
constexpr game_build::Code ALLOCATOR=game_build::hashed(0x2082760,16,"521127e5da8740216485ec63529cd34a118617d2ee43157dea1560e2a05cd3f0");
constexpr game_build::Code OBJECT_PRODUCER=game_build::hashed(0x1f7bcc0,16,"09e934870a1afd978990efc323a7b296454a3b1476291cd4249f46a065d23312");
constexpr game_build::Code OBJECT_FILL=game_build::hashed(0x1f7c980,16,"a62576bb795bb54d543f7766f0f631f886cd602cf35bfacc4152ba0d022b37fa");
template<class Reader> Sample capture(uintptr_t descriptor,const native_geometry_identity::Evidence& owner,const native_command_binding::Image& image,Reader&& reader)noexcept{
    native_command_binding::PreserveError preserve;Sample s;
    if(descriptor<0x10000 || descriptor>0x800000000000ULL-0x200 || !native_command_binding::player(owner)){s.mask=unqualified;return s;}
    if(!image.contains(image.base+game_build::rva(0x4ff0c08),8)){s.mask=unsupported;return s;}
    struct Failure{uint32_t mask;};
    try{
        struct Cell{uintptr_t p{};size_t n{};std::array<uint8_t,144> bytes{};};std::array<Cell,96> cells{};size_t used{},bytes{};
        auto raw=[&](uintptr_t p,void* target,size_t n){if(s.reads>=256 || !n || n>144 || n>16384-bytes || p<0x10000 || p>0x800000000000ULL-n || p>UINTPTR_MAX-n)return false;++s.reads;bytes+=n;return reader(p,target,n);};
        auto read=[&](uintptr_t p,void* target,size_t n){if(used==cells.size() || !raw(p,target,n))throw Failure{unreadable};auto& c=cells[used++];c.p=p;c.n=n;std::memcpy(c.bytes.data(),target,n);};
        auto q=[&](uintptr_t p){uintptr_t v{};read(p,&v,8);return v;};
        auto ptr=[&](uintptr_t p){auto v=q(p);if(v<0x10000 || v>=0x800000000000ULL)throw Failure{bounds};return v;};
        auto i32=[&](uintptr_t p){int32_t v{};read(p,&v,4);return v;};
        const auto same=[&]{return native_command_binding::same(owner,native_geometry_identity::capture(descriptor,raw));};
        if(!same())throw Failure{unqualified};
        const auto signature=[&](uintptr_t reference,const auto& expected){const auto rva=game_build::rva(reference);if(!rva || !image.contains(image.base+rva,16))throw Failure{unsupported};std::array<uint8_t,16> code{};read(image.base+rva,code.data(),16);if(!game_build::code(reference,code,expected))throw Failure{unsupported};};
        signature(0x2082760,ALLOCATOR);signature(0x1f7bcc0,OBJECT_PRODUCER);signature(0x1f7c980,OBJECT_FILL);
        std::array<uint8_t,16> record{};read(descriptor+0x110,record.data(),16);std::memcpy(&s.gpu_base,record.data(),8);std::memcpy(&s.offset,record.data()+8,4);std::memcpy(&s.encoded_count,record.data()+12,4);
        const auto count=s.encoded_count&0x0fffffff;
        if(!(s.encoded_count&0x80000000) || count<9 || count>4096 || s.offset>0x400000)throw Failure{bounds};
        read(descriptor+0x1f3,&s.producer_thread,1);read(owner.instance+0x42,&s.native_instance_index,2);
        if(s.native_instance_index<0 || s.native_instance_index>=4096)throw Failure{bounds};
        s.device=ptr(image.base+game_build::rva(0x4ff0c08));s.threads=i32(s.device+0x180);
        if(s.threads<1 || s.threads>64 || s.producer_thread>=s.threads)throw Failure{bounds};
        s.stream=ptr(s.device+0x398+uint64_t(s.producer_thread)*8);
        const auto fallback=q(s.stream+0x28);
        if(fallback){
            s.getter=NativeObjectConstantGetterProbe::get().lookup(s.stream,fallback,s.gpu_base,s.offset,count,owner,raw);
            if(s.getter.refusal)throw Failure{getter_unavailable};
            s.chunk=s.getter.chunk;s.cpu_base=s.getter.cpu;s.cpu_address=s.cpu_base+uint64_t(s.offset)*16;s.frame=int32_t(s.getter.frame);s.selected_chunk=int32_t(s.getter.slot);s.capacity_float4=0x40000;
        }else{
        s.table=ptr(s.stream);s.chunks=i32(s.stream+8);s.frame=i32(s.stream+0x18);s.current_chunk=i32(s.stream+0x1c);s.capacity_float4=i32(s.stream+0x14);
        if(s.chunks<1 || s.chunks>32 || s.frame<0 || s.frame>1 || s.current_chunk<0 || s.current_chunk>=s.chunks || s.capacity_float4<9 || s.capacity_float4>0x400000 || uint64_t(s.offset)+count>uint32_t(s.capacity_float4))throw Failure{bounds};
        for(int32_t i=0;i<=s.current_chunk;++i){const auto chunk=ptr(s.table+(uint64_t(s.frame)+uint64_t(i)*2)*8);const auto base=q(chunk+0x20);
            if(base==s.gpu_base){if(s.selected_chunk!=-1)throw Failure{gpu_base_unmatched};s.chunk=chunk;s.selected_chunk=i;}}
        if(s.selected_chunk<0)throw Failure{gpu_base_unmatched};
        s.cpu_base=ptr(s.chunk+0x10);const auto chunk_base=i32(s.chunk+0x28);
        if(chunk_base<0 || uint64_t(chunk_base)+s.offset+count>0x400000)throw Failure{bounds};s.chunk_base=uint32_t(chunk_base);
        const auto byte_offset=(uint64_t(s.chunk_base)+s.offset)*16;
        if(s.cpu_base>UINTPTR_MAX-byte_offset)throw Failure{bounds};s.cpu_address=s.cpu_base+byte_offset;
        }
        read(s.cpu_address,s.words.data(),sizeof(s.words));
        for(size_t j=0;j<used;++j){const auto& c=cells[j];std::array<uint8_t,144> again{};if(!raw(c.p,again.data(),c.n))throw Failure{unreadable};if(std::memcmp(again.data(),c.bytes.data(),c.n))throw Failure{changed};}
        if(fallback){
            const auto after=NativeObjectConstantGetterProbe::get().lookup(s.stream,fallback,s.gpu_base,s.offset,count,owner,raw);
            if(after.refusal || after.generation!=s.getter.generation || after.sequence!=s.getter.sequence || after.chunk!=s.getter.chunk ||
               after.resource!=s.getter.resource || after.cpu!=s.getter.cpu || after.gpu!=s.getter.gpu || after.frame!=s.getter.frame || after.slot!=s.getter.slot)throw Failure{changed};
        }
        if(!same())throw Failure{changed};s.mask=0;return s;
    }catch(const Failure& f){s.mask=f.mask;return s;}catch(...){s.mask=unreadable;return s;}
}
inline void write(std::ostream& out,const Sample& s){
    out<<"{\"mask\":"<<s.mask<<",\"reads\":"<<s.reads<<",\"gpu_base\":"<<s.gpu_base<<",\"offset_float4\":"<<s.offset<<",\"encoded_count\":"<<s.encoded_count<<",\"producer_thread\":"<<unsigned(s.producer_thread)
       <<",\"device\":"<<s.device<<",\"thread_count\":"<<s.threads<<",\"stream\":"<<s.stream<<",\"chunk_table\":"<<s.table<<",\"chunks\":"<<s.chunks<<",\"frame\":"<<s.frame<<",\"current_chunk\":"<<s.current_chunk<<",\"selected_chunk\":"<<s.selected_chunk
       <<",\"capacity_float4\":"<<s.capacity_float4<<",\"chunk\":"<<s.chunk<<",\"chunk_base_float4\":"<<s.chunk_base<<",\"cpu_base\":"<<s.cpu_base<<",\"cpu_address\":"<<s.cpu_address<<",\"native_instance_index\":"<<s.native_instance_index<<",\"words\":[";
    if(!s.mask)for(size_t i=0;i<s.words.size();++i){if(i)out<<',';out<<s.words[i];}
    out<<"],\"getter_observation\":";NativeObjectConstantGetterProbe::write(out,s.getter);
    out<<",\"word2_semantic\":\"draw_entry_ordinal_within_logical_mesh_span\",\"native_instance_index_semantic\":\"resource_instance_row_local_index\",\"index_domains_independent\":true,\"native_allocation_capacity_verified\":false,\"shader_consumption_verified\":false,\"gpu_memory_read\":false,\"native_calls\":false,\"atomic\":false}";
}
}
