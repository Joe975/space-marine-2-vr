#include "EngineChainswordRev.h"
#include "VrControllerInput.h"
#include "VrFreshnessTestAccess.h"
#include <map>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
volatile unsigned native_updates{};uintptr_t native_property{};
__declspec(noinline) void native_animation_update(uintptr_t property){native_property=property;native_updates=native_updates+1;}
std::chrono::steady_clock::time_point request_time{};
const VrControllerInput* clock_input{};
unsigned request_clock_reads{};
std::chrono::steady_clock::time_point request_clock(){
    if(clock_input)require(VrFreshnessTestAccess::held(*clock_input),"actual controller clock must execute under its publication mutex");
    ++request_clock_reads;return request_time;
}
// Model a publisher completing while the consumer waits to acquire its lock.
// No sleeps or scheduler assumptions: lock() performs that exact interleaving.
struct PublicationLock {
    bool held{};std::function<void()> publish;
    void lock(){require(!held,"fixture lock cannot recurse");publish();held=true;}
    void unlock(){require(held,"fixture unlock balances lock");held=false;}
};
int main(){try{
    {
        EngineChainswordAudio audio;bool ready=true,registration=true,post_ok=true;unsigned registrations{},posts{},audio_stops{},unregistrations{};uint32_t next=100;std::vector<uint32_t> stopped;std::vector<std::string> events;
        audio.bind({[&]{return ready;},[&](auto id){require(id==audio.object(),"audio uses its own unique object ID");++registrations;return registration;},[&](const char* name,auto id){require(id==audio.object(),"sound never uses the game's emitter");++posts;events.emplace_back(name);return post_ok?++next:0;},[&](auto id){require(id>=101,"only stops IDs returned by our own event posts");stopped.push_back(id);++audio_stops;},[&](auto id){require(id==audio.object(),"only unregisters own emitter");++unregistrations;},[&](auto object,auto id){return object==audio.object() && id==101;},[](auto){return 1;}});
        audio.update(false);require(!posts && !registrations,"idle does not register an emitter");
        audio.update(true);audio.update(true);require(registrations==1 && posts==1 && audio.snapshot().loop==101 && events[0]=="wpn_melee_chswd_charge_loop_start","held trigger posts native loop once");
        require(audio.snapshot().playing && audio.snapshot().listeners==1,"playback diagnostics query the exact returned ID and own listener route");
        audio.update(false);audio.update(false);require(audio_stops==1 && stopped[0]==101 && posts==2 && events[1]=="wpn_melee_chswd_charge_loop_stop","release stops own loop and posts one native tail");
        audio.update(true);require(stopped.back()==102 && posts==3 && registrations==1,"next hold cancels only our preceding tail and reuses own object");
        audio.reset();require(stopped.back()==103 && unregistrations==1,"reset stops own sound and unregisters");
        ready=false;audio.update(true);require(posts==3,"reset leaves audio unbound");
        audio.bind({[&]{return ready;},[&](auto){++registrations;return registration;},[&](const char*,auto){++posts;return post_ok?++next:0;},[&](auto){++audio_stops;},[&](auto){++unregistrations;}});
        audio.update(true);ready=true;audio.update(true);require(posts==3,"engine recovery while held must wait for release");
        audio.update(false);registration=false;audio.update(true);audio.update(true);require(posts==3 && audio.snapshot().failures==1,"registration failure is not retried every animation frame");
        audio.update(false);registration=true;post_ok=false;audio.update(true);audio.update(true);require(posts==4 && !audio.snapshot().loop && audio.snapshot().failures==2,"rejected event fails closed without repeated posts");
        audio.update(false);require(posts==4,"a rejected loop cannot produce a release tail");audio.reset();
    }
    constexpr uintptr_t base=0x10000000,actor=0x20000,player_geometry=0x30000,prop=0x40000,props=0x50000,geometry=0x60000;
    std::map<uintptr_t,unsigned char> memory;
    const auto bytes=[&](uintptr_t p,const void* data,size_t n){for(size_t i=0;i<n;++i)memory[p+i]=static_cast<const unsigned char*>(data)[i];};
    const auto put=[&]<class T>(uintptr_t p,T value){bytes(p,&value,sizeof value);};
    const auto reader=[&](uintptr_t p,void* out,size_t n){for(size_t i=0;i<n;++i){const auto it=memory.find(p+i);if(it==memory.end())return false;static_cast<unsigned char*>(out)[i]=it->second;}return true;};
    put(base+0x4f17758,uintptr_t{0x70000});put(0x70940,uintptr_t{0x80000});put(0x70948,uint32_t{1});put(0x7094c,uint32_t{1});put(0x80000,prop);
    put(prop,base+0x363fd10);put(prop+0x20,props);put(prop+0x30,geometry);put(actor+0xd8,player_geometry);put(geometry+0x58,props);put(geometry+0x68,uintptr_t{0x140000});
    put(props+8,uintptr_t{0x90000});put(0x90010,uintptr_t{0xa0000});
    EngineChainswordRev::String long_name{std::string_view{}};uintptr_t name=0xb0000;uint32_t length=20;
    std::memcpy(long_name.bytes.data(),&name,8);std::memcpy(long_name.bytes.data()+8,&length,4);long_name.bytes[15]=1;
    bytes(0xa0030,&long_name,16);bytes(name,"wpn_melee_chainsword",length);
    put(base+0x4e0e470,uintptr_t{0xc0000});put(base+0x4e0e478,int{1});put(0xc0000,uint16_t{7});
    put(props+0x28,uintptr_t{0xd0000});put(0xd0010,uintptr_t{0xe0000});put(0xe000e,uint16_t{0});put(props+0x20,uint32_t{1});put(props+0x18,uintptr_t{0xf0000});put(0xf0000,uintptr_t{0x100000});
    put(0x1000d8,geometry);put(0x100038,uintptr_t{0x110000});put(0x110020,uintptr_t{0x120000});put(0x120000,uintptr_t{0x130000});put(0x130048,base+0x134c2e0);put(0x1200d8,(uintptr_t{0x123}<<48)|player_geometry);
    const auto string=[](const EngineChainswordRev::String& s){return std::string{reinterpret_cast<const char*>(s.bytes.data()),static_cast<size_t>(15-(s.bytes[15]>>1))};};
    unsigned starts{},stops{},exists{};std::string last;bool supported=true;
    auto& rev=EngineChainswordRev::get();rev.bind(base,{reader,[&](auto p,const auto& s){++exists;require(p==prop,"exists on exact owned property");return supported && (string(s)=="chain_anim" || string(s)=="chain_anim_stop");},
        [&](auto p,const auto& s,float blend,const auto& mode,bool immediate){++starts;last=string(s);require(p==prop && blend==0 && !immediate,"native start ABI uses blend float and deferred queue");require(string(mode)==(last=="chain_anim"?"cycle":"clamp"),"native playback mode matches start/stop");},
        [&](auto p,const auto& s){++stops;require(p==prop && string(s)=="chain_anim","stops only owned chain animation");},[](auto){return 1.25f;}});
    const auto now=EngineChainswordRev::Clock::now();
    rev.tick(actor,actor,0,now);require(!starts && !stops,"idle emits no animation call");
    rev.native_update(prop+8,{actor,.8f},now);require(!starts,"unrelated native property update cannot consume a rev request");
    rev.native_update(prop,{actor,.8f},now);require(starts==1 && last=="chain_anim" && rev.snapshot().base_layer_time==1.25f,"owned native animation update starts actual tooth animation; base time is separate telemetry");
    rev.tick(actor,actor,.8f,now);require(starts==1 && exists==2,"holding does not restart animation or repeat lookup");
    rev.tick(actor,actor,0,now);require(stops==1 && starts==2 && last=="chain_anim_stop" && !rev.snapshot().property,"release queues native stop and deceleration once");
    rev.tick(actor,actor,0,now);require(stops==1,"released trigger does not repeat stop");
    supported=false;rev.tick(actor,actor,1,now);require(starts==2,"missing authored animation fails closed");supported=true;
    rev.tick(actor,actor,1,now);put(0x1200d8,(uintptr_t{0x123}<<48)|(player_geometry+8));rev.tick(actor,actor,0,now);
    require(!rev.snapshot().property && stops==1,"reparented weapon is discarded without calling its old pointer");
    rev.tick(actor,actor,1,now);require(starts==3,"foreign geometry cannot be started");
    put(0x1200d8,(uintptr_t{0x123}<<48)|player_geometry);rev.tick(actor,actor+1,1,now);require(starts==3,"foreign observed actor cannot start effects");
    VrControllerInput input;input.enable(true);input.update(VrControllerSample{},0,true,now);
    VrControllerSample sample;sample.chainsword_rev=.8f;input.update(sample,0,true,now+std::chrono::milliseconds(1));auto pad=input.snapshot(now+std::chrono::milliseconds(2));
    require(pad.chainsword_rev==.8f && !pad.pad.Gamepad.bRightTrigger && !(pad.pad.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER),"rev stays separate from native firearm/melee actions");
    require(!input.snapshot(now+std::chrono::milliseconds(102)).chainsword_rev,"stale input releases rev");
    input.update(sample,0,false,now+std::chrono::milliseconds(3));require(!input.snapshot(now+std::chrono::milliseconds(4)).chainsword_rev,"focus loss releases rev");
    {
        using namespace std::chrono;
        VrControllerInput held(request_clock);held.enable(true);held.update(VrControllerSample{},0,true,now);
        auto time=now+milliseconds(10);bool published{};
        PublicationLock publication{false,[&]{held.update(sample,0,true,time+milliseconds(1));published=true;}};
        const auto request=[&]{
            published=false;
            return locked_clock_read(publication,[&]{return time+milliseconds(published?2:0);},[&](auto read_time){
                return EngineChainswordRev::Request{actor,held.snapshot(read_time).chainsword_rev};
            });
        };
        const auto before_starts=rev.snapshot().starts,before_stops=rev.snapshot().stops;
        rev.native_update(prop,{actor,.8f},time);
        for(int i=0;i<480;++i){
            // The old request sampled 'time' first, then read a sample published at
            // time+1ms under contention: a fresh held trigger became a stop request.
            held.update(sample,0,true,time+milliseconds(1));
            require(!held.snapshot(time).chainsword_rev,"pre-lock timestamp reproduces false future-sample rev release");
            rev.native_update(prop,request(),time+milliseconds(2));
            // An uncontended next read would restart the old false stop, matching
            // the intermittent start/stop cycles in the recorded live hold.
            rev.native_update(prop,{actor,held.snapshot(time+milliseconds(2)).chainsword_rev},time+milliseconds(2));
            time+=milliseconds(20);
        }
        require(rev.snapshot().starts==before_starts+1 && rev.snapshot().stops==before_stops,
            "480 held requests remain one native animation start despite publisher/clock interleaving");
        clock_input=&held;
        request_time=time+milliseconds(2);
        require(held.snapshot().chainsword_rev==.8f,"actual default controller API uses its publication-locked clock");
        XINPUT_STATE current_pad{};uint32_t current_result=ERROR_DEVICE_NOT_CONNECTED;
        const auto before_reads=request_clock_reads;
        held.get_state(&current_result,0,&current_pad);
        require(current_result==ERROR_SUCCESS && request_clock_reads==before_reads+1,
            "actual default native-pad API uses the current snapshot clock");
        require(held.allows_body_motion(true,request_clock),"current body permission also samples time under the input lock");
        require(held.snapshot_current(request_clock).chainsword_rev==.8f,"current input reads use the locked injected clock");
        request_time=time-milliseconds(20);
        require(!held.snapshot_current(request_clock).chainsword_rev,"genuinely future sample still releases rev");
        request_time=time+milliseconds(102);
        require(!held.snapshot_current(request_clock).chainsword_rev,"genuinely stale sample still releases rev");
        held.update(sample,0,false,time);request_time=time;
        rev.native_update(prop,{actor,held.snapshot_current(request_clock).chainsword_rev},time);
        require(rev.snapshot().stops==before_stops+1,"focus loss stops native animation on the next owned native update");
        held.update(VrControllerSample{},0,true,time);held.update(sample,0,true,time);
        rev.native_update(prop,{actor,held.snapshot_current(request_clock).chainsword_rev},time);
        held.update(VrControllerSample{},0,true,time);rev.native_update(prop,{actor,held.snapshot_current(request_clock).chainsword_rev},time);
        require(rev.snapshot().stops==before_stops+2,"real trigger release stops immediately without debounce");
        clock_input=nullptr;
    }
    const auto before_hook_starts=starts,before_hook_stops=stops;
    unsigned audio_posts{},audio_stops{};rev.bind_audio({[]{return true;},[](auto){return true;},[&](const char*,auto){return ++audio_posts;},[&](auto){++audio_stops;},[](auto){}});
    EngineChainswordRev::Request requested{actor,1};
    require(rev.install(reinterpret_cast<void*>(&native_animation_update),[&]{require(native_updates && native_property==prop,"native animation work runs before reading rev request");return requested;}),"native animation hook installs");
    void(*volatile update)(uintptr_t)=native_animation_update;update(prop);
    require(native_updates==1 && starts==before_hook_starts+1 && rev.snapshot().property==prop,"real hooked native update consumes request after original work");
    require(audio_posts==1 && rev.audio_snapshot().loop==1,"owned animation start gates native audio start");
    rev.release_audio_unless(0,0);require(audio_stops==1 && !rev.audio_snapshot().loop,"present-time focus/tracking loss releases sound even while animation jobs pause");
    requested={};update(prop);require(stops==before_hook_stops+1 && starts==before_hook_starts+2 && !rev.snapshot().property,"loss of fresh actor releases teeth from that property's next native update");
    rev.uninstall();update(prop);require(native_updates==3 && starts==before_hook_starts+2,"uninstall restores native animation update");
    {   // Analog rev: trigger depth drives native teeth play rate and own-loop gain.
        using R=EngineChainswordRev;
        require(R::rev_rate(R::DEADZONE)==R::MIN_RATE && R::rev_rate(1)==R::MAX_RATE && R::rev_rate(.3f)<R::rev_rate(.6f) && R::rev_rate(.6f)<R::rev_rate(.9f),"rate rises monotonically from crawl to full rev");
        require(R::rev_rate(std::numeric_limits<float>::quiet_NaN())==R::MIN_RATE && R::rev_rate(2)==R::MAX_RATE,"invalid or overdriven input stays inside the native range");
        std::vector<std::pair<uintptr_t,float>> rates;std::vector<std::string> sequence;
        rev.bind(base,{reader,[&](auto,const auto& s){return string(s)=="chain_anim" || string(s)=="chain_anim_stop";},
            [&](auto,const auto& s,float,const auto&,bool){sequence.push_back("start:"+string(s));},[&](auto,const auto&){sequence.push_back("stop");},[](auto){return 0.f;},
            [&](auto p,float value){rates.push_back({p,value});sequence.push_back("rate");}});
        std::vector<float> gains;
        rev.bind_audio({[]{return true;},[](auto){return true;},[](const char*,auto){return 7u;},[](auto){},[](auto){},{},{},[&](auto,float g){gains.push_back(g);return true;}});
        rev.tick(actor,actor,.3f,now);
        require(rates.size()==1 && rates[0].first==prop && rates[0].second==R::rev_rate(.3f),"starting a rev sets teeth speed from trigger depth");
        require(gains.size()==1 && gains[0]==EngineChainswordAudio::gain(R::rev_amount(.3f)),"rev loop gain follows trigger depth");
        rev.tick(actor,actor,.9f,now);
        require(rates.back().second==R::rev_rate(.9f) && rev.snapshot().play_rate==R::rev_rate(.9f) && gains.back()==EngineChainswordAudio::gain(R::rev_amount(.9f)),"pressing harder speeds teeth and raises volume");
        const auto gain_count=gains.size();rev.tick(actor,actor,.9f,now);require(gains.size()==gain_count,"unchanged trigger does not resend gain");
        sequence.clear();rev.tick(actor,actor,0,now);
        require(sequence.size()==3 && sequence[0]=="rate" && rates.back().second==1 && sequence[1]=="stop" && sequence[2]=="start:chain_anim_stop" && rev.snapshot().play_rate==1,
            "release restores native speed before the authored wind-down");
        sequence.clear();rev.tick(actor,actor,0,now);require(sequence.empty(),"idle leaves native speed alone");
        rev.bind_audio({});
    }
    {   // Resting finger and idle cost: the rev is on by default with -Controls.
        using R=EngineChainswordRev;
        require(!R::rev_held(.09f,false) && !R::rev_held(.15f,false) && R::rev_held(.16f,false),"a resting finger (0.05-0.09) cannot start a rev");
        require(R::rev_held(.11f,true) && !R::rev_held(.10f,true) && !R::rev_held(std::numeric_limits<float>::quiet_NaN(),true),"a running rev holds down to the release threshold");
        unsigned reads{},starts_now{},stops_now{};
        rev.bind(base,{[&](uintptr_t p,void* out,size_t n){++reads;return reader(p,out,n);},[&](auto,const auto& s){return string(s)=="chain_anim" || string(s)=="chain_anim_stop";},
            [&](auto,const auto& s,float,const auto&,bool){if(string(s)=="chain_anim")++starts_now;},[&](auto,const auto&){++stops_now;},[](auto){return 0.f;}});
        reads=0;
        for(int i=0;i<100;++i){rev.native_update(prop,{actor,0},now);rev.native_update(prop+8,{actor,.08f},now);}
        require(!reads && !starts_now,"idle or resting-finger native updates return before any ownership read");
        rev.native_update(prop,{actor,.12f},now);require(!starts_now,"a light touch below the start threshold does not rev");
        rev.native_update(prop,{actor,.3f},now);require(starts_now==1,"a deliberate pull starts the rev");
        rev.native_update(prop,{actor,.12f},now);require(!stops_now && rev.snapshot().property==prop,"easing off above release keeps revving at a crawl");
        rev.native_update(prop,{actor,.08f},now);require(stops_now==1 && !rev.snapshot().property,"settling back to a resting finger releases the rev");
        rev.native_update(prop,{actor,.12f},now);require(starts_now==1,"after release a resting-to-light touch must pass the start threshold again");
    }
    {   // 26 Oct playtest: ChainswordRev refused=379 starts=0 all session. The prop-animation
        // list's capacity only grows; after the Battle Barge it was over the old 256 bound and
        // every rev was refused before the owned prop was even looked at.
        unsigned starts_now{},rates_now{};std::vector<uintptr_t> started;
        const auto bind=[&]{rev.bind(base,{reader,[&](auto,const auto& s){return string(s)=="chain_anim" || string(s)=="chain_anim_stop";},
            [&](auto p,const auto& s,float,const auto&,bool){if(string(s)=="chain_anim"){++starts_now;started.push_back(p);}},[](auto,const auto&){},[](auto){return 0.f;},
            [&](auto,float){++rates_now;}});};
        const auto table=[&](uint32_t count,uint32_t capacity){put(0x70948,count);put(0x7094c,capacity);};
        bind();table(1,512);rev.tick(actor,actor,1,now);
        require(starts_now==1 && rev.snapshot().property==prop,"a grown list (capacity 512, one entry) still finds the chainsword");
        bind();table(EngineChainswordRev::MAX_PROPS+1,EngineChainswordRev::MAX_PROPS+1);rev.tick(actor,actor,1,now);
        require(starts_now==1 && rev.snapshot().reasons.table==1 && rev.snapshot().refused==1,"an implausible entry count is a table refusal, not a crash");
        bind();table(2,1);rev.tick(actor,actor,1,now);require(rev.snapshot().reasons.table==1,"count above capacity is a table refusal");
        bind();table(1,512);put(0x1200d8,(uintptr_t{0x123}<<48)|(player_geometry+8));rev.tick(actor,actor,1,now);
        require(rev.snapshot().reasons.missing==1,"no owned chainsword in the list is a missing refusal");
        put(0x1200d8,(uintptr_t{0x123}<<48)|player_geometry);
        // The hooked update hands over the actor's own chainsword: no search, so neither an
        // unreadable list nor a second owned prop can stop the rev.
        bind();table(0,0);starts_now=0;started.clear();rev.native_update(prop,{actor,1},now);
        require(starts_now==1 && started[0]==prop && rev.snapshot().property==prop && !rev.snapshot().refused,"the updating owned prop revs even when the list does not list it");
        constexpr uintptr_t prop2=0x48000;put(prop2,base+0x363fd10);put(prop2+0x20,props);put(prop2+0x30,geometry);
        put(0x80010,prop2);bind();table(2,2);starts_now=0;started.clear();
        rev.tick(actor,actor,1,now);require(!starts_now && rev.snapshot().reasons.ambiguous==1,"a search with two owned chainswords stays refused (ambiguous)");
        rev.native_update(prop,{actor,1},now);require(starts_now==1 && started[0]==prop,"the hooked update starts the prop that is updating");
        const auto rates_before=rates_now;rev.native_update(prop2,{actor,1},now);
        require(starts_now==1 && rates_now==rates_before && rev.snapshot().property==prop,"the other prop's update never drives the revving prop");
        rev.native_update(prop,{actor,1},now);require(rates_now==rates_before+1,"the revving prop's own update drives it");
        put(prop,uintptr_t{0});rev.native_update(prop2,{actor,1},now);
        require(starts_now==2 && started[1]==prop2 && rev.snapshot().property==prop2,"a revving prop that is gone hands the rev to the actor's other chainsword");
        put(prop,base+0x363fd10);table(1,1);
    }
    std::cout<<"owned native chainsword animation, release and cached rev freshness PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
