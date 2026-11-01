#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

// The general check from docs/SECOND_EYE_SIDE_EFFECTS.md (GENERIC_GUARDS piece 4). The second eye adds
// a camera to the game's render list, so everything in Scene runs once per view. Per-view work should
// double in stereo; per-frame work must not (25 Oct: the pool release ran twice and froze every level
// exit; found from a hang dump after an afternoon). Each hook the mod already has counts its calls;
// at every dispatch completion the counts since the last one go to the mono (one Scene) or stereo
// (two Scenes) bucket. A hook whose stereo rate breaks its kind is reported: `doubled` (a per-frame
// hook running more often in stereo, or a per-view hook more than twice as often) or `missing` (a
// per-view hook that did not run for the second eye). Mixed hooks are reported, never flagged.
// No new hooks: only detours that are installed anyway count (a new hook is another address to port).
class NativeCallCensus {
public:
    enum class Kind:uint32_t {per_view,per_frame,mixed};
    enum class Hook:uint32_t {scene,temporal,grading,grading_index0,reflection,late_effect,early_effect,sharpen,sdk_evaluate,sdk_constants,sdk_constants_viewport0,pool_release,count};
    struct Spec {const char* name;Kind kind;};
    static constexpr size_t HOOKS=size_t(Hook::count);
    // scene is the calibration: two Scenes per stereo dispatch by definition.
    static constexpr std::array<Spec,HOOKS> SPECS{{
        {"scene",Kind::per_view},{"temporal",Kind::per_view},{"grading",Kind::per_view},
        // SECOND_EYE_SIDE_EFFECTS 3: index 0 stepped the eye adaptation twice per stereo frame (26 Oct:
        // 1.00/2.00). Counts only the steps that advance it, so the right eye's step that
        // NativeOrdinaryGrading undoes is left out: doubled again means that undo stopped applying.
        {"grading_index0",Kind::per_frame},
        {"reflection",Kind::per_view},
        // Called from Scene per view, but only when the view's entry allows it (flag bits 17 and 25,
        // the camera-cut byte +0xa35 clear, a target texture). In stereo the mod writes the cut byte
        // of both entries itself (NativeOrdinaryPacket), so the two buckets sample different gates:
        // 26 Oct it ran 2.00 per stereo dispatch for minutes and 0.14-0.95 in other stretches
        // (both eyes quiet: one missing eye cannot go below 1), against 1.00 mono. Not per view.
        {"late_effect",Kind::mixed},
        {"early_effect",Kind::per_view},{"sharpen",Kind::per_view},
        {"sdk_evaluate",Kind::per_view},{"sdk_constants",Kind::mixed}, // the private eye submits its own as well
        {"sdk_constants_viewport0",Kind::per_frame}, // SECOND_EYE_SIDE_EFFECTS 8: viewport 0 may get the right eye's constants too
        {"pool_release",Kind::mixed}, // per-view and per-frame textures; NativePoolReleaseGuard skips the double releases
    }};
    static constexpr uint64_t MIN_DISPATCHES=120;
    static constexpr double PER_FRAME_LIMIT=1.25,PER_VIEW_LOW=1.75,PER_VIEW_HIGH=2.25;

    static NativeCallCensus& get(){static auto* value=new NativeCallCensus;return *value;}
    // Any thread, inside the hook.
    void count(Hook hook){m_pending[size_t(hook)].fetch_add(1,std::memory_order_relaxed);}
    // Calls since the last completion, read before complete() takes them.
    uint64_t pending(Hook hook)const{return m_pending[size_t(hook)].load(std::memory_order_relaxed);}
    // Render thread, at each dispatch completion: views = the dispatch's Scene calls (1 mono, 2 stereo).
    void complete(uint32_t views){
        if(views<1 || views>2){for(auto& p:m_pending)p.store(0,std::memory_order_relaxed);return;}
        auto& bucket=m_buckets[views-1];bucket.dispatches.fetch_add(1,std::memory_order_relaxed);
        bucket.calls[size_t(Hook::scene)].fetch_add(views,std::memory_order_relaxed);
        for(size_t i=0;i<HOOKS;++i)if(i!=size_t(Hook::scene))bucket.calls[i].fetch_add(m_pending[i].exchange(0,std::memory_order_relaxed),std::memory_order_relaxed);
    }
    struct Rate {double mono{},stereo{},ratio{};bool flagged{};bool seen{};};
    struct Summary {uint64_t mono{},stereo{};std::array<Rate,HOOKS> rates{};std::string doubled,missing,line;bool ready{};};
    Summary summary()const{
        Summary s;s.mono=m_buckets[0].dispatches.load();s.stereo=m_buckets[1].dispatches.load();
        s.ready=s.mono>=MIN_DISPATCHES && s.stereo>=MIN_DISPATCHES;
        const auto list=[](std::string& out,const char* name){if(!out.empty())out+=',';out+=name;};
        for(size_t i=0;i<HOOKS;++i){auto& r=s.rates[i];
            const auto mono=m_buckets[0].calls[i].load(),stereo=m_buckets[1].calls[i].load();
            r.seen=mono || stereo;if(!s.mono || !s.stereo || !r.seen)continue;
            r.mono=double(mono)/double(s.mono);r.stereo=double(stereo)/double(s.stereo);r.ratio=r.mono>0?r.stereo/r.mono:0;
            if(!s.ready || r.mono<=0)continue;
            const auto kind=SPECS[i].kind;
            if(kind==Kind::per_frame && r.ratio>PER_FRAME_LIMIT){r.flagged=true;list(s.doubled,SPECS[i].name);}
            if(kind==Kind::per_view && r.ratio>PER_VIEW_HIGH){r.flagged=true;list(s.doubled,SPECS[i].name);}
            if(kind==Kind::per_view && r.ratio<PER_VIEW_LOW){r.flagged=true;list(s.missing,SPECS[i].name);}
        }
        s.line="mono="+std::to_string(s.mono)+" stereo="+std::to_string(s.stereo)+" ready="+(s.ready?"true":"false")+" rates=[";
        bool first=true;for(size_t i=0;i<HOOKS;++i){const auto& r=s.rates[i];if(!r.seen)continue;char text[96];
            std::snprintf(text,sizeof(text),"%s%s:%.2f/%.2f",first?"":",",SPECS[i].name,r.mono,r.stereo);s.line+=text;first=false;}
        s.line+="] doubled=["+s.doubled+"] missing=["+s.missing+"]";return s;
    }
    void reset(){for(auto& p:m_pending)p.store(0);for(auto& b:m_buckets){b.dispatches.store(0);for(auto& c:b.calls)c.store(0);}}
private:
    struct Bucket {std::atomic<uint64_t> dispatches{};std::array<std::atomic<uint64_t>,HOOKS> calls{};};
    std::array<std::atomic<uint64_t>,HOOKS> m_pending{};std::array<Bucket,2> m_buckets{};
};
