#pragma once
#include "GameBuild.h"
#include "NativeRenderProbe.h"
#include <deque>
#include <ostream>
#include <string_view>

// Ordinary colour boundary observation. No PIP registration, camera changes,
// cached COM dereferences or runtime submission. An optional synchronous
// consumer may perform separately guarded inline GPU retention.
class NativeOrdinaryProbe {
public:
    static constexpr uintptr_t POST_RVA=0x1fac430,COMPOSITOR_RVA=0x1f8faf0;
    inline static constexpr game_build::Code POST_BYTES=game_build::hashed(0x1fac430,15,"39333192699e4bb818a608e1823a8e2506440b7d737d80b190d185b28e7207fb");
    inline static constexpr game_build::Code COMPOSITOR_BYTES=game_build::hashed(0x1f8faf0,16,"41b7f5d443a80a4f685e7acc742bb885f6faa4d593b0920b74a62fba85af1a93");
    enum class Stage:uint32_t {post,compositor_before,compositor_after};
    struct RawBoundary {Stage stage{};uintptr_t wrapper{},entry{},renderer_state{};bool aborted{};};
    using RawObserver=std::function<void(const RawBoundary&)>;
    struct Scope {NativeRenderProbe::Scene scene;NativeCameraEntries::Primary primary;};
    struct Event {
        uint64_t serial{};Stage stage{};Scope scope;uintptr_t wrapper{};
        NativeRenderProbe::Output output;
    };
    struct Sample {uint64_t refused{},errors{},dropped{};std::vector<Event> events;};
    using Provider=std::function<std::optional<Scope>()>;
    using Boundary=std::function<void(const Event&)>;
    using Post=void(*)(uintptr_t,uintptr_t*,uintptr_t,uintptr_t,uintptr_t,uint32_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,float);
    using Compositor=void(*)(uintptr_t,uintptr_t,uint8_t);
    static NativeOrdinaryProbe& get();
    static bool requested(std::string_view ordinary,std::string_view pip,std::string_view vr,std::string_view xr,std::string_view synthetic,bool headset=false) {
        const auto off=[](auto value){return value.empty() || value=="0";};
        const bool single_runtime=(vr=="1" && off(xr)) || (xr=="1" && off(vr));
        return ordinary=="1" && off(pip) && (headset?single_runtime:(off(vr) && off(xr))) && off(synthetic);
    }
    static bool matches(uint32_t,std::span<const uint8_t>,std::span<const uint8_t>);
    static bool guarded_in_game(HMODULE);
    // Startup-only raw hook tap. It grants no scene/camera/resource attribution;
    // the pair observer must authenticate its own independent scope.
    bool attach_raw(RawObserver);
    bool installed()const{return m_post && m_compositor;}
    uint64_t raw_errors()const{return m_raw_errors.load();}
    bool install_in_game(HMODULE,NativeRenderProbe::OutputObserver,Boundary={});
    bool install(void* post,void* compositor,Provider,NativeRenderProbe::OutputObserver,Boundary={});
    Sample sample(uint64_t dispatch_sequence) const;
    static void write_capture(std::ostream&,const NativeRenderProbe::Snapshot&,const Sample&,uint32_t width,uint32_t height);
private:
    static void on_post(uintptr_t,uintptr_t*,uintptr_t,uintptr_t,uintptr_t,uint32_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,float);
    static void on_compositor(uintptr_t,uintptr_t,uint8_t);
    void record(Stage,uintptr_t wrapper,uintptr_t entry,uintptr_t renderer_state);
    void observe(Stage,uintptr_t wrapper,uintptr_t entry,uintptr_t renderer_state);
    void abort_raw(uintptr_t wrapper,uintptr_t renderer_state)noexcept;
    std::atomic<Post> m_post{};std::atomic<Compositor> m_compositor{};
    Provider m_provider;NativeRenderProbe::OutputObserver m_outputs;
    Boundary m_boundary;
    RawObserver m_raw;std::atomic<uint64_t> m_raw_errors{};
    mutable std::mutex m_mutex;std::mutex m_install;std::deque<Event> m_events;
    uint64_t m_serial{},m_refused{},m_errors{},m_dropped{};
};
