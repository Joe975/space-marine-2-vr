#include "OrdinaryDlssPresence.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
using P=OrdinaryDlssPresence;

void held_until_a_job_runs_dlss(){
    P p;require(p.held() && !p.missing(),"no pair before a job has proven DLSS runs");
    p.complete(1,false);require(!p.held() && !p.missing(),"a mono job's one evaluation opens it");
    p.complete(2,true);require(!p.held(),"a stereo job's two evaluations keep it open");
}
void one_job_without_evaluation_costs_one_job(){
    P p;p.complete(1,true);p.complete(0,true);require(p.held() && !p.missing(),"the next job waits, and one job reports nothing");
    p.complete(1,true);require(!p.held() && !p.missing() && p.jobs()==0,"the next evaluation clears it and the count");
}
void taa_selections_open_it_like_an_evaluation(){
    P p;p.complete(0,true,1);require(!p.held() && !p.missing(),"a job that selected the TAA targets can carry a pair");
    p.complete(0,true,2);require(!p.held(),"a stereo job selects them twice");
    for(uint64_t i=0;i<P::REPORT_JOBS;++i)p.complete(0,true,0);require(p.held() && p.missing(),"neither: FSR, or TAA stereo off");
    p.complete(0,true,1);require(!p.held() && !p.missing(),"TAA back");
}
void a_change_of_method_holds_one_job(){
    P p;p.complete(1,true);p.complete(0,true,1);require(p.held(),"DLSS to TAA: the next pair must not chain to the last one's proof");
    p.complete(0,true,1);require(!p.held(),"one job only");
    p.complete(1,true);require(p.held(),"and back");
    p.complete(1,true);require(!p.held(),"one job only");
}
void title_screen_and_loads_never_report(){
    P p;for(int i=0;i<10000;++i)p.complete(0,false);
    require(p.held() && !p.missing() && !p.take_change(),"jobs outside play hold stereo and say nothing: DLSS may simply not run there");
}
// The 31 Oct 19:00 run, in the order the log has it, then the way back.
void settings_change_timeline(){
    P p;for(int i=0;i<13058;++i)p.complete(1,true);
    require(!p.held() && !p.take_change(),"DLSS on: open, nothing to report");
    // 19:00:21-19:00:32: applied, still in the pause menu; 400 jobs with no evaluation.
    for(int i=0;i<400;++i)p.complete(0,false);
    require(p.held(),"TAA: the first stereo job after the menu is already held (19:00:32.7 was refused and stopped)");
    require(!p.missing(),"the menu alone reports nothing");
    // 19:00:32.5 on: gameplay. REPORT_JOBS at the 17-40 fps of that minute is 3-7 s.
    for(uint64_t i=1;i<P::REPORT_JOBS;++i)p.complete(0,true);
    require(!p.missing() && !p.take_change(),"one job short of the report");
    p.complete(0,true);require(p.missing() && p.take_change()==true && !p.take_change(),"the setting is reported, once");
    for(int i=0;i<1778;++i)p.complete(0,true);
    require(p.held() && p.missing() && !p.take_change(),"it stays held for as long as the setting does, with no second report");
    // The player turns DLSS back on: stereo may start on the next job.
    p.complete(1,false);require(!p.held() && !p.missing() && p.jobs()==0,"the first evaluation releases it, in the menu too");
    require(p.take_change()==false && !p.take_change(),"the return is reported once");
    // And a second round reports again.
    for(uint64_t i=0;i<P::REPORT_JOBS;++i)p.complete(0,true);
    require(p.take_change()==true,"a second change away from DLSS reports again");
}
}
int main(){
    try{
        held_until_a_job_runs_dlss();one_job_without_evaluation_costs_one_job();taa_selections_open_it_like_an_evaluation();a_change_of_method_holds_one_job();title_screen_and_loads_never_report();settings_change_timeline();
        std::cout<<"ordinary_dlss_presence: ok\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
