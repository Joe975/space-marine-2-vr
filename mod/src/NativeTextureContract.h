#pragma once
#include <Windows.h>
#include "GameBuild.h"
#include "NativeMemory.h"
#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <vector>

// Borrowed POD only. No virtual calls, allocation, COM query or native refcount
// changes. This records the real pool/factory inputs needed to prove an owned
// allocation contract; it does not itself establish ownership or lifetime.
namespace native_texture_contract {
template<class T>bool read(uintptr_t address,T& value) {return native_memory::read(address,value);}
struct Wrapper {
    uintptr_t address{},vtable{};uint64_t flags{};
    std::array<uintptr_t,4> methods{}; // virtual +18/+20/+b0/+c0; never invoked.
    bool readable{};bool operator==(const Wrapper&)const=default;
};
inline Wrapper wrapper(uintptr_t address) {
    Wrapper value;value.address=address;if(!address)return value;
    constexpr std::array<size_t,4> offsets{0x18,0x20,0xb0,0xc0};
    // Two system calls: the wrapper's first 0xa0 bytes, then the vtable's slots +0x18..+0xc8.
    native_memory::Block<0xa0> object;native_memory::Block<0xb0> slots;
    if(address>UINTPTR_MAX-0xa0 || !object.read_from(address))return value;
    object.get(0x98,value.flags);object.get(0,value.vtable);
    if(!value.vtable || value.vtable>UINTPTR_MAX-0xc8 || !slots.read_from(value.vtable+0x18))return value;
    for(size_t i=0;i<4;++i){slots.get(offsets[i]-0x18,value.methods[i]);if(!value.methods[i])return value;}
    value.readable=true;return value;
}
struct Target {
    uint32_t id{};std::array<uint64_t,5> definition{};int32_t record{-1};
    uint8_t no_uav{},variants{};
    std::vector<std::array<uint32_t,6>> initialization_words;
    std::vector<uintptr_t> wrappers;
    bool readable{};bool operator==(const Target&)const=default;
};
struct Snapshot {
    uintptr_t module{},factory{},factory_vtable{},factory_create{},pool{},banks{},records{};
    int32_t pool_mode{-1},bank{-1},bank_count{},record_count{},bank_variant{-1};
    std::array<uint32_t,2> bank_dimensions{};uint32_t selected_id{};
    std::array<Target,2> targets;std::array<Wrapper,9> wrappers;
    bool complete{};bool operator==(const Snapshot&)const=default;
};
// Which authenticated contract fields differ between an observation and the
// contract an owned resource was created against. Zero means the same shape:
// identical module/factory/pool/bank/variant/dimensions/targets, and a record
// table that may only have grown. Diagnostic and gate only; never relaxes a check.
enum Difference:uint32_t {incomplete=1,module_changed=2,factory_changed=4,pool_changed=8,pool_mode_changed=16,banks_moved=32,bank_changed=64,bank_count_changed=128,
    records_moved=256,records_shrank=512,variant_changed=1024,dimensions_changed=2048,targets_changed=4096};
inline uint32_t difference(const Snapshot& observed,const Snapshot& created) {
    const auto& a=observed;const auto& b=created;uint32_t d{};
    if(!a.complete || !b.complete)d|=incomplete;
    if(a.module!=b.module)d|=module_changed;
    if(a.factory!=b.factory || a.factory_vtable!=b.factory_vtable || a.factory_create!=b.factory_create)d|=factory_changed;
    if(a.pool!=b.pool)d|=pool_changed;
    if(a.pool_mode!=b.pool_mode)d|=pool_mode_changed;
    if(a.banks!=b.banks)d|=banks_moved;
    if(a.bank!=b.bank)d|=bank_changed;
    if(a.bank_count!=b.bank_count)d|=bank_count_changed;
    if(a.records!=b.records)d|=records_moved;
    if(a.record_count<b.record_count)d|=records_shrank;
    if(a.bank_variant!=b.bank_variant)d|=variant_changed;
    if(a.bank_dimensions!=b.bank_dimensions)d|=dimensions_changed;
    if(a.targets!=b.targets)d|=targets_changed;
    return d;
}
// The same pool object with its banks, records or targets laid out anew (24 Oct: leaving
// the armoury, 0x1a0 = banks_moved|bank_count_changed|records_moved, for good). The owned
// textures' current contract can't be re-based onto it in place; NativeOrdinaryBloom renews them once
// it persists, and the renewal's bootstrap takes a pair made for the new layout (same_layout) if one exists.
// An incomplete observation (a pool mid-teardown) or another module, factory or pool is not.
inline bool rebuilt(uint32_t d){return d && !(d&(incomplete|module_changed|factory_changed|pool_changed|pool_mode_changed));}
// The native pool reallocates its render-target record table when it outgrows it
// (seen live 21 Oct: Neurothrope fight, 117 -> 125 records, table moved). Every
// authenticated field is unchanged except the table address, so the owned textures
// still match. Returns the baseline with only the address and grown count replaced;
// any other difference (or a shrink) returns nothing and stays refused.
inline std::optional<Snapshot> rebase_records(const Snapshot& observed,const Snapshot& baseline) {
    if(!observed.complete || !baseline.complete || difference(observed,baseline)!=records_moved || !observed.records)return {};
    for(const auto& target:observed.targets)if(!target.readable || target.record<0 || target.record>=observed.record_count)return {};
    auto value=baseline;value.records=observed.records;value.record_count=observed.record_count;
    return value;
}
// The layout a pair of owned textures is made for: everything an observation authenticates except
// where things live. Same: module, factory, pool, pool mode, selected bank, bank count, variant,
// dimensions, and per target the id, definition, no-UAV and variant flags, initialisation rows and
// wrapper count. Free to differ: the bank array and record table addresses, the table's length,
// each target's record index and the addresses of its row wrappers. 26 Oct: every class change
// flips the pool between one and two banks (0x80) and back, and loads move the bank array and the
// table (0x120), so a rebuilt pool is usually a layout an owned pair was already made for.
inline bool same_layout(const Snapshot& observed,const Snapshot& created) {
    if(!observed.complete || !created.complete || (difference(observed,created)&~(banks_moved|records_moved|records_shrank|targets_changed)))return false;
    for(size_t i=0;i<2;++i){const auto& a=observed.targets[i];const auto& b=created.targets[i];
        if(!a.readable || !b.readable || a.id!=b.id || a.definition!=b.definition || a.no_uav!=b.no_uav || a.variants!=b.variants ||
           a.initialization_words!=b.initialization_words || a.wrappers.size()!=b.wrappers.size())return false;}
    return true;
}
// A contract made against one pool re-based onto a rebuilt pool of the same layout, as
// rebase_records does for a moved table: the new bank array, record table and length, target
// records and row wrappers, and the observing job's borrowed wrappers. The result equals the
// observation field for field. Another layout, or a target record outside the new table, returns
// nothing.
inline std::optional<Snapshot> rebase_layout(const Snapshot& observed,const Snapshot& created) {
    if(!same_layout(observed,created) || !observed.banks || !observed.records)return {};
    for(const auto& target:observed.targets)if(target.record<0 || target.record>=observed.record_count)return {};
    auto value=created;value.banks=observed.banks;value.records=observed.records;value.record_count=observed.record_count;
    for(size_t i=0;i<2;++i){value.targets[i].record=observed.targets[i].record;value.targets[i].wrappers=observed.targets[i].wrappers;}
    value.wrappers=observed.wrappers;value.selected_id=observed.selected_id;
    return value;
}
inline Snapshot capture_pair(uintptr_t base,const std::array<uintptr_t,9>& wrappers,const std::array<uint32_t,2>& ids,uint32_t counter) {
    Snapshot value;value.module=base;
    for(size_t i=0;i<wrappers.size();++i)value.wrappers[i]=wrapper(wrappers[i]);
    if(!base || base>UINTPTR_MAX-0x508cab0 ||
       (ids!=std::array<uint32_t,2>{0x16,0x17} && ids!=std::array<uint32_t,2>{0x29,0x2a} &&
        ids!=std::array<uint32_t,2>{0xc4,0xc5} && ids!=std::array<uint32_t,2>{0x77,0x78}))return value;
    value.selected_id=ids[counter&1];
    if(!read(base+game_build::rva(0x508caa8),value.factory) || !value.factory || !read(value.factory,value.factory_vtable) || !value.factory_vtable || value.factory_vtable>UINTPTR_MAX-0x18 ||
       !read(value.factory_vtable+0x10,value.factory_create) || !value.factory_create || !read(base+game_build::rva(0x4ff0bb8),value.pool) || !value.pool || value.pool>UINTPTR_MAX-0x2270 ||
       !read(value.pool+0xc,value.pool_mode) || value.pool_mode<0 || value.pool_mode>2 || !read(value.pool+0x2248,value.banks) || !value.banks ||
       !read(value.pool+0x2250,value.bank_count) || value.bank_count<1 || value.bank_count>256 || !read(value.pool+0x2258,value.bank) || value.bank<0 || value.bank>=value.bank_count ||
       !read(value.pool+0x2260,value.records) || !value.records || !read(value.pool+0x2268,value.record_count) || value.record_count<1 || value.record_count>4096)return value;
    const auto bank_offset=size_t(value.bank)*0x7f0;
    if(value.banks>UINTPTR_MAX-bank_offset-0x7f0 || value.records>UINTPTR_MAX-size_t(value.record_count)*0x4f8)return value;
    const auto bank=value.banks+bank_offset;
    if(!read(bank,value.bank_dimensions) || !read(bank+0xc,value.bank_variant) || value.bank_variant<0 || value.bank_variant>=30)return value;
    for(size_t i=0;i<2;++i) {
        auto& target=value.targets[i];target.id=ids[i];
        if(!read(value.pool+0x10+size_t(target.id)*0x28,target.definition) || !read(bank+0x118+size_t(target.id)*4,target.record) || target.record<0 || target.record>=value.record_count)return value;
        // The whole 0x4f8-byte record in one system call: rows end before +0x2dc, wrappers before +0x4c0.
        const auto record=value.records+size_t(target.record)*0x4f8;int32_t rows{},count{};
        thread_local native_memory::Block<0x4f8> block;
        if(!block.read_from(record))return value;
        block.get(9,target.no_uav);block.get(10,target.variants);block.get(0x2dc,rows);block.get(0x4c0,count);
        if(target.no_uav>1 || target.variants>1 || rows<1 || rows>30 || count<1 || count>60 || count!=rows*(target.no_uav+1))return value;
        for(int32_t n=0;n<rows;++n) {std::array<uint32_t,6> words{};block.get(0xc+size_t(n)*0x18,words);target.initialization_words.push_back(words);}
        for(int32_t n=0;n<count;++n) {uintptr_t native{};block.get(0x2e0+size_t(n)*8,native);if(!native)return value;target.wrappers.push_back(native);}
        target.readable=true;
    }
    for(const auto& native:value.wrappers)if(native.address && !native.readable)return value;
    value.complete=true;return value;
}
inline Snapshot capture(uintptr_t base,const std::array<uintptr_t,9>& wrappers,uint8_t mode,uint32_t counter) {
    constexpr std::array<std::array<uint32_t,2>,3> ids{{{0x16,0x17},{0x29,0x2a},{0xc4,0xc5}}};
    return capture_pair(base,wrappers,mode<3?ids[mode]:std::array<uint32_t,2>{},counter);
}
inline Snapshot capture_global_history(uintptr_t base,const std::array<uintptr_t,9>& wrappers,uint32_t counter) {
    // Actual Scene1805a1e acquisition. Kept separate from temporal mode2 bloom.
    return capture_pair(base,wrappers,{0x77,0x78},counter);
}
inline void write(std::ostream& out,const Snapshot& value) {
    out<<"{\"model\":\"native_texture_contract_v1\",\"read_only\":true,\"native_calls\":false,\"resource_leases\":false,\"complete\":"<<(value.complete?"true":"false")
       <<",\"module_base\":"<<value.module<<",\"factory\":"<<value.factory<<",\"factory_vtable\":"<<value.factory_vtable<<",\"factory_create\":"<<value.factory_create
       <<",\"pool\":"<<value.pool<<",\"pool_mode\":"<<value.pool_mode<<",\"banks\":"<<value.banks<<",\"bank\":"<<value.bank<<",\"bank_count\":"<<value.bank_count
       <<",\"bank_variant\":"<<value.bank_variant<<",\"bank_dimensions\":["<<value.bank_dimensions[0]<<','<<value.bank_dimensions[1]<<"],\"records\":"<<value.records<<",\"record_count\":"<<value.record_count<<",\"selected_id\":"<<value.selected_id<<",\"targets\":[";
    for(size_t i=0;i<2;++i) {if(i)out<<',';const auto& t=value.targets[i];out<<"{\"id\":"<<t.id<<",\"readable\":"<<(t.readable?"true":"false")<<",\"definition\":[";
        for(size_t n=0;n<5;++n){if(n)out<<',';out<<t.definition[n];}out<<"],\"record\":"<<t.record<<",\"no_uav\":"<<uint32_t(t.no_uav)<<",\"variants\":"<<uint32_t(t.variants)<<",\"initialization_words\":[";
        for(size_t n=0;n<t.initialization_words.size();++n){if(n)out<<',';out<<'[';for(size_t j=0;j<6;++j){if(j)out<<',';out<<t.initialization_words[n][j];}out<<']';}out<<"],\"wrappers\":[";
        for(size_t n=0;n<t.wrappers.size();++n){if(n)out<<',';out<<t.wrappers[n];}out<<"]}";
    }
    out<<"],\"wrappers\":[";for(size_t i=0;i<9;++i){if(i)out<<',';const auto& native=value.wrappers[i];out<<"{\"address\":"<<native.address<<",\"readable\":"<<(native.readable?"true":"false")<<",\"vtable\":"<<native.vtable<<",\"flags\":"<<native.flags<<",\"virtual_18_20_b0_c0\":[";
        for(size_t n=0;n<4;++n){if(n)out<<',';out<<native.methods[n];}out<<"]}";}out<<"]}";
}
}
