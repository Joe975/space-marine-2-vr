#pragma once
#include <Windows.h>
#include <intrin.h>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>
#include "PerformanceThreads.h"

// Opt-in CPU wall-time trace. QPC milliseconds share PresentMon --qpc_time_ms's clock.
// These spans include blocking/waiting; they are not GPU timings or CPU utilization.
// Every row carries the recording thread's ID. Once a second the writer also records its own
// cost (trace_writer_cpu, trace_rows), the wall clock (wall_clock: Unix seconds, for picking a
// fight window by time of day) and the key threads' CPU (PerformanceThreads.h).
class PerformanceTrace {
public:
    enum class Mode { full, tracking, copy };
    using Role = performance_threads::Role;
    static PerformanceTrace& get() { static PerformanceTrace trace; return trace; }
    static double now_ms() {
        static const double frequency=[] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
        LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return double(t.QuadPart)*1000.0/frequency;
    }
    static double unix_seconds() {
        FILETIME now{}; GetSystemTimePreciseAsFileTime(&now);
        return double(performance_threads::ticks(now)-116444736000000000ull)/1e7;
    }
    bool open(const std::filesystem::path& output,const std::filesystem::path& control={},unsigned max_rows=1000000,bool probes=false) {
        if(!output.is_absolute() || (!control.empty() && !control.is_absolute()) || !max_rows || max_rows>10000000) return false;
        std::lock_guard lock{m_mutex};
        m_file.open(output,std::ios::out|std::ios::trunc);
        if(!m_file) return false;
        m_file<<"qpc_start_ms,event,duration_ms,value,mode,thread\n"<<std::fixed<<std::setprecision(6);
        m_buffer.reserve(1<<20); m_control=control; m_max_rows=max_rows; m_probes=probes; m_qpc0=now_ms(); m_tsc0=__rdtsc(); m_enabled=true; return true;
    }
    void from_environment() {
        wchar_t output[32768]{},control[32768]{};
        const auto n=GetEnvironmentVariableW(L"SM2VR_PROFILE",output,32768);
        const auto c=GetEnvironmentVariableW(L"SM2VR_PROFILE_CONTROL",control,32768);
        wchar_t rows[16]{};const auto r=GetEnvironmentVariableW(L"SM2VR_PROFILE_MAX_ROWS",rows,16);
        unsigned limit=1000000;
        if(r){
            if(r>=16)return;
            unsigned parsed{};for(unsigned i=0;i<r;++i){if(rows[i]<L'0' || rows[i]>L'9' || parsed>1000000)return;parsed=parsed*10+unsigned(rows[i]-L'0');}
            if(!parsed || parsed>10000000)return;limit=parsed;
        }
        wchar_t probes[8]{};
        const bool detail=GetEnvironmentVariableW(L"SM2VR_PROFILE_PROBES",probes,8)==1 && probes[0]==L'1';
        if(n && n<32768 && c<32768) open(output,c?std::filesystem::path{control}:std::filesystem::path{},limit,detail);
    }
    bool enabled() const { return m_enabled; } // Configured once, before frame/camera callbacks.
    bool probes() const { return m_enabled && m_probes; }
    Mode mode() const { return m_mode; } // Read/written only on the Present thread.
    static const char* name(Mode mode) { return mode==Mode::tracking?"tracking":mode==Mode::copy?"copy":"full"; }
    // The calling thread is the one now playing this role (sampled once a second).
    void thread_role(Role role) { if(m_enabled) m_threads.observe(role,GetCurrentThreadId()); }
    DWORD role_thread(Role role) const { return m_threads.tid(role); }
    // TSC ticks per QPC millisecond since open(); 0 in the first 200 ms or when not tracing.
    double cycles_per_ms() const { return m_enabled?performance_threads::cycles_per_ms(__rdtsc()-m_tsc0,now_ms()-m_qpc0):0; }
    // Capture eligibility is independent of whether the menu-plane correction is enabled.
    // Observed campaign pause/settings publishes 0x908 with native PAUSED clear.
    static unsigned present_flags(bool foreground,bool available,bool paused,bool scripted,
                                  uint32_t published,uint32_t requested,bool framegen) {
        const bool menu=((published|requested)&0x908)==0x908;
        return (foreground?1u:0u) | (paused?2u:0u) | (scripted?4u:0u) |
            (framegen?8u:0u) | (menu?16u:0u) | (!available?32u:0u);
    }
    void poll_control(double time) {
        if(!m_enabled || m_control.empty() || time<m_next_poll) return;
        m_next_poll=time+1000;
        std::ifstream file{m_control};std::string value;
        // Exact, bounded vocabulary; partial writes/missing files preserve the active mode.
        if(std::getline(file,value) && value.size()<32) {
            if(!value.empty() && value.back()=='\r') value.pop_back();
            if(value=="full") m_mode=Mode::full;
            else if(value=="tracking") m_mode=Mode::tracking;
            else if(value=="copy") m_mode=Mode::copy;
        }
    }
    void record(const char* event,double start,double duration,double value=0,Mode mode=Mode::full) {
        if(!m_enabled) return;
        const auto entered=now_ms();
        std::lock_guard lock{m_mutex};
        if(m_rows>=m_max_rows) return;
        write(event,start,duration,value,mode);
        if(start>=m_next_flush) { housekeeping(entered); drain();m_next_flush=start+1000; }
        m_writer_ms+=now_ms()-entered;
    }
    void flush() { std::lock_guard lock{m_mutex};drain(); }
    struct Scope {
        PerformanceTrace& trace;const char* event;double start{},value{};Mode mode{Mode::full};
        explicit Scope(const char* event):Scope(PerformanceTrace::get(),event) {}
        Scope(PerformanceTrace& trace,const char* event):trace(trace),event(event) {
            if(trace.enabled()) start=now_ms();
        }
        ~Scope() { if(start) trace.record(event,start,now_ms()-start,value,mode); }
        Scope(const Scope&)=delete;
        Scope& operator=(const Scope&)=delete;
    };
    // The calling thread's own CPU time over a span: duration_ms is thread CPU (QueryThreadCycleTime,
    // not tick-quantized), value is the span's wall time. CPU well below wall means the thread waited.
    struct CpuScope {
        PerformanceTrace& trace;const char* event;double start{};ULONG64 cycles{};
        explicit CpuScope(const char* event):CpuScope(PerformanceTrace::get(),event) {}
        CpuScope(PerformanceTrace& trace,const char* event):trace(trace),event(event) {
            if(trace.enabled() && QueryThreadCycleTime(GetCurrentThread(),&cycles)) start=now_ms();
        }
        ~CpuScope() {
            if(!start) return;
            ULONG64 end{};if(!QueryThreadCycleTime(GetCurrentThread(),&end) || end<cycles) return;
            const auto wall=now_ms()-start;const auto rate=trace.cycles_per_ms();
            if(rate>0) trace.record(event,start,double(end-cycles)/rate,wall);
        }
        CpuScope(const CpuScope&)=delete;
        CpuScope& operator=(const CpuScope&)=delete;
    };
private:
    // Caller holds m_mutex.
    // Housekeeping rows (counted=false, a handful a second) never use the caller's row budget.
    void write(const char* event,double start,double duration,double value,Mode mode,bool counted=true) {
        if(counted && m_rows>=m_max_rows) return;
        if(counted) ++m_rows;
        // to_chars into a reused buffer: the ostream path cost about 5 us a row on the render thread
        // (27 ms/s at 5,500 rows/s in the 21 Oct smoke test). The buffer goes to the file once a second.
        char line[256];char* p=line;char* const end=line+sizeof line;
        const auto put_number=[&](double v){const auto r=std::to_chars(p,end-1,v,std::chars_format::fixed,6);p=r.ec==std::errc{}?r.ptr:p;*p++=',';};
        const auto put_text=[&](const char* text){while(*text && p<end-24)*p++=*text++;*p++=',';};
        put_number(start);put_text(event);put_number(duration);put_number(value);put_text(name(mode));
        const auto r=std::to_chars(p,end-1,GetCurrentThreadId());p=r.ec==std::errc{}?r.ptr:p;*p++='\n';
        m_buffer.append(line,size_t(p-line));
    }
    void drain() { if(!m_buffer.empty()){m_file.write(m_buffer.data(),std::streamsize(m_buffer.size()));m_buffer.clear();} m_file.flush(); }
    // Caller holds m_mutex. Once a second, before the flush.
    void housekeeping(double now) {
        if(m_last_housekeeping>0 && now>m_last_housekeeping) {
            write("trace_writer_cpu",m_last_housekeeping,now-m_last_housekeeping,m_writer_ms,Mode::full,false);
            write("trace_rows",m_last_housekeeping,now-m_last_housekeeping,double(m_rows-m_rows_at_housekeeping),Mode::full,false);
        }
        write("wall_clock",now,0,unix_seconds(),Mode::full,false);
        if(const auto rate=performance_threads::cycles_per_ms(__rdtsc()-m_tsc0,now-m_qpc0);rate>0) write("tsc_per_ms",now,0,rate,Mode::full,false);
        m_threads.sample(now,[this](const char* event,double start,double duration,double value){write(event,start,duration,value,Mode::full,false);});
        m_writer_ms=0;m_rows_at_housekeeping=m_rows;m_last_housekeeping=now;
    }
    std::mutex m_mutex;
    std::ofstream m_file;
    std::string m_buffer;
    std::filesystem::path m_control;
    bool m_enabled{},m_probes{};
    Mode m_mode{Mode::full};
    double m_next_poll{},m_next_flush{};
    unsigned m_rows{},m_max_rows{1000000};
    double m_qpc0{},m_writer_ms{},m_last_housekeeping{};uint64_t m_tsc0{};unsigned m_rows_at_housekeeping{};
    performance_threads::Monitor m_threads{performance_threads::Monitor::read_thread,performance_threads::Monitor::read_process,performance_threads::Monitor::read_system};
};
