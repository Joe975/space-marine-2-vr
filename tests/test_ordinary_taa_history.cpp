#include "OrdinaryTaaHistory.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
using H=OrdinaryTaaHistory;
constexpr uintptr_t A=0xa000,B=0xb000,NATIVE=0xc000;

// The job the 1 Nov capture shows: both eyes on the game's pair, the right eye sampling the left's image.
void a_bootstrap_stays_on_the_games_textures(){
    H h;h.begin(10,0,0);require(!h.owned(10) && h.previous(10,NATIVE)==NATIVE,"no private pair: the game's own arguments");
    h.temporal(10,true,NATIVE);require(h.accepted(10),"a bootstrap is a valid pair for its one frame");
    require(h.stats().bootstraps==1 && !h.stats().owned,"counted as a bootstrap");
}
void the_first_private_job_resets_then_the_chain_runs(){
    H h;h.begin(10,0,0);h.begin(11,1,B);require(h.owned(11) && h.previous(11,NATIVE)==0,"never the game's texture: it holds the other eye");
    h.temporal(11,true,B);h.begin(12,0,A);require(h.previous(12,B)==B,"the next job samples its own predecessor");
    h.temporal(12,true,A);h.begin(13,1,B);require(h.previous(13,A)==A,"and so on");
    require(h.stats().resets==1,"only the first private job reset");
}
void a_gap_resets_after_mono(){
    H h;h.begin(11,1,B);h.begin(12,0,A);require(h.previous(12,B)==B,"chained");
    h.begin(20,0,A);require(h.previous(20,B)==0,"jobs skipped for mono: no old image is a predecessor");
}
void the_same_parity_twice_resets(){
    H h;h.begin(11,1,B);h.begin(12,1,B);require(h.previous(12,B)==0,"a texture is never its own predecessor");
}
void a_wrong_native_previous_resets_instead_of_sampling_it(){
    H h;h.begin(11,1,B);h.begin(12,0,A);require(h.previous(12,NATIVE)==0,"the bank doesn't name the predecessor: fresh history, never its image");
    require(h.stats().previous_mismatches==1,"counted");
}
void a_bootstrap_breaks_the_chain(){
    H h;h.begin(11,1,B);h.begin(12,0,0);h.begin(13,1,B);require(h.previous(13,A)==0,"after a renewal's bootstrap the pair starts fresh");
}
void a_job_is_accepted_only_on_one_pass_on_its_own_texture(){
    H h;h.begin(11,1,B);require(!h.accepted(11),"no pass yet");
    h.temporal(11,true,B);require(h.accepted(11),"one pass on the private texture");
    h.temporal(11,true,B);require(!h.accepted(11),"two passes");
    H wrong;wrong.begin(11,1,B);wrong.temporal(11,true,A);require(!wrong.accepted(11),"the pass ran on another texture");
    H refused;refused.begin(11,1,B);refused.temporal(11,false,B);require(!refused.accepted(11) && refused.stats().failures==1,"a refused event");
    require(!h.accepted(99),"a job TAA never saw, as with DLSS on");
}
void a_failed_job_is_no_predecessor(){
    H h;h.begin(11,1,B);h.temporal(11,false,B);h.begin(12,0,A);require(h.previous(12,B)==0,"its image is not trusted");
}
void no_private_pair_for_long_stalls(){
    H h;for(uint64_t i=1;i<H::STALL_JOBS;++i)h.begin(i,uint32_t(i),0);require(!h.stalled(),"bootstraps while a pair is being made");
    h.begin(H::STALL_JOBS,0,0);require(h.stalled(),"both eyes would stay on one history: stereo waits");
    h.begin(H::STALL_JOBS+1,1,B);require(!h.stalled(),"a pair clears it");
}
void a_stall_tries_again_on_its_own(){
    H h;uint64_t job=1;const auto bootstraps=[&](uint32_t count){for(uint32_t i=0;i<count;++i,++job)h.begin(job,uint32_t(job),0);};
    const auto wait_out=[&]{for(uint32_t i=1;i<H::RETRY_JOBS;++i)require(h.wait_job() && h.stalled(),"stereo waits, so no right-eye job can clear it");
        require(!h.wait_job() && !h.stalled(),"then a few jobs get through");};
    require(!h.wait_job(),"no stall, no wait");
    bootstraps(H::STALL_JOBS);wait_out();
    bootstraps(H::RETRY_RUN-1);require(!h.stalled(),"a retry keeps going for its short run");
    bootstraps(1);require(h.stalled(),"still no pair: back to waiting, without the long stall run");
    wait_out();h.begin(job,0,B);require(!h.stalled() && !h.wait_job(),"a pair ends it");
}
}

int main(){
    try{
        a_bootstrap_stays_on_the_games_textures();
        the_first_private_job_resets_then_the_chain_runs();
        a_gap_resets_after_mono();
        the_same_parity_twice_resets();
        a_wrong_native_previous_resets_instead_of_sampling_it();
        a_bootstrap_breaks_the_chain();
        a_job_is_accepted_only_on_one_pass_on_its_own_texture();
        a_failed_job_is_no_predecessor();
        no_private_pair_for_long_stalls();
        a_stall_tries_again_on_its_own();
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"ordinary_taa_history: all passed\n";return 0;
}
