#include "FrameSyncProbe.h"
#include "PerformanceTrace.h"
#include <Windows.h>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <fstream>
#include <map>
#include <regex>

namespace {
void require(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }
struct Token : sl::FrameToken {
    uint32_t value{};
    explicit Token(uint32_t v) : value{v} {}
    operator uint32_t() const override { return value; }
};
// A card without Streamline: no markers, so without FrameSyncOwnClock no job has an identity (1 Nov, -EnvSet SM2VR_DIAG_NO_STREAMLINE=1).
void test_own_clock_policy() {
    using O = FrameSyncOwnClock;O own;
    for(unsigned i=1;i<O::AFTER_JOBS;++i)require(own.job(0)==O::Step::markers,"waits for the game's markers first");
    require(own.job(0)==O::Step::started && own.active(),"no marker in AFTER_JOBS jobs starts it");
    require(own.job(0)==O::Step::advance,"then every job advances it");
    require(own.job(1)==O::Step::stopped && !own.active(),"a real marker hands the clock back at once");
    require(own.job(1)==O::Step::markers,"and the count starts over");
    O markers;for(uint64_t i=1;i<5000;++i)require(markers.job(i)==O::Step::markers,"markers arriving: it never starts");
}
void test_own_clock_gives_each_job_an_identity() {
    auto& probe = FrameSyncProbe::get();
    for(unsigned i=1;i<FrameSyncOwnClock::AFTER_JOBS;++i){probe.camera_job();require(!probe.render_camera_frame() && !probe.own_clock(),"no identity yet");}
    uint64_t last{};
    for(int i=0;i<6;++i){probe.camera_job();const auto frame=probe.render_camera_frame();
        require(frame && !frame->invalid && frame->render_id>last,"one open frame per job, each with a new render id");last=frame->render_id;}
    require(probe.own_clock() && !probe.snapshot().markers,"not counted as the game's markers");
    probe.record(FrameSyncProbe::Kind::reset);probe.camera_job();
    require(probe.render_camera_frame() && probe.render_camera_frame()->render_id>last,"survives a device reset, ids still rising");
    probe.record(FrameSyncProbe::Kind::reset);
}
void test_clock() {
    FrameSyncClock clock;
    for (unsigned phase = 0; phase <= 5; ++phase) { clock.marker(phase, 100); }
    auto first = clock.snapshot();
    require(first.rendering && first.presenting && first.rendering->render_id == first.presenting->render_id && first.presenting->eye == 0,
        "render and presentation must retain the same first-eye frame identity");
    const auto first_id = first.presenting->render_id;
    clock.marker(4,100); clock.marker(11,100); clock.marker(12,100);
    require(clock.snapshot().duplicates == 1 && clock.snapshot().ignored == 2 && clock.snapshot().presenting->render_id == first_id,
        "duplicates and generated presentations must not advance eye identity");
    clock.marker(0,101); clock.marker(1,101);
    clock.marker(0,102); clock.marker(1,102); // Overlapping simulation and render/presentation threads.
    clock.marker(2,101); clock.marker(3,101); clock.marker(4,101);
    require(clock.snapshot().simulation->token == 102 && clock.snapshot().rendering->token == 101 && clock.snapshot().presenting->eye == 1,
        "in-flight phases must remain separate instead of using latest simulation token for Present");
    clock.marker(2,102); clock.marker(4,102); // Missing render-submit end.
    require(!clock.snapshot().rendering && clock.snapshot().presenting.has_value() && clock.snapshot().errors == 1,
        "missing phases must invalidate only their own frame binding");
    require(clock.snapshot().presenting->token == 101, "invalid new frame must not replace prior presentation");
    clock.marker(2,999);
    require(clock.snapshot().errors == 2, "a marker without a known simulation start must not invent a frame");
    clock.marker(0,105); clock.marker(1,105); clock.marker(2,105);
    require(clock.snapshot().rendering->eye == 0, "token gaps must restart from the left eye");
    clock.marker(3,105,false);
    require(!clock.snapshot().rendering, "rejected marker must clear that frame's binding");
    clock.marker(0,104);
    require(clock.snapshot().errors == 4, "old simulation token must not rewind the clock");
    const auto id_before_reset = clock.snapshot().simulation ? clock.snapshot().simulation->render_id : first_id;
    clock.reset();
    require(!clock.snapshot().simulation && !clock.snapshot().rendering && !clock.snapshot().presenting, "device reset must clear all bindings");
    clock.marker(0,0xfffffffeu); clock.marker(1,0xfffffffeu); clock.marker(2,0xfffffffeu);
    const auto wrapped_id = clock.snapshot().rendering->render_id;
    require(wrapped_id > id_before_reset && clock.snapshot().rendering->eye == 0, "render identity must remain monotonic across resets");
    clock.marker(0,0xffffffffu); clock.marker(1,0xffffffffu); clock.marker(2,0xffffffffu);
    clock.marker(0,0); clock.marker(1,0); clock.marker(2,0);
    require(clock.snapshot().rendering->eye == 0 && clock.snapshot().rendering->render_id == wrapped_id+2, "32-bit token rollover must preserve eye cadence");
    for (uint32_t token = 1; token < 20; ++token) { clock.marker(0,token); }
    clock.marker(1,0xfffffffeu);
    require(clock.snapshot().errors == 5, "evicted old frames must not be recreated by a late marker");
}
void test_sm2_clock() {
    FrameSyncClock clock{FrameSyncClock::Policy::sm2_cached_start};
    clock.marker(2,99); clock.marker(1,99); // Intro video emits incomplete phases, no main view.
    require(!clock.snapshot().rendering && clock.snapshot().errors == 0, "startup without a simulation start must stay unbound");
    clock.marker(0,100); clock.marker(2,101); clock.marker(3,101); clock.marker(4,101); clock.marker(5,101); clock.marker(1,101);
    const auto first = clock.snapshot();
    require(first.simulation->token == 101 && first.rendering->token == 101 && first.presenting->token == 101 && first.presenting->eye == 0 && first.errors == 0,
        "observed SM2 offset and late simulation-end must bind one rendered eye");
    clock.marker(0,101);
    require(clock.snapshot().simulation->token == 102 && clock.snapshot().presenting->token == 101,
        "next simulation may start before the preceding D3D Present callback without relabeling its image");
    clock.marker(2,102); clock.marker(3,102); clock.marker(4,102); clock.marker(5,102); clock.marker(1,102);
    require(clock.snapshot().presenting->eye == 1 && clock.snapshot().presenting->render_id == first.presenting->render_id+1,
        "SM2 eye cadence must follow mapped simulation starts, not presentation callback count");
    clock.marker(0,102); clock.marker(4,103);
    require(clock.snapshot().errors == 1, "SM2 policy must still reject missing render-submit phases");
    clock.reset();
    clock.marker(0,0xffffffffu); clock.marker(2,0); clock.marker(3,0); clock.marker(4,0);
    require(clock.snapshot().presenting->token == 0 && clock.snapshot().presenting->eye == 0,
        "cached-start mapping must handle token rollover");
}
void test_camera_window() {
    FrameSyncClock clock{FrameSyncClock::Policy::sm2_cached_start};
    clock.marker(0,10);
    require(!clock.camera_frame(), "simulation alone must not bind a render job");
    clock.marker(2,11);
    require(clock.camera_frame() && clock.camera_frame()->token == 11, "open render interval must bind its explicit token");
    clock.marker(0,11);
    require(clock.camera_frame()->token == 11, "overlapping next simulation must not relabel render jobs");
    clock.marker(2,12);
    require(!clock.camera_frame(), "overlapping render intervals are ambiguous");
    clock.marker(3,11);
    require(clock.camera_frame()->token == 12, "closing one interval leaves the other explicit owner");
    clock.marker(3,12);
    require(!clock.camera_frame(), "jobs after render end must remain unbound");
    clock.marker(0,12); clock.marker(2,13); clock.marker(3,13,false);
    require(!clock.camera_frame(), "rejected phases must invalidate camera ownership");
    clock.reset();
    require(!clock.camera_frame(), "reset must clear job ownership");
}
void replay_trace(const char* input_path, const char* output_path) {
    std::ifstream input{input_path};
    std::ofstream output{output_path};
    require(input.good() && output.good(), "trace replay files must open");
    output << "# CPU clock replay; raw events unchanged, candidate bindings recomputed. No GPU/runtime evidence.\n";
    FrameSyncClock clock{FrameSyncClock::Policy::sm2_cached_start};
    const std::regex field{R"((\w+)=(-?\d+))"};
    std::string line;
    unsigned renders{}, presents{}, events{};
    while (std::getline(input,line)) {
        if (line.find("FrameSyncTrace:") == std::string::npos) { continue; }
        std::map<std::string,int64_t> values;
        for (auto it = std::sregex_iterator(line.begin(),line.end(),field); it != std::sregex_iterator(); ++it) {
            values[(*it)[1]] = std::stoll((*it)[2]);
        }
        require(values.size() == 13, "replay event must contain all fields");
        const auto kind = values.at("kind"), phase = values.at("marker");
        const auto token = static_cast<uint32_t>(values.at("frame"));
        if (kind == 4) { clock.reset(); }
        if (kind == 0) { clock.marker(static_cast<uint32_t>(phase),token,values.at("result") == 0); }
        const auto state = clock.snapshot();
        if (kind == 0 && phase == 2 && state.rendering) {
            require(state.rendering->token == token, "replayed render marker must bind its own token");
            ++renders;
        }
        if (kind == 0 && phase == 4 && state.presenting) {
            require(state.presenting->token == token && state.rendering && state.rendering->render_id == state.presenting->render_id,
                "replayed presentation must retain actual rendered-eye identity");
            ++presents;
        }
        const std::map<std::string,int64_t> bindings{
            {"simulation", state.simulation ? int64_t(state.simulation->token) : -1},
            {"render", state.rendering ? int64_t(state.rendering->token) : -1},
            {"presenter", state.presenting ? int64_t(state.presenting->token) : -1},
            {"render_id", state.rendering ? int64_t(state.rendering->render_id) : 0},
            {"present_id", state.presenting ? int64_t(state.presenting->render_id) : 0},
            {"eye", state.rendering ? state.rendering->eye : -1}};
        for (const auto& [name,value] : bindings) {
            line = std::regex_replace(line,std::regex{name+"=-?\\d+\\b"},name+"="+std::to_string(value));
        }
        output << line << '\n';
        ++events;
    }
    require(renders >= 2 && presents >= 2 && clock.snapshot().errors == 0, "recorded trace must replay enough frames without clock errors");
    std::cout << "CPU trace replay: events=" << events << " rendered=" << renders << " presented=" << presents << " phase_errors=" << clock.snapshot().errors << '\n';
}
}

int main(int argc, char** argv) {
    try {
        test_clock();
        test_sm2_clock();
        test_camera_window();
        test_own_clock_policy();
        test_own_clock_gives_each_job_an_identity();
        require(argc == 2 || argc == 4, "fixture DLL required; optional input/output replay paths");
        auto& probe = FrameSyncProbe::get();
        const auto now = std::chrono::steady_clock::now();
        probe.poll(now);
        require(probe.state() == FrameSyncProbe::State::waiting, "late DLL must remain retryable");
        const auto module = LoadLibraryA(argv[1]);
        require(module != nullptr, "fixture load failed");
        const auto ready = reinterpret_cast<void(*)(bool)>(GetProcAddress(module,"fixture_marker_ready"));
        const auto set_result = reinterpret_cast<void(*)(sl::Result)>(GetProcAddress(module,"fixture_marker_result"));
        const auto calls = reinterpret_cast<unsigned(*)()>(GetProcAddress(module,"fixture_marker_calls"));
        const auto arguments = reinterpret_cast<bool(*)(uint32_t,const void*)>(GetProcAddress(module,"fixture_marker_arguments"));
        const auto resolve = reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(module,"slGetFeatureFunction"));
        require(ready && set_result && calls && arguments && resolve, "fixture API missing");
        require(GetProcAddress(module,"slPCLSetMarker") == nullptr, "marker must use feature resolver, not a direct export");
        probe.poll(now + std::chrono::seconds(2));
        require(probe.state() == FrameSyncProbe::State::waiting, "unready feature must remain retryable");
        ready(true);
        void* target{};
        require(resolve(sl::kFeaturePCL,"slPCLSetMarker",target) == sl::Result::eOk && target, "feature must resolve");
        const auto call = reinterpret_cast<PFun_slPCLSetMarker*>(target); // Game may have cached it before hook installation.
        probe.poll(now + std::chrono::seconds(4));
        require(probe.state() == FrameSyncProbe::State::installed, "late-ready feature must hook");
        probe.poll(now + std::chrono::seconds(6));
        const Token token{123};
        require(call(sl::PCLMarker::eSimulationStart,token) == sl::Result::eOk, "marker result must pass through");
        require(calls() == 1 && arguments(0,&token), "cached function must be hooked with exact argument reference and single original call");
        require(probe.snapshot().clock.simulation->token == 124, "hook must map cached start token while retaining raw token in event");
        require(probe.current_camera_frame() && probe.current_camera_frame()->token == 124, "camera thread must retain its own simulation binding");
        std::atomic<bool> other_unbound{}, other_bound{};
        std::thread camera_worker{[&] {
            other_unbound = !probe.current_camera_frame();
            const Token worker_token{200};
            call(sl::PCLMarker::eSimulationStart,worker_token);
            other_bound = probe.current_camera_frame() && probe.current_camera_frame()->token == 201;
        }};
        camera_worker.join();
        require(other_unbound && other_bound && probe.current_camera_frame()->token == 124, "new simulation on another thread must not relabel this camera thread");
        probe.start_trace(2);
        probe.record(FrameSyncProbe::Kind::camera);
        probe.record(FrameSyncProbe::Kind::constants,123);
        probe.record(FrameSyncProbe::Kind::present);
        const auto trace = probe.take_trace();
        require(trace.size() == 2 && trace[0].kind == FrameSyncProbe::Kind::camera && trace[1].token == 123 && trace[0].thread == GetCurrentThreadId(),
            "trace must retain bounded phase/camera/constants events with thread identity");
        require(trace[1].sequence == trace[0].sequence+1 && probe.take_trace().empty(), "trace sequence must be monotonic and drain only once");
        set_result(sl::Result::eErrorMissingInputParameter);
        const Token rendered_token{124};
        require(call(sl::PCLMarker::eSimulationEnd,rendered_token) == sl::Result::eErrorMissingInputParameter, "original marker error must survive");
        require(probe.snapshot().rejected == 1 && !probe.current_camera_frame() && probe.snapshot().clock.simulation->token == 201,
            "rejected phase must invalidate its own frame without removing a newer simulation");
        set_result(sl::Result::eOk);
        const auto count = calls();
        std::thread a{[&] { for (int i = 0; i < 100; ++i) { call(sl::PCLMarker::eOutOfBandPresentStart,token); } }};
        std::thread b{[&] { for (int i = 0; i < 100; ++i) { call(sl::PCLMarker::eOutOfBandPresentEnd,token); } }};
        a.join(); b.join();
        require(probe.recent_out_of_band() && !probe.recent_out_of_band(std::chrono::steady_clock::now()+std::chrono::seconds(2)),
            "out-of-band activity must block tracked submission until a quiet interval");
        require(calls() == count+200 && probe.snapshot().markers == count+200, "concurrent markers must not be lost");
        probe.record(FrameSyncProbe::Kind::reset);
        require(!probe.snapshot().clock.presenting && !probe.snapshot().clock.rendering, "reset event must clear bindings");
        const Token start{500}, render{501};
        call(sl::PCLMarker::eSimulationStart,start);
        call(sl::PCLMarker::eRenderSubmitStart,render);
        std::atomic<bool> migrated_bound{};
        std::thread migrated{[&] {
            const auto frame = probe.render_camera_frame();
            migrated_bound = frame && frame->token == 501;
        }};
        migrated.join();
        require(migrated_bound, "camera worker without simulation TLS must inherit the open render token");
        call(sl::PCLMarker::eRenderSubmitEnd,render);
        require(!probe.render_camera_frame(), "render end must invalidate the global camera window");
        {
            // Profiling: each standard marker becomes a trace row with its raw token; simulation start
            // names the simulation thread; out-of-band markers stay out of the trace.
            const auto path = std::filesystem::temp_directory_path() / ("sm2vr_pcl_" + std::to_string(GetCurrentProcessId()) + ".csv");
            auto& trace = PerformanceTrace::get();
            require(trace.open(path), "profile trace opens");
            const Token sim{600}, submit{601};
            call(sl::PCLMarker::eSimulationStart, sim);
            call(sl::PCLMarker::eOutOfBandPresentStart, sim);
            call(sl::PCLMarker::eRenderSubmitStart, submit);
            call(sl::PCLMarker::eSimulationEnd, submit);
            call(sl::PCLMarker::ePresentEnd, submit);
            trace.flush();
            std::ifstream file{path}; std::string text{std::istreambuf_iterator<char>{file}, {}};
            require(text.find(",pcl_simulation_start,0.000000,600.000000,full,") != std::string::npos &&
                    text.find(",pcl_render_submit_start,0.000000,601.000000,full,") != std::string::npos &&
                    text.find(",pcl_present_end,0.000000,601.000000,full,") != std::string::npos, "standard markers are traced with raw tokens");
            size_t rows = 0;
            for (auto at = text.find(",pcl_"); at != std::string::npos; at = text.find(",pcl_", at + 1)) { ++rows; }
            require(rows == 4, "out-of-band markers are not traced");
            require(trace.role_thread(PerformanceTrace::Role::simulation) == GetCurrentThreadId(), "simulation end names the simulation thread");
        }
        std::cout << "frame sync clock and real feature-function detour passed\n";
        if (argc == 4) { replay_trace(argv[2],argv[3]); }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
