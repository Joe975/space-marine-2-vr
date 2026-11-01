#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

// The right eye's temporal history when the game runs TAA instead of DLSS (docs/TAA_STEREO.md). The
// game's TAA pass picks one of two pool textures by frame parity, so both eyes in a stereo job write the
// same pair and each samples the other eye's image. NativeOrdinaryTaa swaps a private pair in for the
// right eye; this is the chain that decides, job by job, what the pass may sample.
//
// A job's pass samples the previous job's private texture only when that job was the one just before
// it, on the other parity. Anything else (the first private job, stereo back after mono, a renewed
// pair) resets: the pass gets no previous texture and starts a fresh history.
// A job with no private texture is a bootstrap: it stays on the game's textures for that one frame, and
// the temporal hook makes the pair during it.
// Pure logic; the render thread calls it under NativeOrdinaryTaa's lock.
class OrdinaryTaaHistory {
public:
    // Jobs in a row without a private texture before stereo stops relying on TAA. By then a pair can't
    // be made (creation refused, or the pair cap at a new pool layout), and both eyes would stay on one
    // history: a smeared image. Stereo waits instead, as it does with no DLSS. Waiting means no right-eye
    // job, and only one can make a pair, so every RETRY_JOBS mono jobs (about 30 s) a few get through: the
    // pool may have come back to a layout a retired pair fits.
    static constexpr uint32_t STALL_JOBS=120,RETRY_JOBS=900,RETRY_RUN=8;
    struct Stats {uint64_t jobs{},owned{},resets{},bootstraps{},failures{},previous_mismatches{};};
    // At the right eye's selection. owned: the private texture for this parity, 0 for a bootstrap.
    void begin(uint64_t dispatch,uint32_t parity,uintptr_t owned){
        ++m_stats.jobs;Job job{dispatch,owned,0,parity&1u};
        if(owned){
            ++m_stats.owned;m_native_run=0;
            const bool chained=m_last && m_last->dispatch+1==dispatch && m_last->parity!=job.parity && m_last->destination!=owned;
            job.reset=!chained;if(chained)job.expected=m_last->destination;else ++m_stats.resets;
            m_last=job;
        }else {++m_stats.bootstraps;m_last.reset();if(m_native_run<STALL_JOBS)++m_native_run;}
        m_jobs[m_next++%m_jobs.size()]=job;
    }
    // What this job's pass samples, given the native previous argument. Never the other eye's image: a
    // private job whose native argument isn't its own predecessor resets too.
    uintptr_t previous(uint64_t dispatch,uintptr_t native){
        auto* job=find(dispatch);if(!job || !job->destination)return native;
        if(job->reset)return 0;
        if(native==job->expected)return native;
        ++m_stats.previous_mismatches;job->reset=true;return 0;
    }
    bool owned(uint64_t dispatch)const{const auto* job=find(dispatch);return job && job->destination;}
    // After the right eye's pass. Exactly one call per job, on the texture this job chose.
    void temporal(uint64_t dispatch,bool valid,uintptr_t destination){
        auto* job=find(dispatch);if(!job)return;++job->calls;
        if(!valid || job->calls!=1 || (job->destination && destination!=job->destination)){job->failed=true;++m_stats.failures;if(m_last && m_last->dispatch==dispatch)m_last.reset();}
    }
    // The job's proof for the pair, as the private DLSS evaluation's receipt is: its pass ran, once, on the right texture.
    bool accepted(uint64_t dispatch)const{const auto* job=find(dispatch);return job && job->calls==1 && !job->failed;}
    bool stalled()const{return m_native_run>=STALL_JOBS;}
    // Once per job that selected TAA: true while stereo should keep waiting.
    bool wait_job(){
        if(!stalled())return false;
        if(++m_waited<RETRY_JOBS)return true;
        m_waited=0;m_native_run=STALL_JOBS-RETRY_RUN;return false;
    }
    const Stats& stats()const{return m_stats;}
private:
    struct Job {uint64_t dispatch{};uintptr_t destination{},expected{};uint32_t parity{},calls{};bool reset{},failed{};};
    Job* find(uint64_t dispatch){const auto found=std::ranges::find(m_jobs,dispatch,&Job::dispatch);return dispatch && found!=m_jobs.end()?&*found:nullptr;}
    const Job* find(uint64_t dispatch)const{const auto found=std::ranges::find(m_jobs,dispatch,&Job::dispatch);return dispatch && found!=m_jobs.end()?&*found:nullptr;}
    std::array<Job,4> m_jobs{};size_t m_next{};std::optional<Job> m_last;uint32_t m_native_run{},m_waited{};Stats m_stats;
};
