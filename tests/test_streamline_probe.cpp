#include <Windows.h>
#include <StreamlineProbe.h>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    void require(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }
    struct Frame : sl::FrameToken { uint32_t value{1234}; operator uint32_t() const override { return value; } };
    sl::Constants constants()
    {
        sl::Constants v;
        for (unsigned i = 0; i < 4; ++i) {
            const auto row = sl::float4{float(i * 4 + 1), float(i * 4 + 2), float(i * 4 + 3), float(i * 4 + 4)};
            v.cameraViewToClip[i] = v.clipToCameraView[i] = v.clipToPrevClip[i] = v.prevClipToClip[i] = row;
        }
        v.cameraPos = {10, 20, 30}; v.cameraUp = {0, 1, 0}; v.cameraRight = {1, 0, 0}; v.cameraFwd = {0, 0, 1};
        v.jitterOffset = {0.25f, -0.5f}; v.mvecScale = {1, 2};
        v.cameraNear = 0.1f; v.cameraFar = 1000; v.cameraFOV = 1.2f; v.cameraAspectRatio = 1.777f;
        v.depthInverted = sl::eTrue; v.reset = sl::eFalse;
        return v;
    }
}

int main(int argc, char** argv)
{
    try {
        require(argc == 2, "fixture DLL path required");
        auto& probe = StreamlineProbe::get();
        const auto now = std::chrono::steady_clock::now();
        probe.poll(now);
        require(probe.state() == StreamlineProbe::State::waiting, "missing DLL must not block or fail permanently");
        const auto module = LoadLibraryA(argv[1]);
        require(module != nullptr, "fixture must load");
        const auto call = reinterpret_cast<PFun_slSetConstants*>(GetProcAddress(module, "slSetConstants"));
        const auto set_result = reinterpret_cast<void (*)(sl::Result)>(GetProcAddress(module, "fixture_result"));
        const auto count = reinterpret_cast<unsigned (*)()>(GetProcAddress(module, "fixture_calls"));
        const auto arguments = reinterpret_cast<bool (*)(const void*, const void*, const void*)>(GetProcAddress(module, "fixture_arguments"));
        const auto reset_value = reinterpret_cast<sl::Boolean(*)()>(GetProcAddress(module,"fixture_reset"));
        require(call && set_result && count && arguments, "fixture exports missing");
        probe.poll(now + std::chrono::seconds{2});
        require(probe.state() == StreamlineProbe::State::installed, "late-loaded DLL must be hooked");
        probe.poll(now + std::chrono::seconds{4}); // Idempotence: never hook our detour twice.

        auto v = constants();
        const Frame frame;
        const sl::ViewportHandle viewport{7u};
        const auto before = v;
        require(call(v, frame, viewport) == sl::Result::eOk, "success result must pass through");
        require(count() == 1 && arguments(&v, &frame, &viewport), "original must receive the exact references once");
        require(std::memcmp(&before, &v, sizeof(v)) == 0, "observer must not change any constants");
        auto snap = probe.snapshot();
        require(snap.calls == 1 && snap.invalid == 0 && snap.latest.has_value(), "camera must be captured");
        require(snap.latest->frame == 1234 && snap.latest->viewport == 7 && snap.latest->constants_version == 2, "frame/viewport/version must survive");
        require(snap.latest->projection[2].z == 11 && snap.latest->inverse_projection[3].w == 16 &&
            snap.latest->clip_to_previous[1].x == 5 && snap.latest->previous_to_clip[0].y == 2, "matrices must keep row-major order");
        require(snap.latest->position.y == 20 && snap.latest->up.y == 1 && snap.latest->right.x == 1 && snap.latest->forward.z == 1 &&
            snap.latest->jitter.x == 0.25f && snap.latest->motion_vector_scale.y == 2 && snap.latest->near_plane == 0.1f &&
            snap.latest->far_plane == 1000 && snap.latest->fov == 1.2f && snap.latest->aspect == 1.777f &&
            snap.latest->depth_inverted == sl::eTrue && snap.latest->reset == sl::eFalse, "camera metadata must survive");
        v.cameraPos.x = 999;
        require(probe.snapshot().latest->position.x == 10, "snapshot must own values, not borrow game pointers");

        set_result(sl::Result::eErrorMissingInputParameter);
        require(call(v, frame, viewport) == sl::Result::eErrorMissingInputParameter, "errors must pass through unchanged");
        require(probe.snapshot().rejected == 1 && probe.snapshot().latest->result == sl::Result::eErrorMissingInputParameter, "record original errors");
        set_result(sl::Result::eOk);
        const auto good = probe.snapshot().latest->position.x;
        for (unsigned test = 0; test < 5; ++test) {
            auto bad = constants();
            if (test == 0) { bad.cameraFOV = sl::INVALID_FLOAT; }
            if (test == 1) { bad.cameraViewToClip[0].x = std::numeric_limits<float>::quiet_NaN(); }
            if (test == 2) { bad.cameraFar = std::numeric_limits<float>::infinity(); }
            if (test == 3) { bad.structVersion = 0; }
            if (test == 4) { bad.structType = {}; }
            require(call(bad, frame, viewport) == sl::Result::eOk, "bad diagnostics must still reach original");
        }
        require(probe.snapshot().invalid == 5 && probe.snapshot().latest->position.x == good, "invalid inputs must preserve last readable sample");

        // A V1 object ends immediately before the V2 field. Guard the next page to catch overreads.
        SYSTEM_INFO info{}; GetSystemInfo(&info);
        auto* pages = static_cast<unsigned char*>(VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        require(pages != nullptr, "guard allocation failed");
        DWORD old{};
        require(VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old) != 0, "guard page failed");
        auto v1 = constants(); v1.structVersion = 1;
        const auto prefix = reinterpret_cast<const unsigned char*>(&v1.minRelativeLinearDepthObjectSeparation) - reinterpret_cast<const unsigned char*>(&v1);
        auto* short_object = pages + info.dwPageSize - prefix;
        std::memcpy(short_object, &v1, prefix);
        require(call(*reinterpret_cast<sl::Constants*>(short_object), frame, viewport) == sl::Result::eOk, "V1 prefix must be readable");
        require(probe.snapshot().latest->constants_version == 1, "V1 capture must work without V2 tail");
        require(reset_value != nullptr, "reset fixture export required");
        probe.set_force_temporal_reset(true);
        const sl::ViewportHandle reset_main_view{0u};
        require(call(*reinterpret_cast<sl::Constants*>(short_object),frame,reset_main_view) == sl::Result::eOk && reset_value() == sl::eTrue,
            "opt-in reset must change submitted main-view reset without reading V2 tail");
        require(std::memcmp(short_object,&v1,prefix) == 0 && probe.snapshot().latest->reset == sl::eTrue,
            "reset must preserve original game constants and log submitted reset");
        call(v1,frame,viewport);
        require(reset_value() == sl::eFalse && arguments(&v1,&frame,&viewport), "helper constants must remain exact passthrough in reset mode");
        probe.set_force_temporal_reset(false);
        call(v1,frame,reset_main_view);
        require(reset_value() == sl::eFalse && arguments(&v1,&frame,&reset_main_view), "disabled reset must return to exact passthrough");
        VirtualFree(pages, 0, MEM_RELEASE);

        const auto start = probe.snapshot().calls;
        std::atomic<bool> done{};
        std::thread reader{[&] { while (!done) { (void)probe.snapshot(); } }};
        std::vector<std::thread> workers;
        for (unsigned t = 0; t < 4; ++t) {
            workers.emplace_back([&] { const auto data = constants(); for (unsigned i = 0; i < 250; ++i) { call(data, frame, viewport); } });
        }
        for (auto& worker : workers) { worker.join(); }
        done = true; reader.join();
        require(probe.snapshot().calls == start + 1000 && count() == start + 1000, "concurrent hook calls must not be lost or duplicated");
        probe.start_trace(2);
        const sl::ViewportHandle main_view{0u};
        call(v, frame, viewport);
        call(v, frame, main_view);
        v.cameraPos.x = 321;
        call(v, frame, main_view);
        call(v, frame, main_view);
        const auto trace = probe.take_trace();
        require(trace.size() == 2 && trace[0].position.x == 999 && trace[1].position.x == 321, "trace must own a bounded sequence of main-viewport samples");
        require(probe.take_trace().empty(), "draining a trace must not duplicate samples");
        probe.clear_frame_cache();
        Frame indexed;
        for (uint32_t token = 100; token < 110; ++token) { indexed.value = token; call(v,indexed,main_view); }
        require(!probe.frame_camera(101) && probe.frame_camera(102) && probe.frame_camera(109),"frame camera cache must retain eight explicit tokens independently of latest sample");
        indexed.value = 109; call(v,indexed,main_view);
        require(probe.frame_camera(109).has_value(),"identical repeated frame constants are idempotent");
        auto changed = v; changed.cameraPos.x += 1; call(changed,indexed,main_view);
        require(!probe.frame_camera(109),"conflicting constants for one token must be ambiguous");
        indexed.value = 110; call(v,indexed,viewport);
        require(!probe.frame_camera(110),"helper viewport must never become a main-frame camera");
        call(v,indexed,main_view); set_result(sl::Result::eErrorMissingInputParameter); call(v,indexed,main_view);
        require(!probe.frame_camera(110),"rejected constants must invalidate their frame camera");
        set_result(sl::Result::eOk);
        probe.clear_frame_cache(); require(!probe.frame_camera(108),"device reset must clear frame camera cache");
        const auto observer_start=count();unsigned began{},completed{};
        probe.set_constants_observer([&](const sl::Constants& values,const sl::FrameToken& observed_frame,const sl::ViewportHandle& observed_view)->StreamlineProbe::ConstantsComplete{
            require(&values==&v && &observed_frame==&frame && &observed_view==&viewport && count()==observer_start,"observer begins on exact submitted arguments before the one original API call");++began;
            return [&](sl::Result result){require(result==sl::Result::eOk && count()==observer_start+1,"observer completes after original result");++completed;probe.set_constants_observer({});(void)probe.snapshot();};
        });
        require(call(v,frame,viewport)==sl::Result::eOk && began==1 && completed==1 && arguments(&v,&frame,&viewport),"callbacks run outside observer/cache locks without changing native arguments");
        probe.set_constants_observer([](const auto&,const auto&,const auto&)->StreamlineProbe::ConstantsComplete{throw std::runtime_error("observer begin");});
        require(call(v,frame,viewport)==sl::Result::eOk && count()==observer_start+2,"failed observer begin never suppresses or retries the native API");
        probe.set_constants_observer([](const auto&,const auto&,const auto&)->StreamlineProbe::ConstantsComplete{return [](sl::Result){throw std::runtime_error("observer end");};});
        set_result(sl::Result::eErrorMissingInputParameter);require(call(v,frame,viewport)==sl::Result::eErrorMissingInputParameter && count()==observer_start+3,"failed observer completion preserves the original API result");
        probe.set_constants_observer({});set_result(sl::Result::eOk);
        std::cout << "Streamline probe: late load, real detour, passthrough, camera copy, invalid data, V1 guard and concurrency passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
