#include "PerformanceTrace.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <sstream>
#include <tuple>
#include <vector>

static void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() {
    try {
        require(PerformanceTrace::present_flags(true,true,false,false,0,0,false)==1,"known foreground gameplay qualifies");
        require(PerformanceTrace::present_flags(true,true,false,false,0x908,0,false)==17,"captured pause menu must fail qualification without native PAUSED");
        require(PerformanceTrace::present_flags(true,true,false,false,0x900,8,false)==17,"requested menu handover also fails");
        require(PerformanceTrace::present_flags(true,true,false,false,0,0x908,false)==17,"requested settings menu fails");
        require(PerformanceTrace::present_flags(true,true,false,false,0x800,0,false)==1,"partial title/loading mask is not claimed to identify a menu");
        require(PerformanceTrace::present_flags(true,false,false,false,0,0,false)==33,"unknown native state cannot qualify");
        require(PerformanceTrace::present_flags(false,true,true,true,0,0,true)==14,"focus and native pause/cinematic/framegen flags preserved");
        const auto root=std::filesystem::temp_directory_path()/("sm2vr_perf_"+std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(root);
        const auto output=root/"cpu.csv",control=root/"control.txt";
        {
            PerformanceTrace trace;
            require(!trace.enabled() && trace.mode()==PerformanceTrace::Mode::full,"profiling off must preserve full submission");
            require(!trace.open("relative.csv"),"relative output must be rejected");
            require(trace.open(output,control),"absolute output opens");
            {std::ofstream f{control};f<<"tracking\n";}
            trace.poll_control(1000);
            require(trace.mode()==PerformanceTrace::Mode::tracking,"tracking disables both copy and submit");
            trace.record("known",1234.5,2.25,7,trace.mode());
            {std::ofstream f{control};f<<"copy\r\n";}
            trace.poll_control(1500);require(trace.mode()==PerformanceTrace::Mode::tracking,"control I/O must be rate limited");
            trace.poll_control(2000);require(trace.mode()==PerformanceTrace::Mode::copy,"copy mode parses CRLF");
            {std::ofstream f{control};f<<"invalid";}
            trace.poll_control(3000);require(trace.mode()==PerformanceTrace::Mode::copy,"invalid partial write preserves mode");
            {std::ofstream f{control};f<<"full\n";}
            trace.poll_control(4000);require(trace.mode()==PerformanceTrace::Mode::full,"full restores runtime submission");
            {PerformanceTrace::Scope measured{trace,"scoped"};}
            trace.flush();
        }
        std::ifstream in{output};std::stringstream content;content<<in.rdbuf();
        require(content.str().find("1234.500000,known,2.250000,7.000000,tracking")!=std::string::npos,"production writer must preserve units/value/mode");
        require(content.str().find(",scoped,")!=std::string::npos,"production RAII scope must emit timing");
        in.close();
        {
            PerformanceTrace bounded;
            require(!bounded.open(root/"invalid.csv",{},0) && !bounded.open(root/"invalid.csv",{},10000001),"trace bounds reject zero and excessive row limits");
            require(bounded.open(root/"bounded.csv",{},2),"explicit bounded trace opens");
            bounded.record("one",1,0);bounded.record("two",2,0);bounded.record("overflow",3,0);bounded.flush();
            std::ifstream file{root/"bounded.csv"};std::stringstream rows;rows<<file.rdbuf();
            require(rows.str().find(",one,")!=std::string::npos && rows.str().find(",two,")!=std::string::npos && rows.str().find(",overflow,")==std::string::npos,"row limit preserves exactly the requested number of records");
        }
        {
            // This test process has no live profiling session; exercise startup parsing.
            SetEnvironmentVariableW(L"SM2VR_PROFILE",(root/"environment.csv").c_str());
            SetEnvironmentVariableW(L"SM2VR_PROFILE_CONTROL",nullptr);
            for(const auto invalid:{L"0",L"10000001",L"-1",L"2x",L"9999999999999999"}){
                SetEnvironmentVariableW(L"SM2VR_PROFILE_MAX_ROWS",invalid);PerformanceTrace trace;trace.from_environment();
                require(!trace.enabled(),"invalid environment row limit fails closed");
            }
            SetEnvironmentVariableW(L"SM2VR_PROFILE_MAX_ROWS",L"10000000");PerformanceTrace trace;trace.from_environment();
            require(trace.enabled(),"single-frame profiling budget accepted");
            SetEnvironmentVariableW(L"SM2VR_PROFILE",nullptr);SetEnvironmentVariableW(L"SM2VR_PROFILE_MAX_ROWS",nullptr);
        }
        {
            // Key-thread CPU: baseline first, then one row per role per interval, in milliseconds.
            namespace pt=performance_threads;
            std::map<DWORD,uint64_t> cpu{{11,1'000'000},{22,5'000'000}};uint64_t process=10'000'000;pt::SystemTimes system{4'000'000,8'000'000};
            pt::Monitor monitor{[&](DWORD tid)->std::optional<uint64_t>{const auto f=cpu.find(tid);if(f==cpu.end())return std::nullopt;return f->second;},
                [&]()->std::optional<uint64_t>{return process;},[&]()->std::optional<pt::SystemTimes>{return system;}};
            std::vector<std::tuple<std::string,double,double,double>> rows;
            const auto emit=[&](const char* e,double s,double d,double v){rows.emplace_back(e,s,d,v);};
            monitor.observe(pt::Role::simulation,11);monitor.observe(pt::Role::render,22);
            monitor.sample(1000,emit);require(rows.empty(),"the first sample is only a baseline");
            cpu[11]+=9'000'000;cpu[22]+=4'000'000;process+=30'000'000;system.idle+=6'000'000;system.total+=16'000'000;
            monitor.sample(2000,emit);
            const auto find=[&](const char* e){return std::ranges::find_if(rows,[&](const auto& r){return std::get<0>(r)==e;});};
            require(find("thread_cpu_simulation")!=rows.end() && std::get<1>(*find("thread_cpu_simulation"))==1000 && std::get<2>(*find("thread_cpu_simulation"))==1000 && std::get<3>(*find("thread_cpu_simulation"))==900,"simulation thread: 900 ms CPU in a 1000 ms interval");
            require(find("thread_cpu_render")!=rows.end() && std::get<3>(*find("thread_cpu_render"))==400,"render thread CPU in milliseconds");
            require(find("thread_cpu_present")==rows.end(),"an unseen role has no row");
            require(find("process_cpu")!=rows.end() && std::get<3>(*find("process_cpu"))==3000,"process CPU can exceed the interval (many threads)");
            require(find("system_cpu_busy")!=rows.end() && std::abs(std::get<3>(*find("system_cpu_busy"))-0.625)<1e-12,"machine busy fraction from idle/total");
            rows.clear();monitor.observe(pt::Role::render,33);cpu[33]=1;cpu[11]+=1'000'000;monitor.sample(3000,emit);
            require(find("thread_cpu_render")==rows.end() && find("thread_cpu_simulation")!=rows.end(),"a role that changed thread restarts its baseline");
            rows.clear();cpu.erase(11);cpu[33]+=5'000'000;monitor.sample(4000,emit);
            require(find("thread_cpu_simulation")==rows.end() && find("thread_cpu_render")!=rows.end() && std::get<3>(*find("thread_cpu_render"))==500,"a vanished thread gives no row; the new one is measured");
            require(pt::cycles_per_ms(1'000'000,100)==0 && pt::cycles_per_ms(3'000'000,1000)==3000,"TSC rate needs 200 ms of calibration");
        }
        {
            // Production writer: thread column, once-a-second housekeeping and a real thread CPU span.
            PerformanceTrace trace;require(trace.open(root/"housekeeping.csv"),"housekeeping trace opens");
            trace.thread_role(PerformanceTrace::Role::render);
            require(trace.role_thread(PerformanceTrace::Role::render)==GetCurrentThreadId(),"role records the calling thread");
            trace.record("first",1,0);Sleep(250);
            const auto spin=[&]{PerformanceTrace::CpuScope busy{trace,"busy_cpu"};const auto until=PerformanceTrace::now_ms()+80;volatile uint64_t n{};while(PerformanceTrace::now_ms()<until)n=n+1;};
            spin();
            {PerformanceTrace::CpuScope idle{trace,"idle_cpu"};Sleep(80);}
            trace.record("second",2000,0);trace.flush();
            std::ifstream file{root/"housekeeping.csv"};std::string header,line;std::getline(file,header);
            require(header=="qpc_start_ms,event,duration_ms,value,mode,thread","trace header names the thread column");
            std::map<std::string,std::vector<std::array<double,3>>> seen;const auto tid=std::to_string(GetCurrentThreadId());
            while(std::getline(file,line)){
                std::vector<std::string> f;std::stringstream s{line};std::string cell;while(std::getline(s,cell,','))f.push_back(cell);
                require(f.size()==6 && f[5]==tid,"every row ends with the recording thread");
                seen[f[1]].push_back({std::stod(f[0]),std::stod(f[2]),std::stod(f[3])});
            }
            require(seen.count("wall_clock") && seen["wall_clock"].front()[2]>1.7e9,"wall clock is Unix seconds");
            require(seen.count("trace_writer_cpu") && seen.count("trace_rows") && seen["trace_rows"].front()[2]>=1,"writer reports its own cost and row count");
            require(seen.count("thread_cpu_render") && seen["thread_cpu_render"].front()[2]>40,"the render role's CPU covers the busy span");
            require(seen.count("tsc_per_ms") && seen["tsc_per_ms"].front()[2]>100000,"TSC calibration row");
            const auto busy=seen["busy_cpu"].front(),idle=seen["idle_cpu"].front();
            require(busy[2]>=79 && busy[1]>0.5*busy[2] && busy[1]<1.5*busy[2],"a busy span's thread CPU is close to its wall time");
            require(idle[2]>=79 && idle[1]<0.25*idle[2],"a sleeping span's thread CPU is far below its wall time");
        }
        {
            // Writer cost per row (the 21 Oct smoke test measured about 5 us with ostream formatting).
            PerformanceTrace trace;require(trace.open(root/"cost.csv",{},1000000),"cost trace opens");
            constexpr unsigned rows=200000;const auto begin=PerformanceTrace::now_ms();
            for(unsigned i=0;i<rows;++i)trace.record("scene_left_other_wait_cpu",1e8+i*0.01,0.012345,i);
            const auto per_row_us=(PerformanceTrace::now_ms()-begin)*1000/rows;trace.flush();
            std::cout<<"trace writer: "<<per_row_us<<" us per row\n";
            require(per_row_us<3,"the trace writer stays cheap on the render thread");
            std::ifstream file{root/"cost.csv"};std::string header,line;std::getline(file,header);std::getline(file,line);
            require(line.rfind("100000000.000000,scene_left_other_wait_cpu,0.012345,0.000000,full,",0)==0,"buffered rows keep the fixed six-decimal format");
        }
        std::filesystem::remove_all(root);
        std::cout<<"performance trace passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
