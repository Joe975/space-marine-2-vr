#pragma once
#include "NativeGeometryIdentity.h"
#include "GameBuild.h"

// Read-only implementation identity, not a GPU resource/content observation.
namespace native_command_binding {
struct PreserveError {DWORD value{GetLastError()};~PreserveError(){SetLastError(value);}};
struct Image {
    uintptr_t base{},size{};
    std::array<std::pair<uintptr_t,uintptr_t>,96> executable{};
    unsigned count{};bool valid{};
    bool contains(uintptr_t p,size_t bytes=1)const{return valid && p>=base && bytes<=size && p-base<=size-bytes;}
    bool code(uintptr_t p)const{return contains(p) && std::ranges::any_of(executable.begin(),executable.begin()+count,[&](const auto& r){return p>=r.first && p<r.second;});}
};
template<class Reader> Image inspect_image(uintptr_t base,Reader&& read)noexcept{
    PreserveError preserve_error;
    Image out;
    try{
        std::array<uint8_t,64> dos{};std::array<uint8_t,264> nt{};
        if(!base || !read(base,dos.data(),dos.size()))return out;
        uint16_t magic{};uint32_t pe{};std::memcpy(&magic,dos.data(),2);std::memcpy(&pe,dos.data()+0x3c,4);
        if(magic!=0x5a4d || pe<64 || pe>0x100000 || base>UINTPTR_MAX-pe-nt.size() || !read(base+pe,nt.data(),nt.size()))return out;
        uint32_t signature{},stamp{},size{};uint16_t machine{},sections{},optional{},kind{};
        std::memcpy(&signature,nt.data(),4);std::memcpy(&machine,nt.data()+4,2);std::memcpy(&sections,nt.data()+6,2);
        std::memcpy(&stamp,nt.data()+8,4);std::memcpy(&optional,nt.data()+20,2);std::memcpy(&kind,nt.data()+24,2);std::memcpy(&size,nt.data()+80,4);
        if(signature!=0x4550 || machine!=0x8664 || !game_build::supported(stamp) || kind!=0x20b || optional<240 || optional>4096 ||
           !sections || sections>96 || size<0x1000 || size>0x40000000 || base>UINTPTR_MAX-size || pe+24+optional+sections*40ULL>size)return out;
        out.base=base;out.size=size;out.valid=true;
        for(unsigned i=0;i<sections;++i){
            std::array<uint8_t,40> section{};if(!read(base+pe+24+optional+i*40,section.data(),section.size()))return {};
            uint32_t virtual_size{},rva{},raw_size{},flags{};std::memcpy(&virtual_size,section.data()+8,4);std::memcpy(&rva,section.data()+12,4);
            std::memcpy(&raw_size,section.data()+16,4);std::memcpy(&flags,section.data()+36,4);
            const auto bytes=std::max(virtual_size,raw_size);
            if(bytes && (rva>=size || bytes>size-rva))return {};
            if((flags&0x20000000) && bytes)out.executable[out.count++]={base+rva,base+rva+bytes};
        }
        if(!out.count)return {};
        return out;
    }catch(...){return {};}
}
enum Mask:uint32_t {not_requested=1,unqualified_owner=2,unsupported_image=4,unreadable=8,outside_image=16,changed=32};
struct Evidence {uintptr_t command{},vtable{},binding_callback{},callback_rva{};uint32_t mask{not_requested},reads{};bool owner_stable{},pointers_stable{};};
inline bool player(const native_geometry_identity::Evidence& e){
    return !e.mask && e.class_chain_stable && e.weak_roundtrip && e.instance && uint16_t(e.handle>>48)==469 && e.class_name=="pc_marine_pve_story_titus_authority";
}
inline bool same(const native_geometry_identity::Evidence& a,const native_geometry_identity::Evidence& b){
    return player(a) && player(b) && a.handle==b.handle && a.instance==b.instance && a.properties==b.properties && a.type_link==b.type_link &&
        a.class_owner==b.class_owner && a.weak==b.weak && a.kind_flags==b.kind_flags && a.name_address==b.name_address;
}
template<class Reader> Evidence capture(uintptr_t command,uintptr_t descriptor,const native_geometry_identity::Evidence& expected,const Image& image,Reader&& reader)noexcept{
    PreserveError preserve_error;
    Evidence e;e.command=command;
    if(!player(expected)){e.mask=unqualified_owner;return e;}
    if(!image.valid){e.mask=unsupported_image;return e;}
    try{
        auto read=[&](uintptr_t p,void* target,size_t size){++e.reads;return p>=0x10000 && p<0x800000000000ULL && size<=512 && p<=UINTPTR_MAX-size && reader(p,target,size);};
        const auto before=native_geometry_identity::capture(descriptor,read);
        if(!same(expected,before)){e.mask=unqualified_owner;return e;}
        if((command&7) || !read(command,&e.vtable,8)){e.mask=unreadable;return e;}
        if(!image.contains(e.vtable,0x2e0)){e.mask=outside_image;return e;}
        if(!read(e.vtable+0x2d8,&e.binding_callback,8)){e.mask=unreadable;return e;}
        if(!image.code(e.binding_callback)){e.mask=outside_image;return e;}
        uintptr_t after_vtable{},after_callback{};
        if(!read(command,&after_vtable,8) || !read(e.vtable+0x2d8,&after_callback,8)){e.mask=unreadable;return e;}
        if(e.vtable!=after_vtable || e.binding_callback!=after_callback){e.mask=changed;return e;}
        if(!same(before,native_geometry_identity::capture(descriptor,read))){e.mask=changed;return e;}
        e.callback_rva=e.binding_callback-image.base;e.owner_stable=true;e.pointers_stable=true;e.mask=0;return e;
    }catch(...){e.mask=unreadable;return e;}
}
inline void write(std::ostream& out,const Evidence& e){
    out<<"{\"mask\":"<<e.mask<<",\"command\":"<<e.command<<",\"vtable\":"<<e.vtable<<",\"binding_callback\":"<<e.binding_callback
       <<",\"callback_rva\":"<<e.callback_rva<<",\"reads\":"<<e.reads<<",\"owner_stable\":"<<(e.owner_stable?"true":"false")
       <<",\"pointers_stable\":"<<(e.pointers_stable?"true":"false")<<",\"native_calls\":false,\"gpu_binding_verified\":false,\"atomic\":false}";
}
}
