#pragma once
#include "NativeOrdinarySceneScope.h"

// Passive evidence only. descriptor+0x100 is the handle copied by1f7bcc0.
// Native1d8edf0 requires mask0x01 at low48-base+0x108 before subtracting0x70.
// Lightweight/static mask0x02 handles have another layout and are NOT decoded here.
namespace native_geometry_identity {
enum Mask:uint32_t {handle_unreadable=1,invalid_handle=2,kind_unreadable=4,unsupported_kind=8,chain_unreadable=16,bad_length=32,bad_bytes=64,changed=128,budget=256};
struct Evidence {
    uint64_t handle{};uintptr_t instance{},properties{},type_link{},class_owner{},name_address{},weak{},palette_pointer{};
    uint32_t mask{},declared_length{},reads{};uint8_t kind_flags{};bool heap{},weak_roundtrip{},class_chain_stable{},palette_read{},palette_stable{};std::string class_name;
};
template<class Reader> Evidence capture(uintptr_t descriptor,Reader&& reader,size_t handle_offset=0x100){
    Evidence e;
    auto raw=[&](uintptr_t address,void* target,size_t size){++e.reads;return address && size<=512 && address<=UINTPTR_MAX-size && reader(address,target,size);};
    auto read=[&](uintptr_t base,size_t offset,auto& value){return base && base<=UINTPTR_MAX-offset && raw(base+offset,&value,sizeof(value));};
    if(!read(descriptor,handle_offset,e.handle)){e.mask=handle_unreadable;return e;}
    const uintptr_t base=uintptr_t(e.handle&0xffffffffffffULL);
    if(static_cast<int16_t>(e.handle>>48)<0 || base<0x70 || (base&7)){e.mask=invalid_handle;return e;}
    if(!read(base,0x108,e.kind_flags)){e.mask=kind_unreadable;return e;}
    if(!(e.kind_flags&1)){e.mask=unsupported_kind;return e;}
    e.instance=base-0x70;
    // EngineArmProbe reads its captured pose buffer from the same supported
    // instance+0xd0. Retain the raw pointer only; no dereference or owner claim.
    e.palette_read=read(e.instance,0xd0,e.palette_pointer);
    if(!read(e.instance,0x58,e.properties) || !e.properties || !read(e.properties,8,e.type_link) || !e.type_link ||
       !read(e.type_link,0x10,e.class_owner) || !e.class_owner || e.class_owner>UINTPTR_MAX-0x30){e.mask=chain_unreadable;return e;}
    e.name_address=e.class_owner+0x30;std::array<uint8_t,16> header{};
    if(!read(e.name_address,0,header)){e.mask=chain_unreadable;return e;}
    uintptr_t payload=e.name_address;e.heap=(header[15]&1)!=0;
    if(e.heap){std::memcpy(&payload,header.data(),8);std::memcpy(&e.declared_length,header.data()+8,4);}
    else if((header[15]>>1)<=15)e.declared_length=15-(header[15]>>1);
    else {e.mask=bad_length;return e;}
    if(!e.declared_length || e.declared_length>128){e.mask=bad_length;return e;}
    e.class_name.resize(e.declared_length);
    if(!raw(payload,e.class_name.data(),e.class_name.size())){e.class_name.clear();e.mask=chain_unreadable;return e;}
    if(!std::ranges::all_of(e.class_name,[](unsigned char c){return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='/' || c=='.' || c=='-' || c==':' || c=='|';})){
        e.class_name.clear();e.mask=bad_bytes;return e;
    }
    uintptr_t back{};if(read(e.properties,0x30,e.weak) && e.weak && read(e.weak,0,back))e.weak_roundtrip=back==e.properties;
    uint64_t handle{};uint8_t flags{};uintptr_t properties{},type_link{},class_owner{};std::array<uint8_t,16> after{};std::string payload_after(e.class_name.size(),'\0');
    if(!read(descriptor,handle_offset,handle) || !read(base,0x108,flags) || !read(e.instance,0x58,properties) || !read(e.properties,8,type_link) ||
       !read(e.type_link,0x10,class_owner) || !read(e.name_address,0,after) || !raw(payload,payload_after.data(),payload_after.size())){e.mask=chain_unreadable;return e;}
    if(handle!=e.handle || flags!=e.kind_flags || properties!=e.properties || type_link!=e.type_link || class_owner!=e.class_owner || after!=header || payload_after!=e.class_name){e.mask=changed;return e;}
    if(e.weak_roundtrip){uintptr_t weak{},owner{};if(!read(e.properties,0x30,weak) || weak!=e.weak || !read(weak,0,owner) || owner!=e.properties){e.weak_roundtrip=false;e.mask=changed;return e;}}
    uintptr_t palette_after{};
    e.palette_stable=e.palette_read && read(e.instance,0xd0,palette_after) && palette_after==e.palette_pointer;
    e.class_chain_stable=true;return e;
}
inline Evidence capture(uintptr_t descriptor){return capture(descriptor,[](uintptr_t p,void* target,size_t size){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&n) && n==size;});}
inline Evidence capture_handle(uintptr_t pointer){return capture(pointer,[](uintptr_t p,void* target,size_t size){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&n) && n==size;},0);}
inline void write(std::ostream& out,const Evidence& e){
    out<<"{\"handle\":"<<e.handle<<",\"kind_flags\":"<<unsigned(e.kind_flags)<<",\"mask\":"<<e.mask<<",\"reads\":"<<e.reads<<",\"instance\":"<<e.instance
        <<",\"properties\":"<<e.properties<<",\"type_link\":"<<e.type_link<<",\"class_owner\":"<<e.class_owner<<",\"name_address\":"<<e.name_address
        <<",\"declared_length\":"<<e.declared_length<<",\"heap\":"<<(e.heap?"true":"false")<<",\"weak\":"<<e.weak<<",\"weak_roundtrip\":"<<(e.weak_roundtrip?"true":"false")
        <<",\"class_chain_stable\":"<<(e.class_chain_stable?"true":"false")<<",\"palette_pointer\":"<<e.palette_pointer
        <<",\"palette_read\":"<<(e.palette_read?"true":"false")<<",\"palette_stable\":"<<(e.palette_stable?"true":"false")<<",\"class_name\":\""<<e.class_name<<"\"}";
}
}
