#include "NativePlayerPartVisibility.h"
#include <cstring>
#include <iostream>
#include <thread>
#include <future>
#include <condition_variable>
using Parts=NativePlayerPartVisibility;
void check(bool ok,const char* label){if(!ok){std::cerr<<label<<'\n';std::exit(1);}}
struct Fixture {
    std::map<uintptr_t,uint8_t> memory;Parts parts;EngineArmProbe::Pose pose;
    uintptr_t actor=0x10000,instance=0x20000,props=0x30000,entity=0x40000,controller=0x50000,link=0x60000,wrapper=0x70000,network=0x80000,definition=0x90000,context=0xa0000,buffer=0xb0000;
    uintptr_t resource=0x100000,arena=0x200000,ids=0x300000,tokens=0x310000,indices=0x320000,storage=0x400000,global=0x500000;
    std::vector<std::string> names;std::vector<int16_t> parents,siblings,children,slots;
    Parts::Clock::time_point now{std::chrono::seconds(1)};
    std::string mode="native";bool inside{},current=true;size_t write_count{};std::function<bool(uintptr_t,uint32_t)> write_hook;Parts::Api api;
    template<class T>void put(uintptr_t p,const T& value){auto* b=reinterpret_cast<const uint8_t*>(&value);for(size_t i=0;i<sizeof(T);++i)memory[p+i]=b[i];}
    bool read(uintptr_t p,void* out,size_t n){auto* b=static_cast<uint8_t*>(out);for(size_t i=0;i<n;++i){auto it=memory.find(p+i);if(it==memory.end())return false;b[i]=it->second;}return true;}
    template<class T>T at(uintptr_t p){T out{};check(read(p,&out,sizeof(out)),"fixture read");return out;}
    uintptr_t address(size_t id)const{return storage+slots[id]*0x40;}
    size_t id(std::string_view name)const{return std::find(names.begin(),names.end(),name)-names.begin();}
    static std::vector<std::string> titus(){return {"titus_sholder_armor_L_01","titus_sholder_armor_R_01","head_01","helmet_01","titus_hand_R_01","titus_hand_L_01","titus_torso_01"};}
    // Each leaf becomes leaf_grp > leaf > leaf_LOD1..5, as in the game's templates. Filler parts
    // stand in for the thousands of other outfit variants in a class Marine's template.
    explicit Fixture(std::vector<std::string> leaves=titus(),size_t filler=0){
        pose.owner={actor,entity,controller,wrapper,network,definition,instance,now};pose.buffer=buffer;pose.received=now;
        put(actor+0x20,props);put(actor+0x38,entity);put(actor+0xd8,instance);put(instance+0x58,props);put(instance+0xd0,buffer);
        put(controller+0xe0,link);put(link,instance);put(controller+8,entity);put(controller+0xe8,wrapper);put(controller+0x1e9,uint16_t{0x100});put(wrapper+0x10,instance);put(wrapper+0x2858,network);put(wrapper,context);put(context+8,definition);put(network,definition);
        names={"skinned_geometry"};parents={-1};siblings={-1};children={1};
        for(const auto& leaf:leaves){
            const auto group=static_cast<int16_t>(names.size());names.push_back(leaf+"_grp");parents.push_back(0);siblings.push_back(group+7);children.push_back(group+1);
            names.push_back(leaf);parents.push_back(group);siblings.push_back(group+1);children.push_back(group+2);
            for(int i=1;i<=5;++i){names.push_back(leaf+"_LOD"+std::to_string(i));parents.push_back(group+1);siblings.push_back(i==5?group+2:group+2+i);children.push_back(-1);}
        }
        siblings[names.size()-7]=1;
        for(size_t i=0;i<filler;++i){names.push_back("filler_"+std::to_string(i));parents.push_back(-1);siblings.push_back(-1);children.push_back(-1);}
        const auto n=static_cast<int32_t>(names.size());put(instance+0x250,resource);put(global,arena);put(resource+0x118,ids);put(resource+0x128,tokens);put(resource+0x130,n);put(instance+0xa0,indices);put(instance+0xa8,n);put(instance+0xb0,storage);
        uint32_t token=32;
        for(int16_t i=0;i<n;++i){put(ids+i*2,i);put(tokens+i*4,token);put(arena+token,static_cast<uint16_t>(names[i].size()));for(size_t j=0;j<names[i].size();++j)put(arena+token+2+j,names[i][j]);token+=static_cast<uint32_t>(names[i].size()+4);slots.push_back(static_cast<int16_t>(n-1-i));put(indices+i*2,slots[i]);
            for(size_t b=0;b<0x40;++b)memory[address(i)+b]=0;
            put(address(i)+0x20,uint32_t{0x100000|(names[i].find("_LOD")==std::string::npos?0x100u:0u)});put(address(i)+0x32,std::array<int16_t,4>{parents[i],siblings[i],0,children[i]});}
        parts.arena_global_for_test(global);
        api={[this](auto p,void* out,size_t n){return read(p,out,n);},[this](auto p,uint32_t value){check(inside,"write under native instance lock");check(p>=storage+0x20&&(p-storage-0x20)%0x40==0&&(p-storage-0x20)/0x40<names.size(),"writer can touch only runtime record+20, never camera/pose/FPS flags");++write_count;if(write_hook)return write_hook(p,value);put(p,value);return true;},
            [this](const auto&){check(!inside,"no Body/ArmProbe lease mutex under native CS");return current;},
            [this](auto p){check(p==instance&&!inside,"enter exact owned instance");inside=true;},[this](auto){check(inside,"balanced leave");inside=false;},[this]{return now;},[this](const auto&){return Parts::parse(mode);}};parts.configure(api);
        check(parts.control(std::filesystem::path{"C:/Repos/sm2-vr/build/test-player-parts-control.txt"}),"absolute opt-in control");
    }
    void tick(std::string value){mode=value;now+=std::chrono::milliseconds(1100);pose.received=now;pose.owner.received=now;parts.apply(pose);}
    uint32_t flags(std::string_view name){return at<uint32_t>(address(id(name))+0x20);}
    // Rollback restores persistent state and invalidates derived properties;
    // cache readiness is not a saved original and may be rebuilt by native code.
    bool same_except_render_cache(std::map<uintptr_t,uint8_t> original){
        auto current=memory;
        for(size_t i=0;i<names.size();++i){const auto byte=address(i)+0x21;
            original[byte]&=~uint8_t{1};current[byte]&=~uint8_t{1};}
        return current==original;
    }
    void native_rebuild(std::string_view name){const auto p=address(id(name))+0x20;
        put(p,at<uint32_t>(p)|Parts::CACHE_READY);}
};
int main(){
    {Fixture f;const auto helmet=f.id("helmet_01");const auto helmet_flags=f.address(helmet)+0x20;
        f.put(helmet_flags,f.at<uint32_t>(helmet_flags)|Parts::SHADOW);const auto before=f.memory;
        f.tick("head");auto s=f.parts.snapshot();check(s.applied==Parts::Mode::Head&&s.changes.size()==12&&s.writes==12,"head mode applies exactly two named leaf subtrees");
        std::set<uint16_t> selected;for(auto leaf:{"head_01","helmet_01"})for(size_t i=f.id(leaf);i<=f.id(leaf)+5;++i)selected.insert(static_cast<uint16_t>(i));
        for(const auto& change:s.changes)check(selected.contains(change.id),"head mode never selects shoulders, hands, torso or group ancestors");
        auto expected=before;for(auto id:selected){const auto p=f.address(id)+0x20;uint32_t value{};auto* bytes=reinterpret_cast<uint8_t*>(&value);for(size_t i=0;i<sizeof(value);++i)bytes[i]=before.at(p+i);value=(value&~Parts::CACHE_READY)|Parts::SHADOW;for(size_t i=0;i<sizeof(value);++i)expected[p+i]=bytes[i];}
        check(f.memory==expected,"head production path changes only selected runtime flags, no pose/root/camera bytes");
        for(auto leaf:{"head_01","helmet_01"})f.native_rebuild(leaf);const auto writes=f.write_count;f.tick("head");check(f.write_count==writes&&f.parts.snapshot().conflicts==0,"head steady lease accepts native cache regeneration without reasserting");
        f.tick("native");check(f.same_except_render_cache(before)&&f.parts.snapshot().restored==1,"head reset preserves original shadow bits and invalidates derived caches");
        check(f.flags("titus_sholder_armor_L_01")==0x100100&&f.flags("titus_hand_R_01")==0x100100,"head reset leaves shoulders and tracked hand fully native");}
    {Fixture f;f.put(f.indices+f.id("helmet_01")*2,int16_t{-1});const auto before=f.memory;f.tick("head");check(f.memory==before&&f.write_count==0&&!f.parts.snapshot().applied,"head missing helmet mapping refuses entire selection before writes");}
    {Fixture f;const auto before=f.memory;f.tick("native");check(f.memory==before&&f.write_count==0,"default native has no writes");f.tick("left_shoulder");auto s=f.parts.snapshot();check(s.applied==Parts::Mode::LeftShoulder&&s.changes.size()==6,"actual named leaf subtree applies six records");check((f.flags("titus_sholder_armor_L_01")&Parts::MASK)==Parts::SHADOW,"left primary shadow only");check((f.flags("titus_hand_R_01")&(Parts::MASK|Parts::CACHE_READY))==Parts::CACHE_READY,"unrelated hand stays native including cache readiness");
        auto p=f.address(f.id("titus_sholder_armor_L_01"))+0x20;f.put(p,f.at<uint32_t>(p)|0x40000000);f.tick("native");check((f.flags("titus_sholder_armor_L_01")&(Parts::MASK|Parts::CACHE_READY))==0,"restore original shadow bit and invalidate native cache");check(f.flags("titus_sholder_armor_L_01")&0x40000000,"restore preserves independently changed unrelated flag");check(f.parts.snapshot().restored==1,"restore receipt");}
    {Fixture f;f.tick("left_shoulder");f.tick("shoulders_head");check(f.parts.snapshot().changes.size()==24,"combined exact four leaf scopes");f.tick("right_hand");check((f.flags("titus_sholder_armor_L_01")&(Parts::MASK|Parts::CACHE_READY))==0,"switch restores old scope and invalidates properties");check((f.flags("titus_hand_R_01")&Parts::MASK)==Parts::SHADOW,"switch applies actual hand scope");f.tick("reset");check((f.flags("titus_hand_R_01")&(Parts::MASK|Parts::CACHE_READY))==0,"reset returns saved hand shadow state and invalidates properties");}
    {Fixture f;f.tick("left_shoulder");const auto writes=f.write_count;f.tick("left_shoulder");check(f.write_count==writes,"steady applied mode verifies lease without rewriting flags");check(f.parts.control({}),"explicit control disable schedules native restore");f.pose.received=f.now;f.parts.apply(f.pose);check((f.flags("titus_sholder_armor_L_01")&(Parts::MASK|Parts::CACHE_READY))==0&&!f.parts.enabled()&&f.parts.snapshot().applied==Parts::Mode::Native,"clear control restores owned shadow bit with cache invalidation then disables");}
    {Fixture f;f.tick("left_shoulder");const auto writes=f.write_count;
        // Actual live failure: primary changes 0x218081c -> 0x218091c after
        // native 0x20bbb50 rebuilds properties; durable 0x800 remains intact.
        f.native_rebuild("titus_sholder_armor_L_01");f.tick("left_shoulder");
        auto s=f.parts.snapshot();check(s.applied==Parts::Mode::LeftShoulder&&s.conflicts==0&&f.write_count==writes,"native regenerated100 is not an ownership conflict and causes no steady writes");
        check(f.flags("titus_sholder_armor_L_01")&Parts::CACHE_READY,"steady mode preserves rebuilt native properties");
        f.tick("native");check((f.flags("titus_sholder_armor_L_01")&(Parts::MASK|Parts::CACHE_READY))==0&&f.parts.snapshot().restored==1,"reset clears shadow bit and invalidates regenerated cache rather than restoring ready100");
        f.native_rebuild("titus_sholder_armor_L_01");const auto reset_writes=f.write_count;f.tick("native");check(f.write_count==reset_writes&&(f.flags("titus_sholder_armor_L_01")&Parts::CACHE_READY),"native rebuild after reset stays intact with no outstanding lease");}
    {Fixture f;const auto p=f.address(f.id("titus_sholder_armor_L_01"))+0x20;f.put(p,f.at<uint32_t>(p)|Parts::SHADOW);
        f.tick("left_shoulder");f.native_rebuild("titus_sholder_armor_L_01");f.tick("native");check((f.flags("titus_sholder_armor_L_01")&(Parts::MASK|Parts::CACHE_READY))==Parts::SHADOW,"reset preserves pre-existing original shadow bit while invalidating cache");}
    for(auto bad:{"owner","resource","index","topology","mask"}){Fixture f;f.tick("left_shoulder");auto writes=f.write_count;
        if(std::string_view{bad}=="owner")f.pose.owner.actor+=0x1000;
        if(std::string_view{bad}=="resource")f.put(f.instance+0x250,f.resource+0x1000);
        if(std::string_view{bad}=="index")f.put(f.indices+f.id("titus_sholder_armor_L_01")*2,int16_t{-1});
        if(std::string_view{bad}=="topology")f.put(f.address(f.id("titus_sholder_armor_L_01"))+0x38,int16_t{-1});
        if(std::string_view{bad}=="mask"){auto p=f.address(f.id("titus_sholder_armor_L_01"))+0x20;f.put(p,f.at<uint32_t>(p)&~Parts::MASK);}
        f.tick("native");check(f.write_count==writes&&!f.parts.snapshot().applied,"changed owner/resource/record/ownedmask refuses restoration without writes");}
    {Fixture f;f.tick("left_shoulder");const auto before=f.memory;f.mode="invalid";f.tick("invalid");check(f.parts.snapshot().control_status=="invalid"&&f.memory==before,"invalid setting retains last valid state");}
    {Fixture f;auto original=f.memory;size_t calls{};f.write_hook=[&](auto p,uint32_t value){++calls;if(calls==3)return false;f.put(p,value);return true;};f.tick("left_shoulder");check(f.same_except_render_cache(original),"partial batch rollback restores persistent state when writes recover");check(!(f.flags("titus_sholder_armor_L_01")&Parts::CACHE_READY),"rollback invalidates properties rather than restoring saved ready100");check(f.parts.snapshot().failures==1&&f.parts.snapshot().rollback_failures==0,"failure distinct from best effort recovery");}
    {Fixture f;const auto original=f.memory;auto api=f.api;bool failed{};const auto second=f.address(f.id("titus_sholder_armor_L_01_LOD1"))+0x20;
        api.read=[&](auto p,void* out,size_t n){if(f.write_count==1&&p==second&&!failed){failed=true;return false;}return f.read(p,out,n);};f.parts.configure(api);f.tick("left_shoulder");check(failed&&f.same_except_render_cache(original),"per-patch read exception after first write rolls back acquired mask");f.tick("native");check(f.same_except_render_cache(original),"native reset after per-patch read failure cannot leave lost first-acquisition mask");}
    {Fixture f;const auto original=f.memory;bool thrown{};f.write_hook=[&](auto p,uint32_t value){f.put(p,value);if(!thrown){thrown=true;throw std::runtime_error{"fixture writer exception"};}return true;};f.tick("left_shoulder");check(f.same_except_render_cache(original)&&f.parts.snapshot().failures==1,"exception from partial writer runs transaction recovery");}
    {Fixture f;const auto original=f.memory;size_t calls{};f.write_hook=[&](auto p,uint32_t value){++calls;if(calls==2)return false;f.put(p,value);if(calls==1)f.native_rebuild("titus_sholder_armor_L_01");return true;};
        f.tick("left_shoulder");check(f.same_except_render_cache(original)&&f.parts.snapshot().rollback_failures==0,"native cache regeneration after first write does not block persistent shadow-bit rollback");check(!(f.flags("titus_sholder_armor_L_01")&Parts::CACHE_READY),"rollback invalidates regenerated mesh properties");}
    {Fixture f;size_t calls{};f.write_hook=[&](auto p,uint32_t value){if(++calls>=3)return false;f.put(p,value);return true;};f.tick("left_shoulder");check(f.parts.snapshot().rollback_failures>0&&!f.parts.snapshot().applied,"persistent failure not claimed restored");const auto writes=f.write_count;f.write_hook={};f.tick("left_shoulder");f.tick("native");check(f.write_count==writes,"unrecoverable partial flag batch cannot relearn modified bits as native defaults");}
    {Fixture f;size_t reads{};auto api=f.api;api.control=[&](const auto&){++reads;return Parts::parse("left_shoulder");};Parts disabled;disabled.configure(api);disabled.apply(f.pose);check(reads==0&&f.write_count==0,"default disabled consumes no control/owner/memory API");}
    for(auto bad:{"missing","ambiguous","unmapped","alias","cycle","count"}){Fixture f;const auto left=f.id("titus_sholder_armor_L_01"),right=f.id("titus_hand_L_01");
        if(std::string_view{bad}=="missing")f.put(f.tokens+left*4,f.at<uint32_t>(f.tokens+right*4));
        if(std::string_view{bad}=="ambiguous")f.put(f.tokens+right*4,f.at<uint32_t>(f.tokens+left*4));
        if(std::string_view{bad}=="unmapped")f.put(f.indices+left*2,int16_t{-1});
        if(std::string_view{bad}=="alias")f.put(f.indices+right*2,f.slots[left]);
        if(std::string_view{bad}=="cycle")f.put(f.address(left)+0x38,static_cast<int16_t>(left));
        if(std::string_view{bad}=="count")f.put(f.resource+0x130,int32_t{4097});
        f.tick("left_shoulder");check(f.write_count==0&&!f.parts.snapshot().applied,"unknown native name/mapping/topology refuses before writes");}
    {Fixture f;auto api=f.api;api.enter=[&](auto){f.inside=true;f.put(f.actor+0xd8,f.instance+0x1000);};f.parts.configure(api);f.tick("left_shoulder");check(f.write_count==0&&!f.parts.snapshot().applied,"owner changes after prelock lease are rechecked by RPM insideCS");}
    {Fixture f;auto api=f.api;api.control=[&](const auto&){f.now+=std::chrono::milliseconds(200);return Parts::parse("left_shoulder");};f.parts.configure(api);f.tick("left_shoulder");check(f.write_count==0&&!f.parts.snapshot().applied,"actual freshness sampled after control I/O");}
    {Fixture f;const auto path=std::filesystem::absolute("sm2vr_parts_control_fixture.txt");std::filesystem::remove(path);auto api=f.api;api.control=Parts::read_control;f.parts.configure(api);f.parts.control(path);f.tick("unused");check(f.parts.snapshot().control_status=="unavailable","missing actual file is observable");
        {std::ofstream out{path,std::ios::binary};out<<"left_shoulder\r\n";}f.tick("unused");check(f.parts.snapshot().applied==Parts::Mode::LeftShoulder,"real bounded file reader drives actual native records");
        {std::ofstream out{path,std::ios::binary};out<<std::string(64,'x');}f.tick("unused");check(f.parts.snapshot().control_status=="oversized"&&(f.flags("titus_sholder_armor_L_01")&Parts::MASK)==Parts::SHADOW,"oversized file retains last valid mode");
        {std::ofstream out{path,std::ios::binary};out<<"native";}f.tick("unused");check(f.parts.snapshot().applied==Parts::Mode::Native&&(f.flags("titus_sholder_armor_L_01")&(Parts::MASK|Parts::CACHE_READY))==0,"real file reset restores native owned bits and invalidates cache");std::filesystem::remove(path);}
    {Fixture f;f.current=false;const auto original=f.memory;f.tick("left_shoulder");check(f.memory==original&&!f.parts.snapshot().applied,"prelock publication refusal no writes");}
    {Fixture f;auto previous=f.parts.snapshot();check(!f.parts.control("relative.txt"),"relative path refuses");for(auto text:{" native","left_shoulder extra","left_shoulder\nextra","LEFT_SHOULDER",""})check(!Parts::parse(text).mode,"strict scalar mode schema");check(Parts::parse("right_hand\r\n").mode==Parts::Mode::RightHand,"CRLF supported");}
    {Fixture f;std::mutex mutex;std::condition_variable cv;bool entered{},release{};std::atomic<int> polls{};
        f.parts.configure({[&](auto p,void* out,size_t n){return f.read(p,out,n);},[&](auto p,uint32_t value){f.put(p,value);return true;},[&](const auto&){return true;},[](auto){},[](auto){},[&]{return f.now;},[&](const auto&){if(++polls==1){std::unique_lock lock{mutex};entered=true;cv.notify_all();cv.wait(lock,[&]{return release;});return Parts::parse("left_shoulder");}return Parts::parse("native");}});
        auto old=f.pose;std::thread a([&]{f.parts.apply(old);});{std::unique_lock lock{mutex};cv.wait(lock,[&]{return entered;});}f.now+=std::chrono::milliseconds(1500);f.pose.received=f.now;f.parts.apply(f.pose);{std::lock_guard lock{mutex};release=true;}cv.notify_all();a.join();f.pose.received=f.now;f.parts.apply(f.pose);check(f.parts.snapshot().requested==Parts::Mode::Native&&f.parts.snapshot().superseded==1,"late older mode cannot override newer native reset");check((f.flags("titus_sholder_armor_L_01")&(Parts::MASK|Parts::CACHE_READY))==Parts::CACHE_READY,"production late read cannot reactivate part masks");}
    // Production first-person policy: head and helmet hidden, pauldrons kept.
    {Fixture f;check(f.parts.control({}),"clear diagnostic control");const auto before=f.memory;
        f.tick("unused");check(f.write_count==0&&!f.parts.enabled(),"native policy without a control file has no writes");
        f.parts.set_policy(Parts::Mode::Head);check(f.parts.enabled(),"head policy enables without a control file");
        f.tick("unused");auto s=f.parts.snapshot();check(s.applied==Parts::Mode::Head&&s.changes.size()==12,"head policy hides exactly the head and helmet subtrees");
        check(f.flags("titus_sholder_armor_L_01")==0x100100&&f.flags("titus_sholder_armor_R_01")==0x100100,"head policy keeps both pauldrons native");
        check((f.flags("head_01")&Parts::MASK)==Parts::SHADOW&&(f.flags("helmet_01")&Parts::MASK)==Parts::SHADOW,"head and helmet become shadow-only");
        f.parts.set_policy(Parts::Mode::Native);f.tick("unused");check(f.same_except_render_cache(before)&&!f.parts.enabled(),"leaving first person restores the native head and disables");}
    {Parts unbound;unbound.set_policy(Parts::Mode::Head);check(!unbound.enabled()&&unbound.snapshot().requested==Parts::Mode::Native,"policy is inert until the native API is bound");}
    {Fixture f;const auto before=f.memory;f.parts.set_policy(Parts::Mode::Head);f.tick("native");
        check(f.memory==before&&f.write_count==0&&f.parts.snapshot().requested==Parts::Mode::Native,"diagnostic control file overrides the head policy");
        f.parts.control({});f.tick("unused");check(f.parts.snapshot().applied==Parts::Mode::Head,"clearing the diagnostic falls back to the policy");}
    {Fixture f;f.parts.control({});f.parts.set_policy(Parts::Mode::Head);f.tick("unused");
        const auto actor2=f.actor+0x1000;f.put(actor2+0x20,f.props);f.put(actor2+0x38,f.entity);f.put(actor2+0xd8,f.instance);f.pose.owner.actor=actor2;
        f.tick("unused");auto s=f.parts.snapshot();check(s.abandoned==1&&s.applied==Parts::Mode::Head&&s.lost_lease==0,"head policy follows a replacement player actor");
        f.parts.set_policy(Parts::Mode::Native);f.tick("unused");
        check((f.flags("helmet_01")&Parts::MASK)==0&&(f.flags("head_01")&Parts::MASK)==0,"shared records keep their true native originals across actor replacement");}
    // Class Marines (hangar, battle simulator, operations) use outfit templates: thousands of parts,
    // every helmet and head variant, no head_01/helmet_01. Playtest 30 Oct: the head policy refused
    // them all run long (unknown_resource_layout), so only the bone-scale tiny head was left.
    for(auto name:{"head_01_grp","berserk_head_01_grp","helmet_grp","helmet_T_lamenters_grp","soldier_lenses_01_grp","tank_hood_01_grp"})check(Parts::head_group(name),"head, helmet, hood and lenses groups are head parts");
    for(auto name:{"head_01","support_backpack_halo_01_grp","titus_sholder_armor_L_01_grp","headgear_01_grp","belt_vanity_exorcist_G_01_head_A_01_jiggle","soldier_torso_01_grp"})check(!Parts::head_group(name),"backpack halos, pauldrons, torso, bones and partial words are not");
    const std::vector<std::string> outfit{"soldier_shoulder_L_01","head_salamander_01","soldier_helmet_01","soldier_helmet_02","soldier_lenses_01","tank_hood_01","support_backpack_halo_01","soldier_torso_01","soldier_hand_R_01"};
    {Fixture f{outfit,4200};check(f.parts.control({}),"production policy, no diagnostic file");const auto before=f.memory;
        check(f.names.size()>4096,"outfit fixture is larger than the old 4096 cap");
        f.parts.set_policy(Parts::Mode::Head);f.tick("unused");auto s=f.parts.snapshot();
        check(s.applied==Parts::Mode::Head&&s.status=="applied","head policy applies on an outfit template");
        check(s.groups==5&&s.changes.size()==35&&s.name_count==static_cast<int32_t>(f.names.size())&&s.record_count==static_cast<int32_t>(f.names.size()),"five head groups, seven records each, counts logged");
        std::set<uint16_t> head;for(auto leaf:{"head_salamander_01","soldier_helmet_01","soldier_helmet_02","soldier_lenses_01","tank_hood_01"})for(size_t i=f.id(std::string{leaf}+"_grp");i<=f.id(leaf)+5;++i)head.insert(static_cast<uint16_t>(i));
        for(const auto& change:s.changes)check(head.contains(change.id),"only head, helmet, lenses and hood subtrees are selected");
        for(auto id:head)check((f.at<uint32_t>(f.address(id)+0x20)&Parts::MASK)==Parts::SHADOW,"every selected part is shadow-only");
        for(auto leaf:{"soldier_shoulder_L_01","support_backpack_halo_01","soldier_torso_01","soldier_hand_R_01","filler_4199"})check((f.flags(leaf)&Parts::MASK)==0,"pauldron, backpack, torso, hand and other parts stay drawn");
        f.parts.set_policy(Parts::Mode::Native);f.tick("unused");check(f.same_except_render_cache(before)&&f.parts.snapshot().restored==1,"leaving first person restores every outfit part");}
    {Fixture f{outfit,0};f.parts.control({});for(size_t i=f.id("soldier_helmet_02_grp");i<=f.id("soldier_helmet_02")+5;++i){const auto p=f.address(i)+0x1c;f.put(p,f.at<uint32_t>(p)|Parts::OUTFIT_HIDDEN);}
        f.parts.set_policy(Parts::Mode::Head);f.tick("unused");check(f.parts.snapshot().outfit_hidden==7&&f.parts.snapshot().changes.size()==35,"variants the outfit hides are counted and still selected (a helmet change stays covered)");}
    {Fixture f{{"soldier_shoulder_L_01","soldier_torso_01"},0};f.parts.control({});const auto before=f.memory;f.parts.set_policy(Parts::Mode::Head);f.tick("unused");
        check(f.memory==before&&f.write_count==0&&f.parts.snapshot().status=="selected_head_parts_absent","an outfit with no head parts refuses before writes");}
    {Fixture f;f.parts.control({});f.parts.set_policy(Parts::Mode::Head);f.tick("unused");check(f.parts.snapshot().groups==2&&f.parts.snapshot().changes.size()==12,"Titus keeps the exact head_01/helmet_01 leaves");}
    for(auto [field,status]:std::initializer_list<std::pair<const char*,std::string_view>>{{"resource","layout_no_resource"},{"arena","layout_no_arena"},{"storage","layout_no_storage"},{"names","layout_name_count"},{"records","layout_record_count"}}){
        Fixture f;f.parts.control({});const std::string_view which{field};
        if(which=="resource")f.put(f.instance+0x250,uintptr_t{});if(which=="arena")f.put(f.global,uintptr_t{});if(which=="storage")f.put(f.instance+0xb0,uintptr_t{});
        if(which=="names")f.put(f.resource+0x130,int32_t{0x8000});if(which=="records")f.put(f.instance+0xa8,int32_t{-1});
        f.parts.set_policy(Parts::Mode::Head);f.tick("unused");auto s=f.parts.snapshot();
        check(s.status==status&&f.write_count==0,"a refused layout names the field that failed");
        if(which=="names")check(s.name_count==0x8000,"the refused count is logged");}
    {Fixture f{outfit,100};f.parts.control({});size_t name_reads{};auto api=f.api;api.read=[&](auto p,void* out,size_t n){if(p>=f.arena&&p<f.arena+0x100000)++name_reads;return f.read(p,out,n);};f.parts.configure(api);
        f.parts.set_policy(Parts::Mode::Head);f.tick("unused");check(name_reads>0&&f.parts.snapshot().applied==Parts::Mode::Head,"first hide reads the part names");
        f.parts.set_policy(Parts::Mode::Native);f.tick("unused");name_reads=0;f.parts.set_policy(Parts::Mode::Head);f.tick("unused");
        check(name_reads==0&&f.parts.snapshot().applied==Parts::Mode::Head,"hiding again after a takeover reuses the names of the same resource");
        f.parts.set_policy(Parts::Mode::Native);f.tick("unused");f.put(f.tokens+4,f.at<uint32_t>(f.tokens+8));f.parts.set_policy(Parts::Mode::Head);name_reads=0;f.tick("unused");
        check(name_reads>0,"a changed token table reads the names again");}
    {Fixture f{outfit,0};f.parts.control({});size_t reads{};auto api=f.api;api.read=[&](auto p,void* out,size_t n){++reads;return f.read(p,out,n);};f.parts.configure(api);
        f.parts.set_policy(Parts::Mode::Head);f.tick("unused");reads=0;f.tick("unused");check(f.parts.snapshot().applied==Parts::Mode::Head&&reads<40,"a steady lease checks its records with a few bounded reads, not one per record");}
    std::cout<<"Native player part visibility tests passed\n";
}

