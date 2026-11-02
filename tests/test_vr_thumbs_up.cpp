#include "VrThumbsUp.h"
#include <iostream>
#include <stdexcept>
// Curl values below are real samples from the 26 Oct playtest traces
// (build/run/20261026_1*_playtest_*/sm2vr_trace.jsonl, OpenXR hand joints, thumb..pinky).
namespace {
using namespace vr_thumbs_up;
using std::chrono::milliseconds;
void require(bool value,const char* why){if(!value)throw std::runtime_error{why};}
constexpr Curl JOE_THUMBS_UP{0.0f,1.0f,0.435f,1.0f,1.0f};     // right hand, photo freeze 15:02:11
constexpr Curl JOE_FINGER_GUN{0.113f,0.033f,1.0f,1.0f,1.0f};  // right hand, photo freeze 15:04:58
constexpr Curl LEFT_THUMB_UP{0.274f,1.0f,0.998f,1.0f,1.0f};   // left fist, thumb in the air (classes run)
constexpr Curl OPEN_HAND{0.0f,0.087f,0.112f,0.144f,0.113f};   // right hand, photo freeze 14:38:14
constexpr Curl ON_STICK{0.838f,0.9f,1.0f,1.0f,1.0f};          // left thumb on the stick while walking
constexpr Curl ON_B{0.977f,0.245f,0.204f,0.235f,0.203f};      // left thumb on B, photo freeze 14:37:45
constexpr Curl LEFT_OPEN_AIR{0.283f,0.245f,0.204f,0.235f,0.203f}; // left hand open, thumb in the air

// Left-handed play puts B under the right thumb, so the right thumb is the one that lifts.
void photo_hand_cases() {
    const auto t0=Clock::time_point{}+std::chrono::hours(1);
    Shaper right_handed{Mode::off},left_handed{Mode::play,{},1};
    Shaper::Fingers lifted{std::nullopt,LEFT_OPEN_AIR},on_b{std::nullopt,ON_B};
    left_handed.apply(lifted,false,t0);left_handed.apply(lifted,false,t0+milliseconds(400));
    left_handed.apply(on_b,false,t0+milliseconds(500));
    const auto entered=left_handed.apply(on_b,true,t0+milliseconds(1500));
    require(left_handed.photo_latches()==1 && entered[1] && (*entered[1])[0]==LEFT_OPEN_AIR[0],"left-handed, the right thumb lifted before the hold stays up");
    Shaper ordinary{Mode::play};
    ordinary.apply(lifted,false,t0);ordinary.apply(lifted,false,t0+milliseconds(400));ordinary.apply(on_b,false,t0+milliseconds(500));
    ordinary.apply(on_b,true,t0+milliseconds(1500));
    require(ordinary.photo_latches()==0,"right-handed, the right thumb never latches");
}

void shape_cases() {
    const auto up=shape(JOE_THUMBS_UP);
    require(up==Curl{0,1,1,1,1},"a lifted thumb over a closed grip is a full thumbs-up, the loose middle finger closes");
    const auto gun=shape(JOE_FINGER_GUN);
    require(gun==Curl{0,0.033f,1,1,1},"a finger gun keeps its pointing index; only the thumb goes fully up");
    require(shape(LEFT_THUMB_UP)==Curl{0,1,1,1,1},"the left hand's resting 0.27 thumb over a fist goes fully up");
    require(shape(OPEN_HAND)==OPEN_HAND,"an open hand is not a thumbs-up and keeps its live curl");
    require(shape(ON_STICK)==ON_STICK && shape(ON_B)==ON_B,"a thumb on the stick or a button is never lifted");
}

void photo_thumb_cases() {
    const auto t0=Clock::time_point{}+std::chrono::hours(1);
    // Joe's entry at 296.5 s: thumb in the air for 4.5 s, then on B for the 1 s hold; the
    // request came 1.5 s after the thumb came down.
    {
        PhotoThumb photo;
        for(int ms=0;ms<=4500;ms+=33) require(!photo.update(LEFT_THUMB_UP,false,t0+milliseconds(ms)),"nothing is latched in play");
        for(int ms=4500;ms<6000;ms+=33) require(!photo.update(ON_B,false,t0+milliseconds(ms)),"holding B is still play");
        const auto latched=photo.update(ON_B,true,t0+milliseconds(6000));
        require(latched && *latched==LEFT_THUMB_UP[0],"the request recalls the lifted thumb (its lowest curl) for the freeze");
        require(photo.update(ON_B,true,t0+milliseconds(6100))==latched,"the recalled thumb holds through releasing/opening_pause/opening_photo");
        require(!photo.update(ON_B,false,t0+milliseconds(6200)),"the freeze (or the end of the sequence) drops it");
        require(photo.requests()==1 && photo.latches()==1,"one request, one latch");
    }
    // Thumb straight from the stick to B: the 70-120 ms gap in the air is the thumb moving, not a pose.
    {
        PhotoThumb photo;
        for(int ms=0;ms<3000;ms+=33) photo.update(ON_STICK,false,t0+milliseconds(ms));
        for(int ms=3000;ms<3100;ms+=33) photo.update(LEFT_THUMB_UP,false,t0+milliseconds(ms));
        for(int ms=3100;ms<4200;ms+=33) photo.update(ON_B,false,t0+milliseconds(ms));
        require(!photo.update(ON_B,true,t0+milliseconds(4200)),"a transit flicker is not recalled: the thumb stays on B");
        require(photo.requests()==1 && photo.latches()==0,"counted as a request without a latch");
    }
    // A lift that ended long before (thumb back on the stick walking in between).
    {
        PhotoThumb photo;
        for(int ms=0;ms<2000;ms+=33) photo.update(LEFT_THUMB_UP,false,t0+milliseconds(ms));
        for(int ms=2000;ms<5000;ms+=33) photo.update(ON_STICK,false,t0+milliseconds(ms));
        for(int ms=5000;ms<6100;ms+=33) photo.update(ON_B,false,t0+milliseconds(ms));
        require(!photo.update(ON_B,true,t0+milliseconds(6100)),"a lift 4 s before the request is not recalled");
    }
    // Losing the hand breaks a lift in progress.
    {
        PhotoThumb photo;
        for(int ms=0;ms<1000;ms+=33) photo.update(LEFT_THUMB_UP,false,t0+milliseconds(ms));
        photo.update(std::nullopt,false,t0+milliseconds(1000));
        for(int ms=1033;ms<1200;ms+=33) photo.update(LEFT_THUMB_UP,false,t0+milliseconds(ms));
        for(int ms=1200;ms<2300;ms+=33) photo.update(ON_B,false,t0+milliseconds(ms));
        require(!photo.update(ON_B,true,t0+milliseconds(2300)),"only the 170 ms after the hand came back count: too short");
    }
}

void shaper_cases() {
    const auto t0=Clock::time_point{}+std::chrono::hours(2);
    const Shaper::Fingers live{LEFT_THUMB_UP,JOE_FINGER_GUN};
    {
        Shaper off{Mode::off};
        require(off.apply(live,false,t0)==live && off.apply(live,true,t0)==live,"off: the live curl, exactly as before");
    }
    {
        Shaper play{Mode::play};
        const auto out=play.apply(live,false,t0);
        require(out[0]==Curl{0,1,1,1,1} && out[1]==Curl{0,0.033f,1,1,1},"play: both hands shaped in normal play");
        require(play.shaped(0)==1 && play.shaped(1)==1,"shaped hands are counted");
        require(play.apply({OPEN_HAND,std::nullopt},false,t0)==Shaper::Fingers{OPEN_HAND,std::nullopt},"a missing hand stays missing");
    }
    {
        Shaper photo{Mode::photo};
        require(photo.apply(live,false,t0)==live,"photo: normal play keeps the live curl");
        require(photo.shaped(0)==0 && photo.shaped(1)==0,"photo: nothing shaped in normal play");
    }
    {
        Shaper photo{Mode::photo};
        // Left open hand with the thumb in the air, then B: the left hand freezes open with the
        // thumb out instead of pressing B; the right thumbs-up is shaped while entering.
        for(int ms=0;ms<=1000;ms+=33) photo.apply({LEFT_OPEN_AIR,ON_STICK},false,t0+milliseconds(ms));
        for(int ms=1000;ms<2100;ms+=33) photo.apply({ON_B,JOE_THUMBS_UP},false,t0+milliseconds(ms));
        const auto out=photo.apply({ON_B,JOE_THUMBS_UP},true,t0+milliseconds(2100));
        require(out[0]==Curl{LEFT_OPEN_AIR[0],ON_B[1],ON_B[2],ON_B[3],ON_B[4]},"the left thumb is the recalled one; its open fingers stay live");
        require(out[1]==Curl{0,1,1,1,1},"photo: the right thumbs-up is shaped up to the freeze");
        require(photo.photo_requests()==1 && photo.photo_latches()==1,"photo request and latch counted");
    }
    {
        // Recalled left thumb over a closed grip: a full left thumbs-up at the freeze.
        Shaper play{Mode::play};
        for(int ms=0;ms<=1000;ms+=33) play.apply({LEFT_THUMB_UP,std::nullopt},false,t0+milliseconds(ms));
        const Curl fist_on_b{1.0f,1.0f,1.0f,1.0f,1.0f};
        for(int ms=1000;ms<2100;ms+=33) require(play.apply({fist_on_b,std::nullopt},false,t0+milliseconds(ms))[0]==fist_on_b,"during the B hold the thumb is on B");
        require(play.apply({fist_on_b,std::nullopt},true,t0+milliseconds(2100))[0]==Curl{0,1,1,1,1},"lift the thumb, then hold B: a left thumbs-up at the freeze");
    }
}

void parse_cases() {
    require(parse("")==Mode::play && parse("play")==Mode::play && parse("photo")==Mode::photo && parse("off")==Mode::off,"knob values");
    require(!parse("Play") && !parse("1"),"anything else is refused");
    require(std::string_view{name(Mode::photo)}=="photo" && std::string_view{name(Mode::play)}=="play" && std::string_view{name(Mode::off)}=="off","names round-trip");
}
}

int main() {
    try {
        shape_cases();photo_thumb_cases();photo_hand_cases();shaper_cases();parse_cases();
        std::cout<<"thumbs-up shape, photo thumb recall, modes and knob parsing passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
