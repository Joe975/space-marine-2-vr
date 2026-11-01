// Phase 0 of docs/OPENXR_MIGRATION_PLAN.md: log the raw OpenVR controller origins while the
// game runs on OpenXR, so scripts/fit_openxr_grip_offset.py can fit grip -> raw per hand.
// A background OpenVR client: it never starts SteamVR, never submits frames, never reads input.
// Usage: openvr_pose_witness <absolute output.csv> [seconds=120]
// CSV: qpc,hand,m0..m15 in the mod's vr_camera::Matrix order (row vectors, translation 12..14).
#include "OpenVrConversions.h"
#include <Windows.h>
#include <share.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

int main(int argc,char** argv) {
    if(argc<2 || argc>3 || !std::filesystem::path{argv[1]}.is_absolute()) { std::fprintf(stderr,"usage: openvr_pose_witness <absolute output.csv> [seconds]\n"); return 2; }
    const double seconds=argc==3?std::atof(argv[2]):120;
    if(!(seconds>0 && seconds<=3600)) { std::fprintf(stderr,"seconds must be in (0,3600]\n"); return 2; }
    auto error=vr::VRInitError_None;
    auto* system=vr::VR_Init(&error,vr::VRApplication_Background);
    if(!system || error!=vr::VRInitError_None) { std::fprintf(stderr,"VR_Init(Background) failed: %d (is SteamVR running?)\n",int(error)); return 1; }
    // _fsopen with read sharing: the CSV stays readable while logging (fopen_s denies sharing).
    FILE* out=_fsopen(argv[1],"w",_SH_DENYWR); if(!out) { vr::VR_Shutdown(); std::fprintf(stderr,"cannot open %s\n",argv[1]); return 1; }
    std::fprintf(out,"qpc,hand");for(int n=0;n<16;++n) std::fprintf(out,",m%d",n);std::fprintf(out,"\n");
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);std::fprintf(stderr,"qpc frequency %lld\n",frequency.QuadPart);
    const auto end=std::chrono::steady_clock::now()+std::chrono::duration<double>(seconds);
    uint64_t rows{};
    while(std::chrono::steady_clock::now()<end) {
        std::array<vr::TrackedDevicePose_t,vr::k_unMaxTrackedDeviceCount> poses{};
        LARGE_INTEGER counter{};QueryPerformanceCounter(&counter);
        system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding,0,poses.data(),uint32_t(poses.size()));
        const std::array<vr::TrackedDeviceIndex_t,2> roles{system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand),
            system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand)};
        const auto controllers=openvr_conversion::controllers(poses,roles);
        for(size_t hand=0;hand<2;++hand) if(controllers[hand]) {
            std::fprintf(out,"%lld,%zu",counter.QuadPart,hand);for(float v:*controllers[hand]) std::fprintf(out,",%.9g",v);std::fprintf(out,"\n");++rows;
        }
        std::fflush(out); // Stopping the witness early must keep everything logged so far.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::fclose(out);vr::VR_Shutdown();
    std::fprintf(stderr,"wrote %llu rows\n",static_cast<unsigned long long>(rows));
    return rows?0:1;
}
