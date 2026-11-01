#include "synthetic_code.h"
#include "NativeObjectConstantsSnapshot.h"
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
unsigned native_calls{};uintptr_t native_chunk{},native_flags{};DWORD native_incoming{};
std::function<void()> native_steps;uintptr_t native_result=0xeeee0000;
__declspec(noinline) uintptr_t getter_fixture(uintptr_t chunk,uintptr_t flags){
    ++native_calls;native_chunk=chunk;native_flags=flags;native_incoming=GetLastError();
    if(native_steps)native_steps();SetLastError(0x1234abcd);return native_result;
}
__declspec(noinline) uintptr_t getter_call(uintptr_t chunk,uintptr_t flags){
    NativeObjectConstantGetterProbe::Getter volatile target=&getter_fixture;const auto value=target(chunk,flags);volatile auto keep=native_calls;(void)keep;return value;
}
}
struct ObjectConstantGetterTests {
    using Probe=NativeObjectConstantGetterProbe;
    struct Memory {
        std::map<uintptr_t,uint8_t> bytes;
        template<class T> void put(uintptr_t p,const T& value){const auto* b=reinterpret_cast<const uint8_t*>(&value);for(size_t i=0;i<sizeof(value);++i)bytes[p+i]=b[i];}
        bool read(uintptr_t p,void* out,size_t n){SetLastError(0xbad123);for(size_t i=0;i<n;++i){const auto found=bytes.find(p+i);if(found==bytes.end())return false;static_cast<uint8_t*>(out)[i]=found->second;}return true;}
    };
    static void run(const std::filesystem::path& directory){
        auto& probe=Probe::get();Memory m;auto now=Probe::Clock::time_point{}+std::chrono::seconds(10);
        const uintptr_t base=0x140000000,instance=0x200000,properties=0x210000,type=0x220000,owner=0x230000,text=0x240000,weak=0x250000,resource=0x260000;
        const uintptr_t device=0x300000,stream=0x310000,allocator=0x320000,chunk=0x330000,wrapper=0x340000,native_resource=0x350000,cpu=0x360000,descriptor=0x400000;
        const auto put=[&](uintptr_t p,auto value){m.put(p,value);};
        put(instance+0x178,uint8_t{1});put(instance+0x58,properties);put(instance+0xd0,uintptr_t{});put(instance+0x250,resource);put(instance+0x42,int16_t{7});
        put(properties+8,type);put(type+0x10,owner);put(properties+0x30,weak);put(weak,properties);
        const std::string name="pc_marine_pve_story_titus_authority";std::array<uint8_t,16> string_header{};std::memcpy(string_header.data(),&text,8);const auto length=uint32_t(name.size());std::memcpy(string_header.data()+8,&length,4);string_header[15]=1;put(owner+0x30,string_header);
        for(size_t i=0;i<name.size();++i)put(text+i,uint8_t(name[i]));
        const uint64_t handle=(uint64_t(469)<<48)|(instance+0x70);put(descriptor+0x100,handle);
        put(base+0x4ff0c08,device);put(device+0x180,int32_t{16});put(device+0x398,stream);put(stream+0x10,uint8_t{0x10});put(stream+0x28,allocator);put(allocator,uint32_t{16});put(allocator+0x5c0,uint32_t{});
        put(allocator+0x280,int32_t{});put(allocator+0x540,int32_t{});put(allocator+0x40,chunk);put(allocator+0x300,uintptr_t{0x390000});
        put(chunk,base+0x37a4840);put(base+0x37a4848,base+0x2100620);put(base+0x37a4850,base+0x2100640);put(chunk+0x18,wrapper);put(wrapper+0x28,native_resource);put(chunk+0x30,cpu);
        put(base+0x2082760,SYNTH(native_object_constants::ALLOCATOR));put(base+0x1f7bcc0,SYNTH(native_object_constants::OBJECT_PRODUCER));put(base+0x1f7c980,SYNTH(native_object_constants::OBJECT_FILL));
        put(descriptor+0x110,uint64_t{native_result});put(descriptor+0x118,uint32_t{16});put(descriptor+0x11c,uint32_t{0x80000010});put(descriptor+0x1f3,uint8_t{});
        std::array<uint32_t,36> words{};words[0]=17;words[2]=3;words[24]=0x3f800000;words[29]=0x3f800000;words[34]=0x3f800000;put(cpu+16*16,words);
        auto read=[&](uintptr_t p,void* out,size_t n){return m.read(p,out,n);};
        probe.m_base=base;probe.m_directory=directory;probe.m_image={};probe.m_image.base=base;probe.m_image.size=0x6000000;probe.m_image.valid=true;probe.m_read=read;probe.m_now=[&]{return now;};
        require(probe.install(reinterpret_cast<void*>(&getter_fixture)),"actual MinHook getter install");
        Probe::Request request{"getter_lease",GetCurrentProcessId(),instance,0,5000};
        const auto identity=native_geometry_identity::capture(descriptor,read);require(native_command_binding::player(identity),"actual mapped player ownership fixture");
        auto lookup=[&]{return probe.lookup(stream,allocator,native_result,16,16,identity,read);};
        auto arm=[&]{probe.disarm();require(probe.arm(request),"production source lease arms");};
        SetLastError(0x55556666);const auto first=getter_call(chunk,0xfedcba9876543210ULL);
        require(first==native_result && native_calls==1 && native_chunk==chunk && native_flags==0xfedcba9876543210ULL && native_incoming==0x55556666 && GetLastError()==0x1234abcd,"unarmed getter preserves exact native args, return, once and errors");
        require(lookup().refusal==Probe::not_armed,"missing control never collects");
        arm();SetLastError(0x55556666);getter_call(chunk,0xfedcba9876543210ULL);
        require(native_calls==2 && native_flags==0xfedcba9876543210ULL && native_incoming==0x55556666 && GetLastError()==0x1234abcd,"armed getter preserves full-width flags and errors");
        auto locator=lookup();require(!locator.refusal && locator.cpu==cpu && locator.chunk==chunk && locator.resource==native_resource && locator.sequence==1,"original return is joined to qualified current source");
        auto sample=native_object_constants::capture(descriptor,identity,probe.m_image,read);
        require(!sample.mask && sample.words==words && sample.getter.generation==locator.generation && sample.getter.sequence==locator.sequence,"production fallback capture copies nine rows only through original-return mapping");
        require(native_calls==2,"lookups never call a native or GPU getter");
        put(allocator+0x280,int32_t{7});put(allocator+0x540,int32_t{7});
        for(unsigned frame=0;frame<2;++frame)for(unsigned i=0;i<8;++i)if(frame || i)put(allocator+frame*0x2c0+0x40+i*8,uintptr_t(0x500000+frame*0x10000+i*0x100));
        arm();getter_call(chunk,0x10);sample=native_object_constants::capture(descriptor,identity,probe.m_image,read);
        require(!sample.mask && sample.words==words && sample.reads<=256,"combined fallback nine-row capture fits outer256read cap at worst8+8 membership including final lookup");
        std::cout<<"worst8+8 combined reads="<<sample.reads<<'\n';
        put(allocator+0x280,int32_t{});put(allocator+0x540,int32_t{});
        arm();getter_call(chunk,0x11);require(lookup().refusal==Probe::missing_return,"view-token bit1 branch not learned");
        arm();getter_call(chunk,0x20);require(lookup().refusal==Probe::missing_return,"other stream flags not learned");
        arm();getter_call(chunk,0x10);put(wrapper+0x28,native_resource+8);require(lookup().refusal==Probe::source_changed,"changed native resource invalidates source");put(wrapper+0x28,native_resource);
        require(lookup().refusal==Probe::poisoned,"observed A-B-A cannot resurrect old return");
        arm();getter_call(chunk,0x10);native_steps=[&]{put(wrapper+0x28,native_resource+8);};getter_call(chunk,0x10);native_steps={};put(wrapper+0x28,native_resource);
        require(lookup().refusal==Probe::poisoned,"known original getter resource A-B-A with unchanged return poisons permanently");
        arm();getter_call(chunk,0x10);native_steps=[&]{put(chunk+0x30,cpu+8);};getter_call(chunk,0x10);native_steps={};put(chunk+0x30,cpu);
        require(lookup().refusal==Probe::poisoned,"known original getter CPU A-B-A with unchanged return poisons permanently");
        arm();getter_call(chunk,0x10);native_steps=[&]{put(instance+0x250,resource+8);};getter_call(chunk,0x10);native_steps={};put(instance+0x250,resource);
        require(lookup().refusal==Probe::poisoned,"known original getter owner-resource A-B-A with unchanged return poisons permanently");
        arm();native_steps=[&]{put(chunk+0x30,cpu+8);};getter_call(chunk,0x10);native_steps={};put(chunk+0x30,cpu);require(lookup().refusal==Probe::missing_return,"source changed during original call refuses learning");
        arm();native_steps=[&]{now+=std::chrono::seconds(6);};getter_call(chunk,0x10);native_steps={};require(lookup().refusal==Probe::expired,"expiry while original runs refuses learning");
        arm();getter_call(chunk,0x10);const auto old_generation=lookup().generation;now+=std::chrono::seconds(6);require(lookup().refusal==Probe::expired,"expired mapping never remains usable");
        arm();native_steps=[&]{probe.disarm();require(probe.arm(request),"replacement source arms inside original fixture");};getter_call(chunk,0x10);native_steps={};require(lookup().refusal==Probe::missing_return && probe.m_generation!=old_generation,"late original result cannot enter replacement generation");
        arm();bool nested=false;native_steps=[&]{if(!nested){nested=true;getter_call(chunk,0x10);}};const auto before_nested=native_calls;getter_call(chunk,0x10);native_steps={};
        require(native_calls==before_nested+2 && !lookup().refusal && probe.m_records.size()==1,"nested exact native calls keep local pending metadata and a single consistent source");
        arm();probe.m_validations=Probe::VALIDATION_LIMIT;getter_call(chunk,0x10);require(lookup().refusal==Probe::exhausted,"validation work exhaustion refuses lookups");
        arm();probe.m_records.resize(Probe::RECORD_LIMIT);getter_call(chunk,0x10);require(lookup().refusal==Probe::exhausted,"bounded metadata record cap refuses collection and lookup");
        arm();getter_call(chunk,0x10);probe.m_calls=Probe::CALL_LIMIT;require(lookup().refusal==Probe::exhausted,"bounded native observation call budget refuses lookups");
        arm();getter_call(chunk,0x10);probe.m_cheap_reads=Probe::CHEAP_READ_LIMIT;getter_call(chunk,0x10);require(lookup().refusal==Probe::exhausted,"known metadata read reservation exhaustion refuses unsampled lifecycle");
        arm();getter_call(chunk,0x10);bool changed_on_rows=false;auto row_change=[&](uintptr_t p,void* out,size_t n){const auto ok=read(p,out,n);if(p==cpu+256 && !changed_on_rows){changed_on_rows=true;put(chunk+0x30,cpu+8);}return ok;};
        sample=native_object_constants::capture(descriptor,identity,probe.m_image,row_change);put(chunk+0x30,cpu);require(sample.mask!=0,"production capture requalifies getter source after CPU rows");
        arm();getter_call(chunk,0x10);bool expired_on_rows=false;auto row_expiry=[&](uintptr_t p,void* out,size_t n){const auto ok=read(p,out,n);if(p==cpu+256 && !expired_on_rows){expired_on_rows=true;now+=std::chrono::seconds(6);}return ok;};
        sample=native_object_constants::capture(descriptor,identity,probe.m_image,row_expiry);require(sample.mask!=0,"production capture refuses request expiry during row read");
        arm();const auto before_throw=native_calls;native_steps=[] {SetLastError(0xccddeeff);throw std::runtime_error("native fixture throw");};
        bool native_threw=false;try{getter_call(chunk,0x10);}catch(const std::runtime_error&){native_threw=true;}native_steps={};
        require(native_threw && native_calls==before_throw+1 && GetLastError()==0xccddeeff && lookup().refusal==Probe::missing_return,"native getter exception propagates once with its outgoing error and no learned return");
        // Entry attestation happens before MinHook installation only. The real
        // hooked entry need not (and must not) equal its original first bytes.
        require(probe.install_game(),"already installed getter does not reattest patched entry bytes");
        probe.disarm();probe.m_seen.clear();probe.m_next={};request.id="prearmed_getter";
        const auto ack=directory/(request.id+".cb_getter_ack.json"),control=directory/"object_constant_getter_request.txt";
        std::filesystem::remove(ack);std::filesystem::remove(control);
        std::ofstream(control)<<request.id<<' '<<GetCurrentProcessId()<<' '<<instance<<" 0 5000\n";
        require(lookup().refusal==Probe::not_armed,"request file alone does not imply pre-arm acknowledgment");probe.poll();
        require(std::filesystem::exists(ack) && probe.current()==Probe::none,"actual production poll acknowledges fully qualified pre-arm");
        getter_call(chunk,0x10);require(!lookup().refusal,"prearmed actual getter can resolve subsequent allocation");const auto replay=probe.m_generation;now+=std::chrono::seconds(6);probe.poll();
        require(probe.m_generation==replay && lookup().refusal==Probe::expired,"same request ID cannot rearm an expired observation");
        Probe::Request parsed;require(Probe::parse("valid_1 123 0x200000 0 5000\n",parsed) && !Probe::parse("bad 123 0x200000 0 5001",parsed) && !Probe::parse("bad 123 0x200000 0 0",parsed) && !Probe::parse("bad 123 0x200000 0 10 extra",parsed),"strict bounded control parser");
        std::filesystem::remove(ack);std::filesystem::remove(control);probe.disarm();MH_DisableHook(reinterpret_cast<void*>(&getter_fixture));MH_RemoveHook(reinterpret_cast<void*>(&getter_fixture));probe.m_original=nullptr;
    }
};
int main(int argc,char** argv){try{require(argc==2,"absolute fixture directory required");ObjectConstantGetterTests::run(argv[1]);std::cout<<"object constant getter PASS\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
