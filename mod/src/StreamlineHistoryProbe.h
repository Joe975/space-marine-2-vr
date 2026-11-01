#pragma once
#include <Windows.h>
#include <sl_core_api.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <ostream>
#include <vector>
#include "SdkFrameTokens.h"
#include "StreamlineOptionsProbe.h"
#include "OrdinaryDlssContext.h"
#include <array>

// Passive, bounded API argument observations during an explicitly ticketed
// ordinary stereo Scene. Tagged native pointers are POD, never resource leases.
class StreamlineHistoryProbe {
public:
    struct Scope {
        uint64_t dispatch{},render_id{},revision{};uint32_t token{},slot{};uintptr_t entry{},list{};
        bool operator==(const Scope&)const=default;
    };
    struct Tag {uint32_t type{},lifecycle{},state{},resource_type{},width{},height{};uintptr_t native{};std::array<uint32_t,4> extent{};};
    enum class Kind:uint32_t {evaluate,tags,tags_for_frame,constants};
    struct Constants {
        uint32_t version{},reset{},depth_inverted{};
        std::array<float,16> projection{},inverse_projection{},clip_to_previous{},previous_to_clip{};
        std::array<float,3> position{},right{},up{},forward{};
        std::array<float,2> jitter{},motion_scale{};
        float near_plane{},far_plane{},fov{},aspect{};
    };
    struct Event {
        Scope scope;Kind kind{};uint32_t frame{},frame_after{},feature{},thread{};uintptr_t command{},frame_object{};
        std::optional<SdkFrameTokens::Returned> returned_frame;bool returned_frame_current{};
        std::optional<Constants> constants;
        std::optional<sl::Constants> native_constants;
        std::optional<StreamlineOptionsProbe::Receipt> dlss_options;bool options_current{};
        std::optional<uint32_t> viewport;bool readable{true},scope_current{},unknown_inputs{};
        sl::Result result{};std::vector<Tag> tags;
    };
    struct Batch {bool installed{};uint64_t dropped{};std::vector<Event> events;};
    // Synchronous callback inside the actual API call, never from stored events.
    std::function<void(const Event&,bool)> evaluation_boundary;
    using Provider=std::function<std::optional<Scope>()>;
    static StreamlineHistoryProbe& get();
    bool arm(Provider);
    void continuous(bool value){m_continuous=value;}
    using CameraProvider=std::function<std::optional<OrdinaryDlssContext::Cameras>()>;
    void arm_private(CameraProvider cameras,OrdinaryDlssContext::SetOptions options,OrdinaryDlssContext::SetConstants constants){m_cameras=std::move(cameras);m_private.arm(std::move(options),std::move(constants),m_continuous);m_private_requested=true;}
    bool private_requested()const{return m_private_requested.load();}
    // Jobs the private eye must leave alone (a recoverable packet refusal, 23 Oct): their right-eye
    // evaluation stays on the native SDK path. Without this the job (no tracked cameras, so no
    // request) would poison the private context, and every later packet would refuse its SDK frame.
    using Skip=bool(*)(uint64_t dispatch);
    void skip_private(Skip skip){m_skip.store(skip);}
    std::optional<OrdinaryDlssContext::Receipt> private_accepted(uint64_t dispatch)const{
        if(!m_private_requested.load())return {};std::optional<OrdinaryDlssContext::Receipt> accepted;
        for(const auto& r:m_private.receipts())if(r.dispatch==dispatch){if(accepted || r.refusal || r.viewport!=OrdinaryDlssContext::VIEWPORT || (r.options && *r.options!=sl::Result::eOk) || r.constants!=sl::Result::eOk || r.evaluation!=sl::Result::eOk)return {};accepted=r;}return accepted;
    }
    std::function<void(sl::Result)> constants_begin(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&);
    void reset(){m_tokens.returned(0,0,std::nullopt,0,false);m_private.reset();}
    // Cold restart (OrdinaryColdRestart): un-poison the private eye. True when nothing needed doing.
    bool restart_private(){return !m_private_requested.load() || m_private.restart();}
    bool private_poisoned()const{return m_private_requested.load() && m_private.poisoned();}
    void poll(std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
    Batch sample(uint64_t dispatch)const;
    static void write(std::ostream&,const Batch&);
private:
    using SetTag=sl::Result(const sl::ViewportHandle&,const sl::ResourceTag*,uint32_t,sl::CommandBuffer*);
    static sl::Result evaluate(sl::Feature,const sl::FrameToken&,const sl::BaseStructure**,uint32_t,sl::CommandBuffer*);
    static sl::Result tags(const sl::ViewportHandle&,const sl::ResourceTag*,uint32_t,sl::CommandBuffer*);
    static sl::Result tags_for_frame(const sl::FrameToken&,const sl::ViewportHandle&,const sl::ResourceTag*,uint32_t,sl::CommandBuffer*);
    static sl::Result new_frame(sl::FrameToken*&,const uint32_t*);
    std::optional<Event> begin(Kind,sl::CommandBuffer*)const;
    void finish(std::optional<Event>,sl::Result,const sl::FrameToken* frame=nullptr);
    void bind(Event&,const sl::FrameToken&)const;
    sl::Result evaluate_observed(sl::Feature,const sl::FrameToken&,const sl::BaseStructure**,uint32_t,sl::CommandBuffer*);
    static void parse_tags(Event&,const sl::ResourceTag*,uint32_t);
    static void parse_inputs(Event&,const sl::BaseStructure**,uint32_t);
    Provider m_provider;std::atomic_bool m_armed{},m_installed{};bool m_continuous{};
    std::atomic<PFun_slEvaluateFeature*> m_evaluate{};std::atomic<SetTag*> m_tags{};std::atomic<PFun_slSetTagForFrame*> m_tags_frame{};
    std::atomic<PFun_slGetNewFrameToken*> m_new_frame{};SdkFrameTokens m_tokens;
    mutable std::mutex m_mutex;std::mutex m_install;std::map<uint64_t,Batch> m_batches;uint64_t m_untracked{};
    std::chrono::steady_clock::time_point m_next{};
    CameraProvider m_cameras;OrdinaryDlssContext m_private;std::atomic_bool m_private_requested{};std::atomic<Skip> m_skip{};
};
