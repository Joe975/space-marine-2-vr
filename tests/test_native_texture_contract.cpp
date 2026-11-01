#include "NativeMemory.h"
#include "NativeTextureContract.h"
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T>void put(uintptr_t address,size_t offset,const T& value){std::memcpy(reinterpret_cast<void*>(address+offset),&value,sizeof(value));}
struct Fixture {
    uintptr_t base{};std::array<uintptr_t,26> vtable{};
    std::vector<uint8_t> factory=std::vector<uint8_t>(0x200),pool=std::vector<uint8_t>(0x2300),banks=std::vector<uint8_t>(0x7f0*2),records=std::vector<uint8_t>(0x4f8*2);
    std::array<std::array<uint8_t,0x280>,4> wrappers{};std::array<uintptr_t,9> inputs{};
    Fixture(){
        base=reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,0x5090000,MEM_RESERVE,PAGE_NOACCESS));if(!base)throw std::runtime_error("reserve fake module");
        for(size_t page:{size_t(0x508c000),size_t(0x4ff0000)})require(VirtualAlloc(reinterpret_cast<void*>(base+page),0x1000,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"commit fake global page");
        const auto f=reinterpret_cast<uintptr_t>(factory.data()),p=reinterpret_cast<uintptr_t>(pool.data()),b=reinterpret_cast<uintptr_t>(banks.data()),r=reinterpret_cast<uintptr_t>(records.data()),v=reinterpret_cast<uintptr_t>(vtable.data());
        for(size_t i=0;i<vtable.size();++i)vtable[i]=base+0x1000+i*0x100; // Uncommitted, must never be called.
        put(base,0x508caa8,f);put(base,0x4ff0bb8,p);put(f,0,v);put(p,0xc,int32_t(2));put(p,0x2248,b);put(p,0x2250,int32_t(2));put(p,0x2258,int32_t(1));put(p,0x2260,r);put(p,0x2268,int32_t(2));
        put(b+0x7f0,0,std::array<uint32_t,2>{1920,1080});put(b+0x7f0,0xc,int32_t(1));
        for(size_t i=0;i<wrappers.size();++i){const auto w=reinterpret_cast<uintptr_t>(wrappers[i].data());put(w,0,v);put(w,0x98,uint64_t(0x100003+i));}
        for(size_t i=0;i<inputs.size();++i)inputs[i]=reinterpret_cast<uintptr_t>(wrappers[i%4].data());inputs[2]=0;
        for(size_t i=0;i<2;++i){const uint32_t id=0xc4+uint32_t(i);std::array<uint64_t,5> definition{0x100003+i,11+i,22+i,33+i,44+i};put(p,0x10+size_t(id)*0x28,definition);put(b+0x7f0,0x118+size_t(id)*4,int32_t(i));
            put(r+i*0x4f8,9,uint8_t(1));put(r+i*0x4f8,10,uint8_t(1));put(r+i*0x4f8,0x2dc,int32_t(2));put(r+i*0x4f8,0x4c0,int32_t(4));
            for(size_t row=0;row<2;++row)put(r+i*0x4f8,0xc+row*0x18,std::array<uint32_t,6>{uint32_t(960+row),540,1,1,10,uint32_t(i+row)});
            for(size_t n=0;n<4;++n)put(r+i*0x4f8,0x2e0+n*8,reinterpret_cast<uintptr_t>(wrappers[n].data()));
        }
    }
    ~Fixture(){if(base)VirtualFree(reinterpret_cast<void*>(base),0,MEM_RELEASE);}
    native_texture_contract::Snapshot capture(uint32_t counter=7)const{return native_texture_contract::capture(base,inputs,2,counter);}
};
int main(){try{
    Fixture f;const auto calls=native_memory::calls().load();const auto original=f.capture();
    // One system call per native object: was about 90 field reads (22 Oct render-thread profile).
    const auto used=native_memory::calls().load()-calls;std::cout<<"texture contract: "<<used<<" native reads\n";
    require(used<=40,"the contract reads whole records and wrappers, not field by field");require(original.complete && original.selected_id==0xc5 && original.pool_mode==2 && original.bank==1 && original.bank_dimensions==std::array<uint32_t,2>{1920,1080},"actual global/pool/bank offsets and native parity captured");
    require(original.factory_create==f.vtable[2] && original.wrappers[0].methods==std::array<uintptr_t,4>{f.vtable[3],f.vtable[4],f.vtable[22],f.vtable[24]},"exact factory and wrapper virtual slots read without invoking uncommitted code");
    require(original.wrappers[0].flags==0x100003 && !original.wrappers[2].readable && original.wrappers[2].address==0,"raw native flags and null wrapper remain explicit, no lease invented");
    require(original.targets[0].definition[4]==44 && original.targets[1].definition[4]==45 && original.targets[1].record==1 && original.targets[1].initialization_words[1][0]==961 && original.targets[1].wrappers.size()==4,"target definition/record/row/wrapper strides are exact");
    require(f.capture(8).selected_id==0xc4 && f.capture(7)==original,"counter parity and repeat observations remain exact");
    for(uint32_t i=0;i<2;++i){const auto pool_address=reinterpret_cast<uintptr_t>(f.pool.data()),bank_address=reinterpret_cast<uintptr_t>(f.banks.data())+0x7f0;
        put(pool_address,0x10+size_t(0x77+i)*0x28,original.targets[i].definition);put(bank_address,0x118+size_t(0x77+i)*4,int32_t(i));}
    const auto global=native_texture_contract::capture_global_history(f.base,f.inputs,7);
    require(global.complete && global.selected_id==0x78 && global.targets[0].id==0x77 && global.targets[1].id==0x78 && global.targets[1].initialization_words==original.targets[1].initialization_words,"separate Scene global-history pool pair reads exact definitions/records without relabelling bloom mode2");
    require(native_texture_contract::capture_global_history(f.base,f.inputs,8).selected_id==0x77 && f.capture()==original,"global-history parity capture preserves the original mode2 contract");
    require(!native_texture_contract::capture_pair(f.base,f.inputs,{0x76,0x77},7).complete && !native_texture_contract::capture_pair(f.base,f.inputs,{UINT32_MAX,UINT32_MAX},7).complete,"unknown and overflow target pairs refuse before arbitrary pool offsets");
    const auto pool=reinterpret_cast<uintptr_t>(f.pool.data()),bank=reinterpret_cast<uintptr_t>(f.banks.data())+0x7f0,record=reinterpret_cast<uintptr_t>(f.records.data());
    require(original.bank_variant==1,"active native resolution variant read at selected bank+0xc");
    for(int32_t bad:{-1,30,INT32_MAX}){put(bank,0xc,bad);require(!f.capture().complete,"invalid native resolution variant refuses bounded observation");}put(bank,0xc,int32_t(1));
    for(int32_t bad:{-1,2,INT32_MAX}){put(pool,0x2258,bad);require(!f.capture().complete,"invalid selected bank cannot certify native creation inputs");}put(pool,0x2258,int32_t(1));
    for(int32_t bad:{-1,2,INT32_MAX}){put(bank,0x118+0xc4*4,bad);require(!f.capture().complete,"invalid target record refuses bounded observation");}put(bank,0x118+0xc4*4,int32_t(0));
    for(int32_t bad:{0,31,INT32_MAX}){put(record,0x2dc,bad);require(!f.capture().complete,"unbounded initializer rows refuse");}put(record,0x2dc,int32_t(2));
    put(record,0x4c0,int32_t(3));require(!f.capture().complete,"wrapper count must match actual row/no-UAV multiplicity");put(record,0x4c0,int32_t(4));
    put(record,9,uint8_t(2));require(!f.capture().complete,"unknown native no-UAV flag refuses");put(record,9,uint8_t(1));
    put(pool,0xc,int32_t(3));require(!f.capture().complete,"unknown pool mode refuses");put(pool,0xc,int32_t(2));
    const auto native=reinterpret_cast<uintptr_t>(f.wrappers[0].data());put(native,0,uintptr_t(1));require(!f.capture().complete,"unreadable native vtable refuses without dereference/call");put(native,0,reinterpret_cast<uintptr_t>(f.vtable.data()));
    put(native,0x98,uint64_t(0x200001));require(f.capture()!=original,"actual native flag replacement differs from prior contract");put(native,0x98,uint64_t(0x100003));
    require(!native_texture_contract::capture(0,f.inputs,2,7).complete && !native_texture_contract::capture(UINTPTR_MAX,f.inputs,2,7).complete && !native_texture_contract::capture(f.base,f.inputs,255,7).complete,"missing/overflow module and unsupported mode stay incomplete");
    std::ostringstream out;native_texture_contract::write(out,original);require(out.str().find("\"native_calls\":false")!=std::string::npos && out.str().find("\"resource_leases\":false")!=std::string::npos && out.str().find("\"selected_id\":197")!=std::string::npos,"export keeps borrowed native contract separate from actual allocations/leases");
    {   // 21 Oct Neurothrope: the record table moved (difference 0x100) with everything else identical.
        using namespace native_texture_contract;auto moved=original;moved.records+=0x10000;moved.record_count+=8;
        require(difference(moved,original)==records_moved && records_moved==0x100,"a moved, grown record table is exactly difference 0x100");
        const auto rebased=rebase_records(moved,original);
        require(rebased && rebased->records==moved.records && rebased->record_count==moved.record_count && !difference(moved,*rebased),"a pure table move is re-based onto the new address");
        auto kept=*rebased;kept.records=original.records;kept.record_count=original.record_count;require(kept==original,"re-basing changes nothing but the table address and count");
        require(!rebase_records(original,original),"an unchanged contract needs no rebase");
        auto variant=moved;variant.bank_variant=original.bank_variant+1;require(!rebase_records(variant,original),"a move plus a variant change stays refused");
        auto shrank=moved;shrank.record_count=original.record_count-1;require(!rebase_records(shrank,original),"a move that shrinks the table stays refused");
        auto target=moved;target.targets[0].record=target.record_count;require(!rebase_records(target,original),"a target record outside the new table stays refused");
        auto wrapper=moved;wrapper.targets[1].wrappers[0]+=8;require(!rebase_records(wrapper,original),"a move that changes a target wrapper stays refused");
        auto incomplete=moved;incomplete.complete=false;require(!rebase_records(incomplete,original),"an incomplete observation never re-bases");
        auto null=moved;null.records=0;require(!rebase_records(null,original),"a null table never re-bases");
    }
    {   // 26 Oct: class changes and loads rebuild the pool into a layout seen before.
        using namespace native_texture_contract;
        // 0x120 (load): the bank array and the record table moved, and so did the target records and row wrappers.
        auto moved=original;moved.banks+=0x20000;moved.records+=0x10000;moved.record_count=moved.record_count+40;
        moved.targets[0].record=moved.record_count-1;moved.targets[1].wrappers[0]+=0x800;moved.wrappers[1].address+=0x800;moved.selected_id=0xc4;
        require(same_layout(moved,original) && same_layout(original,moved),"a pool laid out anew at other addresses is the same layout");
        const auto rebased=rebase_layout(moved,original);
        require(rebased && *rebased==moved && !difference(*rebased,moved) && difference(moved,original),"re-basing onto the same layout gives exactly the new pool's contract");
        auto shrank=moved;shrank.record_count=1;shrank.targets[0].record=0;shrank.targets[1].record=0;require(same_layout(shrank,original) && rebase_layout(shrank,original),"a shorter table is an address change too");
        auto outside=moved;outside.targets[1].record=outside.record_count;require(same_layout(outside,original) && !rebase_layout(outside,original),"a target record outside the new table never re-bases");
        // 0x80 (class change): one bank more or fewer is another layout.
        auto banks=moved;banks.bank_count=original.bank_count+1;require(difference(banks,original)&bank_count_changed,"a changed bank count is a contract difference");
        require(!same_layout(banks,original) && !rebase_layout(banks,original),"a changed bank count is another layout");
        auto variant=moved;variant.bank_variant=0;require(!same_layout(variant,original),"another resolution variant is another layout");
        auto dimensions=moved;dimensions.bank_dimensions[0]=2560;require(!same_layout(dimensions,original),"another resolution is another layout");
        auto definition=moved;definition.targets[1].definition[0]^=1;require(!same_layout(definition,original),"another target definition is another layout");
        auto rows=moved;rows.targets[0].initialization_words[1][1]++;require(!same_layout(rows,original),"other initialisation rows are another layout");
        auto count=moved;count.targets[0].wrappers.pop_back();require(!same_layout(count,original),"another row wrapper count is another layout");
        auto flags=moved;flags.targets[1].no_uav=0;require(!same_layout(flags,original),"another no-UAV flag is another layout");
        auto bank=moved;bank.bank=0;require(!same_layout(bank,original),"another selected bank is another layout");
        auto factory=moved;factory.factory_create+=0x10;require(!same_layout(factory,original),"another factory is another layout");
        auto pool=moved;pool.pool+=0x10;require(!same_layout(pool,original),"another pool is another layout");
        auto module=moved;module.module+=0x1000;require(!same_layout(module,original),"another module is another layout");
        auto mode=moved;mode.pool_mode=1;require(!same_layout(mode,original),"another pool mode is another layout");
        auto incomplete=moved;incomplete.complete=false;require(!same_layout(incomplete,original) && !same_layout(original,incomplete),"an incomplete observation has no layout");
        auto nulls=moved;nulls.banks=0;require(!rebase_layout(nulls,original),"a null bank array never re-bases");
    }
    std::cout<<"native texture contract: actual POD offsets, bounded pool records and no native calls PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
