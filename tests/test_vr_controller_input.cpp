#include "VrControllerInput.h"
#include <iostream>
#include <stdexcept>
#include <limits>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

void require(bool ok,const char* message) { if (!ok) { throw std::runtime_error{message}; } }
std::chrono::steady_clock::time_point body_gate_time{};
std::chrono::steady_clock::time_point body_gate_clock() { return body_gate_time; }

// Re-arming after a disarm (docs/CONTROL_CUTOUT_2026-10-26.md).
void rearm_rules() {
    using namespace std::chrono;
    using Clock=steady_clock;
    const auto t0=Clock::time_point{seconds(500)};
    const auto at=[&](int ms){return t0+milliseconds(ms);};
    VrControllerSample fight{};fight.move_y=1;fight.fire=.9f;
    const auto armed_input=[&](VrControllerInput::Rearm rearm={}){
        auto input=std::make_unique<VrControllerInput>();require(input->set_rearm(rearm),"valid re-arm rule");input->enable(true);
        input->update(VrControllerSample{},0,true,at(0));input->update(fight,0,true,at(20));
        const auto p=input->snapshot(at(20)).pad.Gamepad;
        require(p.sThumbLY==32767 && p.bRightTrigger>220,"fixture walks and fires before the stall");
        return input;
    };
    {   // A 300 ms stall in a fight: the held stick and trigger carry on with the first sample after it.
        auto input=armed_input();
        for(int ms=120;ms<=320;ms+=20) input->stall(true,at(ms));
        auto s=input->snapshot(at(320));
        require(!s.armed && !s.pad.Gamepad.sThumbLY && !s.pad.Gamepad.bRightTrigger && std::strcmp(s.disarm,"stall")==0,"a stall releases the pad (nothing stale reaches the game)");
        input->update(fight,0,true,at(340));s=input->snapshot(at(340));
        require(s.armed && s.pad.Gamepad.sThumbLY==32767 && s.pad.Gamepad.bRightTrigger>220,"after a stall the held stick and trigger reach the game at once (was: locked until the whole pad rested)");
        require(s.stall_resumes==1 && std::abs(s.last_stall_ms-220)<1,"the resume is counted with the stall's length");
    }
    {   // Only what was held before the stall carries on. A left grip the grenade gesture owned
        // (parry 0 before) comes back as a raw squeeze once the stall resets the gesture: no LB.
        auto input=armed_input();
        for(int ms=120;ms<=320;ms+=20) input->stall(true,at(ms));
        VrControllerSample after=fight;after.parry=1;input->update(after,0,true,at(340));
        auto p=input->snapshot(at(340)).pad.Gamepad;
        require(p.sThumbLY==32767 && p.bRightTrigger>220 && !(p.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER),"a squeeze that appears across a stall waits for release; the stick and trigger carry on");
        after.parry=0;input->update(after,0,true,at(360));after.parry=1;input->update(after,0,true,at(380));
        require(input->snapshot(at(380)).pad.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER,"let go and squeezed again, it parries");
    }
    {   // A long frame (no poll for 250 ms) is the same stall.
        auto input=armed_input();input->update(fight,0,true,at(270));
        require(input->snapshot(at(270)).pad.Gamepad.sThumbLY==32767 && input->snapshot(at(270)).stall_resumes==1,"a late poll resumes like a stall");
    }
    {   // The sprint toggle survives a stall; a held turn does not snap twice.
        auto input=armed_input();VrControllerSample run=fight;run.fire=0;run.sprint=true;input->update(run,0,true,at(40));
        run.sprint=false;run.turn_x=1;require(input->update(run,0,true,at(60))!=0,"fixture snaps once");
        for(int ms=160;ms<=260;ms+=20) input->stall(true,at(ms));
        require(input->update(run,0,true,at(280))==0,"a turn held through a stall does not snap again");
        require(input->snapshot(at(280)).pad.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_THUMB,"running on: the sprint toggle survives the stall");
    }
    {   // Longer than stall_resume: per control. The held stick and trigger wait for release, a new press works.
        auto input=armed_input();
        for(int ms=120;ms<=1300;ms+=20) input->stall(true,at(ms));
        input->update(fight,0,true,at(1320));input->update(fight,0,true,at(1340));
        auto p=input->snapshot(at(1340)).pad.Gamepad;
        require(input->snapshot(at(1340)).armed && !p.sThumbLY && !p.bRightTrigger,"after a stall longer than stall_resume held controls stay at rest");
        VrControllerSample walk=fight;walk.fire=0;input->update(walk,0,true,at(1360));
        walk.fire=.9f;input->update(walk,0,true,at(1380));p=input->snapshot(at(1380)).pad.Gamepad;
        require(p.bRightTrigger>220 && !p.sThumbLY,"a released and pressed trigger fires while the still-held stick waits");
        walk.move_y=0;input->update(walk,0,true,at(1400));walk.move_y=1;input->update(walk,0,true,at(1420));
        require(input->snapshot(at(1420)).pad.Gamepad.sThumbLY==32767 && !input->snapshot(at(1420)).blocked,"the stick works once let go");
    }
    {   // The protection kept: a trigger held through a focus change never fires on recovery.
        auto input=armed_input();input->update(fight,0,false,at(40));
        input->update(fight,0,true,at(60));input->update(fight,0,true,at(80));
        auto p=input->snapshot(at(80)).pad.Gamepad;
        require(!p.bRightTrigger && !p.sThumbLY && std::strcmp(input->snapshot(at(80)).disarm,"game not foreground")==0,"focus recovery: held trigger and stick stay released");
        VrControllerSample other=fight;other.confirm=true;input->update(other,0,true,at(100));
        require(input->snapshot(at(100)).pad.Gamepad.wButtons&XINPUT_GAMEPAD_A,"a control pressed after recovery works at once");
        input->stall(false,at(120));require(!input->snapshot(at(120)).armed && std::strcmp(input->snapshot(at(120)).reason,"game not foreground")==0,"a stall without focus is a plain disarm");
        input->update(fight,0,true,at(140));input->update(fight,0,true,at(160));
        require(!input->snapshot(at(160)).pad.Gamepad.bRightTrigger,"and does not resume");
    }
    {   // A stall that begins on a menu does not resume: A held to close the menu is not a dodge.
        auto input=armed_input();VrControllerSample menu{};menu.menu_navigation=true;menu.confirm=true;input->update(menu,0,true,at(40));
        for(int ms=140;ms<=240;ms+=20) input->stall(true,at(ms));
        VrControllerSample play{};play.confirm=true;input->update(play,0,true,at(260));input->update(play,0,true,at(280));
        require(!(input->snapshot(at(280)).pad.Gamepad.wButtons&XINPUT_GAMEPAD_A) && !input->snapshot(at(280)).pad.Gamepad.sThumbLY,"menu A held across a stall waits for its release");
    }
    {   // A held through a stall: the resume is not a new press, so not a second dodge.
        auto input=armed_input();VrControllerSample play{};play.confirm=true;input->update(play,0,true,at(40));
        require(input->snapshot(at(40)).pad.Gamepad.wButtons&XINPUT_GAMEPAD_A,"fixture presses A");
        for(int ms=140;ms<=240;ms+=20) input->stall(true,at(ms));
        input->update(play,0,true,at(260));input->update(play,0,true,at(280));
        require(input->snapshot(at(280)).stall_resumes==1 && !(input->snapshot(at(280)).pad.Gamepad.wButtons&XINPUT_GAMEPAD_A),"A held through a stall presses nothing new");
    }
    {   // A press that lands on the first sample after a stall is new, and dodges.
        auto input=armed_input();
        for(int ms=120;ms<=240;ms+=20) input->stall(true,at(ms));
        VrControllerSample play{};play.confirm=true;input->update(play,0,true,at(260));
        require(input->snapshot(at(260)).stall_resumes==1 && (input->snapshot(at(260)).pad.Gamepad.wButtons&XINPUT_GAMEPAD_A),"a fresh press across a stall is a dodge");
    }
    {   // A recenter: Left A still held and the left grip held stay blocked; the stick works.
        auto input=armed_input();input->release("recenter",at(40));
        VrControllerSample after{};after.recenter=true;after.parry=1;input->update(after,0,true,at(60));
        after.move_y=1;input->update(after,0,true,at(80));const auto p=input->snapshot(at(80)).pad.Gamepad;
        require(p.sThumbLY==32767 && !(p.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER),"after a recenter the stick works while the held parry waits for release");
        require(std::strcmp(input->snapshot(at(80)).disarm,"recenter")==0,"the recenter is named as the cause");
    }
    {   // SM2VR_INPUT_NEUTRAL_REARM=1: the old rule, the whole pad waits for neutral.
        auto input=armed_input({milliseconds(0),true});
        for(int ms=120;ms<=320;ms+=20) input->stall(true,at(ms));
        input->update(fight,0,true,at(340));input->update(fight,0,true,at(360));
        require(!input->snapshot(at(360)).armed && std::strcmp(input->snapshot(at(360)).reason,"awaiting neutral controls")==0,"whole_pad restores the neutral wait");
        input->update(VrControllerSample{},0,true,at(380));input->update(fight,0,true,at(400));
        require(input->snapshot(at(400)).pad.Gamepad.sThumbLY==32767,"and re-arms on a neutral pad");
    }
    {   // SM2VR_INPUT_STALL_RESUME_MS=0: stalls re-arm per control.
        auto input=armed_input({milliseconds(0),false});
        for(int ms=120;ms<=320;ms+=20) input->stall(true,at(ms));
        input->update(fight,0,true,at(340));input->update(fight,0,true,at(360));
        require(input->snapshot(at(360)).armed && !input->snapshot(at(360)).pad.Gamepad.sThumbLY && input->snapshot(at(360)).stall_resumes==0,"no resume when stall_resume is 0");
    }
    {   using R=VrControllerInput::Rearm;
        require(R::parse(nullptr,nullptr)->stall_resume==milliseconds(1000) && !R::parse(nullptr,nullptr)->whole_pad,"defaults: 1 s resume, per control");
        require(R::parse("250",nullptr)->stall_resume==milliseconds(250) && R::parse("0","1")->whole_pad,"knob values parse");
        require(!R::parse("abc",nullptr) && !R::parse("-5",nullptr) && !R::parse("20000",nullptr) && !R::parse("",nullptr),"bad values are refused");
        VrControllerInput input;require(!input.set_rearm({milliseconds(-1)}) && input.rearm().stall_resume==milliseconds(1000),"an invalid rule keeps the default");
    }
}

// The 26 Oct 18:30 playtest replayed: every disarm window in
// tests/fixtures/control_cutout_2026-10-26.txt (scripts/control_cutout.py --fixture).
// The old rule (whole pad, no resume) must reproduce the recorded windows; the new rule must let
// the held stick and trigger through in every stall, and hold a parry gripped through a recenter.
void replay_fixture(const char* path) {
    using namespace std::chrono;
    struct Row { double t{}; VrControllerSample recorded,intended; };
    struct Window { int index{}; std::string trigger; double start{},end{},recorded{}; std::vector<Row> rows; };
    std::ifstream file{path};require(file.good(),"replay fixture readable");
    std::vector<Window> windows;std::string line;
    while(std::getline(file,line)) {
        std::istringstream in{line};std::string kind;in>>kind;
        if(kind=="w") { Window w;in>>w.index>>w.trigger>>w.start>>w.end>>w.recorded;windows.push_back(w); }
        else if(kind=="s") {
            int index{},guard{},grenade{},untraced{};char loadout{};float mx{},my{},turn{},fire{},aim{},rgrip{},lgrip{},pad{},raw_right{},raw_left{};Row r;
            in>>index>>r.t>>mx>>my>>turn>>fire>>aim>>rgrip>>lgrip>>pad>>guard>>grenade>>untraced>>raw_right>>raw_left>>loadout;
            require(!in.fail() && !windows.empty() && windows.back().index==index,"fixture sample row parses");
            auto& s=r.recorded;s.move_x=mx;s.move_y=my;s.turn_x=turn;s.fire=fire;s.aim=aim;s.melee=rgrip;s.parry=lgrip;s.grenade=pad;
            s.guard=guard;s.grenade_throw=grenade;s.recenter=untraced; // The trace has no face buttons: Left A after a recenter.
            // What the player held. The trigger routing keeps its owner through a stall now, so the
            // trigger as held reaches the input (primary: right fires; pistol: left fires).
            r.intended=s;if(loadout=='p') r.intended.fire=raw_right;else if(loadout=='s') r.intended.fire=raw_left;
            windows.back().rows.push_back(r);
        }
    }
    require(windows.size()==21,"the 18:30 run has 21 disarm windows");
    struct Result { double armed_at{-1},locked{}; bool parry_leaked{}; };
    const auto replay=[](const Window& w,VrControllerInput::Rearm rule,bool intended) {
        VrControllerInput input;input.set_melee_enabled(false); // Grip melee was off in that run (log: melee_enabled=false).
        input.set_rearm(rule);input.enable(true);
        const auto T=[](double t){return steady_clock::time_point{seconds(4000)}+duration_cast<steady_clock::duration>(duration<double>(t));};
        const double first=w.rows.front().t;
        for(double t=first-.6;t<first;t+=.05) input.update(VrControllerSample{},0,true,T(t));
        Result result;bool triggered{},parry_from_start{},previous_locked{};double previous{};
        for(const auto& r:w.rows) {
            const auto& s=intended?r.intended:r.recorded;
            if(!triggered && r.t>=w.start) {
                triggered=true;
                if(w.trigger=="recenter") input.release("recenter",T(w.start-.001));else input.stall(true,T(w.start-.001));
                parry_from_start=r.intended.parry>.35f;
            }
            input.update(s,0,true,T(r.t));
            if(!triggered) {previous=r.t;continue;}
            const auto snap=input.snapshot(T(r.t));const auto& p=snap.pad.Gamepad;
            if(result.armed_at<0 && snap.armed && !snap.blocked) result.armed_at=r.t-w.start;
            if(previous_locked) result.locked+=r.t-previous;
            const bool walking=std::hypot(r.intended.move_x,r.intended.move_y)>.2f,firing=r.intended.fire>.05f;
            previous_locked=((walking && !p.sThumbLX && !p.sThumbLY) || (firing && !p.bRightTrigger)) && r.t<w.end+.1;previous=r.t;
            if(r.intended.parry<=.35f) parry_from_start=false;
            if(parry_from_start && (p.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER)) result.parry_leaked=true;
        }
        return result;
    };
    double old_total{},new_total{};int stalls{},recenters{};
    std::cout<<"window trigger recorded_s old_armed_s old_locked_s new_locked_s\n";
    for(const auto& w:windows) {
        const auto before=replay(w,{milliseconds(0),true},false);
        const auto after=replay(w,{},true);
        std::cout<<w.index<<' '<<w.trigger<<' '<<w.recorded<<' '<<before.armed_at<<' '<<before.locked<<' '<<after.locked<<'\n';
        require(std::abs(before.armed_at-w.recorded)<=.1,"the old rule reproduces each recorded window within a sample");
        if(w.trigger=="stall") {++stalls;require(after.locked==0,"new rule: no stall locks out a held stick or trigger");}
        if(w.trigger=="recenter") {++recenters;require(after.locked<=.07 && !after.parry_leaked,"new rule: a recenter costs at most the arming poll, and a held parry waits for release");}
        old_total+=before.locked;new_total+=after.locked;
    }
    std::cout<<"locked out while holding the stick or trigger: old "<<old_total<<" s, new "<<new_total<<" s\n";
    require(stalls==10 && recenters==11,"10 stalls and 11 recenters in the 18:30 run");
    require(old_total>30 && new_total<.5,"the replay shows the fix: over 30 s locked out before, under 0.5 s after");
}

int main(int argc,char** argv) {
    try {
        rearm_rules();
        if(argc>1) replay_fixture(argv[1]);
        using namespace std::chrono;
        const auto now = steady_clock::now();
        VrControllerInput input;
        XINPUT_STATE pad{}; pad.dwPacketNumber = 123; pad.Gamepad.sThumbRY = 7654;
        const auto native = pad;
        uint32_t result = ERROR_DEVICE_NOT_CONNECTED;
        input.get_state(&result,0,&pad,now);
        require(result == ERROR_DEVICE_NOT_CONNECTED && std::memcmp(&pad,&native,sizeof(pad)) == 0,"disabled VR input must preserve native result and bytes");
        input.configure(true,true,true);
        XINPUT_CAPABILITIES early_caps{};
        input.get_capabilities(&result,0,&early_caps);
        require(result == ERROR_DEVICE_NOT_CONNECTED,"synthetic mode must not advertise a virtual controller during startup");
        input.configure(true,true,false);
        input.get_capabilities(&result,0,&early_caps);
        require(result == ERROR_SUCCESS && early_caps.Type == XINPUT_DEVTYPE_GAMEPAD,
            "startup configuration must advertise neutral controller before first render/controller samples");
        VrControllerSample forward{}; forward.move_y = 1;
        input.update(forward,0,true,now);
        require(input.snapshot(now).pad.Gamepad.sThumbLY == 0,"held stick on enable must wait for its release before movement");
        input.update(forward,0,true,now);
        require(input.snapshot(now).pad.Gamepad.sThumbLY == 0 && input.snapshot(now).blocked==(1u<<VrControllerInput::MOVE),"a stick held when input arms stays at rest until it is let go");
        require(std::strcmp(input.snapshot(now).reason,"active, held controls wait for release") == 0,"held-input rearm diagnostic must explain the release requirement");
        input.update(VrControllerSample{},0,true,now);
        input.update(forward,0,true,now);
        input.get_state(&result,0,&pad,now);
        require(result == ERROR_SUCCESS && pad.Gamepad.sThumbLY == 32767 && pad.Gamepad.sThumbLX == 0 && pad.Gamepad.sThumbRX == 0,"left stick must drive native movement without smooth camera turning");
        const auto packet = pad.dwPacketNumber;
        input.get_state(&result,0,&pad,now);
        require(pad.dwPacketNumber == packet,"repeated game polling must not manufacture input changes");
        input.update(forward,1.57079632679f,true,now);
        require(input.snapshot(now).pad.Gamepad.sThumbLX == 32767 && std::abs(input.snapshot(now).pad.Gamepad.sThumbLY) <= 1,"head-relative forward at ninety degrees must become game-right movement");
        VrControllerSample turn{}; turn.turn_x = 1;
        require(std::abs(input.update(turn,0,true,now)-0.523598776f) < 1e-6f,"right stick must emit a thirty-degree clockwise snap");
        require(input.update(turn,0,true,now) == 0,"held right stick must not repeat snaps");
        turn.turn_x = 0.5f; require(input.update(turn,0,true,now) == 0,"hysteresis must not rearm halfway stick release");
        turn.turn_x = 0; input.update(turn,0,true,now);
        require(input.set_snap_degrees(45) && !input.set_snap_degrees(0),"snap angle must be live and bounded");
        turn.turn_x = -1;
        require(std::abs(input.update(turn,0,true,now)+0.7853981634f) < 1e-6f,"changed angle must affect actual snap output");
        VrControllerSample buttons{}; buttons.confirm = buttons.interact = buttons.sprint = buttons.menu = true;
        for(float degrees:{-180.f,-90.f,0.f,30.f,90.f,180.f}) {
            VrControllerSample dodge;dodge.confirm=true;dodge.move_x=.1f;dodge.move_y=-.1f;
            const float angle=degrees*.01745329252f;
            input.update(dodge,angle,true,now);
            const auto p=input.snapshot(now).pad.Gamepad;
            require(std::abs(p.sThumbLX+32767*std::sin(angle))<1 && std::abs(p.sThumbLY+32767*std::cos(angle))<1,
                "neutral deadzone dodge supplies body-relative backward direction at every heading");
            input.update(VrControllerSample{},angle,true,now);
            require(!input.snapshot(now).pad.Gamepad.wButtons && (input.snapshot(now).pad.Gamepad.sThumbLX || input.snapshot(now).pad.Gamepad.sThumbLY),
                "tap releases A immediately but retains the directional intent for native roll startup");
            input.update(std::nullopt,angle,true,now);
            input.update(VrControllerSample{},angle,true,now);
            require(!input.snapshot(now).pad.Gamepad.sThumbLX && !input.snapshot(now).pad.Gamepad.sThumbLY,
                "tracking loss discards pending neutral-dodge direction");
        }
        {   // Menu navigation: Right A next to the resting stick is a plain confirm, not a dodge.
            VrControllerSample confirm;confirm.confirm=true;confirm.menu_navigation=true;confirm.move_y=-.1f;
            input.update(confirm,0,true,now);auto p=input.snapshot(now).pad.Gamepad;
            require((p.wButtons&XINPUT_GAMEPAD_A) && !p.sThumbLX && !p.sThumbLY,"menu A presses A without a synthesized backwards stick");
            confirm.confirm=false;input.update(confirm,0,true,now);p=input.snapshot(now).pad.Gamepad;
            require(!p.wButtons && !p.sThumbLX && !p.sThumbLY,"releasing menu A leaves no retained dodge direction");
            VrControllerSample pending;pending.confirm=true;input.update(pending,0,true,now);
            pending.confirm=false;pending.menu_navigation=true;input.update(pending,0,true,now);
            require(!input.snapshot(now).pad.Gamepad.sThumbLY,"entering a menu drops a gameplay dodge direction still being held");
            VrControllerSample back;back.interact=true;back.menu_navigation=true;input.update(back,0,true,now);p=input.snapshot(now).pad.Gamepad;
            require(p.wButtons==XINPUT_GAMEPAD_B,"right B on a menu is native B (back)");
            back.menu_navigation=false;input.update(back,0,true,now);
            require(input.snapshot(now).pad.Gamepad.wButtons==XINPUT_GAMEPAD_B,"right B in play is native B too, so a B prompt means the button with B on it");
            input.update(VrControllerSample{},0,true,now);
            VrControllerSample trackpad;trackpad.grenade=1;input.update(trackpad,0,true,now);
            require(input.snapshot(now).pad.Gamepad.wButtons==XINPUT_GAMEPAD_X,"the right trackpad in play is native equipment (X)");
            trackpad.menu_navigation=true;input.update(trackpad,0,true,now);
            require(input.snapshot(now).pad.Gamepad.wButtons==XINPUT_GAMEPAD_B,"the right trackpad on a menu is back (native B)");
            input.update(VrControllerSample{},0,true,now);
        }
        {   // 31 Oct 20:39, 20:49:57, 20:51:15: a 0.6 dead zone and a squared ramp on the menu cursor made it
            // too slow, with halting, and 20:58:10 still scrolled. A menu has the same stick as play.
            VrControllerSample stick;stick.menu_navigation=true;stick.move_y=.8f;input.update(stick,0,true,now);
            const auto firm=input.snapshot(now).pad.Gamepad.sThumbLY;
            VrControllerSample play;play.move_y=.8f;input.update(play,0,true,now);
            require(firm>24000 && firm==input.snapshot(now).pad.Gamepad.sThumbLY,"a firm push moves the menu cursor at the same speed as walking");
            stick.move_y=.5f;input.update(stick,0,true,now);
            require(input.snapshot(now).pad.Gamepad.sThumbLY>10000,"a part push still moves the menu cursor");
            stick.move_y=1;input.update(stick,0,true,now);
            require(input.snapshot(now).pad.Gamepad.sThumbLY==32767,"full stick on a menu is still full stick");
            stick.move_y=0;stick.turn_x=.3f;stick.turn_y=-.25f;input.update(stick,0,true,now);auto p=input.snapshot(now).pad.Gamepad;
            require(p.sThumbRX==9830 && p.sThumbRY==-8192,"the right stick stays raw on a menu: it scrolls lists");
            input.update(VrControllerSample{},0,true,now);
        }
        input.update(buttons,0,true,now);
        require(input.snapshot(now).pad.Gamepad.wButtons == (XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_START),"dodge/menu cancels sprint while other action buttons reach native pad bits");
        const auto pressed = input.snapshot(now).pad;
        const auto stale = input.snapshot(now+milliseconds(101));
        require(std::strcmp(stale.reason,"stale cached input") == 0,"stale diagnostic must identify freshness gate");
        require(stale.pad.Gamepad.wButtons == 0 && stale.pad.dwPacketNumber != pressed.dwPacketNumber,"stale input must publish a packet change so game caches release held buttons");
        input.update(forward,0,true,now+milliseconds(101));
        require(input.snapshot(now+milliseconds(101)).pad.Gamepad.sThumbLY == 32767,"a poll 101 ms late is a stall: the held stick carries on at once (26 Oct, docs/CONTROL_CUTOUT)");
        input.update(VrControllerSample{},0,true,now+milliseconds(101));
        input.update(forward,0,true,now+milliseconds(101));
        input.update(forward,0,false,now+milliseconds(101));
        require(std::strcmp(input.snapshot(now+milliseconds(101)).reason,"game not foreground") == 0,"desktop focus loss must be diagnosable separately from calibration/tracking");
        require(input.snapshot(now+milliseconds(101)).pad.Gamepad.sThumbLY == 0,"focus loss must release movement");
        input.update(forward,0,true,now+milliseconds(101));input.update(forward,0,true,now+milliseconds(101));
        require(input.snapshot(now+milliseconds(101)).pad.Gamepad.sThumbLY == 0,"focus recovery with held input must remain neutral");
        input.update(VrControllerSample{},0,true,now+milliseconds(101));
        input.update(std::nullopt,0,true,now+milliseconds(101));
        require(std::strcmp(input.snapshot(now+milliseconds(101)).reason,"tracking/actions unavailable") == 0,"missing tracking/actions must report distinct gate");
        require(!input.snapshot(now+milliseconds(101)).armed,"lost controller data must disarm movement");
        input.update(VrControllerSample{},0,true,now+milliseconds(101));
        forward.move_x = std::numeric_limits<float>::quiet_NaN();
        input.update(forward,0,true,now+milliseconds(101));
        require(!input.snapshot(now+milliseconds(101)).armed,"invalid action axes must never generate movement");
        require(std::strcmp(input.snapshot(now+milliseconds(101)).reason,"invalid action data") == 0,"invalid-data gate must be observable");
        result = ERROR_DEVICE_NOT_CONNECTED; pad = native;
        input.get_state(&result,1,&pad,now);
        require(result == ERROR_DEVICE_NOT_CONNECTED && std::memcmp(&pad,&native,sizeof(pad)) == 0,"other native gamepad slots must remain untouched");
        XINPUT_CAPABILITIES capabilities{};
        input.get_capabilities(&result,0,&capabilities);
        require(result == ERROR_SUCCESS && capabilities.Type == XINPUT_DEVTYPE_GAMEPAD,"game must discover the virtual pad before polling movement");
        VrControllerInput body_gate; body_gate.enable(true);
        const auto before_poll=steady_clock::time_point{seconds(100)},after_poll=before_poll+milliseconds(1);
        body_gate.update(VrControllerSample{},0,true,after_poll);
        body_gate_time=before_poll;
        require(!body_gate.allows_body_motion(true,body_gate_clock),"pre-poll timestamp must reproduce the closed future-sample body gate");
        body_gate_time=after_poll;
        require(body_gate.allows_body_motion(true,body_gate_clock) && !body_gate.allows_body_motion(false,body_gate_clock),
            "body gate must use its post-poll clock and actual foreground state");
        body_gate_time=after_poll+milliseconds(101);
        require(!body_gate.allows_body_motion(true,body_gate_clock),"post-poll body gate must still reject stale input");
        body_gate.update(std::nullopt,0,true,after_poll); body_gate_time=after_poll;
        require(!body_gate.allows_body_motion(true,body_gate_clock),"lost actions must release native body motion");
        {
            VrControllerInput timed;timed.enable(true);timed.update(VrControllerSample{},0,true,now);
            VrControllerSample tap;tap.confirm=true;timed.update(tap,0,true,now);
            for(int ms=50;ms<=450;ms+=50) {
                timed.update(VrControllerSample{},0,true,now+milliseconds(ms));
                const auto p=timed.snapshot(now+milliseconds(ms)).pad.Gamepad;
                require(p.wButtons==0 && p.sThumbLY==(ms<450?-32767:0),"neutral direction is bounded at 450ms without extending the A press");
            }
            timed.update(tap,0,true,now+milliseconds(500));
            VrControllerSample right;right.move_x=1;timed.update(right,0,true,now+milliseconds(550));
            timed.update(VrControllerSample{},0,true,now+milliseconds(600));
            require(timed.snapshot(now+milliseconds(600)).pad.Gamepad.sThumbLY==0,"explicit stick intent cancels the fallback rather than resuming it on release");
            timed.update(tap,0,true,now+milliseconds(650));timed.update(tap,0,false,now+milliseconds(700));
            require(!timed.snapshot(now+milliseconds(700)).pad.Gamepad.sThumbLY,"focus loss immediately releases pending dodge movement");
        }
        {   // 31 Oct 16:14:21: A held 3 s with the stick centred ran Joe backwards at sprint speed, on the
            // synthesized stick and the game's hold-to-sprint. A held press is now one dodge and nothing else.
            VrControllerInput held;held.enable(true);held.update(VrControllerSample{},0,true,now);
            const auto pad=[&](const VrControllerSample& v,int ms){held.update(v,0,true,now+milliseconds(ms));return held.snapshot(now+milliseconds(ms)).pad.Gamepad;};
            // Steady polls: a gap over 100 ms is a stall, which has its own rules.
            const auto idle=[&](int from,int to){for(int ms=from;ms<=to;ms+=50)pad(VrControllerSample{},ms);};
            VrControllerSample a;a.confirm=true;
            for(int ms=0;ms<350;ms+=50) {const auto p=pad(a,ms);require((p.wButtons&XINPUT_GAMEPAD_A) && p.sThumbLY==-32767,"up to the limit a held press is a tap so far: A and the backwards stick");}
            {const auto p=pad(a,350);require(!p.wButtons && p.sThumbLY==-32767,"native A lets go at the limit, so the game dodges; the direction runs on to 450 ms");}
            pad(a,400);
            for(int ms=450;ms<=3000;ms+=50) {const auto p=pad(a,ms);require(!p.wButtons && !p.sThumbLX && !p.sThumbLY,"A held on sends nothing: no run backwards");}
            pad(VrControllerSample{},3050);
            require(pad(a,3100).wButtons&XINPUT_GAMEPAD_A,"a new press after letting go dodges again");
            idle(3150,3650);
            VrControllerSample sideways;sideways.confirm=true;sideways.move_x=-1;
            require(pad(sideways,3700).wButtons&XINPUT_GAMEPAD_A,"with the stick pushed A dodges that way");
            for(int ms=3750;ms<4050;ms+=50) require(pad(sideways,ms).wButtons&XINPUT_GAMEPAD_A,"A stays down up to the limit");
            {const auto p=pad(sideways,4050);require(!p.wButtons && p.sThumbLX==-32767,"held on, the stick walks and A has let go: no native sprint outside the sprint gate");}
            idle(4100,4950);
            for(int ms=5000;ms<=5340;ms+=20) require(pad(a,ms).wButtons&XINPUT_GAMEPAD_A,"a press under the limit is untouched");
            require(!pad(VrControllerSample{},5350).wButtons,"and ends when it ends");
            idle(5400,5950);
            VrControllerSample confirm=a;confirm.menu_navigation=true;
            for(int ms=6000;ms<=7000;ms+=50) require(pad(confirm,ms).wButtons&XINPUT_GAMEPAD_A,"menu A stays held: the limit is for play");
        }
        {
            VrControllerInput run;run.enable(true);run.update(VrControllerSample{},0,true,now);
            VrControllerSample move;move.move_y=1;move.sprint=true;
            for(float heading:{0.f,1.5707963f,3.1415926f,-1.5707963f}) {
                run.update(move,heading,true,now);
                require(run.snapshot(now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_THUMB,"forward sprint starts at every native camera heading");
                move.sprint=false;run.update(move,heading,true,now);
                require(run.snapshot(now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_THUMB,"click release keeps sprint latched");
                run.update(VrControllerSample{},heading,true,now);
                require(!(run.snapshot(now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_THUMB),"stopping cancels sprint");move.sprint=true;
            }
            run.update(move,0,true,now);run.update(move,0,false,now);
            require(!run.snapshot(now).armed,"focus loss releases and disarms sprint");
            run.update(VrControllerSample{},0,true,now);move.sprint=false;run.update(move,0,true,now);
            require(!(run.snapshot(now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_THUMB),"sprint cannot resume from its old latch after recovery");
            move.sprint=true;move.move_y=0;move.move_x=1;run.update(move,0,true,now);
            require(!(run.snapshot(now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_THUMB),"sideways movement cannot start sprint even with native camera veto removed");
            move={};run.update(move,0,true,now);move.move_y=.45f;move.sprint=true;run.update(move,0,true,now);
            require(!(run.snapshot(now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_THUMB),"native minimum force is checked after the mod's movement deadzone");
        }
        {   // Joe, 31 Oct: a placed melta charge detonates on the right stick click, held. The game wants
            // equipment held 0.3 s (AbilityRoutineDetonate, native X in the controls pack). At 17:49 he
            // held the click 2.7, 2.8 and 2 s on the prompt "right stick down" and nothing happened.
            VrControllerInput equipment;equipment.enable(true);auto t=now;equipment.update(VrControllerSample{},0,true,t);
            const auto pad=[&](const VrControllerSample& v,milliseconds step){t+=step;equipment.update(v,0,true,t);return equipment.snapshot(t).pad.Gamepad.wButtons;};
            const auto x=[&](const VrControllerSample& v,milliseconds step){return (pad(v,step)&XINPUT_GAMEPAD_X)!=0;};
            VrControllerSample click{};click.finisher=true;
            const auto before_the_hold=VrControllerInput::DETONATE_HOLD-milliseconds(20);
            auto buttons=pad(click,milliseconds(10));
            require((buttons&XINPUT_GAMEPAD_RIGHT_THUMB) && !(buttons&XINPUT_GAMEPAD_X),"the click is finisher at once, with no added delay and no equipment");
            buttons=pad(click,before_the_hold);
            require((buttons&XINPUT_GAMEPAD_RIGHT_THUMB) && !(buttons&XINPUT_GAMEPAD_X),"no equipment before the hold");
            buttons=pad(click,milliseconds(30));
            require((buttons&XINPUT_GAMEPAD_X) && (buttons&XINPUT_GAMEPAD_RIGHT_THUMB) && !(buttons&XINPUT_GAMEPAD_B),"held, the click also holds native X, never Right B's button");
            require(VrControllerInput::DETONATE_HOLD>=milliseconds(300) && VrControllerInput::DETONATE_HOLD<=milliseconds(700),"the hold clears a tap (0.2 s in the 17:49 run) and stays about half a second");
            for(int i=0;i<30;++i)require(x(click,milliseconds(20)),"X lasts as long as the click");
            require(!x(VrControllerSample{},milliseconds(20)),"letting go of a long hold releases X at once");
            require(!x(click,milliseconds(10)) && !x(VrControllerSample{},milliseconds(20)),"a tap never reaches X");
            for(int i=0;i<40;++i)require(!x(VrControllerSample{},milliseconds(20)),"and nothing follows it");
            require(!x(click,milliseconds(10)) && x(click,VrControllerInput::DETONATE_HOLD),"a new click starts its own hold");
            const auto detonated=t;
            bool held=true;while(t+milliseconds(20)<detonated+VrControllerInput::EQUIPMENT_MIN_HOLD)held=x(VrControllerSample{},milliseconds(20)) && held;
            require(held && VrControllerInput::EQUIPMENT_MIN_HOLD>=milliseconds(350),"let go just past the hold, X still outlasts the game's 0.3 s");
            require(!x(VrControllerSample{},milliseconds(40)),"X is released after the minimum hold");
            for(const float stick:{-1.f,-.5f,1.f}) {VrControllerSample moved{};moved.turn_y=stick;for(int i=0;i<40;++i)require(!x(moved,milliseconds(20)),"the right stick's up and down are not detonate");}
            x(VrControllerSample{},milliseconds(20));
            auto menu=click;menu.menu_navigation=true;
            for(int i=0;i<40;++i)require(!(pad(menu,milliseconds(20))&(XINPUT_GAMEPAD_X|XINPUT_GAMEPAD_B)),"on a menu a held click is never equipment");
            x(VrControllerSample{},milliseconds(20));
            auto cutscene=click;cutscene.cinematic=true;
            for(int i=0;i<40;++i)require(!x(cutscene,milliseconds(20)),"in a cutscene a held click is never equipment");
            require(!x(click,milliseconds(20)),"a click held out of a cutscene starts its hold there");
            x(VrControllerSample{},milliseconds(20));
            VrControllerSample trackpad{};trackpad.grenade=1;x(VrControllerSample{},milliseconds(20));
            require(x(trackpad,milliseconds(20)) && !x(VrControllerSample{},milliseconds(20)),"the trackpad press is a plain X again (throws; no hold)");
            VrControllerSample thrown{};thrown.grenade_throw=true;x(VrControllerSample{},milliseconds(20));
            require(x(thrown,milliseconds(20)) && !x(VrControllerSample{},milliseconds(20)),"the belt gesture's pulse is its own length (unchanged)");
            x(click,milliseconds(10));x(click,VrControllerInput::DETONATE_HOLD);equipment.update(click,0,false,t+milliseconds(20));
            require(!(equipment.snapshot(t+milliseconds(30)).pad.Gamepad.wButtons&XINPUT_GAMEPAD_X),"focus loss ends the hold");
        }
        {   // Touch controllers (docs/QUEST_STEAM_LINK.md) have no left trackpad: right stick up
            // is stim (D-pad right), the left stick click held a second is the scoreboard (Back).
            VrControllerInput touch;touch.enable(true);auto t=now;VrControllerSample rest{};rest.touch_layout=true;touch.update(rest,0,true,t);
            const auto buttons=[&](const VrControllerSample& v,milliseconds step){t+=step;touch.update(v,0,true,t);return touch.snapshot(t).pad.Gamepad.wButtons;};
            auto up=rest;up.turn_y=1;const auto pressed=t+milliseconds(10);
            require(buttons(up,milliseconds(10))&XINPUT_GAMEPAD_DPAD_RIGHT,"right stick up is stim at once");
            bool held=true;while(t+milliseconds(20)<pressed+VrControllerInput::STIM_MIN_HOLD)held=(buttons(rest,milliseconds(20))&XINPUT_GAMEPAD_DPAD_RIGHT) && held;
            require(held,"a flick holds stim for the minimum hold");
            require(!(buttons(rest,milliseconds(40))&XINPUT_GAMEPAD_DPAD_RIGHT),"stim releases after the minimum hold");
            for(int i=0;i<20;++i)require(buttons(up,milliseconds(20))&XINPUT_GAMEPAD_DPAD_RIGHT,"stick held up holds stim");
            auto half=rest;half.turn_y=.5f;require(buttons(half,milliseconds(20))&XINPUT_GAMEPAD_DPAD_RIGHT,"inside the hysteresis band it stays held");
            require(!(buttons(rest,milliseconds(20))&XINPUT_GAMEPAD_DPAD_RIGHT),"centre releases stim at once");
            require(!(buttons(half,milliseconds(20))&XINPUT_GAMEPAD_DPAD_RIGHT),"a half push from centre is not stim");
            auto down=rest;down.turn_y=-1;
            require(!(buttons(down,milliseconds(20))&(XINPUT_GAMEPAD_DPAD_RIGHT|XINPUT_GAMEPAD_X)),"stick down does nothing on Touch either");
            auto right_click=rest;right_click.finisher=true;buttons(right_click,milliseconds(10));
            require((buttons(right_click,VrControllerInput::DETONATE_HOLD)&XINPUT_GAMEPAD_X) && !(buttons(right_click,milliseconds(20))&XINPUT_GAMEPAD_DPAD_RIGHT),"the held click detonates on Touch as on Index");
            buttons(rest,milliseconds(500));
            auto menu=rest;menu.menu_navigation=true;menu.turn_y=1;require(!(buttons(menu,milliseconds(20))&XINPUT_GAMEPAD_DPAD_RIGHT),"on a menu the right stick is the native stick");
            VrControllerSample index{};index.turn_y=1;buttons(index,milliseconds(20));
            require(!(buttons(index,milliseconds(20))&XINPUT_GAMEPAD_DPAD_RIGHT),"the Index keeps right stick up unbound (stim is its trackpad)");
            auto click=rest;click.sprint=true;buttons(rest,milliseconds(20));
            require(!(buttons(click,milliseconds(20))&XINPUT_GAMEPAD_BACK),"a click is not the scoreboard");
            bool waiting=true;for(int i=0;i<47;++i)waiting=!(buttons(click,milliseconds(20))&XINPUT_GAMEPAD_BACK) && waiting;
            require(waiting,"no scoreboard before a second");
            require(buttons(click,milliseconds(60))&XINPUT_GAMEPAD_BACK,"a one-second click hold is the scoreboard");
            require(buttons(click,milliseconds(500))&XINPUT_GAMEPAD_BACK,"Back stays held with the click (the game's 2 s audio-log skip)");
            require(!(buttons(rest,milliseconds(20))&XINPUT_GAMEPAD_BACK),"releasing the click releases Back");
            auto run=rest;run.move_y=1;run.sprint=true;buttons(rest,milliseconds(20));
            require(buttons(run,milliseconds(20))&XINPUT_GAMEPAD_LEFT_THUMB,"a click while running still toggles sprint on");
            for(int i=0;i<50;++i)buttons(run,milliseconds(20));
            const auto pad=buttons(run,milliseconds(20));
            require((pad&XINPUT_GAMEPAD_BACK) && !(pad&XINPUT_GAMEPAD_LEFT_THUMB),"the scoreboard cancels sprint");
            VrControllerSample index_click{};index_click.sprint=true;buttons(VrControllerSample{},milliseconds(20));
            for(int i=0;i<60;++i)require(!(buttons(index_click,milliseconds(20))&XINPUT_GAMEPAD_BACK),"the Index click hold is never the scoreboard");
            buttons(rest,milliseconds(20));buttons(click,milliseconds(20));touch.update(click,0,false,t+milliseconds(10));
            t+=milliseconds(1100);touch.update(click,0,true,t);
            require(!(buttons(click,milliseconds(20))&XINPUT_GAMEPAD_BACK),"a click held through a focus loss waits for release, never the scoreboard");
        }
        std::cout << "controller mapping, snap turns, freshness, release and XInput boundary checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
