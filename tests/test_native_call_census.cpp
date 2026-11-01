#include "NativeCallCensus.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
using C=NativeCallCensus;using H=C::Hook;

// One dispatch: views Scenes, each hook called per_view_calls times per view, once more per frame.
void dispatch(C& c,uint32_t views,unsigned grading_index0,unsigned temporal,unsigned pool=7){
    for(unsigned i=0;i<grading_index0;++i)c.count(H::grading_index0);
    for(unsigned i=0;i<temporal;++i)c.count(H::temporal);
    for(unsigned i=0;i<pool;++i)c.count(H::pool_release);
    c.complete(views);
}

void healthy(){
    C c;for(unsigned i=0;i<C::MIN_DISPATCHES;++i){dispatch(c,1,1,1);dispatch(c,2,1,2,9);}
    const auto s=c.summary();
    require(s.ready && s.mono==C::MIN_DISPATCHES && s.stereo==C::MIN_DISPATCHES,"enough of each");
    require(s.rates[size_t(H::scene)].mono==1 && s.rates[size_t(H::scene)].stereo==2,"Scene is the calibration: one per mono dispatch, two per stereo");
    require(s.rates[size_t(H::temporal)].ratio==2 && !s.rates[size_t(H::temporal)].flagged,"a per-view hook doubles: fine");
    require(s.rates[size_t(H::grading_index0)].ratio==1 && !s.rates[size_t(H::grading_index0)].flagged,"a per-frame hook stays once: fine");
    require(!s.rates[size_t(H::pool_release)].flagged && s.doubled.empty() && s.missing.empty(),"mixed hooks are reported, never flagged");
    require(!s.rates[size_t(H::sharpen)].seen && s.line.find("sharpen")==std::string::npos,"hooks that never ran are left out");
    require(s.line.find(' ',s.line.find("rates="))>s.line.find(']',s.line.find("rates=")),"the rates list has no spaces (run_report.py reads key=value fields)");
}

// 25 Oct: the second eye re-ran per-frame work (the pool release). A per-frame hook at twice its
// mono rate is doubled; a per-view hook that runs once in stereo missed the second eye.
void flagged(){
    C c;for(unsigned i=0;i<C::MIN_DISPATCHES;++i){dispatch(c,1,1,1);dispatch(c,2,2,1);}
    const auto s=c.summary();
    require(s.rates[size_t(H::grading_index0)].flagged && s.doubled=="grading_index0","a per-frame hook running per eye is doubled");
    require(s.rates[size_t(H::temporal)].flagged && s.missing=="temporal","a per-view hook that ran once in stereo is missing");
    require(s.line.find("doubled=[grading_index0] missing=[temporal]")!=std::string::npos,"the line names both");
    C d;for(unsigned i=0;i<C::MIN_DISPATCHES;++i){dispatch(d,1,1,1);dispatch(d,2,1,5);}
    require(d.summary().doubled=="temporal","a per-view hook far above twice its mono rate is doubled too");
}

// 26 Oct playtest: the late effect ran 1.00 per mono dispatch, and per stereo dispatch 2.00 for
// minutes and 0.14-0.95 in other stretches. Scene calls it per view but only when the view's entry
// allows it, and in stereo the mod writes that entry's camera-cut byte, so it is mixed, not missing.
void gated(){
    require(C::SPECS[size_t(H::late_effect)].kind==C::Kind::mixed,"the late effect is a gated call");
    C c;for(unsigned i=0;i<C::MIN_DISPATCHES;++i){c.count(H::late_effect);c.complete(1);
        if(i%2==0){c.count(H::late_effect);c.count(H::late_effect);}c.complete(2);}
    const auto s=c.summary();const auto& r=s.rates[size_t(H::late_effect)];
    require(s.ready && r.ratio==1 && !r.flagged && s.missing.empty() && s.doubled.empty(),"a gated stretch in stereo is reported, never missing");
    require(s.line.find("late_effect:1.00/1.00")!=std::string::npos,"and its rates stay in the line");
}

void bounded(){
    C c;for(unsigned i=0;i<C::MIN_DISPATCHES-1;++i){dispatch(c,1,1,1);dispatch(c,2,2,1);}
    const auto s=c.summary();require(!s.ready && s.doubled.empty() && s.missing.empty() && s.rates[size_t(H::grading_index0)].ratio==2,"too few dispatches: rates, no verdict");
    c.count(H::temporal);c.count(H::temporal);c.complete(0);c.complete(3);
    require(c.summary().mono==C::MIN_DISPATCHES-1 && c.summary().stereo==C::MIN_DISPATCHES-1,"a dispatch with no or three Scenes is not counted");
    dispatch(c,1,0,0,0);require(c.summary().rates[size_t(H::temporal)].mono*double(c.summary().mono)==double(C::MIN_DISPATCHES-1),"its calls were dropped, not carried into the next dispatch");
    c.reset();require(!c.summary().mono && !c.summary().stereo,"reset");
    require(std::string{C::SPECS[size_t(H::sdk_constants_viewport0)].name}=="sdk_constants_viewport0" && C::SPECS.size()==C::HOOKS,"one spec per hook");
}
}
// The DLSS hold reads a job's evaluations (OrdinaryDlssPresence), and complete() takes them.
void pending(){
    C c;require(!c.pending(H::sdk_evaluate),"nothing counted yet");
    c.count(H::sdk_evaluate);c.count(H::sdk_evaluate);require(c.pending(H::sdk_evaluate)==2 && !c.pending(H::sharpen),"a hook's calls since the last completion");
    c.complete(2);require(!c.pending(H::sdk_evaluate),"the completion takes them");
}

int main(){try{healthy();flagged();gated();bounded();pending();std::cout<<"native call census: calibration, doubled and missing hooks, gated hooks, bounds, pending PASS\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
