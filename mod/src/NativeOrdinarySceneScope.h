#pragma once
#include "NativeRenderProbe.h"
#include "NativeMemory.h"
#include <cstring>
#include <ostream>

// Shared read-only inventory for the authenticated ordinary pair. The bloom
// selector still requires right slot1 explicitly; colour boundaries allow0/1.
namespace ordinary_scene_scope {
struct Scope {NativeRenderProbe::Scene scene;NativeCameraEntries::OrdinaryPair pair;};
struct Evidence {
    uint32_t mask{},reads{},counter{};int32_t count{};uintptr_t active{};uint64_t flags{};
    std::vector<uint64_t> view_flags;
    bool operator==(const Evidence&)const=default;
};
template<class T>bool read(uintptr_t address,T& value){return native_memory::read(address,value);}
inline Evidence inspect(const Scope& scope,uint32_t required_slot=UINT32_MAX){
    Evidence value;const auto& s=scope.scene;const auto& p=scope.pair;const auto slot=required_slot<=1?required_slot:s.slot;
    const auto& eye=slot==1?p.right:p.left;
    if(!s.valid() || s.slot!=slot || slot>1 || s.ordinal!=slot+1 || !s.dispatch_sequence || s.entry!=eye.entry || p.left.slot!=0 || p.right.slot!=1 || p.left.render_list!=s.list || p.right.render_list!=s.list || s.camera_revision_before!=eye.revision || (s.view_flags_before&0x50000044))value.mask|=1;
    if(!NativeCameraEntries::get().current(p))value.mask|=2;
    if(slot>1 || !s.renderer || s.renderer>UINTPTR_MAX-0x2b0 || !s.list || s.list>UINTPTR_MAX-0x29c7c || !s.entry || s.entry>UINTPTR_MAX-8 || s.entry!=s.list+0x168+size_t(slot)*0xa60){value.mask|=4;return value;}
    if(read(s.renderer+0x140,value.active))value.reads|=1;
    if(read(s.list+0x29c78,value.counter))value.reads|=2;
    if(read(s.list+0x29968,value.count))value.reads|=4;
    if(read(s.entry,value.flags))value.reads|=8;
    if(value.reads!=15)value.mask|=8;
    if(value.active!=s.entry)value.mask|=16;
    if(value.counter!=s.render_counter_before)value.mask|=32;
    if(value.count<2 || value.count>64)value.mask|=64;
    else {
    // Every view entry's flags in one system call: entries are 0xa60 apart from list+0x168, and
    // count*0xa60 ends at list+0x29968 at most (the count field itself).
    thread_local std::vector<uint8_t> views;views.resize(size_t(value.count-1)*0xa60+sizeof(uint64_t));
    const bool readable=native_memory::read_bytes(s.list+0x168,views.data(),views.size());
    for(int32_t i=0;i<value.count;++i){uint64_t flags{};
        if(!readable){value.mask|=8;break;}std::memcpy(&flags,views.data()+size_t(i)*0xa60,sizeof(flags));value.view_flags.push_back(flags);
        if(i<2?flags!=s.view_flags_before || (flags&0x50000044):!(flags&0x50000044) || (flags&0x40))value.mask|=1024;
    }}
    if(value.flags!=s.view_flags_before)value.mask|=128;
    return value;
}
inline bool same(const Scope& a,const Scope& b){
    const auto& x=a.scene;const auto& y=b.scene;
    const auto primary=[](const auto& p,const auto& q){return p.add_list==q.add_list && p.render_list==q.render_list && p.entry==q.entry && p.slot==q.slot && p.revision==q.revision && p.render_id==q.render_id && p.token==q.token;};
    return x.dispatch_sequence==y.dispatch_sequence && x.ordinal==y.ordinal && x.slot==y.slot && x.entry==y.entry && x.list==y.list && x.renderer==y.renderer &&
        x.camera_revision_before==y.camera_revision_before && x.render_counter_before==y.render_counter_before && x.view_flags_before==y.view_flags_before &&
        a.pair.producer_list==b.pair.producer_list && a.pair.producer_revision==b.pair.producer_revision && primary(a.pair.left,b.pair.left) && primary(a.pair.right,b.pair.right);
}
inline void write(std::ostream& out,const Evidence& e){
    out<<"{\"model\":\"ordinary_scene_view_inventory_v1\",\"mask\":"<<e.mask<<",\"reads\":"<<e.reads<<",\"counter\":"<<e.counter<<",\"count\":"<<e.count<<",\"active\":"<<e.active<<",\"flags\":"<<e.flags<<",\"view_flags\":[";
    for(size_t i=0;i<e.view_flags.size();++i){if(i)out<<',';out<<e.view_flags[i];}out<<"]}";
}
}
