#include "synthetic_code.h"
#include <EngineCameraProbe.h>
#include "game_build_ported.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cmath>
#include <limits>

namespace
{
    void require(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }
    bool close(float a,float b) { return std::abs(a-b)<1e-4f; }

    void test_vr_pose_math() {
        using namespace vr_camera;
        auto game=IDENTITY;
        game[10]=-1; // Actual game camera: right +X, up +Y, forward -Z (negative determinant).
        game[12]=10; game[13]=20; game[14]=30;
        auto reference=IDENTITY;
        reference[12]=4; reference[13]=1.6f; reference[14]=8;
        auto left=IDENTITY,right=IDENTITY;
        left[12]=-0.032f; right[12]=0.032f;
        Matrix relative{},world{},view{};
        require(relative_eye(reference,reference,left,relative),"recentered left eye must compose");
        require(compose(game,relative,1,world,view),"XR eye must map into the actual game basis");
        require(close(world[12],9.968f) && close(world[13],20) && close(world[14],30),"room height must not be added to game eye height");
        const auto left_world=world;
        require(relative_eye(reference,reference,right,relative) && compose(game,relative,1,world,view),"right eye must compose");
        require(close(world[12]-left_world[12],0.064f),"physical IPD must survive recentering and conversion");
        auto current=reference;
        current[12]+=0.1f; current[13]+=0.2f; current[14]-=0.3f;
        require(relative_eye(reference,current,IDENTITY,relative) && compose(game,relative,2,world,view),"leaning and world scale must compose");
        require(close(world[12],10.2f) && close(world[13],20.4f) && close(world[14],29.4f),"right/up/forward lean must follow game axes");
        const Matrix yaw{0,0,-1,0, 0,1,0,0, 1,0,0,0, 0,0,0,1};
        require(compose(game,yaw,1,world,view),"head yaw must compose");
        require(close(world[8],-1) && close(world[9],0) && close(world[10],0),"positive XR yaw must turn world forward toward -X");
        const auto product=multiply(world,view);
        for (int i=0;i<16;++i) require(close(product[i],IDENTITY[i]),"game view must invert its submitted world transform");
        const Matrix pitch{1,0,0,0, 0,0,1,0, 0,-1,0,0, 0,0,0,1};
        require(compose(game,pitch,1,world,view) && close(world[9],1) && close(world[10],0),"head pitch must turn forward up");
        const Matrix roll{0,1,0,0, -1,0,0,0, 0,0,1,0, 0,0,0,1};
        require(compose(game,roll,1,world,view) && close(world[1],1) && close(world[4],-1) && close(world[10],-1),"head roll must rotate right/up without changing forward");
        require(relative_eye(IDENTITY,yaw,left,relative) && compose(game,relative,1,world,view),"eye offset must rotate with current head");
        require(close(world[12],10) && close(world[14],30.032f),"turned left eye must move along the turned head's right axis");
        auto turned_reference=yaw;
        turned_reference[12]=4; turned_reference[13]=1.6f; turned_reference[14]=8;
        require(relative_eye(turned_reference,turned_reference,left,relative),"recenter must remove reference heading too");
        require(close(relative[12],-0.032f) && close(relative[13],0) && close(relative[14],0),"eye offset must remain head-relative after recentering yaw");
        auto invalid=IDENTITY;
        invalid[0]=std::numeric_limits<float>::quiet_NaN();
        const auto before=world;
        require(!compose(game,invalid,1,world,view) && world==before,"invalid tracking must not mutate output");
        invalid=IDENTITY; invalid[10]=-1;
        require(!compose(game,invalid,1,world,view),"XR input must reject a reflected transform");
        invalid=IDENTITY; invalid[0]=2;
        require(!relative_eye(reference,invalid,left,relative),"scaled tracking transforms must be rejected");
        require(!compose(game,IDENTITY,0,world,view),"zero world scale must be rejected");
        auto rotated_game=game;
        rotated_game[0]=0;rotated_game[2]=-1;rotated_game[8]=-1;rotated_game[10]=0;
        require(compose(rotated_game,right,1,world,view) && close(world[12],10) && close(world[14],29.968f),"eye offset must follow game heading, not global X");
        Matrix anchor{};
        require(level_anchor(pitch,anchor) && anchor == IDENTITY,"vertical recenter gaze must use projected right without an undefined heading");
        auto pitched_game = game;
        const float tilt_c = std::cos(0.174532925f),tilt_s = std::sin(0.174532925f);
        const Matrix tilt{tilt_c,tilt_s,0,0, -tilt_s,tilt_c,0,0, 0,0,1,0, 0,0,0,1};
        pitched_game = multiply(game,tilt);
        require(compose(pitched_game,IDENTITY,1,world,view) && close(world[4],0) && close(world[5],1) && close(world[6],0) &&
            close(world[12],pitched_game[12]) && close(world[13],pitched_game[13]),
            "game camera roll must not tilt VR gravity or change its world anchor position");
        require(compose(pitched_game,roll,1,world,view) && close(world[1],1) && close(world[4],-1),
            "level game anchor must preserve actual physical head tilt");
    }

    struct Call
    {
        void*    list{};
        void*    source{};
        uint64_t args[9]{};
        unsigned count{};
        float    seen[32]{}; // the two camera matrices as the original saw them
    } g_call;

    // Stand-in for the engine function: same eleven-slot signature, records what it was given.
    __declspec(noinline) int64_t fake_add_camera(void* list, void* source, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9,
        uint64_t a10, uint64_t a11)
    {
        g_call.list   = list;
        g_call.source = source;
        if (source != nullptr) { std::memcpy(g_call.seen, source, sizeof(g_call.seen)); }
        const uint64_t args[9]{ a3, a4, a5, a6, a7, a8, a9, a10, a11 };
        std::memcpy(g_call.args, args, sizeof(args));
        ++g_call.count;
        return 0x1122334400000005; // the engine returns a 32-bit index; upper bits must still pass through
    }

    using AddFn = int64_t (*)(void*, void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    // Two fixed call sites, so a return address can be designated as "the main view".
    __declspec(noinline) int64_t call_site_a(AddFn volatile fn, void* camera) { return fn(nullptr, camera, 0, 0, 0, 0, 0, 0, 0, 0, 0) + 1; }
    __declspec(noinline) int64_t call_site_b(AddFn volatile fn, void* camera) { return fn(nullptr, camera, 0, 0, 0, 0, 0, 0, 0, 0, 0) + 2; }

    template <typename T> void put(std::vector<uint8_t>& camera, size_t offset, T value) { std::memcpy(camera.data() + offset, &value, sizeof(T)); }

    std::vector<uint8_t> fake_camera()
    {
        std::vector<uint8_t> camera(EngineCameraProbe::SOURCE_CAMERA_SIZE, 0xAB);
        for (int i = 0; i < 16; ++i) {
            put(camera, i * 4, float(i + 1));          // world: 1..16, position row = 13, 14, 15
            put(camera, 0x40 + i * 4, float(100 + i)); // second matrix
        }
        put(camera, 0x80, 0.1f);
        put(camera, 0x84, 250000.0f);
        put(camera, 0x12c, 0.0f);
        put(camera, 0x130, 0.0f);
        put(camera, 0x134, 3840.0f);
        put(camera, 0x138, 2160.0f);
        put(camera, 0x2cc, int32_t{ -1 });
        put(camera, 0x2d0, 80.0f);
        put(camera, 0x2d4, 50.5f);
        return camera;
    }
}

int main()
{
    try {
        test_vr_pose_math();
        const auto good = synthetic_at<25>(EngineCameraProbe::ADD_CAMERA_RVA);
        auto          bad = std::vector<uint8_t>(std::begin(good), std::end(good));
        bad[5]            = 0x90;
        require(EngineCameraProbe::matches_build(EngineCameraProbe::BUILD_TIMESTAMP, good), "known build and prologue must match");
        require(!EngineCameraProbe::matches_build(EngineCameraProbe::BUILD_TIMESTAMP + 1, good), "another build's timestamp must be refused");
        require(!EngineCameraProbe::matches_build(EngineCameraProbe::BUILD_TIMESTAMP, bad), "changed function bytes must be refused");
        require(!EngineCameraProbe::matches_build(EngineCameraProbe::BUILD_TIMESTAMP, std::span{ good.data(), 8 }), "short read must be refused");
        require(EngineCameraProbe::matches_build(game_build_ported::EXE, game_build_ported::code(EngineCameraProbe::ADD_CAMERA_RVA)), "the 2026-10-21 build's add function must match");
        require(!EngineCameraProbe::matches_build(game_build_ported::EXE + 1, good), "an unported build must be refused");

        auto&      probe  = EngineCameraProbe::get();
        const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        require(probe.state() == EngineCameraProbe::State::idle, "probe starts idle");
        require(probe.install(reinterpret_cast<void*>(&fake_add_camera), module, 0x10000000) == EngineCameraProbe::State::installed, "hook must install");
        require(probe.install(reinterpret_cast<void*>(&fake_add_camera), module, 0x10000000) == EngineCameraProbe::State::installed, "second install must be a no-op");

        auto       camera = fake_camera();
        const auto before = camera;
        int        list{};
        // Call through a volatile pointer so the compiler cannot bypass the patched function body.
        int64_t (*volatile call)(void*, void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = &fake_add_camera;
        const auto result = call(&list, camera.data(), 3, 0xFFFFFFFF00000004, 0x3f8000003f000000, 6, 0xFFFF, 0x8001, 9, 0xFF, 0xDEADBEEFCAFEF00D);

        require(result == 0x1122334400000005, "result must pass through unchanged, upper bits included");
        require(g_call.count == 1 && g_call.list == &list && g_call.source == camera.data(), "original must run once with the same pointers");
        const uint64_t expected[9]{ 3, 0xFFFFFFFF00000004, 0x3f8000003f000000, 6, 0xFFFF, 0x8001, 9, 0xFF, 0xDEADBEEFCAFEF00D };
        require(std::memcmp(g_call.args, expected, sizeof(expected)) == 0, "all nine trailing argument slots must pass through bit-exact");
        require(camera == before, "the source camera must not be modified");

        auto snap = probe.snapshot();
        require(snap.calls == 1 && snap.by_caller.size() == 1, "one call site must be recorded");
        const auto& s = snap.by_caller[0];
        require(s.caller_rva != 0 && s.caller_rva < 0x10000000, "caller must be reported relative to the module");
        require(s.index == 5 && s.calls == 1 && s.source == reinterpret_cast<uintptr_t>(camera.data()), "index and source must be captured");
        require(s.world[0] == 1 && s.world[12] == 13 && s.world[14] == 15 && s.second[0] == 100 && s.second[15] == 115, "matrices must keep their order");
        require(s.near_plane == 0.1f && s.far_plane == 250000.0f && s.viewport[2] == 3840 && s.viewport[3] == 2160, "near/far and viewport offsets");
        require(s.kind == -1 && s.fov_h_deg == 80.0f && s.fov_v_deg == 50.5f, "kind and field-of-view offsets");
        require(std::memcmp(s.args, expected, sizeof(expected)) == 0, "arguments must be captured");

        put(camera, 0x2d0, 73.7f);
        call(&list, camera.data(), 0, 0, 0, 0, 0, 0, 0, 0, 0);
        call(nullptr, nullptr, 0, 0, 0, 0, 0, 0, 0, 0, 0); // a null source is forwarded and not sampled
        snap = probe.snapshot();
        require(g_call.count == 3, "every call must reach the original");
        require(snap.calls == 2 && snap.by_caller.size() == 2, "distinct call sites are kept apart; null source is skipped");
        require(snap.by_caller[1].fov_h_deg == 73.7f && snap.by_caller[1].calls == 1, "latest sample per call site");

        // Control test: the designated main-view call site sees a shifted camera, others do not,
        // and the source is restored after the call.
        auto        unit = fake_camera();
        const float identity[16]{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1 };
        const float inverse[16]{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -10, -20, -30, 1 };
        std::memcpy(unit.data(), identity, sizeof(identity));
        std::memcpy(unit.data() + 0x40, inverse, sizeof(inverse));
        const auto unit_before = unit;
        call_site_a(&fake_add_camera, unit.data());
        snap              = probe.snapshot();
        const auto site_a = snap.by_caller.back().caller_rva;
        require(snap.by_caller.size() == 3 && site_a != 0, "helper call site must be recorded");
        probe.set_main_view_return_rva(site_a);
        probe.set_main_view_shift(0.5f);
        call_site_a(&fake_add_camera, unit.data());
        require(g_call.seen[12] == 10.5f && g_call.seen[13] == 20 && g_call.seen[14] == 30, "main view position must move along its right axis during the call");
        for (const auto& sample : probe.snapshot().by_caller) {
            if (sample.caller_rva == site_a) { require(sample.world[12] == 10.5f && sample.center[0] == 10 && sample.applied_shift == 0.5f, "snapshot must describe the submitted eye, not the restored center"); }
        }
        require(g_call.seen[16 + 12] == -10.5f && g_call.seen[16 + 13] == -20, "world-to-view translation must stay the inverse");
        require(unit == unit_before, "the source camera must be restored after the call");
        call_site_b(&fake_add_camera, unit.data());
        require(g_call.seen[12] == 10 && g_call.seen[16 + 12] == -10, "other call sites must not be shifted");
        probe.set_main_view_shift(0.0f);
        call_site_a(&fake_add_camera, unit.data());
        require(g_call.seen[12] == 10 && unit == unit_before, "zero shift must leave the hook passive");
        probe.set_alternating_shift(0.25f);
        probe.set_alternating_shift(std::numeric_limits<float>::quiet_NaN());
        require(probe.alternating_shift() == 0.25f, "nonfinite amplitude must be refused");
        probe.start_trace(3);
        float previous_shift = 0;
        for (int i = 0; i < 5; ++i) {
            call_site_b(&fake_add_camera, unit.data());
            require(g_call.seen[12] == 10, "helper views must stay unshifted during alternation");
            call_site_a(&fake_add_camera, unit.data());
            const float current_shift = g_call.seen[12] - 10;
            require(std::abs(current_shift) == 0.25f && (i == 0 || current_shift == -previous_shift), "main submissions must alternate equal opposite offsets; helpers must not consume parity");
            require(g_call.seen[28] == -g_call.seen[12] && unit == unit_before, "alternating inverse and source restoration");
            previous_shift = current_shift;
        }
        auto trace = probe.take_trace();
        require(trace.size() == 3 && trace[1].submission == trace[0].submission + 1 && trace[2].submission == trace[1].submission + 1, "trace must be bounded and main-only");
        require(probe.take_trace().empty(), "trace drain must not duplicate samples");
        probe.set_alternating_shift(0);
        call_site_a(&fake_add_camera, unit.data());
        require(g_call.seen[12] == 10 && unit == unit_before, "disabling alternation restores passive submission");
        float rotated[32]{ 0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 2, 3, 1, 0 };
        EngineCameraProbe::shift_camera(rotated, 2.0f);
        require(rotated[12] == 1 && rotated[13] == 2 && rotated[14] == 1 && rotated[16 + 12] == -2, "shift follows the camera's own right axis");

        EngineCameraProbe::VrEye eye{};
        const auto fixed_time = std::chrono::steady_clock::time_point{std::chrono::seconds(100)};
        eye.captured = fixed_time;
        require(eye.valid_at(fixed_time) && eye.valid_at(fixed_time + std::chrono::milliseconds(250)), "freshness endpoints must be inclusive");
        require(!eye.valid_at(fixed_time - std::chrono::nanoseconds(1)) && !eye.valid_at(fixed_time + std::chrono::milliseconds(251)), "future and expired tracking must be rejected without sleeps");
        eye.relative_eye[12]=0.1f; eye.relative_eye[13]=0.2f; eye.relative_eye[14]=-0.3f;
        eye.render_token=42;
        eye.captured=std::chrono::steady_clock::now();
        require(probe.publish_vr_eye(eye),"fresh runtime eye must publish");
        probe.set_main_view_shift(0.5f);
        probe.start_trace(1);
        call_site_a(&fake_add_camera,unit.data());
        require(close(g_call.seen[12],10.1f) && close(g_call.seen[13],20.2f) && close(g_call.seen[14],30.3f),"actual camera detour must apply published headset pose");
        require(close(g_call.seen[28],-10.1f) && close(g_call.seen[29],-20.2f) && close(g_call.seen[30],-30.3f),"detour must update inverse alongside the tracked world matrix");
        require(unit==unit_before,"headset pose must restore source camera byte-exact after original call");
        probe.set_main_view_shift(0);
        const auto pose_trace = probe.take_trace();
        require(pose_trace.size() == 1 && pose_trace[0].vr_pose_applied && pose_trace[0].vr_render_token == 42 && pose_trace[0].applied_shift == 0,
            "trace must identify the tracked image and exclude superseded diagnostic shift");
        for (const auto& sample : probe.snapshot().by_caller) if(sample.caller_rva==site_a)
            require(sample.vr_pose_applied && sample.vr_eye==0 && sample.vr_render_token==42,"pose diagnostics must retain image/eye identity");
        call_site_b(&fake_add_camera,unit.data());
        require(g_call.seen[12]==10 && unit==unit_before,"headset pose must not affect helper/shadow cameras");
        unsigned anchor_calls{};
        probe.set_anchor_provider([&](const vr_camera::Matrix& original,const EngineCameraProbe::VrEye& selected) {
            ++anchor_calls; require(original[12]==10 && selected.render_token==42,"body anchor must receive original camera and selected image identity");
            auto corrected=original; corrected[12]+=1; return corrected;
        });
        call_site_a(&fake_add_camera,unit.data());
        require(close(g_call.seen[12],11.1f) && close(g_call.seen[28],-11.1f) && unit==unit_before,
            "real main-view detour must compose corrected body anchor, inverse and restore persistent source");
        call_site_b(&fake_add_camera,unit.data());
        require(anchor_calls==1 && g_call.seen[12]==10,"body anchor callback must never affect helper views");
        probe.set_anchor_provider([](auto original,const auto&) { original[0]=std::numeric_limits<float>::quiet_NaN(); return original; });
        call_site_a(&fake_add_camera,unit.data());
        require(g_call.seen[12]==10 && unit==unit_before,"invalid body anchor must leave native camera untouched");
        probe.set_anchor_provider({});
        require(!probe.publish_vr_eye(eye),"duplicate render token must not overwrite newer pose");
        auto older = eye; older.render_token = 41; older.relative_eye[12] = 9;
        require(!probe.publish_vr_eye(older), "older render token must not overwrite newer pose");
        call_site_a(&fake_add_camera,unit.data());
        require(close(g_call.seen[12],10.1f), "rejected older pose must preserve current pose");
        auto expired=eye; expired.render_token=43; expired.captured-=std::chrono::seconds(1);
        require(!probe.publish_vr_eye(expired),"expired pose must be refused and tracking cleared");
        call_site_a(&fake_add_camera,unit.data());
        require(g_call.seen[12]==10 && unit==unit_before,"expired update must fall back to normal game view");
        eye.captured=std::chrono::steady_clock::now();
        require(probe.publish_vr_eye(eye),"tracking can restart after clearing");
        auto invalid_eye = eye; invalid_eye.eye = 2;
        require(!probe.publish_vr_eye(invalid_eye), "invalid eye identity must clear override");
        call_site_a(&fake_add_camera,unit.data());
        require(g_call.seen[12]==10 && unit==unit_before,"invalid tracking must restore normal game view");
        require(probe.publish_vr_eye(eye), "valid tracking can restart after invalid input");
        auto turned_eye = eye;
        turned_eye.relative_eye = {0,0,-1,0, 0,1,0,0, 1,0,0,0, 0,0,0,1};
        turned_eye.eye = 1;
        turned_eye.render_token = 43;
        turned_eye.captured = std::chrono::steady_clock::now();
        require(probe.publish_vr_eye(turned_eye), "newer right-eye rotation must publish");
        call_site_a(&fake_add_camera,unit.data());
        require(close(g_call.seen[8],-1) && close(g_call.seen[2],1) && close(g_call.seen[28],-30) && close(g_call.seen[30],10) && unit==unit_before,
            "actual detour must rotate the view, rebuild inverse and restore source");
        {
            // Pitch set explicitly (the default is level since 27 Oct): the detour must carry a tilt through.
            VrViewCamera camera;VrViewSettings settings;settings.mode=VrViewMode::diorama;settings.diorama_pitch=30;
            auto miniature=eye;miniature.relative_eye=vr_camera::IDENTITY;miniature.relative_eye[12]=-.032f;
            miniature.render_token=44;miniature.captured=std::chrono::steady_clock::now();miniature.units_per_metre=9;
            miniature.reference_epoch=1;miniature.view_settings=settings;
            probe.set_anchor_provider([&](const auto& native,const auto& selected) {
                return camera.camera(native,selected.view_settings,selected.reference_epoch,selected.units_per_metre,VrViewCamera::Focus{1,{10,0,30}},false);
            });
            require(probe.publish_vr_eye(miniature),"third-person eye publishes with explicit scale/mode");
            call_site_a(&fake_add_camera,unit.data());
            require(close(g_call.seen[9],-.5f) && close(g_call.seen[12],9.712f) && unit==unit_before,"actual camera detour must retain diorama pitch, scaled stereo offset and persistent native source");
            const auto frozen_x=g_call.seen[12];
            unit[12]+=3;unit[28]-=3;
            call_site_a(&fake_add_camera,unit.data());
            require(close(g_call.seen[12],frozen_x),"actual detour keeps frozen view as native player camera moves");
            unit=unit_before;probe.set_anchor_provider({});
        }
        probe.clear_vr_eye();
        call_site_a(&fake_add_camera,unit.data());
        require(g_call.seen[12]==10 && unit==unit_before,"runtime shutdown must remove headset override");

        unsigned observed{};bool observer_restored{},observer_after{};
        probe.set_main_view_shift(.1f);
        probe.set_main_observer([&](void*,const void* source,const FrameSyncClock::Frame*) {
            ++observed;
            observer_restored=std::memcmp(source,unit_before.data(),unit_before.size())==0;
            observer_after=close(g_call.seen[12],10.1f);
        });
        call_site_a(&fake_add_camera,unit.data());
        call_site_b(&fake_add_camera,unit.data());
        require(observed==1,"additional-view observer must run once for main only");
        require(observer_restored && observer_after,"observer must run after original submission with restored native source");
        probe.set_main_observer([](void*,const void*,const FrameSyncClock::Frame*) {throw std::runtime_error("diagnostic failure");});
        require(call_site_a(&fake_add_camera,unit.data())==0x1122334400000006,"observer exception must preserve engine result");
        require(unit==unit_before,"observer exception must leave source restored");
        probe.set_main_observer({});probe.set_main_view_shift(0);

        std::cout << "engine camera probe tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
}
