#include "OpenVrCombatActions.h"
#include <iostream>
#include <stdexcept>

void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int main() {
    try {
        using namespace std::chrono;
        const auto now=steady_clock::time_point{seconds(10)};
        VrControllerInput firearm;firearm.configure(true,true,false);
        require(!firearm.melee_enabled(),"application controller setup must leave grip melee off by default");
        VrControllerSample gun;gun.melee=1;
        firearm.update(gun,0,true,now);
        require(firearm.snapshot(now).armed,"disabled grip input must not block neutral arming");
        gun.fire=1;gun.aim=.5f;gun.move_y=1;gun.parry=1;gun.interact=true;
        firearm.update(gun,0,true,now);
        auto gun_state=firearm.snapshot(now).pad;
        require(!(gun_state.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER) && gun_state.Gamepad.bRightTrigger==255 &&
            gun_state.Gamepad.bLeftTrigger==128 && gun_state.Gamepad.sThumbLY==32767 &&
            (gun_state.Gamepad.wButtons&(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_LEFT_SHOULDER))==(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_LEFT_SHOULDER),
            "disabled sword inputs must preserve fire, aim, walking, interact and parry at the native pad boundary");
        // Physical swings have their own opt-in in VrSession (trigger-finger gated), so a
        // session swing request reaches native melee even with grip melee off.
        gun.swing_melee=true;firearm.update(gun,0,true,now);
        require(firearm.snapshot(now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER,"session swing request must reach native melee with grip melee disabled");
        gun.swing_melee=false;firearm.update(gun,0,true,now);
        require(!(firearm.snapshot(now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER),"grip force alone must stay off with grip melee disabled");
        VrControllerSample holster;holster.select_primary=true;firearm.update(holster,0,true,now);
        require(firearm.snapshot(now).pad.Gamepad.wButtons==XINPUT_GAMEPAD_DPAD_DOWN,"primary holster pulse maps to state-checked native cycle");
        holster={};holster.select_secondary=true;firearm.update(holster,0,true,now);
        require(firearm.snapshot(now).pad.Gamepad.wButtons==XINPUT_GAMEPAD_DPAD_DOWN,"secondary holster pulse maps to native cycle, never emote");
        holster={};holster.salute=true;firearm.update(holster,0,true,now);
        require(firearm.snapshot(now).pad.Gamepad.wButtons==XINPUT_GAMEPAD_DPAD_LEFT,"raised fist uses native emote without primary or melee");
        VrControllerInput new_fist;new_fist.enable(true);new_fist.update(holster,0,true,now);new_fist.update(holster,0,true,now);
        require(!new_fist.snapshot(now).pad.Gamepad.wButtons,"held emote must not act after focus recovery (it waits for release)");
        holster={};holster.guard=true;firearm.update(holster,0,true,now);
        require(firearm.snapshot(now).pad.Gamepad.wButtons==XINPUT_GAMEPAD_LEFT_SHOULDER,"left-hand guard maps to native parry/block (LB)");
        holster={};holster.grenade_throw=true;firearm.update(holster,0,true,now);
        require(firearm.snapshot(now).pad.Gamepad.wButtons==XINPUT_GAMEPAD_X,"grenade throw gesture maps to native equipment (X)");
        firearm.update(VrControllerSample{},0,true,now);
        {
            VrControllerInput fresh;fresh.enable(true);VrControllerSample raised;raised.guard=true;
            fresh.update(raised,0,true,now);fresh.update(raised,0,true,now);
            require(!(fresh.snapshot(now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER),"a held guard waits for release like any other held control");
        }
        firearm.set_melee_enabled(true);firearm.update(gun,0,true,now);firearm.update(gun,0,true,now);
        require(!(firearm.snapshot(now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER),"opt-in recovery must not attack from a held squeeze");
        firearm.update(VrControllerSample{},0,true,now);firearm.update(gun,0,true,now);
        require(firearm.snapshot(now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER,"explicit opt-in restores native melee");
        const auto melee_packet=firearm.snapshot(now).pad.dwPacketNumber;
        firearm.set_melee_enabled(false);const auto released=firearm.snapshot(now).pad;
        require(!(released.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER) && released.Gamepad.bRightTrigger==255 &&
            released.dwPacketNumber!=melee_packet,"disabling melee releases its cached native button immediately");
        VrControllerInput input;input.enable(true);
        VrControllerSample value;value.fire=1;
        input.update(value,0,true,now);input.update(value,0,true,now);
        require(!input.snapshot(now).pad.Gamepad.bRightTrigger,"held fire must not fire on enable");
        input.update(VrControllerSample{},0,true,now);
        value.fire=0.5f;value.aim=1;value.melee=0.7f;value.parry=0.8f;value.grenade=1;value.finisher=true;value.interact=true;
        input.update(value,0,true,now);
        uint32_t result=ERROR_DEVICE_NOT_CONNECTED;XINPUT_STATE native;
        input.get_state(&result,0,&native,now);
        require(result==ERROR_SUCCESS && native.Gamepad.bRightTrigger==128 && native.Gamepad.bLeftTrigger==255,"real cached GetState must expose independent analog fire/aim");
        require(native.Gamepad.wButtons==(XINPUT_GAMEPAD_RIGHT_SHOULDER|XINPUT_GAMEPAD_LEFT_SHOULDER|XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_RIGHT_THUMB|XINPUT_GAMEPAD_X),"combat and marker activation must reach native Type A buttons together");
        const auto packet=native.dwPacketNumber;
        value.fire=0.6f;input.update(value,0,true,now);
        require(input.snapshot(now).pad.dwPacketNumber!=packet,"trigger-only changes must advance native packet");
        value={};value.melee=value.parry=0.5f;input.update(value,0,true,now);
        require(input.snapshot(now).pad.Gamepad.wButtons==(XINPUT_GAMEPAD_RIGHT_SHOULDER|XINPUT_GAMEPAD_LEFT_SHOULDER),"grips must retain a squeeze in hysteresis band");
        value.melee=value.parry=0.3f;input.update(value,0,true,now);
        require(!input.snapshot(now).pad.Gamepad.wButtons,"grip release must release both native attack buttons");
        const std::array<std::array<float,2>,4> directions{{{0,1},{0,-1},{1,0},{-1,0}}};
        const std::array<WORD,4> expected{XINPUT_GAMEPAD_Y,XINPUT_GAMEPAD_DPAD_DOWN,XINPUT_GAMEPAD_DPAD_RIGHT,XINPUT_GAMEPAD_BACK};
        for(size_t i=0;i<4;++i) {
            input.update(VrControllerSample{},0,true,now);
            value={};value.utility_press=1;value.utility_x=directions[i][0];value.utility_y=directions[i][1];input.update(value,0,true,now);
            require(input.snapshot(now).pad.Gamepad.wButtons==expected[i],"utility quadrant must select native ability/weapon/stim/scoreboard");
            value.utility_x=-directions[i][0];value.utility_y=-directions[i][1];input.update(value,0,true,now);
            require(input.snapshot(now).pad.Gamepad.wButtons==expected[i],"held trackpad quadrant must not switch action before release");
        }
        input.update(VrControllerSample{},0,true,now);value={};value.fire=1;input.update(value,0,true,now);
        const auto pressed=input.snapshot(now).pad;
        const auto stale=input.snapshot(now+milliseconds(101));
        require(!stale.pad.Gamepad.bRightTrigger && stale.pad.dwPacketNumber!=pressed.dwPacketNumber,"trigger-only stale release must change packet");
        input.update(value,0,false,now);
        require(!input.snapshot(now).pad.Gamepad.bRightTrigger,"focus loss must release held fire");
        input.update(value,0,true,now);input.update(value,0,true,now);require(!input.snapshot(now).pad.Gamepad.bRightTrigger,"focus recovery with trigger held must stay neutral");
        value={};input.update(value,0,true,now);value.aim=NAN;input.update(value,0,true,now);
        require(!input.snapshot(now).armed,"nonfinite trigger must disarm all inputs");
        XINPUT_CAPABILITIES caps{};result=ERROR_DEVICE_NOT_CONNECTED;input.get_capabilities(&result,0,&caps);
        require(caps.Gamepad.bLeftTrigger==255 && caps.Gamepad.bRightTrigger==255 && (caps.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER) &&
            (caps.Gamepad.wButtons&(XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_LEFT))==(XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_LEFT),"pad discovery must advertise combat axes/buttons and holster D-pad directions");
        {VrControllerInput pulse;pulse.enable(true);pulse.update(VrControllerSample{},0,true,now);
         const auto before=pulse.snapshot(now).pad;pulse.test_pulse(XINPUT_GAMEPAD_DPAD_UP,now);
         const auto during=pulse.snapshot(now+milliseconds(100)).pad,after=pulse.snapshot(now+milliseconds(151)).pad;
         require(during.Gamepad.wButtons==XINPUT_GAMEPAD_DPAD_UP && during.dwPacketNumber!=before.dwPacketNumber,"D-pad test pulse reaches the native pad with a new packet");
         require(!(after.Gamepad.wButtons&XINPUT_GAMEPAD_DPAD_UP) && after.dwPacketNumber!=during.dwPacketNumber,"D-pad test pulse releases after 150 ms");}
        std::array<vr::VRActionHandle_t,7> handles{1,2,3,4,5,6,7};
        auto analog=[](auto handle,auto& data){data.bActive=true;data.x=handle==6 ? -0.8f:0.9f;data.y=0.7f;return vr::VRInputError_None;};
        auto digital=[](auto,auto& data){data.bActive=true;data.bState=true;return vr::VRInputError_None;};
        require(openvr_combat::poll(handles,8,9,analog,digital,value) && value.fire==0.9f && value.aim==0.9f && value.utility_x==-0.8f && value.utility_y==0.7f && value.finisher && value.fire_touch_valid && value.fire_touch,"production OpenVR poll must decode analog/digital fields correctly");
        auto touch_off=[](auto handle,auto& data){data.bActive=true;data.bState=handle!=9;return vr::VRInputError_None;};
        require(openvr_combat::poll(handles,8,9,analog,touch_off,value) && value.finisher && value.fire_touch_valid && !value.fire_touch,"lifted trigger finger must read as untouched");
        require(openvr_combat::poll(handles,8,0,analog,digital,value) && !value.fire_touch_valid && !value.fire_touch,"missing trigger-touch binding must be reported invalid without failing combat");
        auto error=[](auto,auto&){return vr::VRInputError_NoData;};
        require(!openvr_combat::poll(handles,8,9,error,error,value) && value.fire==0 && value.aim==0 && !value.finisher && !value.fire_touch && !value.fire_touch_valid,"runtime errors must release old combat action values");
        handles[0]=0;value.move_y=1;value.interact=true;
        require(!openvr_combat::poll(handles,8,9,analog,digital,value) && value.fire==0 && value.move_y==1 && value.interact,"old bindings with missing combat must preserve locomotion/interact");
        auto inactive=[](auto,auto&){return vr::VRInputError_None;};
        require(!openvr_combat::poll(handles,8,9,inactive,inactive,value) && value.melee==0 && value.parry==0,"inactive bindings must release combat");
        auto corrupt=[](auto,auto& data){data.bActive=true;data.x=NAN;return vr::VRInputError_None;};
        require(!openvr_combat::poll(handles,8,9,corrupt,digital,value) && value.fire==0 && value.utility_x==0,"invalid runtime axes must be refused before mapping");
        // Native combo logic distinguishes a held melee input from repeated taps. Check
        // the actual GetState boundary at deterministic poll/sample times, without sleeps.
        VrControllerInput melee;melee.enable(true);
        melee.update(VrControllerSample{},0,true,now);
        VrControllerSample squeeze;squeeze.melee=0.9f;
        uint32_t held_packet{};
        for(int frame=1;frame<=80;++frame) {
            const auto sample_time=now+milliseconds(frame*16);
            melee.update(squeeze,0,true,sample_time);
            result=ERROR_DEVICE_NOT_CONNECTED;
            melee.get_state(&result,0,&native,sample_time+milliseconds(8));
            require(result==ERROR_SUCCESS && native.Gamepad.wButtons==XINPUT_GAMEPAD_RIGHT_SHOULDER,
                "held right grip must stay held through native polls for charged melee");
            if(frame==1) held_packet=native.dwPacketNumber;
            require(native.dwPacketNumber==held_packet,"held melee must not manufacture repeated press edges");
        }
        const auto release_time=now+milliseconds(1296);
        melee.update(VrControllerSample{},0,true,release_time);
        melee.get_state(&result,0,&native,release_time);
        require(native.Gamepad.wButtons==0 && native.dwPacketNumber!=held_packet,"charged melee must release at the native boundary");
        for(int tap=0;tap<3;++tap) {
            const auto press_time=release_time+milliseconds(16+tap*64);
            melee.update(squeeze,0,true,press_time);
            melee.get_state(&result,0,&native,press_time);
            require(native.Gamepad.wButtons==XINPUT_GAMEPAD_RIGHT_SHOULDER,"each combo tap must reach native input");
            const auto press_packet=native.dwPacketNumber;
            melee.update(VrControllerSample{},0,true,press_time+milliseconds(32));
            melee.get_state(&result,0,&native,press_time+milliseconds(32));
            require(native.Gamepad.wButtons==0 && native.dwPacketNumber!=press_packet,"combo taps need distinct native release edges");
        }
        std::cout<<"combat polling, native mapping, release and rearm passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
