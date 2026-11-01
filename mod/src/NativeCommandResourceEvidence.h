#pragma once
#include "NativeCommandBindingIdentity.h"
#include "GameBuild.h"

namespace native_command_resources {
enum Mask:uint32_t {not_requested=1,unsupported=2,unreadable=4,changed=8,out_of_bound=16,owner_changed=32};
struct Resource {
    uint32_t mask{not_requested},slot{},stage{},offset_float4{},encoded_count{},expected_bytes{},cached_bytes{};
    uintptr_t descriptor_offset{},cache_offset{};uint64_t gpu_base{},expected_gpu_va{},cached_gpu_va{};
    bool native_bind_condition{},post_read{},cache_matches{};
    std::array<uint8_t,16> raw{};
};
struct Evidence {uint32_t mask{not_requested},post_mask{not_requested},reads{};bool post_observed{};std::array<Resource,2> records;};
constexpr game_build::Code BINDER_SIGNATURE=game_build::hashed(0x2141f10,16,"2022caeaa557bd2b9ec7e5dac0a44361493fce7f404aff87167167d794b7d7b8");
template<class Reader> Evidence before(uintptr_t descriptor,const native_geometry_identity::Evidence& expected_owner,const native_command_binding::Evidence& binding,const native_command_binding::Image& image,Reader&& reader)noexcept{
    native_command_binding::PreserveError error;Evidence e;
    if(binding.mask || !binding.owner_stable || !binding.pointers_stable || binding.callback_rva!=game_build::rva(0x2141f10) || !image.contains(image.base+game_build::rva(0x36d0cb0),8)){e.mask=unsupported;return e;}
    try{
        auto read=[&](uintptr_t p,void* target,size_t n){++e.reads;return p>=0x10000 && p<0x800000000000ULL && n<=512 && p<=UINTPTR_MAX-n && reader(p,target,n);};
        if(!native_command_binding::same(expected_owner,native_geometry_identity::capture(descriptor,read))){e.mask=owner_changed;return e;}
        std::array<uint8_t,16> signature{};
        const std::array<std::array<uint32_t,2>,2> expected{{{5,2},{8,2}}};
        std::array<std::array<uint32_t,2>,2> slots{};
        if(!read(binding.binding_callback,signature.data(),signature.size()) || !read(image.base+game_build::rva(0x36d0c98),slots[0].data(),8) || !read(image.base+game_build::rva(0x36d0cb0),slots[1].data(),8)){e.mask=unreadable;return e;}
        if(!game_build::code(0x2141f10,signature,BINDER_SIGNATURE) || slots!=expected){e.mask=unsupported;return e;}
        for(size_t i=0;i<2;++i){auto& r=e.records[i];r.slot=slots[i][0];r.stage=slots[i][1];r.descriptor_offset=i?0x138:0x110;r.cache_offset=(r.slot+0x11af+r.stage*15)*16;
            if(descriptor>UINTPTR_MAX-r.descriptor_offset-r.raw.size() || !read(descriptor+r.descriptor_offset,r.raw.data(),r.raw.size())){r.mask=unreadable;continue;}
            std::memcpy(&r.gpu_base,r.raw.data(),8);std::memcpy(&r.offset_float4,r.raw.data()+8,4);std::memcpy(&r.encoded_count,r.raw.data()+12,4);
            r.native_bind_condition=int32_t(r.encoded_count)<0;
            // Reproduce the native uint32 multiply/alignment, retaining flags.
            const uint32_t effective_count=r.encoded_count&0x0fffffff;
            if(r.offset_float4>0x1000000 || effective_count>0x100000 || r.gpu_base>UINT64_MAX-uint64_t(r.offset_float4)*16){r.mask=out_of_bound;continue;}
            r.expected_gpu_va=r.gpu_base+uint64_t(r.offset_float4)*16;r.expected_bytes=(r.encoded_count*uint32_t(16)+255u)&0xffffff00u;
            r.mask=0;
        }
        for(const auto& r:e.records)if(!r.mask){std::array<uint8_t,16> again{};if(!read(descriptor+r.descriptor_offset,again.data(),again.size()) || again!=r.raw){e.mask=changed;return e;}}
        std::array<uint8_t,16> signature_after{};std::array<std::array<uint32_t,2>,2> slots_after{};
        if(!read(binding.binding_callback,signature_after.data(),signature_after.size()) || !read(image.base+game_build::rva(0x36d0c98),slots_after[0].data(),8) || !read(image.base+game_build::rva(0x36d0cb0),slots_after[1].data(),8)){e.mask=unreadable;return e;}
        if(signature!=signature_after || slots!=slots_after){e.mask=changed;return e;}
        if(!native_command_binding::same(expected_owner,native_geometry_identity::capture(descriptor,read))){e.mask=owner_changed;return e;}
        e.mask=0;return e;
    }catch(...){e.mask=unreadable;return e;}
}
template<class Reader> void after(Evidence& e,uintptr_t command,uintptr_t descriptor,const native_geometry_identity::Evidence& expected,
    const native_command_binding::Evidence& binding,const native_command_binding::Image& image,Reader&& reader)noexcept{
    native_command_binding::PreserveError error;
    if(e.mask || e.post_observed)return;e.post_observed=true;
    try{
        const auto current=native_command_binding::capture(command,descriptor,expected,image,reader);
        e.reads+=current.reads;
        if(current.mask || current.vtable!=binding.vtable || current.binding_callback!=binding.binding_callback){e.post_mask=owner_changed;return;}
        auto read=[&](uintptr_t p,void* target,size_t n){++e.reads;return p>=0x10000 && p<0x800000000000ULL && n<=512 && p<=UINTPTR_MAX-n && reader(p,target,n);};
        const auto supported=[&]{std::array<uint8_t,16> code{};std::array<uint32_t,2> object_slot{},material_slot{};
            return read(binding.binding_callback,code.data(),code.size()) && read(image.base+game_build::rva(0x36d0c98),object_slot.data(),8) && read(image.base+game_build::rva(0x36d0cb0),material_slot.data(),8) &&
                game_build::code(0x2141f10,code,BINDER_SIGNATURE) && object_slot==std::array<uint32_t,2>{5,2} && material_slot==std::array<uint32_t,2>{8,2};};
        if(!supported()){e.post_mask=changed;return;}
        for(auto& r:e.records){if(r.mask)continue;
            std::array<uint8_t,16> raw{};
            if(!read(descriptor+r.descriptor_offset,raw.data(),raw.size()) || raw!=r.raw){r.mask=changed;continue;}
            if(command>UINTPTR_MAX-r.cache_offset-12 || !read(command+r.cache_offset,&r.cached_gpu_va,8) || !read(command+r.cache_offset+8,&r.cached_bytes,4)){r.mask=unreadable;continue;}
            uint64_t again_va{};uint32_t again_bytes{};std::array<uint8_t,16> after_raw{};
            if(!read(command+r.cache_offset,&again_va,8) || !read(command+r.cache_offset+8,&again_bytes,4) || !read(descriptor+r.descriptor_offset,after_raw.data(),after_raw.size()) ||
               again_va!=r.cached_gpu_va || again_bytes!=r.cached_bytes || after_raw!=r.raw){r.mask=changed;continue;}
            r.post_read=true;r.cache_matches=r.native_bind_condition && r.cached_gpu_va==r.expected_gpu_va && r.cached_bytes==r.expected_bytes;
        }
        const auto final_binding=native_command_binding::capture(command,descriptor,expected,image,reader);
        e.reads+=final_binding.reads;
        if(final_binding.mask || final_binding.vtable!=binding.vtable || final_binding.binding_callback!=binding.binding_callback){e.post_mask=owner_changed;return;}
        if(!supported()){e.post_mask=changed;return;}
        e.post_mask=0;
    }catch(...){e.post_mask=unreadable;}
}
inline void write(std::ostream& out,const Evidence& e){
    out<<"{\"mask\":"<<e.mask<<",\"post_mask\":"<<e.post_mask<<",\"post_observed\":"<<(e.post_observed?"true":"false")<<",\"reads\":"<<e.reads<<",\"records\":[";
    for(size_t i=0;i<e.records.size();++i){if(i)out<<',';const auto& r=e.records[i];
        out<<"{\"mask\":"<<r.mask<<",\"descriptor_offset\":"<<r.descriptor_offset<<",\"slot\":"<<r.slot<<",\"stage\":"<<r.stage<<",\"cache_offset\":"<<r.cache_offset
           <<",\"gpu_base\":"<<r.gpu_base<<",\"offset_float4\":"<<r.offset_float4<<",\"encoded_count\":"<<r.encoded_count<<",\"native_bind_condition\":"<<(r.native_bind_condition?"true":"false")
           <<",\"expected_gpu_va\":"<<r.expected_gpu_va<<",\"expected_bytes\":"<<r.expected_bytes<<",\"post_read\":"<<(r.post_read?"true":"false")
           <<",\"cached_gpu_va\":"<<r.cached_gpu_va<<",\"cached_bytes\":"<<r.cached_bytes<<",\"cache_matches\":"<<(r.cache_matches?"true":"false")<<'}';
    }
    out<<"],\"gpu_memory_read\":false,\"gpu_binding_verified\":false,\"shader_consumption_verified\":false,\"atomic\":false}";
}
}
