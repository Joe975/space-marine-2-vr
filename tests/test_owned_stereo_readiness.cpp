// Owned stereo readiness: the resume window after a brief same-owner refusal
// (docs/STEREO_PAUSES_2026-10-26.md). The three-job startup rules themselves are covered in
// test_native_tracked_ordinary_camera.cpp (owned_readiness_rules); this file checks only what
// the resume window changes, then replays the 18:30 campaign run's pauses.
#include "OwnedStereoReadiness.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using R=OwnedStereoReadiness;
using namespace std::chrono_literals;

void require(bool ok,const char* message){if(!ok)throw std::runtime_error{message};}

R::Observation fresh(R::Clock::time_point now){return {11,{11,22,33,44,55,66,77},now,now,true,true,true};}
// The player state the render job sees after a long game frame: published 150 ms ago.
R::Observation stale(R::Clock::time_point now){auto value=fresh(now-150ms);return value;}

// Jobs refused before the next ready one, from job `job` on at `dt` intervals.
struct Runner {
    R readiness;uint64_t job{};R::Clock::time_point now{R::Clock::time_point{}+100s};std::chrono::milliseconds dt{40};
    bool step(const R::Observation& value){++job;now+=dt;return readiness.observe(job,value,now);}
    bool step_fresh(){return step(fresh(now+dt));}
    // A fresh observation of another owner (one identity part changed).
    bool step_other(size_t part,uintptr_t id){auto value=fresh(now+dt);value.owner[part]=id;if(!part)value.actor=id;return step(value);}
    void ready(){for(int i=0;i<3;++i)step_fresh();require(readiness.snapshot().reason==R::Reason::ready,"fixture reaches readiness");}
    unsigned settle(){unsigned refused{};while(!step_fresh()){if(++refused>10)throw std::runtime_error{"never ready"};}return refused;}
};

void resume_rules(){
    // Each refusal that only says the player state was late keeps the credit.
    for(int fault=0;fault<5;++fault){Runner r;r.ready();
        auto value=fresh(r.now+r.dt);
        if(fault==0)value=stale(r.now+r.dt);
        if(fault==1)value.actor=0;          // the body tracker past 100 ms (actor_missing)
        if(fault==2)value.current=false;    // the arm pose past 100 ms in the native check (owner_invalid)
        if(fault==3)value.pose=false;       // no pose published yet this frame (pose_missing)
        if(fault==4){++r.job;r.now+=r.dt;r.readiness.gap(true);} // a frame-clock skip
        else require(!r.step(value),"the late job itself is still refused");
        require(R::keeps_credit(r.readiness.snapshot().reason) || fault==4,"a freshness reason");
        require(r.step_fresh() && r.readiness.snapshot().resumes==1 && r.readiness.snapshot().consecutive==3,"the next fresh job of the same owner is ready at once");
    }
    // Everything else settles over three jobs again, as before.
    for(int fault=0;fault<9;++fault){Runner r;r.ready();auto value=fresh(r.now+r.dt);
        if(fault==0)value.menu=true;
        if(fault==1)value.loading=true;
        if(fault==2)value.world=false;
        if(fault==3)value.received+=1s; // a future timestamp
        if(fault==4){value.owner[2]=123;} // a replaced controller: refused as owner_changed, then settles
        if(fault==5){++r.job;r.now+=r.dt;r.readiness.gap();} // a foreign view, a suspension, a restart
        else if(fault==6){value=R::Observation{};value.viewer=true;require(r.step(value),"the viewer is ready");}
        else if(fault==7){value=R::Observation{};value.cutscene=true;require(r.step(value),"a cutscene is ready");}
        else if(fault==8){r.step(stale(r.now+r.dt));r.now+=1100ms;} // late for longer than the window
        else require(!r.step(value),"refused");
        if(fault==4){require(r.readiness.snapshot().reason==R::Reason::owner_changed,"owner change");
            // The replaced owner needs its own three jobs; the old owner's credit is gone too.
            require(!r.step_other(2,123) && r.step_other(2,123),"a replaced owner needs three jobs");
            auto late=stale(r.now+r.dt);late.owner[2]=123;r.step(late);
            require(r.step_other(2,123) && r.readiness.snapshot().resumes==1,"then the new owner resumes like any other");continue;}
        require(r.settle()==2 && r.readiness.snapshot().resumes==0,"menu, loading, invalid world, future, gap(), viewer, cutscene or a long refusal settles over three jobs");
    }
    // The credit is the exact owner: a different owner after the refusal settles.
    {Runner r;r.ready();r.step(stale(r.now+r.dt));
     require(!r.step_other(6,999) && !r.step_other(6,999) && r.step_other(6,999) && !r.readiness.snapshot().resumes,"another owner after the refusal needs three jobs");}
    // A second check of the refused job cannot earn the credit (the producer checks twice per job).
    {Runner r;r.ready();require(!r.step(stale(r.now+r.dt)),"refused");
     require(!r.readiness.observe(r.job,fresh(r.now),r.now) && r.readiness.snapshot().consecutive==0,"the same job checked again stays refused");
     require(r.step_fresh() && r.readiness.snapshot().resumes==1,"the next job resumes");}
    // Repeated late jobs within the window: each one costs only itself.
    {Runner r;r.ready();for(int i=0;i<5;++i){require(!r.step(stale(r.now+r.dt)),"late");require(r.step_fresh(),"resumes");}
     require(r.readiness.snapshot().resumes==5,"five resumes");}
    // Several late jobs in a row, still inside the window, then one fresh job.
    {Runner r;r.ready();for(int i=0;i<10;++i)r.step(stale(r.now+r.dt));require(r.step_fresh(),"400 ms late, then ready at once");}
    // The window counts from the last ready job.
    {Runner r;r.ready();r.dt=200ms;for(int i=0;i<4;++i)r.step(stale(r.now+r.dt));require(r.step_fresh(),"1000 ms since the last ready job: inside");}
    {Runner r;r.ready();r.dt=201ms;for(int i=0;i<4;++i)r.step(stale(r.now+r.dt));require(!r.step_fresh() && r.settle()==1,"1005 ms: outside, settles");}
    // resume(0) is the old rule.
    {Runner r;r.readiness.resume(0ms);r.ready();r.step(stale(r.now+r.dt));require(r.settle()==2 && !r.readiness.snapshot().resumes,"resume 0: three jobs after every refusal");}
    // Startup never resumes: no owner has been proven yet.
    {Runner r;r.step(stale(r.now+r.dt));require(r.settle()==2,"no credit before the first ready job");}
    // Knob parsing (SM2VR_STEREO_RESUME_MS).
    require(R::resume_from("0")==0ms && R::resume_from("1500")==1500ms && R::resume_from("10000")==10000ms,"whole ms parse");
    require(!R::resume_from("") && !R::resume_from(nullptr) && !R::resume_from("-1") && !R::resume_from("10001") && !R::resume_from("12x") && !R::resume_from("1.5"),"anything else is refused");
    {R value;require(value.resume()==R::RESUME && R::RESUME==1000ms,"default 1000 ms");value.resume(-5ms);require(value.resume()==R::RESUME,"negative keeps the default");
     value.resume(60s);require(value.resume()==R::MAX_RESUME,"capped");}
}

// tests/fixtures/stereo_pauses_2026-10-26.txt (scripts/stereo_pauses.py --fixture): the 18:30
// campaign run's 59 pauses. For each gameplay pause (hitch, clock, freshness) the fixture holds the
// mono jobs before the two settling ones. The old rule must reproduce the recorded mono count;
// the new rule must cost only the triggering jobs. A clock skip is gap(true), a hitch or a
// freshness refusal is a stale observation.
void replay_fixture(const char* path){
    std::ifstream file{path};require(file.good(),"replay fixture readable");
    struct Pause{int index{};std::string cause;double start{};int ms{},mono{},trigger{};};
    std::vector<Pause> pauses;std::string line;
    while(std::getline(file,line)){std::istringstream in{line};std::string kind;in>>kind;if(kind!="p")continue;
        Pause p;in>>p.index>>p.cause>>p.start>>p.ms>>p.mono>>p.trigger;require(!in.fail(),"fixture row parses");pauses.push_back(p);}
    require(pauses.size()==59,"the 18:30 run has 59 pauses");
    const auto replay=[](const Pause& p,std::chrono::milliseconds window,bool same_owner){
        Runner r;r.readiness.resume(window);r.dt=std::chrono::milliseconds(std::max(1,p.ms/(p.mono+1)));r.ready();
        for(int i=0;i<p.trigger;++i){if(p.cause=="clock"){++r.job;r.now+=r.dt;r.readiness.gap(true);}else require(!r.step(stale(r.now+r.dt)),"trigger refused");}
        if(!same_owner){unsigned refused{};while(!r.step_other(0,12))if(++refused>10)throw std::runtime_error{"never ready"};return p.trigger+int(refused);}
        return p.trigger+int(r.settle());
    };
    int old_total{},new_total{},replayed{},by_cause_old[3]{},by_cause_new[3]{};const char* causes[3]{"hitch","clock","freshness"};
    std::cout<<"pause cause recorded_ms recorded_mono old_mono new_mono\n";
    for(const auto& p:pauses){
        if(p.trigger<1){require(p.cause!="hitch" && p.cause!="clock" && p.cause!="freshness","a gameplay pause has a trigger");continue;}
        const int before=replay(p,0ms,true),after=replay(p,R::RESUME,true),other=replay(p,R::RESUME,false);
        std::cout<<p.index<<' '<<p.cause<<' '<<p.ms<<' '<<p.mono<<' '<<before<<' '<<after<<'\n';
        require(before==p.mono,"the old rule reproduces the recorded mono jobs of every gameplay pause");
        require(after==p.trigger && after==p.mono-2,"the new rule costs only the triggering jobs");
        require(other==p.mono,"a different owner after the pause still settles over three jobs");
        for(int c=0;c<3;++c)if(p.cause==causes[c]){by_cause_old[c]+=before;by_cause_new[c]+=after;}
        old_total+=before;new_total+=after;++replayed;
    }
    for(int c=0;c<3;++c)std::cout<<causes[c]<<": mono jobs old "<<by_cause_old[c]<<", new "<<by_cause_new[c]<<'\n';
    std::cout<<"gameplay pauses replayed "<<replayed<<": mono jobs old "<<old_total<<", new "<<new_total<<'\n';
    require(replayed==40 && old_total-new_total==80,"40 gameplay pauses, two settling jobs saved in each");
}

int main(int argc,char** argv){try{
    resume_rules();
    if(argc>1)replay_fixture(argv[1]);
    std::cout<<"owned stereo readiness tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}}
