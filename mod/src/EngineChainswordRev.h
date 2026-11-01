#pragma once
#include <Windows.h>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <array>
#include <string_view>
#include <algorithm>
#include <cmath>
#include <utility>
#include "EngineChainswordAudio.h"

// Owned native teeth and rev audio. No attack input, body animation or movement.
class EngineChainswordRev {
public:
    using Clock=std::chrono::steady_clock;
    struct String {
        std::array<unsigned char,16> bytes{};
        explicit String(std::string_view text) { if(text.size()<=15){std::copy(text.begin(),text.end(),bytes.begin());bytes[15]=static_cast<unsigned char>((15-text.size())*2);} }
    };
    struct Api {
        std::function<bool(uintptr_t,void*,size_t)> read;
        std::function<bool(uintptr_t,const String&)> exists;
        std::function<void(uintptr_t,const String&,float,const String&,bool)> start;
        std::function<void(uintptr_t,const String&)> stop;
        std::function<float(uintptr_t)> time;
        std::function<void(uintptr_t,float)> rate; // Optional native PropAnim SetPlayRate.
    };
    // GetCurAnimTime observes layer zero, not the chain's overlay layer.
    // refused counts every refusal; the reasons split the start refusals (a rev that never
    // started): table = the prop-animation list was unreadable or over its bound, missing =
    // no owned chainsword in it, ambiguous = two owned chainswords, animation = the owned prop
    // lacks chain_anim/chain_anim_stop. 26 Oct played 379 refusals and no reason to tell them apart.
    struct Refusals {uint64_t table{},missing{},ambiguous{},animation{};};
    struct Snapshot {bool bound{};uintptr_t actor{},property{};uint64_t starts{},stops{},refused{};float base_layer_time{},play_rate{1};Refusals reasons{};};
    // Upper bound on the prop-animation list walked by a search (count, not capacity). The
    // list's capacity only grows: the Battle Barge left it above the old 256 bound for good.
    static constexpr uint32_t MAX_PROPS=8192;
    // Trigger depth to teeth speed: a slow idle crawl just past the deadzone up to a
    // fast full-trigger rev. The curve keeps fine control in the first half of travel.
    static constexpr float DEADZONE=.05f,MIN_RATE=.35f,MAX_RATE=2.5f;
    // A finger resting on the Index trigger reads about 0.05-0.09 (Joe's 20 October OpenXR
    // trace: 5 s of such samples). A rev starts above START and runs until the trigger falls
    // to RELEASE, so a rested finger neither starts one nor keeps one crawling.
    static constexpr float START=.15f,RELEASE=.10f;
    static bool rev_held(float request,bool revving){return std::isfinite(request) && request>(revving?RELEASE:START);}
    static float rev_amount(float request){return std::isfinite(request)?std::clamp((request-DEADZONE)/(1-DEADZONE),0.f,1.f):0.f;}
    static float rev_rate(float request){const float s=rev_amount(request);return MIN_RATE+(MAX_RATE-MIN_RATE)*s*std::sqrt(s);}
    struct Request {uintptr_t actor{};float force{};};
    static EngineChainswordRev& get();
    bool bind_in_game(HMODULE);
    bool install_in_game(HMODULE,std::function<Request()>);
    bool install(void*,std::function<Request()>);
    void uninstall();
    bool installed()const{return m_target!=nullptr;}
    void native_update(uintptr_t property,const Request&,Clock::time_point);
    void bind(uintptr_t base,Api);
    void bind_audio(EngineChainswordAudio::Api);
    void release_audio_unless(uintptr_t actor,float request);
    EngineChainswordAudio::Snapshot audio_snapshot()const;
    // updating: the prop animation whose native update is running (the hook's argument). A
    // rev starts on it when it is the actor's chainsword, without searching the global list.
    void tick(uintptr_t observed_actor,uintptr_t active_actor,float request,Clock::time_point,uintptr_t updating=0);
    Snapshot snapshot()const;
private:
    template<class T> bool read(uintptr_t p,T& out)const{return p && m_api.read && m_api.read(p,&out,sizeof out);}
    uintptr_t q(uintptr_t p)const {uintptr_t v{};read(p,v);return v;}
    std::optional<uintptr_t> geometry_property(uintptr_t props)const;
    bool owned(uintptr_t prop,uintptr_t actor)const;
    enum class Found {prop,table,missing,ambiguous};
    std::pair<Found,uintptr_t> find(uintptr_t actor)const;
    static void update_hook(uintptr_t);
    using Update=void(*)(uintptr_t);
    Update m_original{};void* m_target{};std::function<Request()> m_request;
    mutable std::mutex m_mutex;uintptr_t m_base{};Api m_api;Snapshot m_state;
    EngineChainswordAudio m_audio;
};
