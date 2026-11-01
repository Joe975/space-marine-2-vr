#include "ForeignViewHold.h"
#include "OrdinaryDlssPresence.h"
#include "Sm2Probe.h"
#include "EngineOutputSize.h"
#include "GameBuild.h"
#include "EngineCameraProbe.h"
#include "StreamlineProbe.h"
#include "StreamlineHistoryProbe.h"
#include "FrameSyncProbe.h"
#include "EngineProjectionHook.h"
#include "ImageCameraBinding.h"
#include "OpenVrBackend.h"
#include "OpenXrBackend.h"
#include "VrRuntimeChoice.h"
#include "EngineControllerRegistration.h"
#include "EngineBodyProbe.h"
#include "EngineBodyFollowing.h"
#include "EngineChainswordRev.h"
#include "EngineMeleeSweep.h"
#include "EngineParryProbe.h"
#include "ChainswordRevRequest.h"
#include "EngineRigidBodyVelocityProbe.h"
#include "NativeGrenadeLaunch.h"
#include "EngineArmProbe.h"
#include "EngineArmDriver.h"
#include "NativePlayerPartVisibility.h"
#include "EnginePlayerOpacity.h"
#include "EnginePlayerVisibilityProbe.h"
#include "EngineWorldMarkers.h"
#include "EngineScreenUi.h"
#include "EngineWeaponProbe.h"
#include "PerformanceTrace.h"
#include "NativePipExperiment.h"
#include "NativeRenderProbe.h"
#include "NativeTemporalProbe.h"
#include "NativeOrdinaryPairColour.h"
#include "NativeOrdinaryTextureCreation.h"
#include "NativeOrdinaryTaa.h"
#include "NativeOrdinaryBloom.h"
#include "NativeObjectConstantGetterProbe.h"
#include "NativeOrdinaryLateEffect.h"
#include "NativeOrdinarySharpen.h"
#include "NativeOrdinaryVolumeFrame.h"
#include "NativeOrdinaryShadingControl.h"
#include "NativeOrdinaryLightVisibility.h"
#include "NativeOrdinaryReflectionSelection.h"
#include "NativeOrdinaryShadowFit.h"
#include "NativeOrdinaryShadowCache.h"
#include "NativeOrdinaryShadowLights.h"
#include "NativeOcclusionHarvestOnce.h"
#include "NativeOrdinaryLightSection.h"
#include "NativeSecondCameraPlainLightTest.h"
#include "NativeTrackerHandleGuard.h"
#include "NativePoolReleaseGuard.h"
#include "NativeTrackedOrdinaryCamera.h"
#include "GpuQueueTimeline.h"
#include "NativeHzbCulling.h"
#include "NativeHzbControl.h"
#include "VrGazeCamera.h"
#include "VrTestBackend.h"
#include "NativeFaultEvidence.h"
#include "NativeSceneFaultEvidence.h"
#include "HangWatchdog.h"
#include "NativeOrdinaryProbe.h"
#include "NativeCameraTransfer.h"
#include "NativePipLens.h"
#include <spdlog/fmt/ranges.h>
#include "NativePipTonemap.h"
#include "GpuTextureReadback.h"
#include "GpuClosedListProbe.h"

#include <Framework.hpp>
#include <imgui.h>
#include <fstream>
#include <iomanip>

bool Sm2Probe::on_message(HWND window,UINT message,WPARAM key,LPARAM detail)
{
    if(!m_view_shortcuts_enabled) return true;
    return !m_view_hotkeys.message(GetForegroundWindow()==window,message,key,detail);
}

namespace {
// Diagnostic: the temporal calls in the first dumps (what the 1 Nov TAA capture read, docs/TAA_STEREO.md).
bool temporal_dumps(){static const bool value=[]{char flag[4]{};return GetEnvironmentVariableA("SM2VR_DIAG_TEMPORAL_DUMPS",flag,sizeof(flag))==1 && flag[0]=='1';}();return value;}
// The one reading of the native flags (SceneMode.h). screen_menu: the UI-coverage detector, present thread only.
scene_mode::Scene scene_of(const EngineCinematicState::Snapshot& s,bool screen_menu=false)
{
    return scene_mode::classify({s.available,s.published,s.requested,s.scripted,screen_menu});
}

// On a game build the address table does not know, nothing is hooked (every module fails closed).
// Porting starts offline from the new Retail.exe (scripts/scan_game_build.py, then
// docs/GAME_PATCH_2026-10-21.md); the DLL carries no signatures to scan with.
void report_unknown_game_build()
{
    const auto stamp=game_build::active();
    spdlog::info("GameBuild: exe timestamp {:#x} ({}) known={}",stamp,game_build::name(stamp),game_build::known(stamp));
    if(game_build::known(stamp)) return;
    spdlog::warn("GameBuild: unknown build; nothing is hooked. Port the table: scripts/scan_game_build.py, scripts/gen_game_build_table.py");
}

// A live control file: the variable's absolute path if set, else `name` beside shading.txt (the launch's
// dump folder, which the dev launcher and the installer both set up, as for light_section.txt). The
// launcher no longer sets a variable per file, so a plain launch has the players' environment.
std::filesystem::path control_path(const wchar_t* variable,const wchar_t* name)
{
    std::wstring value(32768,L'\0');
    if(const auto length=GetEnvironmentVariableW(variable,value.data(),32768);length && length<32768)
        if(std::filesystem::path path{value.c_str()};path.is_absolute()) return path;
    const auto length=GetEnvironmentVariableW(L"SM2VR_ORDINARY_SHADING_CONTROL",value.data(),32768);
    if(!length || length>=32768) return {};
    const std::filesystem::path shading{value.c_str()};
    return shading.is_absolute()?shading.parent_path()/name:std::filesystem::path{};
}
}

std::optional<std::string> Sm2Probe::on_initialize()
{
    report_unknown_game_build();
    {char trace[4]{};m_status_detail=GetEnvironmentVariableA("SM2VR_TRACE",trace,sizeof trace)==1 && trace[0]=='1';
     spdlog::info("Sm2Probe: status_detail={} (per-object lines in the 5 s status need SM2VR_TRACE=1)",m_status_detail);}
    {
        char text[32]{};const auto length=GetEnvironmentVariableA("SM2VR_DESKTOP_WINDOW",text,sizeof text);
        if(length>0 && length<sizeof text) {
            m_desktop_window=desktop_window::parse(std::string_view{text,length});
            spdlog::info("DesktopWindow: requested={} parsed={}",text,m_desktop_window.has_value());
        }
    }
    char ordinary_headset[8]{};m_ordinary_headset=GetEnvironmentVariableA("SM2VR_ORDINARY_HEADSET",ordinary_headset,sizeof(ordinary_headset))==1 && ordinary_headset[0]=='1';
    char native_hud[8]{};m_native_hud_requested=m_ordinary_headset && GetEnvironmentVariableA("SM2VR_NATIVE_HUD",native_hud,sizeof native_hud)==1 && native_hud[0]=='1';
    if(m_ordinary_headset){
        NativeOrdinaryPacket::get().continuous(true);StreamlineHistoryProbe::get().continuous(true);NativeOrdinaryBloom::get().continuous(true);NativeOrdinaryLateEffect::get().continuous(true);
        NativeOrdinaryPacket::get().tracked([](uint64_t render)->std::optional<NativeOrdinaryPacket::TrackedSource>{const auto r=NativeTrackedOrdinaryCamera::get().receipt(render);
            if(!r || !r->accepted || r->refusal!=NativeTrackedOrdinaryCamera::Refusal::none || !r->prepared || !r->pair)return {};
            return NativeOrdinaryPacket::TrackedSource{*r->prepared,r->pair->producer_list,r->pair->producer_revision};});
    }
    char perf_second[8]{};GetEnvironmentVariableA("SM2VR_ORDINARY_PERF_SECOND",perf_second,sizeof(perf_second));
    m_ordinary_perf=std::string_view(perf_second)=="1";
    if(m_ordinary_perf){
        const auto environment=GetEnvironmentStringsW();bool conflict=!environment;
        if(environment){for(auto entry=environment;*entry;entry+=wcslen(entry)+1){const std::wstring_view item{entry};const auto equals=item.find(L'=');if(equals==std::wstring_view::npos)continue;
            if(NativeOrdinaryCamera::performance_conflict(item.substr(0,equals),item.substr(equals+1)))conflict=true;
        }FreeEnvironmentStringsW(environment);}
        if(conflict)return "Ordinary performance mode refuses runtime/PIP/other ordinary experiment flags";
        wchar_t control[32768]{};const auto size=GetEnvironmentVariableW(L"SM2VR_ORDINARY_PERF_CONTROL",control,32768);
        if(!size || size>=32768 || !std::filesystem::path{control}.is_absolute())return "Ordinary performance mode requires an absolute control file";
        m_ordinary_perf_control=control;
    }
    PerformanceTrace::get().from_environment();
    wchar_t handoff_control[32768]{};
    const auto handoff_length=GetEnvironmentVariableW(L"SM2VR_ORDINARY_HANDOFF_CONTROL",handoff_control,32768);
    if(m_ordinary_headset && handoff_length && handoff_length<32768)m_handoff.configure(handoff_control);
    wchar_t stereo_hold[32768]{};
    const auto stereo_hold_length=GetEnvironmentVariableW(L"SM2VR_STEREO_HOLD_CONTROL",stereo_hold,32768);
    if(m_ordinary_headset && stereo_hold_length && stereo_hold_length<32768)
        spdlog::info("StereoHold: control={} configured={}",std::filesystem::path{stereo_hold}.string(),m_stereo_hold.configure(stereo_hold));
    std::wstring light_hold(32768,L'\0');  // heap, not stack: on_initialize's frame is near the init thread's stack size; diagnostic: docs/RIGHT_EYE_FLICKER_2026-10-28.md
    const auto light_hold_length=GetEnvironmentVariableW(L"SM2VR_DIAG_LIGHT_HOLD_CONTROL",light_hold.data(),32768);
    if(m_ordinary_headset && light_hold_length && light_hold_length<32768){NativeOrdinaryLightVisibility::get().configure_hold(light_hold.c_str());
        spdlog::warn("OrdinaryLightHold: control={} configured={} (diagnostic: holds a light's last visible occlusion result)",std::filesystem::path{light_hold.c_str()}.string(),NativeOrdinaryLightVisibility::get().hold_configured());}
    std::wstring list_copy(32768,L'\0');  // heap, not stack: on_initialize's frame is near the init thread's stack size; prototype: docs/RIGHT_EYE_FLICKER_2026-10-28.md
    const auto list_copy_length=GetEnvironmentVariableW(L"SM2VR_DIAG_LIGHT_LIST_COPY_CONTROL",list_copy.data(),32768);
    if(m_ordinary_headset && list_copy_length && list_copy_length<32768){NativeOrdinaryLightVisibility::get().configure_copy(list_copy.c_str());
        spdlog::warn("LightListCopy: control={} configured={} (prototype: view 1 uses view 0's light list)",std::filesystem::path{list_copy.c_str()}.string(),NativeOrdinaryLightVisibility::get().copy_configured());}
    {   // Decide once for lights (docs/DECIDE_ONCE_STEREO.md): SM2VR_STEREO_LIGHTS capsort (default) | shared | copy | native,
        // and a live control file for A/Bs (stereo_lights.txt beside shading.txt, or SM2VR_STEREO_LIGHTS_CONTROL).
        if(const auto control=control_path(L"SM2VR_STEREO_LIGHTS_CONTROL",L"stereo_lights.txt");m_ordinary_headset && !control.empty()){m_stereo_lights_path=control;
            NativeOrdinaryLightVisibility::get().configure_light_dump(m_stereo_lights_path.parent_path());}  // light_dump.txt N: raw view lists
        char text[16]{};const auto text_length=GetEnvironmentVariableA("SM2VR_STEREO_LIGHTS",text,sizeof text);
        auto mode=stereo_light_list::Mode::capsort;
        if(text_length && text_length<sizeof text)if(const auto parsed=stereo_light_list::parse(text))mode=*parsed;
        NativeOrdinaryLightVisibility::get().set_stereo_mode(mode);
        if(m_ordinary_headset)spdlog::info("StereoLights: mode={} control={}",stereo_light_list::name(mode),m_stereo_lights_path.string());
    }
    std::wstring cap_sort(32768,L'\0');  // heap, not stack: on_initialize's frame is near the init thread's stack size; candidate fix: docs/RIGHT_EYE_FLICKER_2026-10-28.md
    const auto cap_sort_length=GetEnvironmentVariableW(L"SM2VR_DIAG_LIGHT_CAP_SORT_CONTROL",cap_sort.data(),32768);
    if(m_ordinary_headset && cap_sort_length && cap_sort_length<32768)
        spdlog::warn("LightCapSort: control={} configured={} (candidate fix: over 240 lights keep the nearest)",std::filesystem::path{cap_sort.c_str()}.string(),m_cap_sort.configure(cap_sort.c_str()));
    std::wstring light_clamp(32768,L'\0');  // heap, not stack: on_initialize's frame is near the init thread's stack size; candidate fix: docs/RIGHT_EYE_FLICKER_2026-10-28.md
    const auto light_clamp_length=GetEnvironmentVariableW(L"SM2VR_DIAG_LIGHT_CLAMP_CONTROL",light_clamp.data(),32768);
    if(m_ordinary_headset && light_clamp_length && light_clamp_length<32768){NativeOrdinaryLightVisibility::get().configure_clamp(light_clamp.c_str());
        spdlog::warn("LightClamp: control={} configured={} (candidate fix: passing occlusion results read as 4)",std::filesystem::path{light_clamp.c_str()}.string(),NativeOrdinaryLightVisibility::get().clamp_configured());}
    wchar_t list_log[8]{};  // diagnostic: docs/RIGHT_EYE_FLICKER_2026-10-28.md (LightListProbe.h)
    if(m_ordinary_headset && GetEnvironmentVariableW(L"SM2VR_DIAG_LIGHT_LIST_LOG",list_log,8)==1 && list_log[0]==L'1'){
        const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto stamp=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew)->FileHeader.TimeDateStamp;
        // 0x1e8a900 (cache membership, read-only) is 0x1e8ac40 in 0x6ad10fcb; checked by the digest of its first 16 bytes.
        constexpr auto prologue=sha256::parse("8fda219747513f201b35b1f98ecd9837937a1cc2ec76b4985e1af61acde1814c");
        const uintptr_t fn_rva=stamp==far_light_cache::REFERENCE_BUILD?0x1e8a900:stamp==far_light_cache::PATCHED_BUILD?0x1e8ac40:0;
        std::array<uint8_t,16> bytes{};SIZE_T n{};
        const bool ok=fn_rva && far_light_cache::cache_rva(stamp) && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(base+fn_rva),bytes.data(),16,&n) && n==16 && sha256::hash(bytes)==prologue;
        NativeOrdinaryLightVisibility::get().configure_list_log(ok?base+far_light_cache::cache_rva(stamp):0,ok?reinterpret_cast<bool(*)(uintptr_t,uintptr_t)>(base+fn_rva):nullptr);
        spdlog::warn("LightList: logging on, cache membership {} (diagnostic)",ok?"read":"unavailable");
    }
    std::wstring far_light(32768,L'\0');  // heap, not stack: on_initialize's frame is near the init thread's stack size; diagnostic: docs/RIGHT_EYE_FLICKER_2026-10-28.md
    const auto far_light_length=GetEnvironmentVariableW(L"SM2VR_DIAG_FAR_LIGHT_CACHE_CONTROL",far_light.data(),32768);
    if(m_ordinary_headset && far_light_length && far_light_length<32768)
        spdlog::warn("FarLightCache: control={} configured={} (diagnostic: disables the far-light shadow cache)",std::filesystem::path{far_light.c_str()}.string(),m_far_light.configure(far_light.c_str()));
    std::wstring async_lighting(32768,L'\0');  // heap, not stack: on_initialize's frame is near the init thread's stack size; diagnostic: docs/RIGHT_EYE_FLICKER_2026-10-28.md
    const auto async_lighting_length=GetEnvironmentVariableW(L"SM2VR_DIAG_ASYNC_LIGHTING_CONTROL",async_lighting.data(),32768);
    if(m_ordinary_headset && async_lighting_length && async_lighting_length<32768)
        spdlog::warn("AsyncLighting: control={} configured={} (diagnostic: moves clustered-lighting passes off async compute)",std::filesystem::path{async_lighting.c_str()}.string(),m_async_lighting.configure(async_lighting.c_str()));
    wchar_t dump_path[32768]{};
    const auto dump_length=GetEnvironmentVariableW(L"SM2VR_EYE_DUMP_DIR",dump_path,32768);
    if(dump_length && dump_length<32768 && std::filesystem::path{dump_path}.is_absolute()) {
        std::error_code error;
        if(std::filesystem::is_directory(dump_path,error)) m_eye_dump_directory=dump_path;
    }
    // The screen-movie layout hook only feeds the eye snapshots' state.json and its own status
    // lines; nothing in play reads it (release tidy review, 26 Oct). SM2VR_DIAG_SCREEN_UI=1 forces it.
    {char screen_ui[4]{};m_screen_ui_requested=!m_eye_dump_directory.empty() ||
        (GetEnvironmentVariableA("SM2VR_DIAG_SCREEN_UI",screen_ui,sizeof screen_ui)==1 && screen_ui[0]=='1');}
    // Configure a neutral XInput bridge early; native registration runs on the input-update
    // thread. Head/controller actions remain gated until the runtime has valid tracking.
    char runtime[32]{},controllers[32]{},synthetic[32]{},melee[32]{},openxr[32]{};
    const bool xr_requested=GetEnvironmentVariableA("SM2VR_OPENXR",openxr,sizeof(openxr)) && std::atoi(openxr)==1;
    // Both runtimes drive the same controls; the backend owns the runtime-specific input.
    // Conflicting selections stay disabled, as the session does.
    const bool vr_requested=GetEnvironmentVariableA("SM2VR_OPENVR",runtime,sizeof(runtime)) && std::atoi(runtime) == 1;
    const bool synthetic_requested=GetEnvironmentVariableA("SM2VR_SYNTHETIC_EYES",synthetic,sizeof(synthetic)) && std::atoi(synthetic) == 1;
    const auto selected=vr_runtime_choice(vr_requested,xr_requested,synthetic_requested);
    m_controller_input.configure(selected==VrRuntimeChoice::openvr || selected==VrRuntimeChoice::openxr,
        GetEnvironmentVariableA("SM2VR_CONTROLLERS",controllers,sizeof(controllers)) && std::atoi(controllers) == 1,
        synthetic_requested,GetEnvironmentVariableA("SM2VR_MELEE",melee,sizeof(melee)) && std::atoi(melee)==1);
    wchar_t view_path[32768]{};
    auto view_length=GetEnvironmentVariableW(L"SM2VR_CAMERA_CONFIG",view_path,32768);
    if(view_length && view_length<32768 && std::filesystem::path{view_path}.is_absolute()) m_view_settings_path=view_path;
    else if(!view_length) {
        view_length=GetEnvironmentVariableW(L"SM2VR_CALIBRATION",view_path,32768);
        if(view_length && view_length<32768 && std::filesystem::path{view_path}.is_absolute()) m_view_settings_path=std::filesystem::path{view_path}.parent_path()/"index_camera_modes.calibration";
    }
    char chase_cam[8]{};
    m_chase_launch=GetEnvironmentVariableA("SM2VR_CHASE_CAM",chase_cam,sizeof(chase_cam)) && std::atoi(chase_cam)==1;
    // A chase launch's pack keeps the game's own cameras, so head-camera modes would be wrong all
    // launch: the mode is locked to chase, and only the file's knobs (chase_scale) are used. Saves
    // write back the file's own mode, never the live one (VrViewFile).
    m_view_settings=m_view_file.start(VrViewSettings::load(m_view_settings_path),m_chase_launch);
    if(m_chase_launch) spdlog::info("VrView: chase camera launch; camera modes locked");
    m_controller_input.native_passthrough(m_view_settings.mode!=VrViewMode::first_person);
    if (m_controller_input.needs_dispatch()) {
        EngineControllerRegistration::get().install_in_game(GetModuleHandleW(nullptr),m_controller_input);
    }
    char body_probe[32]{},body_follow[32]{},arm_probe[32]{},arm_driver[32]{},arm_demo[32]{};
    spdlog::info("CinematicState: bound={}",EngineCinematicState::get().bind_in_game(GetModuleHandleW(nullptr)));
    const bool arms=GetEnvironmentVariableA("SM2VR_ARM_PROBE",arm_probe,sizeof(arm_probe)) && std::atoi(arm_probe)==1;
    const bool hands=GetEnvironmentVariableA("SM2VR_HANDS",arm_driver,sizeof(arm_driver)) && std::atoi(arm_driver)==1 && m_controller_input.motion_preference();
    const bool demo=!hands && !m_controller_input.snapshot().enabled && GetEnvironmentVariableA("SM2VR_ARM_DEMO",arm_demo,sizeof(arm_demo)) && std::atoi(arm_demo)==1;
    const bool following=(hands || (GetEnvironmentVariableA("SM2VR_BODY_FOLLOW",body_follow,sizeof(body_follow)) && std::atoi(body_follow)==1)) && m_controller_input.motion_preference();
    char weapon_probe[32]{},weapon_aim[32]{};
    const bool aiming=GetEnvironmentVariableA("SM2VR_WEAPON_AIM",weapon_aim,sizeof(weapon_aim)) && std::atoi(weapon_aim)==1;
    const bool weapons=aiming || (GetEnvironmentVariableA("SM2VR_WEAPON_PROBE",weapon_probe,sizeof(weapon_probe)) && std::atoi(weapon_probe)==1);
    if (xr_requested || std::atoi(runtime)==1 || weapons || hands || demo || arms || following || (GetEnvironmentVariableA("SM2VR_BODY_PROBE",body_probe,sizeof(body_probe)) && std::atoi(body_probe) == 1)) {
        const auto installed = EngineBodyProbe::get().install_in_game(GetModuleHandleW(nullptr));
        EngineBodyProbe::get().enable(installed);
        spdlog::info("EngineBodyProbe: passive={} installed={}",installed,installed);
        if(weapons && installed) {
            const auto ready=EngineWeaponProbe::get().install_in_game(GetModuleHandleW(nullptr));EngineWeaponProbe::get().enable(ready);
            spdlog::info("WeaponProbe: passive=true installed={}",ready);
        }
        if(installed && m_controller_input.motion_preference()) {
            char opaque[32]{},hidden[32]{};
            const bool keep_opaque=GetEnvironmentVariableA("SM2VR_PLAYER_OPAQUE",opaque,sizeof(opaque)) && std::atoi(opaque)==1;
            // Without SM2VR_PLAYER_OPAQUE only the distance-fade pair is installed (it is live).
            const auto visibility=EnginePlayerOpacity::get().install_in_game(GetModuleHandleW(nullptr),keep_opaque);
            EnginePlayerOpacity::get().enable(visibility && keep_opaque);
            char distance_fade[32]{};
            const auto distance_configured=GetEnvironmentVariableA("SM2VR_PLAYER_DISTANCE_FADE",distance_fade,sizeof(distance_fade));
            char distance_scope[32]{};
            GetEnvironmentVariableA("SM2VR_DISTANCE_FADE_SCOPE",distance_scope,sizeof(distance_scope));
            EnginePlayerOpacity::get().distance_control(visibility && (!distance_configured || std::atoi(distance_fade)==1) ?
                (std::string_view{distance_scope}=="player"?"opaque":"all") : "native");
            EngineArmDriver::get().hide_head(GetEnvironmentVariableA("SM2VR_HIDE_HEAD",hidden,sizeof(hidden)) && std::atoi(hidden)==1);
            spdlog::info("PlayerOpacity: installed={} enabled={} opacity_hooks={}",visibility,EnginePlayerOpacity::get().snapshot().enabled,EnginePlayerOpacity::get().opacity_installed());
            spdlog::info("PlayerVisibilityFilter: installed={}",EnginePlayerVisibilityProbe::get().install_in_game(GetModuleHandleW(nullptr)));
        }
        // Controls-mode stereo readiness needs a passive native pose even if the
        // optional hand-writing driver is disabled. No virtual input is needed.
        if ((hands || demo || arms || m_controller_input.motion_preference()) && installed) {
            const auto arm_installed=EngineArmProbe::get().install_in_game(GetModuleHandleW(nullptr));
            EngineArmProbe::get().enable(arm_installed);
            if ((hands || demo) && arm_installed) { EngineArmDriver::get().configure_native(demo); EngineArmDriver::get().enable(true); }
            spdlog::info("ArmProbe: installed={} read_only={} target_source={}",arm_installed,!(hands || demo),hands?"Index":demo?"diagnostic":"none");
            // The melee hit check sweeps GUN_R as the arm driver writes it: give it the sword's path
            // instead (docs/MELEE_SWEEP.md). SM2VR_MELEE_SWEEP: replay (default), animation or hand;
            // melee_sweep.txt beside shading.txt (or SM2VR_MELEE_SWEEP_CONTROL): polled once a second, same words.
            if(hands && arm_installed) {
                char sweep_word[16]{};const auto sweep_length=GetEnvironmentVariableA("SM2VR_MELEE_SWEEP",sweep_word,sizeof sweep_word);
                const auto sweep_mode=sweep_length && sweep_length<sizeof sweep_word?vr_melee_sweep::parse(std::string_view{sweep_word,sweep_length}):std::optional{vr_melee_sweep::Mode::replay};
                if(!sweep_mode) spdlog::warn("MeleeSweep: SM2VR_MELEE_SWEEP must be replay, animation or hand; using replay");
                EngineMeleeSweep::get().set_mode(sweep_mode.value_or(vr_melee_sweep::Mode::replay));
                const bool sweep_installed=EngineMeleeSweep::get().install_in_game(GetModuleHandleW(nullptr));
                if(sweep_installed) EngineArmDriver::get().set_sword_observer([](const auto& s){EngineMeleeSweep::get().observe({s.instance,s.sword,s.root,s.final,s.native,s.time});});
                m_melee_sweep_control_path=control_path(L"SM2VR_MELEE_SWEEP_CONTROL",L"melee_sweep.txt");
                spdlog::info("MeleeSweep: installed={} mode={} control={}",sweep_installed,vr_melee_sweep::name(EngineMeleeSweep::get().mode()),m_melee_sweep_control_path.string());
            }
            // Photo mode keeps the player's arm and finger pose unless the profile asks for the game's own.
            char native_pose[8]{};
            m_viewer_keep_pose=!(GetEnvironmentVariableA("SM2VR_PHOTO_NATIVE_POSE",native_pose,sizeof native_pose) && std::atoi(native_pose)==1);
            spdlog::info("VrViewer: photo_pose={}",m_viewer_keep_pose?"player":"native");
            // Thumbs-up: play (default), photo (only up to the photo freeze) or off (docs/THUMBS_UP_2026-10-26.md).
            char thumbs_up[16]{};
            const auto thumbs_length=GetEnvironmentVariableA("SM2VR_THUMBS_UP",thumbs_up,sizeof thumbs_up);
            const auto thumbs_mode=thumbs_length<sizeof thumbs_up?vr_thumbs_up::parse(std::string_view{thumbs_up,thumbs_length}):std::nullopt;
            if(!thumbs_mode) spdlog::warn("VrThumbsUp: SM2VR_THUMBS_UP must be play, photo or off; using play");
            { std::lock_guard lock{m_thumbs_up_mutex};m_thumbs_up=vr_thumbs_up::Shaper{thumbs_mode.value_or(vr_thumbs_up::Mode::play)}; }
            spdlog::info("VrThumbsUp: mode={}",vr_thumbs_up::name(thumbs_mode.value_or(vr_thumbs_up::Mode::play)));
            if(aiming && arm_installed && EngineWeaponProbe::get().installed() && (hands || demo)) {
                EngineWeaponProbe::get().aim_driver().enable(true);
                EngineArmDriver::get().set_weapon_observer([](const auto& pose){EngineWeaponProbe::get().aim_driver().publish(pose);});
                EngineArmDriver::get().follow_weapon(true);m_weapon_demo=demo;
                wchar_t path[32768]{};const auto length=GetEnvironmentVariableW(L"SM2VR_WEAPON_CONFIG",path,32768);
                if(length && length<32768 && std::filesystem::path{path}.is_absolute()) m_weapon_settings_path=path;
                spdlog::info("WeaponAimDriver: enabled=true source={}",demo?"diagnostic":"Index right wrist");
            }
        }
        if (following && installed) {
            wchar_t guard_path[32768]{};
            auto guard_length=GetEnvironmentVariableW(L"SM2VR_CAMERA_GUARD_CONFIG",guard_path,32768);
            if(guard_length && guard_length<32768 && std::filesystem::path{guard_path}.is_absolute()) m_camera_guard_path=guard_path;
            else if(!guard_length) {
                guard_length=GetEnvironmentVariableW(L"SM2VR_CALIBRATION",guard_path,32768);
                if(guard_length && guard_length<32768 && std::filesystem::path{guard_path}.is_absolute()) m_camera_guard_path=std::filesystem::path{guard_path}.parent_path()/"index_camera_guard.calibration";
            }
            wchar_t takeover_path[32768]{};
            const auto takeover_length=GetEnvironmentVariableW(L"SM2VR_DIAG_TAKEOVER_CONTROL",takeover_path,32768);
            if(takeover_length && takeover_length<32768 && std::filesystem::path{takeover_path}.is_absolute()) {
                m_takeover_control_path=takeover_path;spdlog::warn("TakeoverControl: diagnostic control file {}",m_takeover_control_path.string());
            }
            const auto follow_installed=EngineBodyFollowing::get().install_in_game(GetModuleHandleW(nullptr));
            EngineBodyFollowing::get().enable(follow_installed);
            if(EngineBodyFollowing::get().diag_no_yaw() || EngineBodyFollowing::get().diag_no_room())
                spdlog::warn("BodyFollowing: diagnostic SM2VR_DIAG_FOLLOW_OFF no_yaw={} no_room={}",EngineBodyFollowing::get().diag_no_yaw(),EngineBodyFollowing::get().diag_no_room());
            char velocity_probe[8]{};
            if(GetEnvironmentVariableA("SM2VR_GRENADE_NATIVE_PROBE",velocity_probe,sizeof velocity_probe) && std::string_view{velocity_probe}=="1"){
                char layout[8]{};
                const bool layout_verified=GetEnvironmentVariableA("SM2VR_GRENADE_EQUIPMENT_LAYOUT",layout,sizeof layout) && std::string_view{layout}=="192";
                if(layout_verified){
                    NativeGrenadeLaunch::get().bind(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)),[](uintptr_t p,void* out,size_t size){SIZE_T got{};return p && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),out,size,&got) && got==size;},[]{return EngineBodyProbe::get().active_actor();});
                    EngineRigidBodyVelocityProbe::get().set_launch_override([](auto body,auto value,auto caller){return NativeGrenadeLaunch::get().rewrite(body,value,caller);});
                }
                char samples[8]{};
                const bool observe=GetEnvironmentVariableA("SM2VR_GRENADE_VELOCITY_SAMPLES",samples,sizeof samples) && std::string_view{samples}=="1";
                EngineRigidBodyVelocityProbe::get().set_observe(observe);
                spdlog::info("GrenadeLaunch: installed_layout_verified={} velocity_samples={}",layout_verified,observe);
                spdlog::info("RigidVelocity: probe_installed={}",EngineRigidBodyVelocityProbe::get().install_in_game(GetModuleHandleW(nullptr)));
            }
            char rev_candidate[8]{};
            if(follow_installed && GetEnvironmentVariableA("SM2VR_CHAINSAW_REV",rev_candidate,sizeof rev_candidate) && std::string_view{rev_candidate}=="1"){
                const bool rev_installed=EngineChainswordRev::get().install_in_game(GetModuleHandleW(nullptr),[this]{
                    return read_chainsword_rev_request(EngineBodyProbe::get(),m_controller_input);
                });
                spdlog::info("ChainswordRev: candidate_installed={}",rev_installed);
            }
            // With -Trace: the game's attack indications and parry outcomes as trace notes (EngineParryProbe.h).
            {char trace_flag[4]{};
             if(GetEnvironmentVariableA("SM2VR_TRACE",trace_flag,sizeof trace_flag)==1 && trace_flag[0]=='1')
                spdlog::info("ParryProbe: installed={} (trace notes attack_event, parry_sound)",EngineParryProbe::get().install_in_game(GetModuleHandleW(nullptr)));}
            if (follow_installed) { m_first_person_anchor=[this](const auto& game,const auto& eye){
                if(const auto head=EngineArmDriver::get().native_head()) EngineBodyFollowing::get().head_bone(head->actor,head->position,head->received);
                // A takeover hands back once its animation has settled (docs/ANIMATION_CAMERA.md).
                if(const auto motion=EngineArmDriver::get().native_motion()) EngineBodyFollowing::get().character_motion({motion->received,motion->actor,motion->points});
                auto anchor=EngineBodyFollowing::get().camera(game,eye);
                trace_camera(game,anchor,eye);
                observe_gaze_camera(game,anchor,eye);
                const auto driver=EngineArmDriver::get().snapshot();
                if ((driver.enabled || driver.head_hidden) && m_vr_session && !eye.synthetic) {
                    EngineArmDriver::Targets targets;
                    const auto now=std::chrono::steady_clock::now(); const auto poses=m_vr_session->controller_poses(now);
                    const auto actor=EngineBodyProbe::get().active_actor(now); const auto body=EngineBodyFollowing::get().snapshot().follow;
                    if (body.anchored && body.actor==actor) {
                        // Head hiding follows the fresh tracked camera, even when controllers sleep.
                        targets.actor=actor;targets.reference=eye.reference_epoch;targets.epoch=eye.tracking_epoch;targets.captured=now;
                        const auto pad=m_controller_input.snapshot().pad.Gamepad;targets.firearm_trigger=pad.bRightTrigger>XINPUT_GAMEPAD_TRIGGER_THRESHOLD || pad.bLeftTrigger>XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
                    }
                    if (targets.actor && poses && poses->reference_epoch==eye.reference_epoch && poses->tracking_epoch==eye.tracking_epoch) {
                        targets.captured=poses->captured;
                        // Grips and body placement are lengths on the Marine; the wrists below use the eyes' scale.
                        targets.units_per_metre=poses->units_per_metre/poses->fit_factor;
                        // Thumbs-up shape and, up to the photo freeze, the left thumb from before the B hold.
                        const bool entering=vr_viewer::before_freeze(m_vr_session->viewer_status().state);
                        vr_thumbs_up::Shaper::Fingers fingers{};
                        for (size_t hand=0;hand<2;++hand) if (poses->relative[hand]) fingers[hand]=poses->fingers[hand];
                        { std::lock_guard lock{m_thumbs_up_mutex};fingers=m_thumbs_up.apply(fingers,entering,now); }
                        for (size_t hand=0;hand<2;++hand) if (poses->relative[hand]) {
                            targets.wrists[hand]=vr_arm::wrist_world(anchor,*poses->relative[hand],EngineArmDriver::get().grip(hand),poses->units_per_metre);
                            targets.fingers[hand]=fingers[hand];
                        }
                    }
                    EngineArmDriver::get().targets(targets);
                    const auto throw_pose=m_grenade_aim_candidate && targets.actor?m_vr_session->grenade_aim(anchor,targets.actor,eye.tracking_epoch,now):std::nullopt;
                    EngineWeaponProbe::get().aim_driver().throw_aim(std::nullopt);
                    NativeGrenadeLaunch::get().publish(throw_pose,now);
                }
                return anchor;
            }; }
            spdlog::info("BodyFollowing: installed={} facing_enabled={} room_enabled=false",follow_installed,follow_installed);
        }
    }
    if (hands) {
        wchar_t path[32768]{};
        auto length=GetEnvironmentVariableW(L"SM2VR_HANDS_CALIBRATION",path,32768);
        if (length && length<32768 && std::filesystem::path{path}.is_absolute()) { m_arm_settings_path=path; }
        else if (!length) {
            length=GetEnvironmentVariableW(L"SM2VR_CALIBRATION",path,32768);
            if (length && length<32768 && std::filesystem::path{path}.is_absolute()) { m_arm_settings_path=std::filesystem::path{path}.parent_path()/"index_hands.calibration"; }
        }
    }
    EngineCameraProbe::get().set_anchor_provider([this](const auto& game,const auto& eye){return camera_anchor(game,eye);});
    char pip[32]{},pip_width[32]{},pip_height[32]{},pip_frames[32]{},pip_shift[32]{},pip_lens[32]{},pip_tags[32]{};
    if(GetEnvironmentVariableA("SM2VR_PIP_EXPERIMENT",pip,sizeof(pip)) && std::atoi(pip)==1) {
        NativePipExperiment::Config config;
        if(GetEnvironmentVariableA("SM2VR_PIP_WIDTH",pip_width,sizeof(pip_width))) config.size.width=std::atoi(pip_width);
        if(GetEnvironmentVariableA("SM2VR_PIP_HEIGHT",pip_height,sizeof(pip_height))) config.size.height=std::atoi(pip_height);
        if(GetEnvironmentVariableA("SM2VR_PIP_FRAMES",pip_frames,sizeof(pip_frames))) config.frames=static_cast<uint32_t>(std::atoi(pip_frames));
        if(GetEnvironmentVariableA("SM2VR_PIP_SHIFT",pip_shift,sizeof(pip_shift))) config.shift=static_cast<float>(std::atof(pip_shift));
        if(GetEnvironmentVariableA("SM2VR_PIP_LENS_SHIFT",pip_lens,sizeof(pip_lens))) config.lens_shift=static_cast<float>(std::atof(pip_lens));
        config.pose_tags=GetEnvironmentVariableA("SM2VR_PIP_POSE_TAGS",pip_tags,sizeof(pip_tags)) && std::atoi(pip_tags)==1;
        char retain[32]{};config.retain_until_exit=GetEnvironmentVariableA("SM2VR_PIP_RETAIN_UNTIL_EXIT",retain,sizeof(retain)) && std::atoi(retain)==1;
        const bool desktop=std::atoi(runtime)!=1 && !xr_requested && std::atoi(synthetic)!=1;
        auto& experiment=NativePipExperiment::get();
        const bool armed=desktop && experiment.bind_in_game(GetModuleHandleW(nullptr)) && experiment.arm(config);
        m_pip_diagnostics=armed;
        m_pip_pose_tags=armed && config.pose_tags;
        const bool render_probe=armed && NativeRenderProbe::get().install_in_game(GetModuleHandleW(nullptr),[]{return NativePipExperiment::get().snapshot().active;},
            [](uintptr_t resource){return RenderTargetProbe::get().observe(resource);});
        char final_lens[32]{},lens_y[32]{},sweep[32]{};float vertical{};
        if(GetEnvironmentVariableA("SM2VR_PIP_LENS_Y_SHIFT",lens_y,sizeof(lens_y))) vertical=static_cast<float>(std::atof(lens_y));
        m_pip_lens_sweep=GetEnvironmentVariableA("SM2VR_PIP_FINAL_LENS_SWEEP",sweep,sizeof(sweep)) && std::atoi(sweep)==1;
        const bool final_requested=GetEnvironmentVariableA("SM2VR_PIP_FINAL_LENS",final_lens,sizeof(final_lens)) && std::atoi(final_lens)==1;
        m_pip_final_lens=armed && final_requested && std::isfinite(vertical) && std::abs(vertical)<=.25f &&
            NativePipLens::get().install_in_game(GetModuleHandleW(nullptr),[vertical,sweeping=m_pip_lens_sweep]() -> std::optional<NativePipLens::Target> {
                const auto receipt=NativeRenderProbe::current_camera_entries();if(!receipt) return std::nullopt;
                const auto camera=NativePipExperiment::get().inspect_frame_camera(*receipt);if(!camera) return std::nullopt;
                const auto phase=sweeping?std::min(uint32_t((camera->updates-1)/512),3u):0u;
                const float sign=phase==1?0.f:phase==2?-1.f:1.f;
                return NativePipLens::Target{camera->record,camera->world,camera->lens_shift*sign,vertical*sign,phase,camera->updates,camera->recent_worlds,
                    camera->history,camera->current_eligible,camera->lock_wait_ms,receipt};
            });
        spdlog::info("PipFinalLens: requested={} installed={} vertical={} sweep={}",final_requested,m_pip_final_lens,vertical,m_pip_lens_sweep);
        char scene_tone[32]{},main_grading[32]{},scene_effects[32]{};
        const bool grading_requested=GetEnvironmentVariableA("SM2VR_PIP_MAIN_GRADING",main_grading,sizeof(main_grading)) && std::atoi(main_grading)==1;
        const bool effects_requested=GetEnvironmentVariableA("SM2VR_PIP_SCENE_EFFECTS",scene_effects,sizeof(scene_effects)) && std::atoi(scene_effects)==1;
        m_pip_scene_tonemap=armed && GetEnvironmentVariableA("SM2VR_PIP_SCENE_TONEMAP",scene_tone,sizeof(scene_tone)) && std::atoi(scene_tone)==1 &&
            NativePipTonemap::get().install_in_game(GetModuleHandleW(nullptr),[sweeping=m_pip_lens_sweep] {
                const auto receipt=NativeRenderProbe::current_camera_entries();if(!receipt) return false;
                const auto camera=NativePipExperiment::get().frame_camera(*receipt);if(!camera) return false;
                const auto phase=std::min(uint32_t((camera->updates-1)/512),3u);
                if(sweeping && phase%2==0) return false;
                return NativePipTonemap::owned(NativeRenderProbe::current_renderer(),camera->record,camera->recent_worlds);
            },grading_requested,effects_requested);
        spdlog::info("PipSceneTonemap: installed={} main_grading={} scene_effects={}",m_pip_scene_tonemap,grading_requested,effects_requested);
        wchar_t directory[32768]{};const auto length=GetEnvironmentVariableW(L"SM2VR_PIP_DUMP_DIR",directory,32768);
        if(armed && length && length<32768 && std::filesystem::path{directory}.is_absolute() && std::filesystem::is_directory(directory)) m_pip_dump_directory=directory;
        if(m_pip_final_lens && m_pip_lens_sweep && !m_pip_dump_directory.empty()) for(unsigned phase=0;phase<4;++phase)
            std::filesystem::create_directories(m_pip_dump_directory/("phase_"+std::to_string(phase)));
        if(armed) EngineCameraProbe::get().set_main_observer([](void*,const void* source,const FrameSyncClock::Frame* frame) {
            const auto state=EngineCinematicState::get().sample();
            NativePipExperiment::get().on_main(source,frame?frame->render_id:0,state.available && (state.published|state.requested)==0,frame?std::optional{frame->token}:std::nullopt);
            const auto pip=NativePipExperiment::get().snapshot();
            if(pip.active && (pip.updates==1 || pip.updates%120==0)) {
                spdlog::info("PipExperimentFrame: render_id={} index={} texture_index={} texture={:x} allocated={}x{} center=[{},{},{}] shifted=[{},{},{}]",
                    pip.render_id,pip.index,pip.texture_index,pip.texture,pip.allocated.width,pip.allocated.height,pip.center[0],pip.center[1],pip.center[2],pip.shifted[0],pip.shifted[1],pip.shifted[2]);
                if(pip.updates==1 && pip.texture) {
                    std::array<uintptr_t,64> words{},methods{};SIZE_T bytes{};
                    if(ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(pip.texture),words.data(),sizeof(words),&bytes) && bytes==sizeof(words)) {
                        ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(words[0]),methods.data(),sizeof(methods),&bytes);
                        const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                        for(size_t i=0;i<words.size();++i) spdlog::info("PipTextureLayout: offset={:#x} value={:#x} method_rva={:#x}",i*8,words[i],methods[i]>=base?methods[i]-base:0);
                    }
                }
            }
        });
        spdlog::info("PipExperiment: armed={} desktop={} requested={}x{} frames={} shift={} lens_shift={} render_probe={} scene_probe={} runtime_submission=false",armed,desktop,config.size.width,config.size.height,config.frames,config.shift,config.lens_shift,render_probe,NativeRenderProbe::get().scene_installed());
        if(armed) {NativeCameraEntries::get().enable();spdlog::info("PipCameraTransfer: installed={}",NativeCameraTransfer::get().install_in_game(GetModuleHandleW(nullptr)));}
    }
    if(m_ordinary_perf){
        NativeCameraEntries::get().enable();const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const bool armed=NativeOrdinaryCamera::get().arm([base]{uintptr_t renderer{};SIZE_T bytes{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(base+game_build::rva(0x4e0bea0)),&renderer,sizeof(renderer),&bytes) && bytes==sizeof(renderer)?renderer:uintptr_t{};},{4,600,.064f,false,true});
        if(!armed)return "Ordinary performance camera could not arm";
        spdlog::info("OrdinaryPerf: armed=true initially_enabled=false warmup=600 scene_observers=false runtime_submission=false");
    }
    char ordinary[32]{};GetEnvironmentVariableA("SM2VR_ORDINARY_OBSERVER",ordinary,sizeof(ordinary));
    if(NativeOrdinaryProbe::requested(ordinary,pip,runtime,openxr,synthetic,m_ordinary_headset)) {
        wchar_t path[32768]{};const auto length=GetEnvironmentVariableW(L"SM2VR_ORDINARY_DUMP_DIR",path,32768);
        if(length && length<32768 && std::filesystem::path{path}.is_absolute() && std::filesystem::is_directory(path)) {
            m_ordinary_dump_directory=path;const auto module=GetModuleHandleW(nullptr);
            const auto outputs=[](uintptr_t resource){return RenderTargetProbe::get().observe(resource);};
            const bool dispatch=NativeRenderProbe::get().install_in_game(module,[this]{return m_ordinary_diagnostics.load();},outputs);
            // The second-eye scene pass releases per-frame pool textures twice; without this guard the
            // game's pool list drifts and a level exit freezes (NativePoolReleaseGuard.h).
            if(dispatch)spdlog::info("PoolReleaseGuard: installed={}",NativePoolReleaseGuard::get().install_in_game(module));
            // Reuse the sole Scene entry detour. No PIP is registered; its lens provider is empty.
            const bool scene=dispatch && NativePipLens::get().install_in_game(module,[]()->std::optional<NativePipLens::Target>{return std::nullopt;});
            char inline_copy[16]{};GetEnvironmentVariableA("SM2VR_ORDINARY_INLINE_COPY",inline_copy,sizeof(inline_copy));m_ordinary_inline_requested=std::string_view(inline_copy)=="1";
            char private_output[16]{};GetEnvironmentVariableA("SM2VR_ORDINARY_PRIVATE_OUTPUT",private_output,sizeof(private_output));m_ordinary_private_requested=m_ordinary_inline_requested && std::string_view(private_output)=="1";
            char mirror[16]{};GetEnvironmentVariableA("SM2VR_ORDINARY_DESKTOP_MIRROR",mirror,sizeof(mirror));m_ordinary_mirror_requested=m_ordinary_private_requested && std::string_view(mirror)=="1";
            char second[16]{};GetEnvironmentVariableA("SM2VR_ORDINARY_SECOND_CAMERA",second,sizeof(second));
            m_ordinary_second_requested=std::string_view(second)=="1" && !m_ordinary_inline_requested;
            char fault_pod[16]{};GetEnvironmentVariableA("SM2VR_ORDINARY_FAULT_POD",fault_pod,sizeof(fault_pod));
            m_ordinary_fault_pending=m_ordinary_second_requested && std::string_view(fault_pod)=="1";
            if(m_ordinary_fault_pending)spdlog::info("OrdinaryFaultPod: requested=true waiting_for_game_module=true suppress_exception=false");
            // Every headset run: evidence for the 23 Oct level-load fault (PLAYTEST_2026-10-23_1948). Off with SM2VR_SCENE_FAULT_POD=0.
            char scene_fault[8]{};GetEnvironmentVariableA("SM2VR_SCENE_FAULT_POD",scene_fault,sizeof(scene_fault));
            // Every headset run: one thread-stack dump if no frame is presented for 10 s (24 Oct barge freeze).
            if(m_ordinary_headset){const bool armed=HangWatchdog::get().start(m_ordinary_dump_directory/L"hang.dmp",HangWatchdog::THRESHOLD,std::chrono::seconds(1),
                    [](const std::filesystem::path& path,bool ok,std::chrono::milliseconds stalled){spdlog::error("HangWatchdog: no frame presented for {} ms; thread dump written={} path={}",stalled.count(),ok,path.string());});
                spdlog::info("HangWatchdog: armed={} threshold_s=10",armed);}
            if(m_ordinary_headset && std::string_view(scene_fault)!="0"){auto& scene_pod=NativeSceneFaultEvidence::get();const bool armed=scene_pod.arm(module,m_ordinary_dump_directory/L"scene_fault_pod.bin");
                spdlog::info("SceneFaultPod: armed={} timestamp={:#x} first_chance=true suppress_exception=false",armed,scene_pod.timestamp());}
            char pair_colour[8]{};GetEnvironmentVariableA("SM2VR_ORDINARY_PAIR_COLOUR",pair_colour,sizeof(pair_colour));
            char pair_retention[8]{};GetEnvironmentVariableA("SM2VR_ORDINARY_PAIR_RETENTION",pair_retention,sizeof(pair_retention));
            const std::array<const char*,7> dependency_names{"SM2VR_ORDINARY_PAIR_COLOUR","SM2VR_ORDINARY_PRIVATE_DLSS","SM2VR_ORDINARY_NATIVE_PACKET","SM2VR_ORDINARY_TEMPORAL_PROBE","SM2VR_ORDINARY_NATIVE_CREATE","SM2VR_ORDINARY_NATIVE_BLOOM","SM2VR_ORDINARY_CONSECUTIVE"};
            std::array<std::array<char,8>,7> dependency_values{};std::array<std::string_view,7> dependencies{};
            for(size_t i=0;i<dependencies.size();++i){GetEnvironmentVariableA(dependency_names[i],dependency_values[i].data(),8);dependencies[i]=dependency_values[i].data();}
            m_ordinary_pair_requested=GpuOrdinaryPairRetention::requested(pair_retention,dependencies,m_ordinary_second_requested,m_ordinary_inline_requested);
            if(std::string_view(pair_retention)=="1" && !m_ordinary_pair_requested)spdlog::error("OrdinaryPairRetention: refused incompatible/missing explicit desktop dependencies");
            bool pair_armed{};
            if(scene && m_ordinary_second_requested && std::string_view(pair_colour)=="1"){
                NativeOrdinaryPairColour::Consumer consumer;if(m_ordinary_pair_requested)consumer=[this](const auto* event){ordinary_pair_boundary(event);};
                pair_armed=NativeOrdinaryPairColour::get().arm_in_game(module,outputs,std::move(consumer));spdlog::info("OrdinaryPairColour: armed={} resource_leases=false final_writer_verified=false",pair_armed);}
            const bool colour=scene && (!m_ordinary_pair_requested || pair_armed) && NativeOrdinaryProbe::get().install_in_game(module,outputs,[this](const auto& event){ordinary_boundary(event);});
            NativeCameraEntries::get().enable();const bool transfer=colour && NativeCameraTransfer::get().install_in_game(module);
            m_ordinary_diagnostics=dispatch && scene && colour && transfer;
            if(m_ordinary_second_requested && m_ordinary_diagnostics){char consecutive[16]{};GetEnvironmentVariableA("SM2VR_ORDINARY_CONSECUTIVE",consecutive,sizeof(consecutive));NativeOrdinaryCamera::Config camera_config;camera_config.consecutive=std::string_view(consecutive)=="1";const auto base=reinterpret_cast<uintptr_t>(module);m_ordinary_second_requested=m_ordinary_headset || NativeOrdinaryCamera::get().arm([base]{uintptr_t renderer{};SIZE_T bytes{};if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(base+game_build::rva(0x4e0bea0)),&renderer,sizeof(renderer),&bytes) || bytes!=sizeof(renderer))return uintptr_t{};return renderer;},camera_config);
                spdlog::info("OrdinarySecondSequence: consecutive={} warmup={} samples={}",camera_config.consecutive,camera_config.cadence,camera_config.samples);
                spdlog::info("OrdinarySecondCamera: armed={} samples=4 cadence=241 shift=0.064 runtime_submission=false histories_verified=false",m_ordinary_second_requested);
                if(m_ordinary_second_requested)StreamlineHistoryProbe::get().arm([]()->std::optional<StreamlineHistoryProbe::Scope>{
                    const auto scene=NativeRenderProbe::current_scene();const auto pair=NativeRenderProbe::current_ordinary_pair();
                    if(!scene || !pair || !scene->valid() || scene->slot>1)return std::nullopt;const auto& eye=scene->slot?pair->right:pair->left;
                    if(eye.entry!=scene->entry || eye.render_list!=scene->list || scene->camera_revision_before!=eye.revision)return std::nullopt;
                    return StreamlineHistoryProbe::Scope{scene->dispatch_sequence,eye.render_id,eye.revision,eye.token,scene->slot,eye.entry,scene->list};
                });
                if(m_ordinary_second_requested){StreamlineOptionsProbe::get().enable();StreamlineProbe::get().set_constants_observer([](const auto& constants,const auto& frame,const auto& viewport){return StreamlineHistoryProbe::get().constants_begin(constants,frame,viewport);});
                    char private_dlss[16]{};GetEnvironmentVariableA("SM2VR_ORDINARY_PRIVATE_DLSS",private_dlss,sizeof(private_dlss));
                    if(std::string_view(private_dlss)=="1")StreamlineHistoryProbe::get().arm_private([this]()->std::optional<OrdinaryDlssContext::Cameras>{
                        const auto scene=NativeRenderProbe::current_scene();const auto pair=NativeRenderProbe::current_ordinary_pair();if(!scene || !pair || !scene->valid() || scene->slot>1)return std::nullopt;
                        const auto views=NativeRenderProbe::read_views(scene->list);if(!views || views->size()!=2 || (*views)[0].slot!=0 || (*views)[1].slot!=1 || !(*views)[0].main() || !(*views)[1].main())return std::nullopt;
                        uint32_t counter{};uintptr_t active{};SIZE_T bytes{};
                        if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(scene->list+0x29c78),&counter,sizeof(counter),&bytes) || bytes!=sizeof(counter) || counter!=scene->render_counter_before ||
                            !ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(scene->renderer+0x140),&active,sizeof(active),&bytes) || bytes!=sizeof(active) || active!=scene->entry)return std::nullopt;
                        OrdinaryDlssContext::Cameras cameras;cameras.counter=counter;cameras.renderer=scene->renderer;
                        for(size_t i=0;i<2;++i){cameras.world[i]=(*views)[i].world;cameras.projection[i]=(*views)[i].projection;cameras.inverse_projection[i]=(*views)[i].inverse_projection;}
                        if(m_ordinary_headset){const auto packet=NativeOrdinaryPacket::get().receipt(scene->dispatch_sequence);if(!packet || !packet->applied || packet->refusal || !packet->tracked)return {};
                            OrdinaryDlssContext::Tracked tracked;tracked.dispatch=scene->dispatch_sequence;for(size_t i=0;i<2;++i){tracked.world[i]=packet->tracked->prepared.eyes[i].world;tracked.projection[i]=packet->tracked->prepared.eyes[i].projection;}cameras.tracked=tracked;}
                        return cameras;
                    },[](const auto& viewport,const auto& options){return StreamlineOptionsProbe::get().submit_observed(viewport,options);},[](const auto& constants,const auto& frame,const auto& viewport){return StreamlineProbe::get().submit_observed(constants,frame,viewport);});
                    StreamlineHistoryProbe::get().skip_private(&NativeOrdinaryPacket::recoverable_job); // a refused, never shown job keeps the private eye intact
                    spdlog::info("OrdinaryPrivateDLSS: enabled={} viewport={} bounded=true runtime_submission=false histories_verified=false",StreamlineHistoryProbe::get().private_requested(),OrdinaryDlssContext::VIEWPORT);
                    char packet[16]{};GetEnvironmentVariableA("SM2VR_ORDINARY_NATIVE_PACKET",packet,sizeof(packet));
                    if(std::string_view(packet)=="1" && StreamlineHistoryProbe::get().private_requested()){
                        const bool armed=NativeOrdinaryPacket::get().arm_in_game(module,[](uint64_t dispatch)->std::optional<NativeOrdinaryPacket::Sdk>{
                            // TAA's proof: the dispatch counts jobs as the SDK ordinal and token do, so the packet's and the bloom's continuity hold.
                            if(NativeOrdinaryTaa::get().accepted(dispatch))return NativeOrdinaryPacket::Sdk{1,dispatch,uint32_t(dispatch)};
                            const auto receipt=StreamlineHistoryProbe::get().private_accepted(dispatch);if(!receipt)return {};
                            const auto batch=StreamlineHistoryProbe::get().sample(dispatch);if(!batch.installed || batch.dropped)return {};std::optional<NativeOrdinaryPacket::Sdk> sdk;
                            for(const auto& event:batch.events)if(event.scope.slot==1 && event.kind==StreamlineHistoryProbe::Kind::evaluate && event.feature==uint32_t(sl::kFeatureDLSS)){
                                if(sdk || event.viewport!=OrdinaryDlssContext::VIEWPORT || !event.readable || !event.scope_current || event.unknown_inputs || event.result!=sl::Result::eOk || !event.returned_frame_current || !event.returned_frame || event.returned_frame->ordinal!=receipt->sdk_ordinal || event.returned_frame->token!=receipt->token)return {};
                                sdk=NativeOrdinaryPacket::Sdk{event.returned_frame->epoch,event.returned_frame->ordinal,event.returned_frame->token};}
                            return sdk;
                        });spdlog::info("OrdinaryNativePacket: armed={} samples=4 runtime_submission=false native_gpu_histories_verified=false unsupported_offsets=5dc,874,8b4",armed);
                    }
                    char temporal[16]{};GetEnvironmentVariableA("SM2VR_ORDINARY_TEMPORAL_PROBE",temporal,sizeof(temporal));
                    if(std::string_view(temporal)=="1")spdlog::info("OrdinaryTemporalProbe: installed={} resource_leases=false native_gpu_histories_verified=false",NativeTemporalProbe::get().install_in_game(module,outputs));
                }}
            else m_ordinary_second_requested=false;
            spdlog::info("OrdinaryObserver: enabled={} dispatch={} scene={} colour={} transfer={} pip_enabled=false colour_shims=false runtime_submission=false",
                m_ordinary_diagnostics.load(),dispatch,scene,colour,transfer);
        } else spdlog::error("OrdinaryObserver: refused missing absolute existing dump directory");
    } else if(std::string_view(ordinary)=="1") spdlog::error("OrdinaryObserver: refused incompatible PIP/runtime/synthetic mode");
    spdlog::info("Sm2Probe: on_initialize (dx12={}, dx11={})", g_framework->is_dx12(), g_framework->is_dx11());
    return Mod::on_initialize();
}

std::optional<std::string> Sm2Probe::on_initialize_d3d_thread()
{
    // The framework starts with its F11 overlay open and the launchers delete vr_config.txt, so
    // every run used to build and draw the whole panel each frame (and hold mouse and keyboard)
    // until F11. Start closed, after the config load; F11 still opens it (Framework::on_message).
    g_framework->set_draw_ui(false,false);
    spdlog::info("Sm2Probe: F11 overlay closed at start");
    if(m_ordinary_diagnostics) {
        const auto& hook=g_framework->get_d3d12_hook();
        if(m_ordinary_inline_requested || m_ordinary_pair_requested)m_render_targets.observe_binding=[this](uintptr_t command,std::span<const uintptr_t> resources,bool known){m_pip_completion.bound(command,resources,known);};
        if(m_ordinary_pair_requested)m_render_targets.transform_binding=[this](auto* command,const auto& binding,auto views){return m_ordinary_pair_retention.transform(command,binding,views);};
        if(m_ordinary_private_requested)m_render_targets.transform_binding=[this](auto* command,const auto& binding,auto views){return m_ordinary_private_output.transform(command,binding,views);};
        const bool rtv=hook && m_render_targets.install(hook->get_device());
        spdlog::info("OrdinaryRtvProbe: installed={} early={}",rtv,m_render_targets.status().early_creation);
        if(!rtv) m_ordinary_diagnostics=false;
        char native_creation[16]{};GetEnvironmentVariableA("SM2VR_ORDINARY_NATIVE_CREATE",native_creation,sizeof(native_creation));
        if(rtv && m_ordinary_second_requested && !m_ordinary_inline_requested && std::string_view(native_creation)=="1" && NativeTemporalProbe::get().sample(0).installed)
        {
            spdlog::info("OrdinaryNativeCreate: armed={} creation_only=true textures=2 native_pass_substitution=false retirement=process_lifetime",NativeOrdinaryTextureCreation::get().arm_in_game(GetModuleHandleW(nullptr),hook->get_device()));
            char taa[4]{};if(GetEnvironmentVariableA("SM2VR_ORDINARY_TAA",taa,sizeof(taa))==1 && taa[0]=='1')NativeOrdinaryTaa::get().enable(NativeOrdinaryTextureCreation::taa().arm_like(NativeOrdinaryTextureCreation::get()));
            spdlog::info("OrdinaryTaa: enabled={}",NativeOrdinaryTaa::get().enabled());
            char skip[4]{};if(GetEnvironmentVariableA("SM2VR_DIAG_NO_NATIVE_REFERENCE",skip,sizeof(skip))==1 && skip[0]=='1')
                spdlog::warn("OrdinaryNativeCreate: diagnostic SM2VR_DIAG_NO_NATIVE_REFERENCE: no extra native reference on the owned pair (not safe for play)");
        }
        char native_bloom[16]{};GetEnvironmentVariableA("SM2VR_ORDINARY_NATIVE_BLOOM",native_bloom,sizeof(native_bloom));
        if(std::string_view(native_bloom)=="1"){
            const bool completion=NativeRenderProbe::get().attach_completion([this](const auto& snapshot){
                const auto evaluations=NativeCallCensus::get().pending(NativeCallCensus::Hook::sdk_evaluate);NativeCallCensus::get().complete(snapshot.scene_calls);
                if(m_ordinary_headset && snapshot.scene_calls){const auto mode=scene_of(EngineCinematicState::get().snapshot()).mode;const auto taa=NativeOrdinaryTaa::get().take_calls();GpuCompositorOutput::clear_native(taa!=0);OrdinaryDlssPresence::get().complete(evaluations,mode==scene_mode::Mode::gameplay || mode==scene_mode::Mode::hub,taa);}
                NativeOrdinaryBloom::get().complete(snapshot);
                // A dispatch that drew a game PIP behind the owned pair (it survived: the bytes past the
                // game's HDR table happened to be harmless). Evidence, and the hold if the add hook missed it.
                {std::array<ForeignViewHold::View,16> views{};size_t n{};for(const auto& v:snapshot.before)if(n<views.size())views[n++]={v.slot,v.flags};
                 if(const auto slot=ForeignViewHold::behind_pair({views.data(),n})){ForeignViewHold::get().seen(std::chrono::steady_clock::now(),*slot,true);
                    int32_t hdr{-1};SIZE_T bytes{};ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(snapshot.list+0x168+size_t(*slot)*0xa60+0x188),&hdr,sizeof(hdr),&bytes);
                    static std::atomic_uint32_t logged{};if(logged.fetch_add(1)<8)spdlog::error("StereoForeignView: dispatch={} slot={} hdr_index={} views={} behind_pair=true (a game PIP view drawn behind the owned pair)",snapshot.sequence,*slot,hdr,n);}}
                const auto packet=NativeOrdinaryPacket::get().receipt(snapshot.sequence);
                const auto bloom_record=NativeOrdinaryBloom::get().record(snapshot.sequence);
                // Unlimited recovery: per-episode lines for the first 32 episodes, then every 64th.
                const auto recovery=NativeOrdinaryBloom::get().recovery_status();const bool log_episode=OrdinaryStereoRecovery::log_episode(recovery.policy.episodes);
                if(m_ordinary_headset && bloom_record && bloom_record->resumed && (!bloom_record->recovered || log_episode))
                    spdlog::info("OrdinaryHeadsetResumed: dispatch={} previous={} accepted={} bloom_refusal={} history_reset={}",snapshot.sequence,bloom_record->previous_dispatch,bloom_record->accepted,uint32_t(bloom_record->refusal),bloom_record->reset);
                if(m_ordinary_headset && bloom_record && bloom_record->restart && OrdinaryStereoRecovery::log_episode(recovery.restarts))
                    spdlog::warn("OrdinaryHeadsetRestarted: dispatch={} last_accepted={} restarts={} suspended={} (camera cut that cannot chain: fresh bootstrap, owned pair kept)",snapshot.sequence,recovery.last_accepted,recovery.restarts,bloom_record->suspended);
                // A recoverable packet refusal (level transition) that suspended the bloom is not a stop.
                const bool failed=m_ordinary_headset && !m_ordinary_headset_failure_dumped &&
                    ((packet && packet->refusal && !(packet->recoverable && bloom_record && bloom_record->suspended)) || (bloom_record && bloom_record->refusal!=NativeOrdinaryBloom::Refusal::none && !bloom_record->suspended));
                // A recoverable refusal suspends owned stereo (native mono + flat panel) instead of stopping it.
                if(m_ordinary_headset && recovery.renewals>m_ordinary_renewals_logged){m_ordinary_renewals_logged=recovery.renewals;
                    // reused=true: a retired pair made for this layout is current again (no new textures); generation counts pairs created.
                    const auto& creation=NativeOrdinaryTextureCreation::get();const auto renewal=creation.renewal();
                    spdlog::warn("OrdinaryBloomRenewed: reused={} renewals={} generation={} cap={} retired={} difference={:#x} episode={} ({})",
                        renewal.reused,recovery.renewals,renewal.generation,NativeOrdinaryTextureCreation::GENERATIONS,creation.retired(),recovery.renewed_difference,recovery.policy.episodes,
                        renewal.reused?"native pool rebuilt to a layout seen before; its retired pair is current again at the bootstrap":"native pool rebuilt to a new layout; owned pair retired, a fresh pair at the bootstrap");}
                if(m_ordinary_headset && recovery.rebases>m_ordinary_rebases_logged){m_ordinary_rebases_logged=recovery.rebases;
                    if(OrdinaryStereoRecovery::log_episode(recovery.rebases))spdlog::warn("OrdinaryBloomContractRebased: records={:#x}->{:#x} record_count={}->{} rebases={} episode={} (native record table moved; owned bloom contract otherwise identical)",
                        recovery.rebased_from,recovery.rebased_to,recovery.rebased_count_from,recovery.rebased_count_to,recovery.rebases,recovery.policy.episodes);}
                const bool suspended_first=m_ordinary_headset && bloom_record && bloom_record->suspended && recovery.policy.episodes>m_ordinary_suspension_logged;
                if(suspended_first)m_ordinary_suspension_logged=recovery.policy.episodes;
                if(suspended_first && recovery.policy.episodes==33)spdlog::info("OrdinaryHeadsetSuspended: more than 32 episodes; per-episode lines now every 64th episode");
                if(suspended_first && log_episode){
                    const auto owned=NativeOrdinaryTextureCreation::get().owned_pair();const auto& seen=bloom_record->contract;
                    spdlog::error("OrdinaryHeadsetSuspended: dispatch={} bloom_refusal={} packet_refusal={} contract_failure={} contract_difference={:#x} last_accepted={} episode={} transition_hold={} refused_attempts={} created_variant={} observed_variant={} created_bank_dimensions={}x{} observed_bank_dimensions={}x{} created_records={:#x} observed_records={:#x} record_count={}->{}",
                        snapshot.sequence,uint32_t(bloom_record->refusal),packet?packet->refusal:999,NativeOrdinaryBloom::name(bloom_record->contract_failure),bloom_record->contract_difference,recovery.last_accepted,recovery.policy.episodes,recovery.policy.transition,recovery.policy.refused_attempts,
                        owned?owned->contract.bank_variant:-1,seen?seen->bank_variant:-1,owned?owned->contract.bank_dimensions[0]:0,owned?owned->contract.bank_dimensions[1]:0,seen?seen->bank_dimensions[0]:0,seen?seen->bank_dimensions[1]:0,
                        owned?owned->contract.records:0,seen?seen->records:0,owned?owned->contract.record_count:0,seen?seen->record_count:0);}
                if(m_ordinary_headset && bloom_record && bloom_record->recovered && log_episode)
                    spdlog::info("OrdinaryHeadsetRecovered: dispatch={} previous={} episode={} transition_episodes={} cutscene_holds={} settle_breaks={} held_ms={} attempts={} refused_attempts={} recoveries={} suspended_records={} dropped_pairs={}",snapshot.sequence,bloom_record->previous_dispatch,
                        recovery.policy.episodes,recovery.policy.transition_episodes,recovery.policy.cutscene_holds,recovery.policy.settle_breaks,std::chrono::duration_cast<std::chrono::milliseconds>(OrdinaryStereoRecovery::Clock::now()-recovery.policy.since).count(),recovery.policy.attempts,recovery.policy.refused_attempts,recovery.policy.recoveries,recovery.suspended_records,m_ordinary_suspended_pairs);
                const bool dump=m_ordinary_headset && ((snapshot.ordinary_pair && m_ordinary_headset_dumps<4) || failed || (bloom_record && bloom_record->resumed && m_ordinary_headset_dumps<8) || (suspended_first && m_ordinary_headset_dumps<16));
                if(failed){m_ordinary_headset_failure_dumped=true;spdlog::error("OrdinaryHeadsetStopped: dispatch={} packet_refusal={} bloom_refusal={} scene_calls={} scene_refused={} scene_dropped={}",snapshot.sequence,packet?packet->refusal:999,bloom_record?uint32_t(bloom_record->refusal):999,snapshot.scene_calls,snapshot.scene_refused,snapshot.scene_dropped);}
                if(dump){
                    std::ofstream out{m_ordinary_dump_directory/("headset_packet_"+std::to_string(m_ordinary_headset_dumps++)+".json")};
                    out<<"{\"packet\":";NativeOrdinaryPacket::get().write_receipt(out,snapshot.sequence);out<<",\"bloom\":";NativeOrdinaryBloom::get().write(out,snapshot.sequence);out<<",\"sdk\":";StreamlineHistoryProbe::write(out,StreamlineHistoryProbe::get().sample(snapshot.sequence));if(temporal_dumps()){out<<",\"temporal\":";NativeTemporalProbe::write(out,NativeTemporalProbe::get().sample(snapshot.sequence));}out<<"}";
                }
                if(m_ordinary_pair_ready)m_ordinary_pair_retention.complete(snapshot,NativeOrdinaryPacket::get().receipt(snapshot.sequence).value_or(NativeOrdinaryPacket::Receipt{}));
                if(m_ordinary_headset && m_handoff.queued())m_handoff.visit(PresentHandoff::Point::dispatch_complete,[this]{
                    if(!m_native_hud_requested)capture_ordinary_pair(true);
                });
                if(dump){
                    std::ofstream out{m_ordinary_dump_directory/("headset_handoff_"+std::to_string(snapshot.sequence)+".json")};
                    out<<"{\"colour\":";NativeOrdinaryPairColour::write(out,NativeOrdinaryPairColour::get().sample(snapshot.sequence),0,0);
                    out<<",\"pairs\":[";bool comma{};for(const auto& pair:m_ordinary_pair_retention.samples()){if(comma)out<<',';comma=true;GpuOrdinaryPairRetention::write(out,pair);}out<<"]}";
                    if(const auto& hook=g_framework->get_d3d12_hook();hook && hook->get_swap_chain()){
                        Microsoft::WRL::ComPtr<IDXGISwapChain3> swap;if(SUCCEEDED(hook->get_swap_chain()->QueryInterface(IID_PPV_ARGS(&swap)))){
                            DXGI_SWAP_CHAIN_DESC1 desc{};swap->GetDesc1(&desc);spdlog::info("OrdinaryHeadsetBuffers: count={} current={}",desc.BufferCount,swap->GetCurrentBackBufferIndex());
                            for(UINT i=0;i<std::min(desc.BufferCount,UINT(8));++i){Microsoft::WRL::ComPtr<ID3D12Resource> buffer;if(SUCCEEDED(swap->GetBuffer(i,IID_PPV_ARGS(&buffer))))spdlog::info("OrdinaryHeadsetBuffer: index={} identity={}",i,GpuObjectIdentity::get(buffer.Get()));}
                        }
                    }
                }});
            // Fed by that completion, so only with it: stereo waits while the game runs no DLSS.
            if(completion && m_ordinary_headset)NativeTrackedOrdinaryCamera::get().set_dlss_gate([]{return OrdinaryDlssPresence::get().held();});
            const bool bloom=completion && rtv && m_ordinary_second_requested && !m_ordinary_inline_requested && NativeOrdinaryTextureCreation::get().armed() && NativeOrdinaryBloom::get().install_in_game(GetModuleHandleW(nullptr));
            spdlog::info("OrdinaryNativeBloom: installed={} bootstrap=unchanged owned_jobs=3 previous_reset=true colour_shims=false runtime_submission=false",bloom);
            if(bloom)NativeTrackedOrdinaryCamera::get().set_resume_gate([]{return NativeOrdinaryBloom::get().resume_gate();});
            // A level-transition hold that sees a cutscene settles in 0.5 s instead of 8 s (PLAYTEST_2026-10-24_1720).
            if(bloom)NativeOrdinaryBloom::get().cutscene_source([]{const auto state=EngineCinematicState::get().sample();return state.available && state.scripted;});
            // A pool rebuilt for good (24 Oct, leaving the armoury) renews the owned pair instead of waiting forever.
            if(bloom)NativeOrdinaryBloom::get().renewal([](const native_texture_contract::Snapshot& observed){auto& creation=NativeOrdinaryTextureCreation::get();const bool renewed=creation.renew(observed);
                // At the cap a rebuilt pool of a new layout can't be renewed: stereo waits until the pool returns to a layout
                // one of the pairs was made for (26 Oct 11:55, 78 s flat at the old cap of 4, before retired pairs were reused).
                if(!renewed && creation.generation()>=NativeOrdinaryTextureCreation::GENERATIONS){static std::atomic_bool logged{};
                    if(!logged.exchange(true))spdlog::error("OrdinaryBloomRenewed: refused generation={} cap={} retired={} (a new pool layout and no fresh owned pair left; stereo waits for a layout a pair was made for)",
                        creation.generation(),NativeOrdinaryTextureCreation::GENERATIONS,creation.retired());}
                return renewed;});
            if(!bloom){NativeOrdinaryCamera::get().request_stop();NativeTrackedOrdinaryCamera::get().request_stop();}
        }
        if(m_ordinary_pair_requested){
            char sdk_pixels[8]{};GetEnvironmentVariableA("SM2VR_ORDINARY_SDK_PIXELS",sdk_pixels,sizeof(sdk_pixels));m_ordinary_sdk_requested=std::string_view(sdk_pixels)=="1";
            if(m_ordinary_sdk_requested){
                m_ordinary_sdk_capture.enabled(!m_ordinary_headset);
                m_pip_completion.sdk_copies(&m_ordinary_sdk_capture);
                NativeOrdinarySharpen::get().boundary=[this](const auto& scope,uintptr_t wrapper,uint32_t stage){uintptr_t resource{};if(wrapper && ordinary_scene_scope::read(wrapper+0x278,resource))m_ordinary_sdk_capture.request(scope.scene.dispatch_sequence,scope.scene.slot,stage,resource);};
                NativeOrdinaryLateEffect::get().early_input=[this](const auto& scope,uintptr_t wrapper){uintptr_t resource{};if(wrapper && ordinary_scene_scope::read(wrapper+0x278,resource))m_ordinary_sdk_capture.request(scope.scene.dispatch_sequence,scope.scene.slot,7,resource);};
                NativeOrdinaryBloom::get().colour_input=[this](const auto& scope,uintptr_t wrapper){uintptr_t resource{};if(wrapper && ordinary_scene_scope::read(wrapper+0x278,resource))m_ordinary_sdk_capture.request(scope.scene.dispatch_sequence,scope.scene.slot,8,resource);};
                m_ordinary_sdk_capture.current_scope=[]()->std::optional<GpuSdkCapture::Scope>{const auto s=NativeRenderProbe::current_scene();if(!s || s->slot>1 || !NativeRenderProbe::current_ordinary_pair())return {};return GpuSdkCapture::Scope{s->dispatch_sequence,s->slot};};
                NativeOrdinaryBloom::get().tone_output=[this](const auto& scope,uintptr_t wrapper){uintptr_t resource{};if(ordinary_scene_scope::read(wrapper+0x278,resource))m_ordinary_sdk_capture.request(scope.scene.dispatch_sequence,scope.scene.slot,2,resource);};
                NativeOrdinaryBloom::get().tone_inputs=[this](const auto& scope,std::array<uintptr_t,3> wrappers){for(uint32_t i=0;i<3;++i){uintptr_t resource{};if(wrappers[i] && ordinary_scene_scope::read(wrappers[i]+0x278,resource))m_ordinary_sdk_capture.request(scope.scene.dispatch_sequence,scope.scene.slot,4+i,resource);}};
                StreamlineHistoryProbe::get().evaluation_boundary=[this](const auto& event,bool after){
                    if(!m_ordinary_pair_ready || !event.readable || event.unknown_inputs || event.feature!=sl::kFeatureDLSS || !event.command || event.scope.slot>1)return;
                    auto* command=reinterpret_cast<ID3D12GraphicsCommandList*>(event.command);
                    if(!m_pip_completion.open_recording(command))return;
                    for(const auto& tag:event.tags)if(tag.type==(after?sl::kBufferTypeScalingOutputColor:sl::kBufferTypeScalingInputColor)){
                        GpuCompletionProbe::internal([&]{m_ordinary_sdk_capture.append(event.scope.dispatch,event.scope.slot,after,command,reinterpret_cast<ID3D12Resource*>(tag.native),D3D12_RESOURCE_STATES(tag.state));});
                    }
                };
            }
            m_pip_completion.inline_copies(&m_ordinary_inline);
            const bool commands=rtv && NativeOrdinaryBloom::get().installed() && m_pip_completion.install(hook->get_device(),hook->get_command_queue(),[this](uintptr_t handle){return m_render_targets.canonical_resource(handle);},{},[this](uintptr_t command){return m_render_targets.observes_binding(command);});
            Microsoft::WRL::ComPtr<IDXGISwapChain3> swap;
            m_ordinary_pair_ready=commands && hook->get_swap_chain() && SUCCEEDED(hook->get_swap_chain()->QueryInterface(IID_PPV_ARGS(&swap))) &&
                m_ordinary_pair_retention.configure(hook->get_device(),hook->get_command_queue(),swap.Get(),m_ordinary_inline,m_render_targets,[this](auto* command){return m_pip_completion.open_recording(command);},[](auto* command,auto handle){GpuCompletionProbe::internal([&]{const float black[]{0,0,0,1};command->ClearRenderTargetView(handle,black,0,nullptr);});},[this](uint64_t tag){return m_pip_completion.finish_inline(tag);});
            spdlog::info("OrdinaryPairRetention: configured={} command_hooks={} pairs=2 distinct_outputs=4 retirement=process_lifetime shader_override=false native_histories_verified=false runtime_submission=false",m_ordinary_pair_ready.load(),commands);
            if(!m_ordinary_pair_ready){NativeOrdinaryCamera::get().request_stop();NativeTrackedOrdinaryCamera::get().request_stop();}
        }
        if(rtv && m_ordinary_inline_requested) {
            m_pip_completion.inline_copies(&m_ordinary_inline);
            const bool commands=m_pip_completion.install(hook->get_device(),hook->get_command_queue(),[this](uintptr_t handle){return m_render_targets.canonical_resource(handle);},{},[this](uintptr_t command){return m_render_targets.observes_binding(command);});
            Microsoft::WRL::ComPtr<IDXGISwapChain3> swap;DXGI_SWAP_CHAIN_DESC1 desc{};
            std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> leases;std::vector<ID3D12Resource*> sources;
            bool buffers=commands && hook->get_swap_chain() && SUCCEEDED(hook->get_swap_chain()->QueryInterface(IID_PPV_ARGS(&swap))) && SUCCEEDED(swap->GetDesc1(&desc)) && desc.BufferCount>0 && desc.BufferCount<=8;
            if(buffers)for(UINT i=0;i<desc.BufferCount;++i){Microsoft::WRL::ComPtr<ID3D12Resource> buffer;if(FAILED(swap->GetBuffer(i,IID_PPV_ARGS(&buffer)))){buffers=false;break;}sources.push_back(buffer.Get());leases.push_back(std::move(buffer));}
            if(buffers && m_ordinary_private_requested){const bool owned=m_ordinary_private_output.configure(hook->get_device(),sources.front(),[this](auto* command){return m_pip_completion.open_recording(command);},[](auto* command,auto handle){GpuCompletionProbe::internal([&]{const float black[]{0,0,0,1};command->ClearRenderTargetView(handle,black,0,nullptr);});});
                spdlog::info("OrdinaryPrivateOutput: configured={} shader_override=false native_compositor_replay=false desktop_mirror={} retirement=process_lifetime",owned,m_ordinary_mirror_requested);
                if(owned)sources={m_ordinary_private_output.texture()};else buffers=false;}
            m_ordinary_inline_ready=buffers && m_ordinary_inline.configure(hook->get_device(),hook->get_command_queue(),sources);
            spdlog::info("OrdinaryInlineCopy: enabled={} command_hooks={} buffers={} runtime_submission=false",m_ordinary_inline_ready.load(),commands,sources.size());
        }
    }
    if(m_pip_diagnostics) {
        const auto& hook=g_framework->get_d3d12_hook();
        m_render_targets.observe_binding=[this](uintptr_t command,std::span<const uintptr_t> resources,bool known){m_pip_completion.bound(command,resources,known);};
        spdlog::info("PipRtvProbe: installed={}",hook && m_render_targets.install(hook->get_device()));
        m_pip_completion_enabled=hook && m_pip_completion.install(hook->get_device(),hook->get_command_queue(),[this](uintptr_t handle){return m_render_targets.canonical_resource(handle);},[]() -> std::optional<GpuCompletionProbe::Origin> {
            const auto receipt=NativeRenderProbe::current_camera_entries();if(!receipt) return std::nullopt;
            const auto origin=NativePipExperiment::get().render_origin(NativeRenderProbe::current_renderer(),{},&*receipt);
            if(!origin) return std::nullopt;
            return GpuCompletionProbe::Origin{origin->render_id,origin->token,origin->record,origin->source_world,origin->render_world,
                receipt->revision,receipt->render_list,receipt->pip_entry};
        },[this](uintptr_t command){return m_render_targets.observes_binding(command);});
        spdlog::info("PipCompletionProbe: installed={}",m_pip_completion_enabled);
        const auto capabilities=m_pip_completion.barrier_capabilities();
        spdlog::info("PipBarrierCapabilities: known={} enhanced_supported={} enhanced_hook_installed={} base_writer_hook_mask={}",
            capabilities.known,capabilities.enhanced_supported,capabilities.enhanced_hook_installed,capabilities.base_writer_hook_mask);
        spdlog::info("PipAllocationObserver: hook_mask={}",m_pip_completion.allocation_status().hook_mask);
        const auto early=GpuSignatureProbe::get().status();spdlog::info("PipEarlySignatureState: installed={} device_calls={} implementations={} signature_calls={} annotated={} failed={}",early.early_installed,early.device_calls,early.implementations,early.signature_calls,early.annotated,early.failed);
        spdlog::info("PipBindingCapabilities: known={} list4_supported={} hook_mask={}",capabilities.binding_known,capabilities.list4_supported,capabilities.binding_hook_mask);
        const auto extended=m_pip_completion.extended_writer_status();spdlog::info("PipExtendedCapabilities: known={} list1={} list4={} list6={} hook_mask={}",extended.known,extended.list1,extended.list4,extended.list6,extended.hook_mask);
    }
    log_swap_chain("d3d thread init");
    m_last_log_time = std::chrono::steady_clock::now();
    return Mod::on_initialize_d3d_thread();
}

void Sm2Probe::log_swap_chain(const char* reason)
{
    const auto& hook = g_framework->get_d3d12_hook();
    if (!hook || hook->get_swap_chain() == nullptr) {
        spdlog::info("Sm2Probe: {}: no D3D12 swap chain", reason);
        return;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    hook->get_swap_chain()->GetDesc1(&desc);
    spdlog::info("Sm2Probe: {}: swap chain {}x{} format={} buffers={} swap_effect={} flags={:#x}, display {}x{}, render {}x{}, device={:x}, queue={:x}", reason,
        desc.Width, desc.Height, static_cast<int>(desc.Format), desc.BufferCount, static_cast<int>(desc.SwapEffect), desc.Flags, hook->get_display_width(),
        hook->get_display_height(), hook->get_render_width(), hook->get_render_height(), reinterpret_cast<uintptr_t>(hook->get_device()),
        reinterpret_cast<uintptr_t>(hook->get_command_queue()));
    // Eye-size decoupling (docs/EYE_SIZE.md) depends on whether the game sizes its output from
    // the window or from its config: record DXGI scaling and the window's client size too.
    HWND window{};
    RECT client{};
    const bool has_client=SUCCEEDED(hook->get_swap_chain()->GetHwnd(&window)) && window && GetClientRect(window,&client);
    spdlog::info("Sm2Probe: {}: swap chain scaling={} alpha={} window client {}x{}", reason, static_cast<int>(desc.Scaling),
        static_cast<int>(desc.AlphaMode), has_client?client.right-client.left:-1, has_client?client.bottom-client.top:-1);
}

void Sm2Probe::apply_desktop_window()
{
    const auto& hook = g_framework->get_d3d12_hook();
    auto* chain = hook ? hook->get_swap_chain() : nullptr;
    HWND window{};
    RECT client{};
    DXGI_SWAP_CHAIN_DESC1 desc{};
    BOOL fullscreen{};
    if (!chain || FAILED(chain->GetHwnd(&window)) || !window || !GetClientRect(window, &client) || FAILED(chain->GetDesc1(&desc)) ||
        FAILED(chain->GetFullscreenState(&fullscreen, nullptr))) {
        spdlog::info("DesktopWindow: unavailable (no swap chain window)");
        return;
    }
    const desktop_window::Size current{static_cast<uint32_t>(client.right - client.left), static_cast<uint32_t>(client.bottom - client.top)};
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const auto decision = desktop_window::decide(*m_desktop_window, current, {desc.Width, desc.Height}, fullscreen != FALSE,
        (style & WS_CAPTION) == WS_CAPTION, IsZoomed(window) != FALSE);
    bool requested = false;
    if (decision == desktop_window::Decision::apply) {
        const auto extended = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        RECT outer{0, 0, static_cast<LONG>(m_desktop_window->width), static_cast<LONG>(m_desktop_window->height)};
        // Asynchronous: the window thread may be waiting on this render thread.
        requested = AdjustWindowRectEx(&outer, style, FALSE, extended) &&
            SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
        ++m_desktop_window_attempts;
    }
    spdlog::info("DesktopWindow: decision={} requested_client={}x{} client={}x{} output={}x{} attempt={} set={}", desktop_window::name(decision),
        m_desktop_window->width, m_desktop_window->height, current.width, current.height, desc.Width, desc.Height, m_desktop_window_attempts, requested);
}

void Sm2Probe::on_device_reset()
{
    m_desktop_window_pending = true;
    m_handoff.reset();
    // The stereo producers and the pair retention stop now (their GPU-side leases are about to
    // go) and come back on a later present: the swap chain object, device and queue survive a
    // ResizeBuffers, so nothing they depend on has changed (docs/HEADSET_STANDBY_DEVICE_RESET_2026-10-22.md).
    const bool rearm=m_ordinary_pair_requested && m_ordinary_pair_ready;
    if(m_ordinary_pair_requested){m_ordinary_pair_ready=false;m_ordinary_pair_retention.invalidate();{NativeOrdinaryCamera::get().request_stop();NativeTrackedOrdinaryCamera::get().request_stop();}}
    m_ordinary_inline_ready=false;
    m_ordinary_private_stopped=true;
    if(!m_ordinary_mirror.reset())spdlog::error("OrdinaryMirror: reset refused pending GPU leases; control disabled");
    const bool inline_clean=m_ordinary_inline.reset();
    if(!inline_clean)spdlog::error("OrdinaryInlineCopy: reset refused pending GPU leases; control disabled");
    m_pip_completion.clear_target();
    m_render_targets.device_reset();
    const bool eyes_clean=m_eye_capture.reset(true);
    if (!eyes_clean) { spdlog::error("EyeCapture: reset could not drain pending copies"); }
    const bool runtime_clean=m_ordinary_runtime_images.reset(true);
    if(!runtime_clean)spdlog::error("OrdinaryRuntimeImages: reset could not drain pending copies");
    if(rearm){m_ordinary_rearm_pending=true;m_ordinary_rearm_attempts=0;m_ordinary_rearm_clean=inline_clean && eyes_clean && runtime_clean;}
    m_last_gpu_pair = 0;
    FrameSyncProbe::get().record(FrameSyncProbe::Kind::reset);
    StreamlineHistoryProbe::get().reset();
    StreamlineOptionsProbe::get().reset();
    EngineCameraProbe::get().clear_vr_eye();
    StreamlineProbe::get().clear_frame_cache();
    if (m_vr_session) { m_vr_session->enable(false); m_vr_session->enable(m_runtime_enabled); }
    log_swap_chain("device reset");
}

// Present thread, after on_device_reset: bring owned stereo back on the recreated swap chain.
// The retention resumes only on the same swap chain object with same-sized buffers, and the
// producer only between native jobs; both are retried for a bounded number of presents. The
// outcome line is read by the installer window (installer/app/sm2-launch.ps1).
void Sm2Probe::rearm_ordinary_headset()
{
    if(!m_ordinary_rearm_pending)return;
    constexpr unsigned ATTEMPTS=600; // about ten seconds of presents
    const auto give_up=[this](const char* reason){m_ordinary_rearm_pending=false;
        spdlog::error("OrdinaryHeadsetRearm: armed=false reason={} attempts={} rearms={}",reason,m_ordinary_rearm_attempts,m_ordinary_rearms);};
    if(!m_ordinary_rearm_clean){give_up("gpu_leases_pending");return;}
    ++m_ordinary_rearm_attempts;
    const auto& hook=g_framework->get_d3d12_hook();Microsoft::WRL::ComPtr<IDXGISwapChain3> swap;
    if(!hook || !hook->get_swap_chain() || FAILED(hook->get_swap_chain()->QueryInterface(IID_PPV_ARGS(&swap)))){if(m_ordinary_rearm_attempts>=ATTEMPTS)give_up("no_swap_chain");return;}
    if(!m_ordinary_pair_retention.restart(swap.Get(),hook->get_command_queue())){if(m_ordinary_rearm_attempts>=ATTEMPTS)give_up("retention");return;}
    // The reset poisoned the private DLSS eye (OrdinaryDlssContext::reset), so without this the first
    // pair is refused (packet refusal 9, 31 Oct 19:00) and the reset costs a cold restart at best.
    // The bloom chain is left to its own suspension: a rebuilt pool renews it as at a level load.
    if(!NativeOrdinaryPacket::get().restart() || !StreamlineHistoryProbe::get().restart_private()){if(m_ordinary_rearm_attempts>=ATTEMPTS)give_up("packet");return;}
    if(!NativeTrackedOrdinaryCamera::get().restart()){if(m_ordinary_rearm_attempts>=ATTEMPTS)give_up("producer_busy");return;}
    NativeOrdinaryCamera::get().restart(); // desktop-only diagnostic view; not armed in the player profile
    m_ordinary_pair_ready=true;m_ordinary_rearm_pending=false;++m_ordinary_rearms;m_ordinary_headset_failure_dumped=false;
    spdlog::info("OrdinaryHeadsetRearm: armed=true attempts={} rearms={} (warmup and owned readiness settle again before the next pair)",m_ordinary_rearm_attempts,m_ordinary_rearms);
}

// Present thread: the last rung of stereo recovery (OrdinaryColdRestart). Owned stereo stopped for
// any reason the suspension, renewal, chain restart and re-arm paths don't cover: once the stop has
// lasted OrdinaryColdRestart::SETTLE outside a loading screen, every part of the stack goes back to
// its startup state (pair retention, packet, private DLSS eye, bloom chain with a fresh owned
// pair, producer with its warmup and owned readiness). Each step refuses while it has work in
// flight; the whole sequence is retried on later presents. Over budget: stopped for the session.
void Sm2Probe::cold_restart_ordinary_headset()
{
    auto& producer=NativeTrackedOrdinaryCamera::get();
    // Without a configured pair retention (configuration failed, or a re-arm gave up) there is
    // nothing to restart onto; that stop is already logged where it happened.
    if(!m_ordinary_headset || !producer.armed() || !NativeOrdinaryBloom::get().installed() || (!m_ordinary_pair_ready && !m_ordinary_rearm_pending))return;
    const auto now=std::chrono::steady_clock::now();
    const bool loading=scene_of(EngineCinematicState::get().snapshot()).loading();
    const auto retention=m_ordinary_pair_retention.status();
    // Without DLSS a restart could only be refused again, so it spends no budget until DLSS is back.
    const auto action=m_cold_restart.observe(now,producer.stopped(),m_ordinary_rearm_pending,loading || OrdinaryDlssPresence::get().held());
    const auto describe=[&]{
        return fmt::format("bloom_stopped={} packet_stopped={} private_poisoned={} retention_stopped={} retention_refusal={} pair_ready={} generation={}",
            NativeOrdinaryBloom::get().stopped(),NativeOrdinaryPacket::get().stopped(),StreamlineHistoryProbe::get().private_poisoned(),retention.stopped,uint32_t(retention.refusal),
            m_ordinary_pair_ready.load(),NativeOrdinaryTextureCreation::get().generation());};
    if(action==OrdinaryColdRestart::Action::give_up){
        spdlog::error("OrdinaryHeadsetStopped: budget=exhausted cold_restarts={} in_window={} attempts={} {} (stopped for the session)",m_cold_restart.total(),m_cold_restart.in_window(),m_cold_restart.attempts(),describe());return;}
    if(action!=OrdinaryColdRestart::Action::attempt)return;
    if(retention.stopped){
        const auto& hook=g_framework->get_d3d12_hook();Microsoft::WRL::ComPtr<IDXGISwapChain3> swap;
        if(!hook || !hook->get_swap_chain() || FAILED(hook->get_swap_chain()->QueryInterface(IID_PPV_ARGS(&swap))))return;
        // As after a device reset: no copy of a refused pair may still be pending.
        if(!m_ordinary_runtime_images.reset() || !m_ordinary_inline.reset() || !m_ordinary_pair_retention.restart(swap.Get(),hook->get_command_queue()))return;
    }
    if(!NativeOrdinaryPacket::get().restart() || !StreamlineHistoryProbe::get().restart_private() || !NativeOrdinaryBloom::get().cold_restart() || !producer.restart())return;
    NativeOrdinaryCamera::get().restart(); // desktop-only diagnostic view; not armed in the player profile
    m_cold_restart.restarted(now);m_ordinary_headset_failure_dumped=false;
    spdlog::warn("OrdinaryHeadsetColdRestart: cold_restarts={} in_window={} budget={} {} (stack back to its startup state; warmup and owned readiness settle again)",
        m_cold_restart.total(),m_cold_restart.in_window(),OrdinaryColdRestart::BUDGET,describe());
}

// Present thread: says when the game stops or starts running DLSS (OrdinaryDlssPresence). Stereo
// can't run without it, so the headset shows the flat game until it is back. The lines are read by
// the installer window (installer/app/sm2-launch.ps1).
void Sm2Probe::report_ordinary_dlss()
{
    const auto missing=OrdinaryDlssPresence::get().take_change();if(!missing)return;
    if(*missing)spdlog::warn("OrdinaryDlssHold: held=true jobs={} submitted={} (the game is running neither DLSS nor TAA, which stereo needs: flat panel until one is on again)",OrdinaryDlssPresence::get().jobs(),m_ordinary_submitted);
    else spdlog::info("OrdinaryDlssHold: held=false submitted={} (DLSS or TAA is back: stereo resumes once owned readiness settles)",m_ordinary_submitted);
}

// Present thread: the readiness-stall guard (OrdinaryStereoRecovery.h ReadinessStall). In plain play
// with the producer running, owned readiness refusing every job for a player-state reason for
// ReadinessStall::LIMIT makes the observation probes forget their rows and learn the level afresh.
void Sm2Probe::guard_readiness_stall()
{
    auto& producer=NativeTrackedOrdinaryCamera::get();if(!m_ordinary_headset || !producer.armed())return;
    const auto now=std::chrono::steady_clock::now();
    const auto scene=scene_of(EngineCinematicState::get().snapshot());
    const auto readiness=producer.readiness();
    const bool expect_player=scene.mode==scene_mode::Mode::gameplay || scene.mode==scene_mode::Mode::hub;
    const auto action=m_readiness_stall.observe(now,expect_player,!producer.stopped() && !producer.suspended(),ReadinessStall::player_reason(readiness.reason),readiness.ready_jobs);
    if(action==ReadinessStall::Action::none)return;
    const auto bodies=EngineBodyProbe::get().snapshot();
    const auto line=fmt::format("StereoStall: action={} reason={} scene={} stalled_ms={} tries={} total={} body_trackers={} bodies={} overflow={}",
        ReadinessStall::name(action),OwnedStereoReadiness::name(readiness.reason),scene_mode::name(scene.mode),m_readiness_stall.stalled(now).count(),
        m_readiness_stall.tries(),m_readiness_stall.total(),bodies.trackers.size(),bodies.bodies.size(),bodies.overflow);
    // The hub flag (0x800) is also the main menu and loads, which have no player: only plain play is an error.
    if(action==ReadinessStall::Action::give_up){
        if(scene.mode==scene_mode::Mode::gameplay)spdlog::error("{} (readiness still blocked after {} rediscoveries; headset stays on the flat panel)",line,ReadinessStall::TRIES);
        else spdlog::info("{} (no player in this hub scene: the main menu or a load)",line);
        return;}
    spdlog::warn("{} (stereo waiting on the player's state in plain play: the probes forget their rows and learn the level afresh)",line);
    EngineBodyProbe::get().forget();EngineWeaponProbe::get().forget();
}

// Present thread: one line per scene-mode change and per mask first seen this session
// (scripts/run_report.py checks the masks against config/known_run_states.json). A mask no rule
// explains, or one with a bit no rule reads, logs at warn. Bounded: after 2000 changes, every 100th.
void Sm2Probe::log_scene(const scene_mode::Scene& scene)
{
    const auto event=m_scene_watch.update(scene);if(!event)return;
    if(!event->first_mask && m_scene_watch.changes()>2000 && m_scene_watch.changes()%100)return;
    const auto line=fmt::format("SceneMode: mode={} mask={:#x} previous={} unknown_bits={:#x} first_mask={} changes={} masks={}",
        scene_mode::name(event->mode),event->mask,scene_mode::name(event->previous),scene.unknown_bits,event->first_mask,m_scene_watch.changes(),m_scene_watch.masks());
    if(scene.novel())spdlog::warn("{}",line);else spdlog::info("{}",line);
}

// Present thread, with the 5 s status lines: the per-eye call census (NativeCallCensus.h). The
// summary once a minute; a warning whenever the set of doubled or missing hooks changes.
void Sm2Probe::log_call_census()
{
    const auto census=NativeCallCensus::get().summary();if(!census.mono && !census.stereo)return;
    const auto flags=census.doubled+"|"+census.missing;
    if(flags!=m_call_census_flags){m_call_census_flags=flags;if(flags!="|"){spdlog::warn("CallCensus: {} (a per-frame hook runs per eye, or a per-view hook skipped an eye: docs/SECOND_EYE_SIDE_EFFECTS.md)",census.line);return;}}
    if(m_call_census_lines++%12==0)spdlog::info("CallCensus: {}",census.line);
}

namespace {
// Present thread only: logs guard install decisions once and each captured trip with its bytes.
void log_tracker_guard()
{
    auto& guard=NativeTrackerHandleGuard::get();
    static std::array<bool,2> reported{};
    const auto stats=guard.stats();
    for(size_t i=0;i<2;++i)if(!reported[i] && (stats.installed[i] || stats.refused[i])){reported[i]=true;
        spdlog::info("TrackerHandleGuard: image={} installed={} refused={} enabled={} (getter falls back to the native no-geometry branch, two-handle validity reports invalid, only when the native read would fault)",i?"dll":"exe",stats.installed[i],stats.refused[i],guard.enabled());}
    std::array<NativeTrackerHandleGuard::Trip,NativeTrackerHandleGuard::TRIP_SLOTS> trips{};
    const auto count=guard.take_trips(trips);
    for(size_t i=0;i<count;++i){const auto& t=trips[i];
        spdlog::warn("TrackerHandleGuardTrip: image={} kind={} thread={} object={:x} vtable={:x} value={:x} bytes_valid={} bytes={}",t.image==NativeTrackerHandleGuard::Image::dll?"dll":"exe",t.kind==NativeTrackerHandleGuard::Kind::pair?"pair_validity":"getter",t.thread,t.object,t.vtable,t.value,t.bytes_valid,NativeTrackerHandleGuard::hex(t.bytes));}
}
}

void Sm2Probe::on_present()
{
    HangWatchdog::get().present();
    NativeObjectConstantGetterProbe::get().poll();
    if(m_ordinary_perf && std::chrono::steady_clock::now()>=m_ordinary_perf_poll){
        m_ordinary_perf_poll=std::chrono::steady_clock::now()+std::chrono::seconds(1);
        std::ifstream input{m_ordinary_perf_control};std::string mode;std::getline(input,mode);if(!mode.empty() && mode.back()=='\r')mode.pop_back();
        NativeOrdinaryCamera::get().performance_enabled(NativeOrdinaryCamera::performance_control(mode));
        const auto stats=NativeOrdinaryCamera::get().statistics();
        spdlog::info("OrdinaryPerf: mode={} appended={} attempts={} frame={} camera_tid={} present_tid={} stopped={} refusals={}",mode,stats.appended,stats.attempts,stats.last_render_id,stats.thread,GetCurrentThreadId(),stats.stopped,fmt::join(stats.refusals,","));
    }
    auto& performance=PerformanceTrace::get();
    m_handoff.poll(PerformanceTrace::now_ms());
    performance.poll_control(PerformanceTrace::now_ms());
    // Profiling diagnostic: every queue submission, fence signal/wait and GPU completion (GpuQueueTimeline.h).
    if(!m_queue_timeline_tried && performance.enabled()){
        wchar_t timeline[8]{};const auto& hook=g_framework->get_d3d12_hook();
        if(GetEnvironmentVariableW(L"SM2VR_PROFILE_QUEUE_TIMELINE",timeline,8)!=1 || timeline[0]!=L'1')m_queue_timeline_tried=true;
        else if(hook && hook->get_device() && hook->get_command_queue()){m_queue_timeline_tried=true;
            spdlog::info("GpuQueueTimeline: installed={} execute_rows={}",GpuQueueTimeline::get().install(hook->get_device(),hook->get_command_queue()),GpuQueueTimeline::get().execute_hooked());}
    }
    if(m_stereo_hold.configured()){
        const auto now=PerformanceTrace::now_ms();const auto due=now>=m_stereo_hold_next;
        if(const auto changed=m_stereo_hold.poll(now)){NativeTrackedOrdinaryCamera::get().hold(*changed);spdlog::info("StereoHold: mono={} submitted={}",*changed,m_ordinary_submitted);}
        if(due){m_stereo_hold_next=now+1000;performance.record("stereo_hold",now,0,m_stereo_hold.held()?1:0);}
    }
    for(const auto& line:NativeOrdinaryLightVisibility::get().take_list_lines())spdlog::info("{}",line);
    if(m_ordinary_headset && !m_stereo_lights_path.empty()){
        const auto now=PerformanceTrace::now_ms();
        if(now>=m_stereo_lights_next && (m_stereo_lights_next=now+1000,m_stereo_lights_stamp.changed(m_stereo_lights_path))){std::ifstream file{m_stereo_lights_path};std::string line;
            if(std::getline(file,line) && line.size()<16)if(const auto parsed=stereo_light_list::parse(line))if(*parsed!=NativeOrdinaryLightVisibility::get().stereo_mode()){
                NativeOrdinaryLightVisibility::get().set_stereo_mode(*parsed);spdlog::info("StereoLights: mode={}",stereo_light_list::name(*parsed));}}
    }
    if(m_cap_sort.configured() || m_ordinary_headset){
        // Code byte write on the Present thread, between frames (the light job runs on the render thread).
        // The diagnostic -LightCapSortControl file wins; otherwise the stereo light mode decides.
        const auto now=PerformanceTrace::now_ms();std::optional<light_cap_sort::Mode> changed;
        if(m_cap_sort.configured())changed=m_cap_sort.poll(now);
        else{const auto want=stereo_light_list::cap_by_distance(NativeOrdinaryLightVisibility::get().stereo_mode())?light_cap_sort::Mode::distance:light_cap_sort::Mode::native;
            if(!m_cap_want_set || want!=m_cap_want){m_cap_want=want;m_cap_want_set=true;changed=want;}}
        const auto cap_mode=m_cap_sort.configured()?m_cap_sort.mode():m_cap_want;
        if(changed || now>=m_cap_sort_next){
            const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
            const auto rva=light_cap_sort::site_rva(nt->FileHeader.TimeDateStamp);const auto at=rva?base+rva:0;
            const auto read=[](uintptr_t a,light_cap_sort::Bytes& b){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(a),b.data(),b.size(),&n) && n==b.size();};
            const auto write=[](uintptr_t a,uint8_t v){DWORD old{};if(!VirtualProtect(reinterpret_cast<void*>(a),1,PAGE_EXECUTE_READWRITE,&old))return false;
                *reinterpret_cast<volatile uint8_t*>(a)=v;DWORD ignored{};VirtualProtect(reinterpret_cast<void*>(a),1,old,&ignored);FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(a),1);return true;};
            const auto result=light_cap_sort::apply(cap_mode,at,read,write);
            // The 5 s re-check logs only when it did something or found something wrong.
            light_cap_sort::Bytes now_bytes{};const auto readable=(changed || result!=light_cap_sort::Result::unchanged) && at && read(at,now_bytes);
            if(changed || result!=light_cap_sort::Result::unchanged)spdlog::info("LightCapSort: mode={} result={} immediate={} changed={}",cap_mode==light_cap_sort::Mode::distance?"distance":"native",light_cap_sort::name(result),readable?int(now_bytes[light_cap_sort::IMMEDIATE]):-1,changed.has_value());
            if(now>=m_cap_sort_next)m_cap_sort_next=now+5000;
        }
    }
    if(m_far_light.configured()){
        const auto now=PerformanceTrace::now_ms();const auto changed=m_far_light.poll(now);
        if(changed || now>=m_far_light_next){
            const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
            const auto rva=far_light_cache::cache_rva(nt->FileHeader.TimeDateStamp);
            const auto at=rva?base+rva+far_light_cache::DISABLE_OFFSET:0;
            const auto read=[](uintptr_t a,uint8_t& v){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(a),&v,1,&n) && n==1;};
            const auto result=m_far_light_switch.apply(m_far_light.mode(),at,read,[](uintptr_t a,uint8_t v){SIZE_T n{};return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(a),&v,1,&n) && n==1;});
            uint8_t value{9};if(at)read(at,value);
            spdlog::info("FarLightCache: mode={} result={} disable_byte={} changed={}",m_far_light.mode()==far_light_cache::Mode::off?"off":"native",far_light_cache::name(result),int(value),changed.has_value());
            if(now>=m_far_light_next)m_far_light_next=now+5000;
        }
    }
    if(m_async_lighting.configured()){
        // Polled here, between frames on the thread that records them, so a Scene never sees the
        // switches change between its fence signal and wait.
        const auto now=PerformanceTrace::now_ms();
        const auto changed=m_async_lighting.poll(now);
        if(changed || now>=m_async_lighting_next){
            const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));namespace al=native_async_lighting;
            const al::Read read=[](uintptr_t at,uint8_t& v){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),&v,1,&n) && n==1;};
            const al::Write write=[](uintptr_t at,uint8_t v){SIZE_T n{};return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),&v,1,&n) && n==1;};
            const auto result=m_async_switches.apply(m_async_lighting.mode(),base,al::rva(al::TILED_SWITCH),al::rva(al::ASYNC_SWITCH),read,write);
            uint8_t tiled{9},async{9};if(al::rva(al::TILED_SWITCH))read(base+al::rva(al::TILED_SWITCH),tiled);if(al::rva(al::ASYNC_SWITCH))read(base+al::rva(al::ASYNC_SWITCH),async);
            // The async gate 0x209a9e0 also needs device([0x4ff0c08])+0x80 bit 0 set and byte 0x4ff0be0 == 0
            // (build/flicker_re/NOTES.md); 0x4ff0be0 sits between two table entries that moved by the same amount.
            uint64_t device{};uint32_t device_flags{0xffffffff};uint8_t gate{9};
            if(const auto d=game_build::rva(0x4ff0c08)){SIZE_T n{};if(ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(base+d),&device,8,&n) && device)ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(device+0x80),&device_flags,4,&n);}
            if(const auto g=game_build::rva(0x4ff0bd0))read(base+g+0x10,gate);
            if(changed || result!=al::Result::unchanged || now>=m_async_lighting_next)
                spdlog::info("AsyncLighting: mode={} result={} tiled={} async={} changed={} device_flag0={} gate_byte={}",al::name(m_async_lighting.mode()),al::name(result),tiled,async,changed.has_value(),
                    device_flags==0xffffffff?-1:int(device_flags&1),int(gate));
            if(now>=m_async_lighting_next)m_async_lighting_next=now+5000;
        }
    }
    PerformanceTrace::Scope present_scope{performance,"present_hook"};
    performance.thread_role(PerformanceTrace::Role::present);
    const auto performance_mode=performance.mode();
    present_scope.mode=performance_mode;
    StreamlineProbe::get().poll();
    if(m_ordinary_second_requested){StreamlineHistoryProbe::get().poll();StreamlineOptionsProbe::get().poll();}
    FrameSyncProbe::get().poll();
    EngineCameraProbe::get().install_in_game(GetModuleHandleW(nullptr));
    EngineProjectionHook::get().install_in_game(GetModuleHandleW(nullptr));
    EngineWorldMarkers::get().install_in_game(GetModuleHandleW(nullptr));
    capture_pip_experiment();
    capture_ordinary_observation();
    // After the fault pod poll above: the pod checks the native getter prologues before the guard hooks them.
    NativeTrackerHandleGuard::get().poll();
    log_tracker_guard();
    mirror_ordinary_output();
    capture_ordinary_inline();
    const auto hud_poll_now=std::chrono::steady_clock::now();
    if(hud_poll_now>=m_next_hud_settings_poll && !m_hud_settings_path.empty()){
        m_next_hud_settings_poll=hud_poll_now+std::chrono::seconds(1);
        if(m_hud_stamps[0].changed(m_hud_settings_path))if(const auto settings=VrCompactHud::Settings::load(m_hud_settings_path))VrCompactHud::get().set_settings(*settings);
        if(m_hud_stamps[1].changed(m_native_hud_settings_path))if(const auto settings=VrNativeHud::load(m_native_hud_settings_path))m_native_hud_crop=*settings;
        if(m_hud_stamps[2].changed(m_native_prompt_settings_path))if(const auto settings=VrNativePrompt::load(m_native_prompt_settings_path))m_native_prompt=*settings;
        if(m_hud_stamps[3].changed(m_native_hud_corners_path))if(const auto settings=VrNativeHudCorners::load(m_native_hud_corners_path))m_native_hud_corners=*settings;
        if(m_hud_stamps[4].changed(m_aim_dot_path))if(const auto settings=VrAimDot::load(m_aim_dot_path))m_aim_dot=*settings;
        if(m_hud_stamps[5].changed(m_hud_reticle_path))if(const auto settings=VrHudReticle::load(m_hud_reticle_path))m_hud_reticle=*settings;
        if(m_vr_session && !m_viewer_play_path.empty() && m_viewer_play_stamp.changed(m_viewer_play_path))if(const auto play=vr_viewer::Play::load(m_viewer_play_path);play && *play!=m_vr_session->viewer_play()){
            m_vr_session->set_viewer_play(*play);spdlog::info("VrViewerPlay: reloaded mode={} still_speed_metres_per_second={} still_seconds={} clip_seconds={} limit_seconds={}",vr_viewer::Play::name(play->mode()),
                play->values.values[vr_viewer::Play::STILL_SPEED],play->values.values[vr_viewer::Play::STILL_SECONDS],play->values.values[vr_viewer::Play::CLIP_SECONDS],play->values.values[vr_viewer::Play::LIMIT_SECONDS]);}
    }
    capture_ordinary_pair();
    if(m_pip_final_lens) {
        const auto lens=NativePipLens::get().snapshot();
        if(lens.skipped!=m_pip_logged_lens_skips) {
            if(!lens.skip_events.empty() && lens.skip_events.front().serial>m_pip_logged_lens_skips+1)
                spdlog::warn("PipLensSkipOverflow: missing={}",lens.skip_events.front().serial-m_pip_logged_lens_skips-1);
            for(const auto& event:lens.skip_events) if(event.serial>m_pip_logged_lens_skips)
                spdlog::warn("PipLensSkip: serial={} reason={} slot={} record={} thread={} world=[{}] active_ownership_verified=false",
                    event.serial,event.reason,event.slot,event.record,event.thread,fmt::join(event.world,","));
            m_pip_logged_lens_skips=lens.skipped;
        }
        if(lens.unexpected_errors!=m_pip_logged_lens_errors) {
            spdlog::warn("PipLensUnexpectedErrors: total={}",lens.unexpected_errors);m_pip_logged_lens_errors=lens.unexpected_errors;
        }
        if(lens.rejected!=m_pip_logged_lens_rejections) {
            if(!lens.rejection_events.empty() && lens.rejection_events.front().serial>m_pip_logged_lens_rejections+1)
                spdlog::warn("PipLensRejectionOverflow: missing={} total={}",lens.rejection_events.front().serial-m_pip_logged_lens_rejections-1,lens.rejected);
            for(const auto& event:lens.rejection_events) if(event.serial>m_pip_logged_lens_rejections) {
                spdlog::warn("PipLensRejection: serial={} reason={} phase={} updates={} slot={} record={} thread={} candidates={} current_error={} nearest_error={}",
                    event.serial,event.reason,event.phase,event.updates,event.slot,event.record,event.thread,event.candidates,event.current_pose_error,event.nearest_pose_error);
                spdlog::warn("PipLensRejectedPose: serial={} world=[{}] target=[{}]",event.serial,fmt::join(event.world,","),fmt::join(event.target_world,","));
                spdlog::warn("PipLensHistoryClock: serial={} current_eligible={} lock_wait_ms={} retained={}",event.serial,event.current_eligible,event.lock_wait_ms,event.history.size());
                for(const auto& pose:event.history) {
                    float error{};for(size_t i=0;i<16;++i) error=std::max(error,std::abs(event.world[i]-pose.world[i]));
                    spdlog::warn("PipLensHistory: serial={} render_id={} updates={} token={} token_valid={} age_ms={} eligible={} matrix_error={}",
                        event.serial,pose.render_id,pose.updates,pose.token.value_or(0),pose.token.has_value(),pose.age_ms,pose.eligible,error);
                }
            }
            for(size_t reason=1;reason<lens.reason_counts.size();++reason) if(lens.reason_counts[reason])
                spdlog::warn("PipLensRejectionCount: reason={} count={} total={}",reason,lens.reason_counts[reason],lens.rejected);
            m_pip_logged_lens_rejections=lens.rejected;
        }
    }
    if (m_present_count == 0) {
        if(m_screen_ui_requested) EngineScreenUi::get().install_in_game(GetModuleHandleW(nullptr));
        char markers[32]{};
        EngineWorldMarkers::get().enable(!GetEnvironmentVariableA("SM2VR_WORLD_MARKERS",markers,sizeof(markers)) || std::atoi(markers)!=0);
        char count[32]{}, interval[32]{}, gpu[32]{};
        if (GetEnvironmentVariableA("SM2VR_FRAME_TRACE", count, sizeof(count))) {
            m_frame_trace_count = static_cast<unsigned>(std::clamp(std::atoi(count), 0, 16384));
            FrameSyncProbe::get().start_trace(m_frame_trace_count);
        }
        if (GetEnvironmentVariableA("SM2VR_FRAME_TRACE_INTERVAL", interval, sizeof(interval))) { m_frame_trace_interval = std::clamp(std::atof(interval),5.0,600.0); }
        m_gpu_capture_enabled = GetEnvironmentVariableA("SM2VR_GPU_CAPTURE",gpu,sizeof(gpu)) && std::atoi(gpu) == 1;
        char synthetic[32]{};
        char openvr[32]{},openxr[32]{};
        const bool vr_requested=GetEnvironmentVariableA("SM2VR_OPENVR",openvr,sizeof(openvr)) && std::atoi(openvr)==1;
        m_openxr_selected=GetEnvironmentVariableA("SM2VR_OPENXR",openxr,sizeof(openxr)) && std::atoi(openxr)==1;
        char controllers[32]{}, snap[32]{};
        const bool controller_mode = GetEnvironmentVariableA("SM2VR_CONTROLLERS",controllers,sizeof(controllers)) && std::atoi(controllers) == 1;
        const bool synthetic_enabled = GetEnvironmentVariableA("SM2VR_SYNTHETIC_EYES",synthetic,sizeof(synthetic)) && std::atoi(synthetic) == 1;
        const auto choice=vr_runtime_choice(vr_requested,m_openxr_selected,synthetic_enabled);
        m_runtime_enabled=choice==VrRuntimeChoice::openvr || choice==VrRuntimeChoice::openxr;
        if(choice==VrRuntimeChoice::invalid) spdlog::error("VrSession: conflicting/synthetic runtime selection; runtime initialization disabled");
        if (m_runtime_enabled) {
            m_controller_input.configure(m_runtime_enabled,controller_mode,synthetic_enabled);
            if (GetEnvironmentVariableA("SM2VR_SNAP_DEGREES",snap,sizeof(snap))) { m_controller_input.set_snap_degrees(float(std::atof(snap))); }
            {   // How input re-arms after a stall or a loss (docs/CONTROL_CUTOUT_2026-10-26.md).
                char stall_ms[16]{},neutral[8]{};
                const bool has_stall=GetEnvironmentVariableA("SM2VR_INPUT_STALL_RESUME_MS",stall_ms,sizeof(stall_ms))>0;
                const bool has_neutral=GetEnvironmentVariableA("SM2VR_INPUT_NEUTRAL_REARM",neutral,sizeof(neutral))>0;
                if(const auto rearm=VrControllerInput::Rearm::parse(has_stall?stall_ms:nullptr,has_neutral?neutral:nullptr)) m_controller_input.set_rearm(*rearm);
                else spdlog::warn("VrControllers: SM2VR_INPUT_STALL_RESUME_MS='{}' is not 0-{} ms; keeping the default",stall_ms,VrControllerInput::Rearm::MAX_STALL_RESUME.count());
                const auto rearm=m_controller_input.rearm();
                spdlog::info("VrControllers: rearm stall_resume_ms={} rule={}",rearm.stall_resume.count(),rearm.whole_pad?"whole_pad (SM2VR_INPUT_NEUTRAL_REARM=1)":"per_control");
            }
            m_gpu_capture_enabled = true;
            std::unique_ptr<VrBackend> backend;
            if(m_openxr_selected) backend=std::make_unique<OpenXrBackend>();
            else backend=std::make_unique<OpenVrBackend>();
            wchar_t test_directory[32768]{};const auto test_length=GetEnvironmentVariableW(L"SM2VR_TEST_DRIVER",test_directory,32768);
            if(test_length && test_length<32768 && controller_mode && m_ordinary_headset && std::filesystem::path{test_directory}.is_absolute()){
                char head_test[8]{};const bool allow_head=GetEnvironmentVariableA("SM2VR_TEST_HEAD",head_test,sizeof(head_test))==1 && head_test[0]=='1';
                backend=std::make_unique<VrTestBackend>(std::move(backend),test_directory,allow_head,[]{DWORD pid{};GetWindowThreadProcessId(GetForegroundWindow(),&pid);return pid==GetCurrentProcessId();});
                spdlog::warn("VrTestDriver: exclusive scripted controllers enabled; scripted_head={}",allow_head);
            }
            m_vr_session = std::make_unique<VrSession>(std::move(backend));
            if(m_ordinary_headset)m_vr_session->set_render_path(VrSession::RenderPath::ordinary);
            {   // docs/OPENXR_SUBMIT_THREAD.md: runtime frame calls off the render/Present thread (OpenXR default;
                // SM2VR_OPENXR_NO_SUBMIT_THREAD=1 keeps them on it).
                char no_thread[8]{};
                const bool opted_out=GetEnvironmentVariableA("SM2VR_OPENXR_NO_SUBMIT_THREAD",no_thread,sizeof(no_thread))==1 && no_thread[0]=='1';
                const bool applied=m_openxr_selected && !opted_out;
                if(applied)m_vr_session->set_submit_thread(true,[](const std::function<void()>& step){GpuCompletionProbe::internal(step);});
                spdlog::info("VrSession: submit_thread applied={} opted_out={}",applied,opted_out);
            }
            m_view_shortcuts_enabled=true;
            spdlog::info("VrBackend: selected={}",m_openxr_selected?"OpenXR":"OpenVR");
            m_vr_session->enable(true);
            char swing_enabled[32]{};
            m_vr_session->enable_swing_melee(GetEnvironmentVariableA("SM2VR_SWING_MELEE",swing_enabled,sizeof(swing_enabled)) && std::atoi(swing_enabled)==1);
            m_vr_session->set_swing_observer([](auto now){EngineMeleeSweep::get().swing(now);});
            char holsters[32]{};
            m_vr_session->enable_holsters(GetEnvironmentVariableA("SM2VR_HOLSTERS",holsters,sizeof(holsters)) && std::atoi(holsters)==1);
            char touch_gate[32]{};
            m_vr_session->require_trigger_lifted(GetEnvironmentVariableA("SM2VR_SWING_TOUCH_GATE",touch_gate,sizeof(touch_gate)) && std::atoi(touch_gate)==1);
            wchar_t holster_path[32768]{};
            const auto holster_path_length=GetEnvironmentVariableW(L"SM2VR_HOLSTER_CONFIG",holster_path,32768);
            if(holster_path_length && holster_path_length<32768 && std::filesystem::path{holster_path}.is_absolute()) {
                m_holster_settings_path=holster_path;
                const auto loaded=VrHolster::Settings::load(m_holster_settings_path);
                spdlog::info("VrHolster: enabled={} config_loaded={}",m_vr_session->holsters_enabled(),loaded && m_vr_session->set_holster_settings(*loaded));
            } else spdlog::info("VrHolster: enabled={} config_loaded=false",m_vr_session->holsters_enabled());
            setup_gestures_and_trace();
            wchar_t swing_path[32768]{};
            const auto swing_path_length=GetEnvironmentVariableW(L"SM2VR_SWING_CONFIG",swing_path,32768);
            if(swing_path_length && swing_path_length<32768 && std::filesystem::path{swing_path}.is_absolute()) m_swing_settings_path=swing_path;
            char stereo[32]{}, scale[32]{};
            m_vr_stereo = GetEnvironmentVariableA("SM2VR_STEREO",stereo,sizeof(stereo)) && std::atoi(stereo) == 1;
            m_vr_session->set_stereo(m_vr_stereo);
            wchar_t eye_source_path[32768]{};const auto eye_source_length=GetEnvironmentVariableW(L"SM2VR_EYE_SOURCE_CONTROL",eye_source_path,32768);
            if(eye_source_length && eye_source_length<32768 && std::filesystem::path{eye_source_path}.is_absolute()){
                m_vr_session->set_eye_source_control(eye_source_path);
                spdlog::warn("VrEyeSourceDiagnostic: control={} (stereo diagnostic; submitted eyes may be deliberately wrong)",std::filesystem::path{eye_source_path}.string());
            }
            if (GetEnvironmentVariableA("SM2VR_UNITS_PER_METRE",scale,sizeof(scale))) {
                const auto value = float(std::atof(scale));
                if (m_vr_session->set_scale(value)) { m_vr_calibration.units_per_metre = value; }
            }
            wchar_t calibration_path[32768]{};
            const auto path_length = GetEnvironmentVariableW(L"SM2VR_CALIBRATION",calibration_path,32768);
            if (path_length > 0 && path_length < 32768) {
                const std::filesystem::path path{calibration_path};
                if (path.is_absolute()) { m_calibration_path = path; }
                else { spdlog::error("VrCalibration: configuration path must be absolute"); }
            }
            // Load life-size calibration before validating the diorama scale multiplier.
            poll_calibration();
            if(!m_calibration_path.empty()) {
                m_support_settings_path=m_calibration_path.parent_path()/"index_support.calibration";
                if(const auto settings=VrSupportGrip::Settings::load(m_support_settings_path)) m_vr_session->set_support_settings(*settings);
                m_hud_settings_path=m_calibration_path.parent_path()/"index_hud.calibration";
                if(const auto settings=VrCompactHud::Settings::load(m_hud_settings_path)) VrCompactHud::get().set_settings(*settings);
                m_native_hud_settings_path=m_calibration_path.parent_path()/"index_native_hud.calibration";
                if(const auto settings=VrNativeHud::load(m_native_hud_settings_path))m_native_hud_crop=*settings;
                m_native_prompt_settings_path=m_calibration_path.parent_path()/"index_native_prompt.calibration";
                if(const auto settings=VrNativePrompt::load(m_native_prompt_settings_path))m_native_prompt=*settings;
                m_native_hud_corners_path=m_calibration_path.parent_path()/"index_native_hud_corners.calibration";
                if(const auto settings=VrNativeHudCorners::load(m_native_hud_corners_path))m_native_hud_corners=*settings;
                m_viewer_play_path=m_calibration_path.parent_path()/"index_viewer_play.calibration";
                if(const auto settings=vr_viewer::Play::load(m_viewer_play_path)) m_vr_session->set_viewer_play(*settings);
                else spdlog::warn("VrViewerPlay: {} missing or invalid; using the defaults",m_viewer_play_path.string());
                {const auto play=m_vr_session->viewer_play();spdlog::info("VrViewerPlay: mode={} still_speed_metres_per_second={} still_seconds={} clip_seconds={} limit_seconds={}",vr_viewer::Play::name(play.mode()),
                    play.values.values[vr_viewer::Play::STILL_SPEED],play.values.values[vr_viewer::Play::STILL_SECONDS],play.values.values[vr_viewer::Play::CLIP_SECONDS],play.values.values[vr_viewer::Play::LIMIT_SECONDS]);}
                m_aim_dot_path=m_calibration_path.parent_path()/"index_aim_dot.calibration";
                if(const auto settings=VrAimDot::load(m_aim_dot_path))m_aim_dot=*settings;
                spdlog::info("AimDot: enabled={} size_degrees={} opacity={} first_person={}",m_aim_dot.enabled(),m_aim_dot.values.values[VrAimDot::SIZE],
                    m_aim_dot.values.values[VrAimDot::OPACITY],m_aim_dot.values.values[VrAimDot::FIRST_PERSON]);
                m_hud_reticle_path=m_calibration_path.parent_path()/"index_hud_reticle.calibration";
                if(const auto settings=VrHudReticle::load(m_hud_reticle_path))m_hud_reticle=*settings;
                spdlog::info("HudReticle: enabled={} height={} width={} centre=[{},{}] first_person={}",m_hud_reticle.enabled(),m_hud_reticle.values.values[VrHudReticle::HEIGHT],m_hud_reticle.values.values[VrHudReticle::WIDTH],
                    m_hud_reticle.values.values[VrHudReticle::CENTRE_X],m_hud_reticle.values.values[VrHudReticle::CENTRE_Y],m_hud_reticle.values.values[VrHudReticle::FIRST_PERSON]);
                spdlog::info("NativeHudCorners: enabled={} panel width={} centre_y={} distance={}",m_native_hud_corners.enabled(),m_native_hud_corners.values.values[VrNativeHudCorners::WIDTH],
                    m_native_hud_corners.values.values[VrNativeHudCorners::CENTRE_Y],m_native_hud_corners.values.values[VrNativeHudCorners::DISTANCE]);
                spdlog::info("NativePrompt: enabled={} crop=[{},{},{},{}] distance={} native_fov={}",m_native_prompt.enabled(),m_native_prompt.values.values[1],m_native_prompt.values.values[2],
                    m_native_prompt.values.values[3],m_native_prompt.values.values[4],m_native_prompt.values.values[5],m_native_prompt.values.values[6]);
            }
            wchar_t menu_path[32768]{};
            const auto menu_length=GetEnvironmentVariableW(L"SM2VR_MENU_CONFIG",menu_path,32768);
            if(menu_length && menu_length<32768 && std::filesystem::path{menu_path}.is_absolute()) m_menu_settings_path=menu_path;
            else if(!menu_length && !m_calibration_path.empty()) m_menu_settings_path=m_calibration_path.parent_path()/"index_menu.calibration";
            poll_menu_settings();
            if(!apply_view_settings(m_view_settings)) {m_view_settings={};apply_view_settings(m_view_settings);}
            if(!m_ordinary_headset)EngineCameraProbe::get().set_eye_provider([this](const FrameSyncClock::Frame& frame) { return m_vr_session->shared_eye(frame); });
            else if(m_ordinary_pair_ready){
                const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));auto& producer=NativeTrackedOrdinaryCamera::get();char warmup[16]{};GetEnvironmentVariableA("SM2VR_STEREO_WARMUP_JOBS",warmup,sizeof(warmup));m_stereo_warmup=NativeTrackedOrdinaryCamera::warmup_from(warmup);producer.warmup(m_stereo_warmup);
                {   // Slot 1 is otherwise HZB-culled as a shadow camera (missing props in the right eye).
                    char keep[8]{};const bool keep_native=GetEnvironmentVariableA("SM2VR_NATIVE_HZB",keep,sizeof(keep)) && std::atoi(keep)==1;
                    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
                    auto* flag=reinterpret_cast<uint8_t*>(base+game_build::rva(native_hzb_culling::SWITCH_RVA));
                    const auto result=native_hzb_culling::apply(nt->FileHeader.TimeDateStamp,keep_native,
                        [flag](uint8_t& v){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),flag,&v,1,&n) && n==1;},
                        [flag](uint8_t v){SIZE_T n{};return WriteProcessMemory(GetCurrentProcess(),flag,&v,1,&n) && n==1;});
                    spdlog::info("NativeHzbCulling: {} (SM2VR_NATIVE_HZB=1 keeps native)",native_hzb_culling::name(result));
                }
                char second_id[8]{};if(GetEnvironmentVariableA("SM2VR_SECOND_CAMERA_ID",second_id,sizeof(second_id))){producer.set_second_camera_id(std::atoi(second_id));
                    spdlog::warn("SecondCameraId: diagnostic override={} (slot-1 native camera id byte)",producer.second_camera_id());}
                const auto tracking_missing=std::make_shared<std::atomic<bool>>(false);
                const auto provider_failures=std::make_shared<std::array<std::atomic<unsigned>,2>>();
                const bool owned_controls=m_controller_input.motion_preference();
                const bool armed=producer.arm([base]{uintptr_t renderer{};SIZE_T bytes{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(base+game_build::rva(0x4e0bea0)),&renderer,sizeof(renderer),&bytes) && bytes==sizeof(renderer)?renderer:uintptr_t{};},
                    []{return FrameSyncProbe::get().render_camera_frame();},[this,tracking_missing,provider_failures](std::span<const uint8_t> source,const auto& frame)->std::optional<ordinary_tracked_camera::Pair>{
                        tracking_missing->store(false);
                        const auto now=std::chrono::steady_clock::now();const auto tracking=m_vr_session->ordinary_frame(frame,now);if(!tracking){tracking_missing->store(true);
                            if((*provider_failures)[0].fetch_add(1)<12)spdlog::warn("OrdinaryProviderFailure: job={} stage=tracking_none tracking_missing=true elapsed_ms={}",frame.render_id,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-now).count());return {};}
                        vr_camera::Matrix game{};std::memcpy(game.data(),source.data(),64);const auto anchor=camera_anchor(game,tracking->eyes[0]);
                        const auto floor=EngineBodyFollowing::get().body_position(now);
                        m_vr_session->note_first_person_anchor(anchor,tracking->eyes[0],floor?std::optional{(*floor)[1]}:std::nullopt); // viewer mode starts from here
                        trace_camera(game,anchor,tracking->eyes[1]); // Both eyes share this anchor; eye 0 is traced inside camera_anchor.
                        const auto prepared=ordinary_tracked_camera::prepare(source,anchor,frame,*tracking,now);
                        if(!prepared && (*provider_failures)[1].fetch_add(1)<12)spdlog::warn("OrdinaryProviderFailure: job={} stage=prepare_none tracking_missing=false game_rigid={} anchor_rigid={} left_valid_at_acquisition={} right_valid_at_acquisition={} elapsed_ms={} tracking_age_ms={}",frame.render_id,vr_camera::rigid(game,false),vr_camera::rigid(anchor,false),tracking->eyes[0].valid_at(now),tracking->eyes[1].valid_at(now),std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-now).count(),std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-tracking->eyes[0].captured).count());
                        return prepared;
                    },4,true,[tracking_missing]{return tracking_missing->load();},{},owned_controls?NativeTrackedOrdinaryCamera::Readiness{[this]{
                        OwnedStereoReadiness::Observation value;
                        // Viewer mode: the game is frozen in photo mode and the free camera owns the view.
                        if(m_vr_session->viewer_owns_camera()){value.viewer=true;return value;}
                        // The loading screen: what renders behind it is the half-streamed level, so stand
                        // down (the flat loading screen shows instead), even with the cutscene bit up.
                        const auto scene=scene_of(EngineCinematicState::get().snapshot());
                        if(scene.loading()){value.loading=true;return value;}
                        // A story cutscene: the held spot or the cinematic camera owns the view; no player needed.
                        if(scene.cutscene()){value.cutscene=true;return value;}
                        // A menu over a rendered world (mission select): stand down so the panel shows it.
                        value.menu=m_menu_gate.load();
                        value.actor=EngineBodyProbe::get().active_actor();
                        const auto pose=EngineArmProbe::get().snapshot_current().pose;
                        if(pose){const auto& o=pose->owner;value.pose=true;value.owner={o.actor,o.entity,o.controller,o.wrapper,o.network,o.definition,o.instance};
                            value.received=pose->received;value.owner_received=o.received;value.world=pose->world_valid;
                            value.current=EngineArmProbe::get().current_current(*pose) && EngineBodyProbe::get().active_actor()==value.actor;}
                        return value;
                    }}:NativeTrackedOrdinaryCamera::Readiness{});
                // A brief refusal with the owner unchanged resumes at once instead of settling over three
                // jobs again (OwnedStereoReadiness::resume, docs/STEREO_PAUSES_2026-10-26.md). 0: old rule.
                {char resume[16]{};const auto n=GetEnvironmentVariableA("SM2VR_STEREO_RESUME_MS",resume,sizeof(resume));
                 if(n && n<sizeof(resume)){if(const auto value=OwnedStereoReadiness::resume_from(resume))producer.readiness_resume(*value);
                    else spdlog::warn("OrdinaryHeadset: SM2VR_STEREO_RESUME_MS={} refused (whole ms 0..{}); default kept",resume,OwnedStereoReadiness::MAX_RESUME.count());}}
                spdlog::info("OrdinaryHeadset: armed={} warmup={} synchronized_eyes=true owned_readiness={} same_owner_jobs=3 resume_ms={}",armed,m_stereo_warmup,owned_controls,producer.readiness_resume().count());
                // No pair while a game PIP view shows: behind the pair its HDR index overruns the game's table (docs/LOAD_CRASH_2026-10-26.md).
                producer.set_foreign_gate([]{return ForeignViewHold::get().held(std::chrono::steady_clock::now());});
                // Timer audit check (OwnedStereoReadiness.h MenuHold): run without the 3 s hold after a menu.
                char hold[16]{};if(GetEnvironmentVariableA("SM2VR_DIAG_MENU_HOLD_MS",hold,sizeof(hold))){m_menu_hold.settle(std::chrono::milliseconds(std::atoi(hold)));
                    spdlog::warn("MenuHold: diagnostic SM2VR_DIAG_MENU_HOLD_MS: settle_ms={} (default {})",m_menu_hold.settle().count(),MenuHold::SETTLE.count());}
            }
        }
        char reset_temporal[32]{};
        m_temporal_reset = GetEnvironmentVariableA("SM2VR_RESET_TEMPORAL",reset_temporal,sizeof(reset_temporal)) && std::atoi(reset_temporal) == 1;
        StreamlineProbe::get().set_force_temporal_reset(m_temporal_reset);
        spdlog::info("TemporalReset: diagnostic_reset={}",m_temporal_reset);
        if (synthetic_enabled) {
            EngineCameraProbe::get().set_eye_provider([](const FrameSyncClock::Frame& frame) -> std::optional<EngineCameraProbe::VrEye> {
                EngineCameraProbe::VrEye eye{};
                eye.eye = frame.eye; eye.render_token = frame.render_id; eye.synthetic = true;
                eye.captured = std::chrono::steady_clock::now();
                eye.relative_eye[12] = (frame.eye ? 0.032f : -0.032f) + float(frame.render_id % 127)*0.0005f;
                eye.fov = frame.eye ? vr_projection::Tangents{-0.9f,1.1f,-1.1f,0.9f} : vr_projection::Tangents{-1.1f,0.9f,-0.9f,1.1f};
                return eye;
            });
            spdlog::info("VrSynthetic: per-frame diagnostic poses/projections enabled; runtime submission forbidden");
        }
        m_next_frame_trace = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(m_frame_trace_interval));
    }
    if (m_frame_trace_interval > 0 && m_frame_trace_count > 0 && std::chrono::steady_clock::now() >= m_next_frame_trace) {
        FrameSyncProbe::get().start_trace(m_frame_trace_count);
        m_next_frame_trace = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(m_frame_trace_interval));
    }
    FrameSyncProbe::get().record(FrameSyncProbe::Kind::present);
    trace_game();trace_weapon();
    if(vr_trace::Recorder::get().enabled()) {
        auto& rev=EngineChainswordRev::get();const auto state=rev.snapshot();
        vr_trace::Recorder::get().chainsword_rev({std::chrono::steady_clock::now(),rev.installed(),state.property!=0,state.starts,state.stops,state.refused,state.play_rate,rev.audio_snapshot().gain});
    }
    vr_trace::Recorder::get().tick();
    poll_weapon_settings();poll_camera_guard();poll_takeover_control();poll_melee_sweep_control();poll_menu_settings();
    log_scene(scene_of(EngineCinematicState::get().sample(),m_screen_menu.active(std::chrono::steady_clock::now())));
    if (m_vr_session) {
        poll_calibration(); poll_arm_settings(); poll_swing_settings();poll_view_settings();
        m_vr_session->poll(std::chrono::steady_clock::now(),g_framework->get_d3d12_hook()->get_command_queue());
        const auto input_now = std::chrono::steady_clock::now();
        if (m_controller_input.needs_dispatch() && input_now >= m_next_controller_hook_poll) {
            EngineControllerRegistration::get().install_in_game(GetModuleHandleW(nullptr),m_controller_input);
            m_next_controller_hook_poll = input_now+std::chrono::seconds(3);
        }
        DWORD foreground_pid{};
        GetWindowThreadProcessId(GetForegroundWindow(),&foreground_pid);
        const bool foreground=foreground_pid==GetCurrentProcessId();
        const auto view_keys=m_view_hotkeys.take(foreground);
        if(view_keys.cycle && !m_chase_launch) {auto value=m_view_settings;value.mode=VrViewSettings::next(value.mode);apply_view_settings(value,true);}
        const bool native_recenter=m_controller_input.take_native_recenter();
        const bool auto_recenter=m_view_camera.take_recenter(); // The Marine walked off the diorama table.
        if(view_keys.recenter || (foreground && native_recenter) || auto_recenter) {m_vr_session->recenter();spdlog::info("VrView: recenter requested mode={} automatic={}",VrViewSettings::name(m_view_settings.mode),auto_recenter);}
        // Gamepad Back held + D-pad up/down: world bigger/smaller in diorama and chase.
        if(const int steps=m_controller_input.take_native_scale_steps();steps && foreground) {
            auto value=m_view_settings;
            if(value.resize_world(steps) && apply_view_settings(value,true)) spdlog::info("VrView: world scale steps={} mode={} effective_units_per_metre={}",steps,VrViewSettings::name(value.mode),value.scale(m_vr_calibration.units_per_metre));
        }
        if(const auto body=m_vr_session->body_scale();body.readings!=m_body_readings_logged) {
            m_body_readings_logged=body.readings;
            const auto reading=body.last?VrBodyScale::name(*body.last):"none";
            if(body.last && *body.last!=VrBodyScale::Reading::accepted)
                spdlog::warn("VrBodyScale: recenter reading {} (stand straight to recenter); keeping eye_height={} factor={:.3f}",reading,body.player_eye_height.value_or(0),body.factor);
            else spdlog::info("VrBodyScale: reading={} eye_height={} reference={} factor={:.3f} units_per_metre={:.3f}",reading,body.player_eye_height.value_or(0),body.reference_eye_height.value_or(0),body.factor,body.units_per_metre);
        }
        // Viewer mode suspends every first-person feature except the arm pose up to the freeze
        // (below), so the frozen marine renders whole.
        const bool first_person=first_person_features();
        const auto swing_actor=EngineBodyProbe::get().active_actor();
        m_vr_session->allow_swing_melee(m_runtime_enabled && foreground_pid==GetCurrentProcessId() && EngineBodyFollowing::get().snapshot().follow.anchored && !FrameSyncProbe::get().recent_out_of_band()?swing_actor:0);
        const auto controls_now=std::chrono::steady_clock::now();
        // Third person has no body following to anchor; its HUD shows in the same scenes.
        const bool hud_view=first_person?EngineBodyFollowing::get().snapshot().follow.anchored:
            m_view_settings.mode!=VrViewMode::first_person && !m_vr_session->viewer_engaged();
        VrCompactHud::get().allow(hud_view && foreground?swing_actor:0,m_vr_session->epoch());
        const auto selected_weapon=EngineWeaponProbe::get().aim_driver().pose(swing_actor,controls_now);
        const std::optional<bool> secondary_equipped=selected_weapon && selected_weapon->epoch==m_vr_session->epoch() && selected_weapon->hand?
            std::optional{*selected_weapon->hand==0}:std::nullopt;
        m_logged_secondary_equipped=secondary_equipped;
        const auto native_state=EngineCinematicState::get().snapshot();
        // The arm driver counts every native pose publication. Two after its head hiding was
        // switched off for the viewer means the game has republished a full-size head, so the
        // freeze will not keep a shrunken one. The arms keep the player's pose meanwhile.
        const bool character_native=m_viewer_release_from && EngineArmDriver::get().snapshot().updates>=*m_viewer_release_from+2;
        // Gaze camera candidate: turn the native (interaction/focus) camera toward the gaze.
        // VrSession applies it to first-person gameplay pads only (never menu or viewer).
        const auto gaze_follow=EngineBodyFollowing::get().snapshot().follow;
        // Plain gameplay and the walkable hub (SceneMode gaze_camera: no cutscene, no menu over the
        // world, none opening); any other scene or an unreadable state keeps the native right stick centred.
        // While the class ability (native Y) is held on the pad last published, the camera
        // closes to the deadband so a grapnel or auspex scan aims at the gaze centre.
        const bool ability_held=(m_controller_input.snapshot(controls_now).pad.Gamepad.wButtons&XINPUT_GAMEPAD_Y)!=0;
        const auto gaze_stick=vr_gaze::Steering::get().stick(first_person && foreground && !m_vr_session->viewer_engaged() &&
            scene_of(native_state,m_screen_menu.active(controls_now)).gaze_camera() && !m_screen_menu.opening(controls_now) &&
            VrBodyFollowing::gaze_steers(gaze_follow.camera_mode,gaze_follow.anchored),controls_now,ability_held);
        // A viewer play ends once the Marine's own animation has run and settled (VrViewer.h vr_viewer::Motion).
        if(const auto motion=EngineArmDriver::get().native_motion()) m_vr_session->note_character_motion({motion->received,motion->actor,motion->points});
        m_vr_session->poll_controllers(m_controller_input,foreground_pid == GetCurrentProcessId(),controls_now,
            EngineBodyFollowing::get().movement_heading_offset(controls_now),secondary_equipped,
            native_state.available?std::optional{native_state.published|native_state.requested}:std::nullopt,character_native,
            EngineBodyFollowing::get().ground_speed(controls_now),
            gaze_stick?std::optional{std::array<float,2>{gaze_stick->x,gaze_stick->y}}:std::nullopt);
        // A weapon switch went out (trackpad down, or a holster draw: both are native D-pad down). Only
        // then may the held primary give way to the pistol in the left hand (vr_weapon::GripSwitch).
        if(m_controller_input.snapshot().pad.Gamepad.wButtons&XINPUT_GAMEPAD_DPAD_DOWN) EngineArmDriver::get().weapon_switch(std::chrono::steady_clock::now());
        log_viewer(native_state.published|native_state.requested);
        EngineBodyFollowing::get().allow(first_person && m_controller_input.allows_body_motion(foreground_pid==GetCurrentProcessId()));
        const auto facing_input=m_controller_input.snapshot();
        EngineChainswordRev::get().release_audio_unless(swing_actor,facing_input.chainsword_rev);
        EngineBodyFollowing::get().dodge(facing_input.armed && (facing_input.pad.Gamepad.wButtons&XINPUT_GAMEPAD_A),std::chrono::steady_clock::now());
        // Light and power/held strikes and parries: the game turns the Marine onto its melee target.
        EngineBodyFollowing::get().melee(facing_input.armed && VrBodyFollowing::melee_press(facing_input.pad.Gamepad.wButtons),std::chrono::steady_clock::now());
        // The buttons the game is sent: a melee combo's final strike stays in first person.
        EngineBodyFollowing::get().combo(facing_input.armed?facing_input.pad.Gamepad.wButtons:0,std::chrono::steady_clock::now());
        // The stick the game is sent: a takeover returns early once the Marine walks along it.
        EngineBodyFollowing::get().stick(facing_input.armed?facing_input.pad.Gamepad.sThumbLX/32767.f:0.f,facing_input.armed?facing_input.pad.Gamepad.sThumbLY/32767.f:0.f,std::chrono::steady_clock::now());
        // After the poll, so the phase that just started counts: up to the freeze the arms and
        // fingers keep the player's pose and only the head is restored (vr_viewer::pose_edits).
        const auto viewer_state=m_vr_session->viewer_status().state;
        const auto edits=vr_viewer::pose_edits(m_view_settings.mode==VrViewMode::first_person,viewer_state,m_viewer_keep_pose);
        EngineArmDriver::get().allow(edits.arms && m_runtime_enabled && foreground_pid==GetCurrentProcessId() && EngineBodyFollowing::get().snapshot().follow.anchored,
            m_vr_session->epoch(),edits.hide_head);
        // Baseline only once head hiding is actually off in the driver.
        if(viewer_state==VrViewerSequencer::State::off) m_viewer_release_from.reset();
        else if(!edits.hide_head && !m_viewer_release_from) {
            const auto driver=EngineArmDriver::get().snapshot();m_viewer_release_from=driver.updates;m_viewer_release_hands=driver.hands;
        }
    }
    DWORD weapon_foreground{};GetWindowThreadProcessId(GetForegroundWindow(),&weapon_foreground);
    const auto weapon_poses=m_vr_session?m_vr_session->controller_poses():std::nullopt;
    const auto weapon_epoch=m_vr_session?m_vr_session->epoch():0;
    const auto weapon_gate=VrWeaponAim::gate(first_person_features() && weapon_foreground==GetCurrentProcessId(),m_weapon_demo,m_runtime_enabled,
        weapon_poses && weapon_poses->relative[1] && weapon_poses->tracking_epoch==weapon_epoch && EngineBodyFollowing::get().snapshot().follow.anchored,weapon_epoch);
    EngineWeaponProbe::get().aim_driver().allow(weapon_gate.allowed,weapon_gate.epoch);
    // Third person too: the native camera stays in the helmet, so its camera-distance fade
    // would turn the whole Marine translucent when seen from the diorama or shoulder camera.
    const bool third_person=m_view_settings.mode!=VrViewMode::first_person;
    // A viewer play too: with no execution the game's camera is back at the helmet while the game runs.
    const bool viewer_play=m_vr_session && m_vr_session->viewer_playing();
    EnginePlayerOpacity::get().allow((first_person_features() || third_person || viewer_play) && m_vr_session && m_runtime_enabled && !FrameSyncProbe::get().recent_out_of_band());
    // First-person head hiding by native part flags: head_01/helmet_01 become
    // shadow-only (pauldrons stay); any other view restores the native head. So do a native
    // camera and a takeover once its view is clear of the head (docs/ANIMATION_CAMERA.md).
    NativePlayerPartVisibility::get().set_policy(EngineArmDriver::get().snapshot().head_hidden && first_person_features() && m_vr_session && m_runtime_enabled &&
        !EngineBodyFollowing::get().snapshot().follow.show_head ?
        NativePlayerPartVisibility::Mode::Head : NativePlayerPartVisibility::Mode::Native);
    EnginePlayerVisibilityProbe::get().refresh_owner();
    const bool framegen_blocked = m_vr_session && FrameSyncProbe::get().recent_out_of_band();
    if (framegen_blocked && !m_vr_framegen_blocked) {
        m_vr_session->reset_reference();
        spdlog::warn("VrSession: out-of-band presentations observed; blocking headset image submission");
    }
    m_vr_framegen_blocked = framegen_blocked;
    if(performance.enabled()) {
        const auto cinematic=EngineCinematicState::get().snapshot();
        present_scope.value=PerformanceTrace::present_flags(weapon_foreground==GetCurrentProcessId(),
            cinematic.available,cinematic.paused,cinematic.scripted,cinematic.published,cinematic.requested,framegen_blocked);
    }
    if (!m_ordinary_headset && !framegen_blocked && performance_mode!=PerformanceTrace::Mode::tracking) { capture_eye_image(); }
    if (m_vr_session && !m_ordinary_headset && !framegen_blocked && performance_mode==PerformanceTrace::Mode::full) {
        if (const auto pair = m_eye_capture.pair()) { m_vr_session->submit(*pair,g_framework->get_d3d12_hook()->get_command_queue()); }
    }
    apply_scripted_camera_shift();
    log_temporal_trace();
    log_frame_trace();
    ++m_present_count;
    if (m_present_count == 1) {
        log_swap_chain("first present");
    }
    // The game may re-create or resize its window when it applies user settings; re-check after
    // each device reset, a bounded number of times so it never fights the game.
    if (m_desktop_window && m_desktop_window_pending && m_desktop_window_attempts < 4) {
        m_desktop_window_pending = false;
        apply_desktop_window();
    }

    const auto now     = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<float>(now - m_last_log_time).count();
    if (elapsed >= 5.0f) {
        m_fps = static_cast<float>(m_present_count - m_last_logged_present) / elapsed;
        spdlog::info("Sm2Probe: presents={} fps={:.1f} ui_draws={}", m_present_count, m_fps, m_ui_draw_count);
        const auto cinematic=EngineCinematicState::get().snapshot();
        spdlog::info("CinematicState: available={} scripted={} paused={} published={:#x} requested={:#x} samples={} failures={} transitions={}",
            cinematic.available,cinematic.scripted,cinematic.paused,cinematic.published,cinematic.requested,cinematic.samples,cinematic.failures,cinematic.transitions);
        if(NativePoolReleaseGuard::get().installed()){const auto pool=NativePoolReleaseGuard::get().counters();
            const auto game=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));std::string callers;
            for(const auto& c:pool.callers)if(c.address)callers+=fmt::format("{}{:#x}x{}",callers.empty()?"":",",c.address>=game?c.address-game:c.address,c.skipped);
            spdlog::info("PoolReleaseGuard: calls={} skipped={} last_skipped_key={:#x} callers=[{}]",pool.calls,pool.skipped,pool.last_skipped_key,callers);
            // The release's invariant on every mode-2 pool it has seen (up to four); the used count over time also shows a leak.
            for(const auto address:pool.pools)if(address){const auto h=native_pool_release_guard::health(address);
                if(native_pool_release_guard::healthy(h))spdlog::info("PoolHealth: pool={:#x} used={} capacity={} sorted=true",address,h.count,h.capacity);
                else spdlog::warn("PoolHealth: pool={:#x} known={} used={} capacity={} sorted={} (the in-use list has drifted: a level exit may freeze)",address,h.known,h.count,h.capacity,h.sorted);}}
        log_call_census();
        {const auto guard=NativeTrackerHandleGuard::get().stats();spdlog::info("TrackerHandleGuard: enabled={} exe_installed={} dll_installed={} exe_refused={} dll_refused={} getter_trips=[{},{}] pair_trips=[{},{}] identity_fallbacks=[{},{}] reapplied=[{},{}] remapped=[{},{}] page_queries={}",NativeTrackerHandleGuard::get().enabled(),guard.installed[0],guard.installed[1],guard.refused[0],guard.refused[1],guard.getter_trips[0],guard.getter_trips[1],guard.pair_trips[0],guard.pair_trips[1],guard.fallback_identity[0],guard.fallback_identity[1],guard.reapplied[0],guard.reapplied[1],guard.remapped[0],guard.remapped[1],guard.queries);}
        if (EngineBodyProbe::get().installed()) {
            const auto bodies = EngineBodyProbe::get().snapshot();
            spdlog::info("BodyProbe: updates={} tracker_calls={} rejected={} overflow={} evicted={} bodies={} trackers={}",
                bodies.updates,bodies.tracker_calls,bodies.rejected,bodies.overflow,bodies.evicted,bodies.bodies.size(),bodies.trackers.size());
            if(m_status_detail) {
            for (const auto& body:bodies.bodies) {
                spdlog::info("BodyCandidate: body={:x} actor={:x} entity={:x} vtable={:x} calls={} age={:.3f} pos=[{},{},{}] forward=[{},{},{}] requested=[{},{},{}]",
                    body.body,body.actor,body.entity,body.body_vtable,body.calls,std::chrono::duration<float>(now-body.received).count(),
                    body.position[0],body.position[1],body.position[2],body.forward[0],body.forward[1],body.forward[2],
                    body.requested_position[0],body.requested_position[1],body.requested_position[2]);
            }
            for (const auto& tracker:bodies.trackers) {
                spdlog::info("BodyRootTracker: object={:x} caller={:#x} vtable={:x} method_rva={:#x} owner_actor={:x} calls={} valid={} pos=[{},{},{}]",
                    tracker.object,tracker.caller_rva,tracker.vtable,tracker.world_method_rva,tracker.owner_actor,tracker.calls,tracker.valid,
                    tracker.world[12],tracker.world[13],tracker.world[14]);
                for (size_t i=0;i<tracker.words.size();i+=4) {
                    spdlog::info("BodyRootWords: object={:x} offset={:#x} words=[{:x},{:x},{:x},{:x}]",tracker.object,i*8,
                        tracker.words[i],tracker.words[i+1],tracker.words[i+2],tracker.words[i+3]);
                }
            }
            }
        }
        if(EngineWeaponProbe::get().installed()) {
            const auto weapons=EngineWeaponProbe::get().snapshot();
            spdlog::info("WeaponProbe: calls={} worlds={} rejected={} overflow={} targets={}",weapons.calls,weapons.worlds,weapons.rejected,weapons.overflow,weapons.targets.size());
            spdlog::info("WeaponAimProbe: calls={} owned={}",weapons.aim_calls,weapons.aim_owned);
            const auto driven=EngineWeaponProbe::get().aim_driver().pose(EngineBodyProbe::get().active_actor(),now);
            const auto gun_settings=EngineArmDriver::get().weapon_settings();
            spdlog::info("WeaponGrip: values=[{},{},{},{},{},{}]",gun_settings.values[0],gun_settings.values[1],gun_settings.values[2],gun_settings.values[3],gun_settings.values[4],gun_settings.values[5]);
            spdlog::info("WeaponAimDriver: enabled={} ray_overrides={} target_overrides={} fresh_pose={} sequence={} origin=[{},{},{}] forward=[{},{},{}]",EngineWeaponProbe::get().aim_driver().enabled(),weapons.ray_overrides,weapons.target_overrides,driven.has_value(),driven?driven->sequence:0,
                driven?driven->world[12]:0,driven?driven->world[13]:0,driven?driven->world[14]:0,driven?driven->world[8]:0,driven?driven->world[9]:0,driven?driven->world[10]:0);
            if(weapons.aim) {
                const auto& aim=*weapons.aim;
                spdlog::info("WeaponAim: control={:x} controller={:x} actor={:x} calls={} age_ms={} direction_only={} aimed={} input_valid={} input_origin=[{},{},{}] input_forward=[{},{},{}] reads={}",
                    aim.control,aim.controller,aim.actor,aim.calls,std::chrono::duration<double,std::milli>(now-aim.received).count(),aim.direction_only,aim.aimed,aim.target_valid,
                    aim.input_target[12],aim.input_target[13],aim.input_target[14],aim.input_target[8],aim.input_target[9],aim.input_target[10],aim.read_count);
                if(m_status_detail) for(uint32_t index=0;index<aim.read_count;++index) {
                    const auto& read=aim.reads[index];
                    spdlog::info("WeaponAimRead: index={} tracker={:x} caller={:#x} method={:#x} valid={} origin=[{},{},{}] forward=[{},{},{}]",index,read.tracker,read.caller,read.method,read.valid,
                        read.world[12],read.world[13],read.world[14],read.world[8],read.world[9],read.world[10]);
                    if(read.scene) spdlog::info("WeaponAimScene: index={} scene={:x}",index,read.scene);
                }
            }
            if(m_status_detail) for(const auto& target:weapons.targets) {
                spdlog::info("WeaponTarget: control={:x} scene={:x} tracker={:x} actor={:x} active_actor={:x} owned={} calls={} age_ms={} caller={:#x} method={:#x} origin=[{},{},{}] forward=[{},{},{}] target=[{},{},{}] max_distance={}",
                    target.control,target.scene,target.tracker,target.actor,target.active_actor,target.owned,target.calls,std::chrono::duration<double,std::milli>(now-target.received).count(),target.caller,target.world_method_rva,
                    target.world[12],target.world[13],target.world[14],target.world[8],target.world[9],target.world[10],target.target[0],target.target[1],target.target[2],target.max_distance);
                spdlog::info("WeaponTrackerWords: tracker={:x} words=[{:x},{:x},{:x},{:x},{:x},{:x},{:x},{:x},{:x},{:x},{:x},{:x}]",target.tracker,
                    target.tracker_words[0],target.tracker_words[1],target.tracker_words[2],target.tracker_words[3],target.tracker_words[4],target.tracker_words[5],
                    target.tracker_words[6],target.tracker_words[7],target.tracker_words[8],target.tracker_words[9],target.tracker_words[10],target.tracker_words[11]);
                spdlog::info("WeaponParent: control={:x} scene_container={:x} player_root={:x} depth={}",target.control,target.scene_owner,target.root_scene,target.parent_depth);
            }
        }
        if (EngineBodyFollowing::get().installed()) {
            const auto follow=EngineBodyFollowing::get().snapshot();
            const auto settings=EngineBodyFollowing::get().settings();
            spdlog::info("CameraHandover: mode={} transitions={} radius_metres={} height_metres={} limits=[{},{}] takeovers={} takeover_enabled={} standing_height_units={} show_head={} head_clear_metres={} height_lead_units={} height_lead_seconds={} cutscene_hold={} cutscene_hold_enabled={} rides={} riding={} travel_speed_metres_per_second={} ride_enabled={} ride_speed_metres_per_second={} vignette={} ride_vignette={} releases={} released={} release_speed_metres_per_second={} release_seconds={} still_releases={} execution_still_seconds={} combos={} combo_camera={} combo_camera_radius_metres={} combo_strikes={} combo_gap_seconds={} combo_camera_seconds={}",
                static_cast<int>(follow.follow.camera_mode),follow.follow.camera_transitions,follow.follow.camera_radius,follow.follow.camera_height,
                settings.camera_radius_metres,settings.camera_height_metres,follow.follow.takeovers,settings.takeover_enabled,
                follow.follow.standing_height,follow.follow.show_head,settings.head_clear_metres,follow.follow.height_lead,settings.height_lead_seconds,follow.follow.cutscene_hold,settings.cutscene_hold,
                follow.follow.rides,follow.follow.riding,follow.follow.travel_speed,settings.ride_enabled,settings.ride_speed_metres_per_second,follow.follow.vignette,settings.ride_vignette,
                follow.follow.releases,follow.follow.released,settings.release_speed_metres_per_second,settings.release_seconds,follow.follow.still_releases,settings.execution_still_seconds,
                follow.follow.combos,follow.follow.combo_camera,settings.combo_camera_radius_metres,settings.combo_strikes,settings.combo_gap_seconds,settings.combo_camera_seconds);
            spdlog::info("BodyFollowing: enabled={} actor={:x} body={:x} context={:x} anchored={} turns={} yaw={} body_forward=[{},{},{}] facing_error_degrees={} melee_holds={} melee_facing={} context_calls={} matched_contexts={} native_velocity=[{},{},{}] room_enabled={} room_steps={} error_metres={} consumed=[{},{},{}] requested_velocity=[{},{},{}] gain={} max_speed={} deadzone={}",
                EngineBodyFollowing::get().enabled(),follow.follow.actor,follow.follow.body,follow.follow.context,follow.follow.anchored,follow.follow.turns,follow.follow.yaw,
                follow.body_forward[0],follow.body_forward[1],follow.body_forward[2],follow.facing_error_degrees,follow.follow.melee_holds,follow.follow.melee_facing,
                follow.context_calls,follow.matched_contexts,follow.native_velocity[0],follow.native_velocity[1],follow.native_velocity[2],settings.room_enabled,follow.follow.room_steps,
                follow.follow.room_error,follow.follow.consumed[0],follow.follow.consumed[1],follow.follow.consumed[2],
                follow.follow.requested[0],follow.follow.requested[1],follow.follow.requested[2],settings.gain,settings.max_speed,settings.deadzone);
        }
        if (EngineArmProbe::get().installed()) {
            const auto arms=EngineArmProbe::get().snapshot(now);
            spdlog::info("ArmProbe: updates={} matched={} publications={} captures={} rejected={} pose_valid={} world_scaled={} last_world_scale={:.6f} world_refused={}",
                arms.updates,arms.matched,arms.publications,arms.captures,arms.rejected,arms.pose.has_value(),arms.world_scaled,arms.last_world_scale,arms.world_refused);
            // The entity world the probe last refused (not rigid, nor a uniform scale of one), once per new refusal.
            if(arms.refused_world && arms.world_refused!=m_logged_world_refusals) {
                m_logged_world_refusals=arms.world_refused;const auto& w=*arms.refused_world;
                spdlog::warn("ArmWorldRefused: refusals={} x=[{},{},{},{}] y=[{},{},{},{}] z=[{},{},{},{}] t=[{},{},{},{}]",arms.world_refused,
                    w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7],w[8],w[9],w[10],w[11],w[12],w[13],w[14],w[15]);
            }
            if (arms.pose) {
                const auto& pose=*arms.pose; const auto& owner=pose.owner;
                spdlog::info("ArmOwner: actor={:x} entity={:x} controller={:x} wrapper={:x} network={:x} definition={:x} instance={:x} animset={} rig={:x} mapping={:x} buffer={:x} bones={} layout_valid={} world_valid={} world_scale={:.6f}",
                    owner.actor,owner.entity,owner.controller,owner.wrapper,owner.network,owner.definition,owner.instance,pose.animset,pose.rig,pose.mapping,pose.buffer,pose.bones.size(),pose.layout!=nullptr,pose.world_valid,pose.world_scale);
                if(m_status_detail) for (const auto& bone:pose.bones) {
                    spdlog::info("ArmLocalBone: rig_index={} skin_index={} name={} parent={} right=[{},{},{}] up=[{},{},{}] forward=[{},{},{}] position=[{},{},{}]",
                        bone.rig_index,bone.skin_index,pose.layout?pose.layout->names[bone.rig_index]:"unknown",pose.layout?pose.layout->parents[bone.rig_index]:-2,bone.local[0],bone.local[1],bone.local[2],bone.local[4],bone.local[5],bone.local[6],bone.local[8],bone.local[9],bone.local[10],bone.local[12],bone.local[13],bone.local[14]);
                }
            }
        }
        if (EnginePlayerOpacity::get().installed()) {
            char fade_path[MAX_PATH]{};
            if(GetEnvironmentVariableA("SM2VR_PLAYER_DISTANCE_FADE_CONTROL",fade_path,sizeof(fade_path)) && std::filesystem::path{fade_path}.is_absolute()) {
                std::ifstream control{fade_path};std::string mode;
                if(std::getline(control,mode) && mode.size()<32) {if(!mode.empty() && mode.back()=='\r')mode.pop_back();EnginePlayerOpacity::get().distance_control(mode);}
            }
            const auto opacity=EnginePlayerOpacity::get().snapshot();
            spdlog::info("PlayerOpacity: enabled={} allowed={} calls={} owned={} overrides={} released={} invalid={} actor={:x} instance={:x} native={} effective={}",opacity.enabled,opacity.allowed,opacity.calls,opacity.owned,opacity.overrides,opacity.released,opacity.invalid,opacity.actor,opacity.instance,opacity.native,opacity.effective);
            spdlog::info("PlayerRenderOpacity: getter_installed={} reads={} owned_reads={} overrides={} native={} effective={}",opacity.getter_installed,opacity.reads,opacity.owned_reads,opacity.read_overrides,opacity.render_native,opacity.render_effective);
            spdlog::info("PlayerRenderFade: native_faded_reads={} changed_reads={} min_native={}",opacity.native_faded_reads,opacity.changed_reads,opacity.render_min_native);
            const auto draws=EnginePlayerOpacity::get().draw_snapshot();
            spdlog::info("PlayerDrawOpacity: installed={} owned={} bypassed={} failures={}",draws.installed,draws.owned,draws.bypassed,draws.failures);
            const auto params=EnginePlayerOpacity::get().params_snapshot();
            spdlog::info("PlayerDistanceFade: installed={} enabled={} owned={} changed={} failures={} modes={},{},{},{} flags={:x} scale={} offset={} padding={}",params.installed,params.enabled,params.owned,params.changed,params.failures,params.modes[0],params.modes[1],params.modes[2],params.modes[3],params.flags,params.scale,params.offset,params.padding);
            spdlog::info("DistanceFadeScope: all_objects={} builds={} tracked={} invalid={} capacity_skips={} swept={} evicted={}",params.all_objects,params.builds,params.tracked,params.invalid,params.capacity_skips,params.swept,params.evicted);
            const auto filter=EnginePlayerVisibilityProbe::get().snapshot();
            spdlog::info("PlayerVisibilityFilter: installed={} owned_calls={} rejected={} mask_removed={} before={:#x} after={:#x} instance={:x}",filter.installed,filter.owned_calls,filter.rejected,filter.mask_removed,filter.last_before,filter.last_after,filter.instance);
        }
        if (EngineArmDriver::get().snapshot().enabled || EngineArmDriver::get().snapshot().head_hidden) {
            const auto driver=EngineArmDriver::get().snapshot(); const auto settings=EngineArmDriver::get().settings();
            spdlog::info("ArmDriver: updates={} hands={} bone_writes={} rejected={} failures={} left_error={} right_error={}",
                driver.updates,driver.hands,driver.bone_writes,driver.rejected,driver.failures,driver.errors[0],driver.errors[1]);
            spdlog::info("FingerDriver: enabled={} hands={} rejected={} left_open_angle={} right_open_angle={}",driver.fingers_enabled,driver.finger_hands,driver.finger_rejected,driver.finger_open_angles[0],driver.finger_open_angles[1]);
            {
                std::lock_guard lock{m_thumbs_up_mutex};
                // shaped_*: finger targets turned into a thumbs-up; photo_latches: photo entries that kept the lifted left thumb.
                spdlog::info("VrThumbsUp: mode={} shaped_left={} shaped_right={} photo_requests={} photo_latches={}",vr_thumbs_up::name(m_thumbs_up.mode()),
                    m_thumbs_up.shaped(0),m_thumbs_up.shaped(1),m_thumbs_up.photo_requests(),m_thumbs_up.photo_latches());
            }
            spdlog::info("HeadVisibility: hidden={} updates={} rejected={}",driver.head_hidden,driver.head_updates,driver.head_rejected);
            const auto part_visibility=NativePlayerPartVisibility::get().snapshot();
            if(driver.invalidate_pose_enabled)spdlog::info("ArmPoseInvalidation: calls={} refused={} called={} valid={} dirty_before={} dirty_after={} status={}",driver.invalidation_calls,driver.invalidation_refused,driver.invalidation.called,driver.invalidation.valid,
                driver.invalidation.before?std::to_string(*driver.invalidation.before):"unavailable",driver.invalidation.after?std::to_string(*driver.invalidation.after):"unavailable",driver.invalidation.status);
            if(driver.resolve_pose_enabled)spdlog::info("ArmPoseResolution: flush_called={} resolved={} verified_hands={} world_error={} recovery_calls={} recovery_failures={} poisoned={}",
                driver.invalidation.flush_called,driver.invalidation.resolved,driver.invalidation.verified_hands,
                driver.invalidation.world_error?std::to_string(*driver.invalidation.world_error):"unavailable",driver.recovery_calls,driver.recovery_failures,driver.pose_recovery_failed);
            if(part_visibility.configured || part_visibility.updates || part_visibility.refused || part_visibility.requested!=NativePlayerPartVisibility::Mode::Native)spdlog::info("PlayerPartVisibility: requested={} applied={} status={} control={} revision={} updates={} writes={} restored={} refused={} lost_lease={} conflicts={} failures={} rollback_failures={} abandoned={} instance={:x} resource={:x} names={} records={} groups={} outfit_hidden={}",
                NativePlayerPartVisibility::name(part_visibility.requested),part_visibility.applied?NativePlayerPartVisibility::name(*part_visibility.applied):"unknown",
                part_visibility.status,part_visibility.control_status,part_visibility.revision,part_visibility.updates,part_visibility.writes,part_visibility.restored,part_visibility.refused,
                part_visibility.lost_lease,part_visibility.conflicts,part_visibility.failures,part_visibility.rollback_failures,part_visibility.abandoned,part_visibility.instance,part_visibility.resource,part_visibility.name_count,part_visibility.record_count,part_visibility.groups,part_visibility.outfit_hidden);
            spdlog::info("BodyRenderPlacement: back_metres={} updates={} rejected={} suspended={}",driver.body_back_metres,driver.body_placement_updates,driver.body_placement_rejected,driver.body_placement_suspended);
            spdlog::info("BodyPlacementControlState: configured_back_metres={} applied_back_metres={} status={} revision={} polls={} rejected={} superseded={}",driver.body_back_metres,driver.body_applied_back_metres,
                vr_body_placement::CONTROL_STATUS[static_cast<size_t>(driver.body_control_status)],driver.body_control_revision,driver.body_control_polls,driver.body_control_rejected,driver.body_control_superseded);
            const auto refusal=[&](vr_body_placement::Refusal why){return driver.body_placement_refusals[static_cast<size_t>(why)];};
            spdlog::info("BodyPlacementRefusals: settings={} layout={} topology={} mapping={} hierarchy={} direction={} output={} arm_solve={} weapon_attachment={}",
                refusal(vr_body_placement::Refusal::Settings),refusal(vr_body_placement::Refusal::Layout),refusal(vr_body_placement::Refusal::Topology),refusal(vr_body_placement::Refusal::Mapping),
                refusal(vr_body_placement::Refusal::Hierarchy),refusal(vr_body_placement::Refusal::Direction),refusal(vr_body_placement::Refusal::Output),refusal(vr_body_placement::Refusal::ArmSolve),refusal(vr_body_placement::Refusal::Attachment));
            for (size_t hand=0;hand<2;++hand) {
                spdlog::info("ArmSettings: hand={} grip_metres=[{},{},{}] grip_degrees=[{},{},{}] elbow_pole=[{},{},{}]",
                    hand,settings.grips[hand].metres[0],settings.grips[hand].metres[1],settings.grips[hand].metres[2],settings.grips[hand].degrees[0],settings.grips[hand].degrees[1],settings.grips[hand].degrees[2],settings.elbow_poles[hand][0],settings.elbow_poles[hand][1],settings.elbow_poles[hand][2]);
            }
        }
        const auto frames = FrameSyncProbe::get().snapshot();
        spdlog::info("FrameSync: state={} markers={} rejected={} phase_errors={} duplicate={} out_of_band={}",
            static_cast<int>(FrameSyncProbe::get().state()), frames.markers, frames.rejected, frames.clock.errors, frames.clock.duplicates, frames.clock.ignored);
        spdlog::info("EyeCapture: enabled={} copied={} missing_camera={} busy={} errors={} diagnostic_pairs={} tracked_pair={}",
            m_gpu_capture_enabled,m_gpu_copied,m_gpu_missing,m_gpu_busy,m_gpu_errors,m_gpu_pairs,m_eye_capture.pair().has_value());
        spdlog::info("CameraImageMatch: same_frame={} previous_frame={} missing={}",m_gpu_pose_matches,m_gpu_previous_pose_matches,m_gpu_pose_missing);
        if (m_vr_session) {
            const auto stats = m_vr_session->statistics();
            const auto observer=m_view_camera.snapshot();
            spdlog::info("VrView: mode={} anchored={} captures={} follows={} recenters={} anchor=[{},{},{}] effective_units_per_metre={}",VrViewSettings::name(m_view_settings.mode),
                observer.anchored,observer.captures,observer.follows,observer.recenters,observer.anchor[12],observer.anchor[13],observer.anchor[14],m_vr_session->body_scale().units_per_metre);
            spdlog::info("VrFingerStatus: {}",m_vr_session->finger_status());
            spdlog::info("VrSession: status={} epoch={} submitted_pairs={} per_eye_hz={:.1f} failed_transfers={} last_render_id={} oldest_pose_age_ms={:.2f} submit_cpu_ms={:.2f} stereo={} units_per_metre={} height_offset_metres={} head_forward_offset_metres={} worn_references={} submit_thread={} submit_replaced={}",
                m_vr_session->status(),m_vr_session->epoch(),stats.submitted_pairs,
                double(stats.submitted_pairs-m_last_vr_submitted)/elapsed,stats.failed_transfers,stats.last_render_id,
                stats.oldest_pose_age_ms,stats.submit_cpu_ms,m_vr_stereo,m_vr_calibration.units_per_metre,m_vr_calibration.height_offset_metres,m_vr_calibration.head_forward_offset_metres,stats.worn_references,
                stats.submit_thread,stats.submit_replaced);
            m_last_vr_submitted = stats.submitted_pairs;
            {const auto sprint=m_vr_session->sprint_camera(std::chrono::steady_clock::now());
             spdlog::info("VrSprintCamera: offset_metres={} blend_seconds={} active={} applied_metres={:.3f} engagements={} refusals={}",
                m_vr_calibration.sprint_forward_offset_metres,m_vr_calibration.sprint_blend_seconds,sprint.active,sprint.offset_metres,sprint.engagements,sprint.refusals);}
            const auto input = m_controller_input.snapshot();
            spdlog::info("VrCombat: fire={} aim={} buttons={:#x} melee_enabled={}",input.pad.Gamepad.bRightTrigger,input.pad.Gamepad.bLeftTrigger,input.pad.Gamepad.wButtons,m_controller_input.melee_enabled());
            const auto rev=EngineChainswordRev::get().snapshot();
            if(engine_output_size::Hook::get().snapshot().requested) spdlog::info("EngineOutputSize: {}",engine_output_size::Hook::get().status());
            const auto ordinary_stats=m_vr_session->statistics();
            spdlog::info("OrdinarySubmission: refusals=[{}] generations=[{}] pose_age_ms={:.2f}",fmt::join(ordinary_stats.ordinary_refusals,","),fmt::join(ordinary_stats.ordinary_generations,","),ordinary_stats.ordinary_pose_age_ms);
            spdlog::info("ChainswordRev: bound={} actor={:x} property={:x} starts={} stops={} refused={} base_layer_time={} play_rate={:.2f} audio_gain={:.2f} refused_table={} refused_missing={} refused_ambiguous={} refused_animation={}",rev.bound,rev.actor,rev.property,rev.starts,rev.stops,rev.refused,rev.base_layer_time,rev.play_rate,
                EngineChainswordRev::get().audio_snapshot().gain,rev.reasons.table,rev.reasons.missing,rev.reasons.ambiguous,rev.reasons.animation);
            const auto velocities=EngineRigidBodyVelocityProbe::get().snapshot();
            const auto grenade_launch=NativeGrenadeLaunch::get().snapshot();
            if(grenade_launch.redirected || grenade_launch.refused)spdlog::info("GrenadeLaunch: redirected={} refused={} actor={:x} projectile={:x} before=[{}] after=[{}] refused_pose={} refused_type={} refused_owner={} last_refused_type={}",grenade_launch.redirected,grenade_launch.refused,grenade_launch.actor,grenade_launch.projectile,fmt::join(grenade_launch.before,","),fmt::join(grenade_launch.after,","),
                grenade_launch.refused_pose,grenade_launch.refused_type,grenade_launch.refused_owner,grenade_launch.last_refused_type.empty()?std::string{"none"}:grenade_launch.last_refused_type);
            static uint64_t logged_redirect{};
            for(const auto& r:grenade_launch.history)if(r.serial>logged_redirect){
                spdlog::info("GrenadeRedirect: serial={} projectile={:x} before=[{}] after=[{}]",r.serial,r.projectile,fmt::join(r.before,","),fmt::join(r.after,","));
                logged_redirect=r.serial;
            }
            static uint64_t logged_velocity{};
            if(velocities.calls)spdlog::info("RigidVelocitySummary: calls={} samples={} rejected={} overflow={}",velocities.calls,velocities.samples.size(),velocities.rejected,velocities.overflow);
            for(const auto& sample:velocities.samples)if(sample.serial>logged_velocity){
                spdlog::info("RigidVelocity: serial={} body={:x} handle={:x} owner_props={:x} system={:x} system_owner={:x} caller={:x} name={} velocity=[{}]",sample.serial,sample.body,sample.handle,sample.owner_props,sample.system,sample.system_owner,sample.caller,sample.name,fmt::join(sample.velocity,","));
                for(const auto& prop:sample.properties)spdlog::info("RigidVelocityProperty: serial={} address={:x} words=[{:x}]",sample.serial,prop.address,fmt::join(prop.words,","));
                logged_velocity=sample.serial;
            }
            const auto audio=EngineChainswordRev::get().audio_snapshot();
            spdlog::info("ChainswordRevAudio: bound={} registered={} held={} playing={} listeners={} loop={} tail={} starts={} stops={} failures={}",audio.bound,audio.registered,audio.held,audio.playing,audio.listeners,audio.loop,audio.tail,audio.starts,audio.stops,audio.failures);
            const auto swing=m_vr_session->swing_statistics();const auto swing_settings=m_vr_session->swing_settings();
            spdlog::info("VrSwing: enabled={} swings={} left_hand_vetoes={} wrist_flicks={} discontinuities={} speed={} travel={} ready={} pressed={} speed_threshold={} travel_threshold={} reach={} rearm={} pulse_ms={} cooldown_ms={}",
                m_vr_session->swing_melee_enabled(),swing.swings,swing.vetoes,swing.flicks,swing.discontinuities,swing.speed,swing.travel,swing.ready,swing.pressed,
                swing_settings.values[0],swing_settings.values[1],swing_settings.values[2],swing_settings.values[3],swing_settings.values[4],swing_settings.values[5]);
            if(EngineParryProbe::get().installed()) {
                const auto parry=EngineParryProbe::get().snapshot();
                spdlog::info("ParryProbe: sounds={} indications={} sound_calls={} event_calls={}",parry.sounds,parry.indications,parry.sound_calls,parry.event_calls);
            }
            if(EngineMeleeSweep::get().installed()) {
                const auto sweep=EngineMeleeSweep::get().snapshot();
                spdlog::info("MeleeSweep: mode={} windows={} replays={} animation={} no_swing={} reads={} replaced={} refused={} player_reads={} detector_updates={} samples={} last_window_ms={:.0f} last_swing_age_ms={:.0f} last_path={}",
                    vr_melee_sweep::name(sweep.sweep.mode),sweep.sweep.windows,sweep.sweep.replays,sweep.sweep.animation,sweep.sweep.no_swing,sweep.sweep.reads,sweep.replaced,sweep.refused,
                    sweep.player_reads,sweep.detector_updates,sweep.sweep.samples,sweep.sweep.last_window_ms,sweep.sweep.last_swing_age_ms,sweep.sweep.last_path);
            }
            const auto holster=m_vr_session->holster_statistics();
            log_gestures();
            // grip: how the game holds the gun (vr_weapon::Grip; two_handed = a heavy weapon, held as a
            // primary). grip_switches: settled changes, so a reading that flaps shows as a rising count.
            // left_refused: frames a left (pistol) reading was refused: a primary held, no weapon switch asked.
            // primary_draws: pistol-to-primary switches landed by the draw (the gun left both sockets).
            // fit_seated: false while a primary landed off its socket uses the carried gun's fit.
            const auto arm_weapon=EngineArmDriver::get().snapshot();
            spdlog::info("VrHolster: enabled={} primary={} secondary={} salutes={} misses={} zone={} hand=[{},{},{}] last_press_slot={} last_press=[{},{},{}] touch_gate={} held={} grip={} grip_switches={} left_refused={} primary_draws={} fit_seated={} supports={}",
                m_vr_session->holsters_enabled(),holster.primary,holster.secondary,holster.salutes,holster.misses,static_cast<int>(holster.zone),holster.height,holster.lateral,holster.forward,
                static_cast<int>(holster.last_slot),holster.last_press[0],holster.last_press[1],holster.last_press[2],m_vr_session->trigger_lift_required(),
                m_logged_secondary_equipped?(*m_logged_secondary_equipped?"secondary":"primary"):"unknown",
                arm_weapon.weapon_grip?vr_weapon::name(*arm_weapon.weapon_grip):"none",arm_weapon.weapon_grip_switches,arm_weapon.weapon_left_refused,arm_weapon.weapon_primary_draws,arm_weapon.weapon_seated,m_vr_session->support_statistics().engagements);
            spdlog::info("VrControllers: enabled={} armed={} status={} pad_packet={} move=[{},{}] buttons={:#x} snaps={} snap_degrees={} native_state_polls={} input_gate={} native_hook={} native_updates={} native_registrations={} native_failures={} native_dispatch_healthy={} native_dispatch_repairs={}",
                input.enabled,input.armed,m_vr_session->input_status(),input.pad.dwPacketNumber,input.pad.Gamepad.sThumbLX,
                input.pad.Gamepad.sThumbLY,input.pad.Gamepad.wButtons,input.snaps,input.snap_degrees,EngineControllerRegistration::get().state_polls(),input.reason,
                static_cast<int>(EngineControllerRegistration::get().status()),EngineControllerRegistration::get().updates(),
                EngineControllerRegistration::get().registrations(),EngineControllerRegistration::get().failures(),
                EngineControllerRegistration::get().dispatch_healthy(),EngineControllerRegistration::get().dispatch_repairs());
            if (m_status_detail && input.enabled) if (const auto poses=m_vr_session->controller_poses(now)) {
                for (size_t hand=0;hand<poses->relative.size();++hand) {
                    const auto& pose=poses->relative[hand];
                    spdlog::info("VrHandPose: hand={} source=raw_controller valid={} reference={} epoch={} units_per_metre={} age_ms={:.2f} position=[{},{},{}] right=[{},{},{}] up=[{},{},{}] back=[{},{},{}]",
                        hand==0 ? "left" : "right",pose.has_value(),poses->reference_epoch,poses->tracking_epoch,poses->units_per_metre,
                        std::chrono::duration<double,std::milli>(now-poses->captured).count(),
                        pose ? (*pose)[12] : 0,pose ? (*pose)[13] : 0,pose ? (*pose)[14] : 0,
                        pose ? (*pose)[0] : 0,pose ? (*pose)[1] : 0,pose ? (*pose)[2] : 0,
                        pose ? (*pose)[4] : 0,pose ? (*pose)[5] : 0,pose ? (*pose)[6] : 0,
                        pose ? (*pose)[8] : 0,pose ? (*pose)[9] : 0,pose ? (*pose)[10] : 0);
                    const auto curls=poses->fingers[hand].value_or(vr_finger::Curl{});
                    spdlog::info("VrFingerCurl: hand={} valid={} thumb={} index={} middle={} ring={} pinky={}",hand,poses->fingers[hand].has_value(),curls[0],curls[1],curls[2],curls[3],curls[4]);
                }
            }
        }
        for (int eye = 0; eye < 2; ++eye) if (const auto image = m_eye_capture.latest(eye)) {
            const auto desc = image->texture()->GetDesc();
            spdlog::info("EyeCaptureImage: eye={} frame={} render_id={} generation={} fence={} ready={} tracked={} texture={:x} size={}x{}",
                eye,image->metadata.game_token,image->metadata.render_id,image->generation,image->fence_value,image->ready(),image->metadata.tracked_stereo,
                reinterpret_cast<uintptr_t>(image->texture()),desc.Width,desc.Height);
        }
        const auto projections = EngineProjectionHook::get().snapshot();
        const auto markers=EngineWorldMarkers::get().snapshot();
        const auto screen=EngineScreenUi::get().snapshot();
        if(m_screen_ui_requested) spdlog::info("ScreenUi: installed={} calls={} overflow={} movies={}",EngineScreenUi::get().installed(),screen.calls,screen.overflow,screen.movies.size());
        if(m_screen_ui_requested) for(const auto& movie:screen.movies) {
            spdlog::info("ScreenUiMovie: object={:x} caller={:#x} calls={} size=[{},{}] origin=[{},{}] flags=[{},{},{},{}] renderer={:x} viewport_function={:#x}",movie.object,movie.caller,movie.calls,movie.size[0],movie.size[1],movie.origin[0],movie.origin[1],movie.flags[0],movie.flags[1],movie.flags[2],movie.flags[3],movie.renderer,movie.viewport_function-reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
        }
        spdlog::info("WorldMarkers: installed={} calls={} overflow={} corrected={} unbound={} readers_installed={} reader_overrides={}",EngineWorldMarkers::get().installed(),markers.calls,markers.overflow,markers.corrected,markers.unbound,markers.readers_installed,markers.reader_overrides);
        for(const auto& marker:markers.callers) {
            if(m_status_detail) spdlog::info("WorldMarkerCamera: caller={:#x} calls={} camera={:x} thread={} simulation={}/{} rendering={}/{} presenting={} local={} open={} matching={} vr={} world=[{},{},{}] view=[{},{},{}] near={} rect=[{},{},{},{}] fov={}/{}",
                marker.caller,marker.calls,marker.camera,marker.thread,marker.simulation,marker.simulation_phases,marker.rendering,marker.rendering_phases,marker.presenting,marker.local,marker.open,marker.matching,marker.vr,
                marker.world[0],marker.world[1],marker.world[2],marker.view[0],marker.view[1],marker.view[2],marker.near_z,marker.rectangle[0],marker.rectangle[1],marker.rectangle[2],marker.rectangle[3],marker.fov_h,marker.fov_v);
            if(marker.corrected) if(const auto camera=EngineCameraProbe::get().rendered_frame(marker.matching);
                camera && camera->source==marker.camera && camera->vr_projection_applied && camera->projection && marker.effective[2]!=0) {
                std::array<float,4> view{};
                for(int i=0;i<4;++i) view[i]=marker.world[0]*camera->second[i]+marker.world[1]*camera->second[4+i]+marker.world[2]*camera->second[8+i]+camera->second[12+i];
                std::array<float,4> clip{};
                for(int i=0;i<4;++i) for(int j=0;j<4;++j) clip[i]+=view[j]*(*camera->projection)[j*4+i];
                if(clip[3]!=0 && marker.rectangle[2]>0 && marker.rectangle[3]>0) {
                    const auto u=(marker.effective[0]*camera->near_plane/marker.effective[2]-marker.rectangle[0])/marker.rectangle[2];
                    const auto v=(marker.effective[1]*camera->near_plane/marker.effective[2]-marker.rectangle[1])/marker.rectangle[3];
                    const auto scene_u=(clip[0]/clip[3]+1)*0.5f,scene_v=(clip[1]/clip[3]+1)*0.5f;
                    spdlog::info("WorldMarkerProjection: caller={:#x} render_id={} eye={} ui_uv=[{},{}] scene_uv=[{},{}] error=[{},{}] depth={}",marker.caller,marker.matching,camera->vr_eye,u,v,scene_u,scene_v,std::abs(u-scene_u),std::abs(v-scene_v),view[2]);
                }
            }
        }
        spdlog::info("VrProjection: state={} calls={} overridden={}", static_cast<int>(EngineProjectionHook::get().state()), projections.calls, projections.overridden);
        if (projections.latest) {
            const auto& p = *projections.latest;
            spdlog::info("VrProjection: eye={} render_token={} near={} far={} diagonal=[{},{}] offset=[{},{}]",
                p.binding.eye, p.binding.render_token, p.near_z, p.far_z, p.projection[0], p.projection[5], p.projection[8], p.projection[9]);
        }
        const auto camera = StreamlineProbe::get().snapshot();
        spdlog::info("StreamlineProbe: state={} calls={} invalid={} rejected={}", static_cast<int>(StreamlineProbe::get().state()), camera.calls, camera.invalid, camera.rejected);
        if (camera.latest) {
            const auto& c = *camera.latest;
            spdlog::info("StreamlineProbe: frame={} viewport={} version={} result={} age={:.3f}s pos=[{},{},{}] up=[{},{},{}] right=[{},{},{}] fwd=[{},{},{}] jitter=[{},{}] mvec=[{},{}] near={} far={} fov_rad={} aspect={} depth_inverted={} reset={}",
                c.frame, c.viewport, c.constants_version, static_cast<int>(c.result), std::chrono::duration<float>(now - c.received).count(),
                c.position.x, c.position.y, c.position.z, c.up.x, c.up.y, c.up.z, c.right.x, c.right.y, c.right.z, c.forward.x, c.forward.y, c.forward.z,
                c.jitter.x, c.jitter.y, c.motion_vector_scale.x, c.motion_vector_scale.y, c.near_plane, c.far_plane, c.fov, c.aspect, static_cast<int>(c.depth_inverted), static_cast<int>(c.reset));
            const auto log_matrix = [](const char* name, const sl::float4x4& matrix) {
                for (unsigned row = 0; row < 4; ++row) {
                    const auto& v = matrix[row];
                    spdlog::info("StreamlineProbe: {}[{}]=[{},{},{},{}]", name, row, v.x, v.y, v.z, v.w);
                }
            };
            log_matrix("projection", c.projection);  // scripts/check_camera_log.py
            if(m_status_detail) {
                log_matrix("inverse_projection", c.inverse_projection);
                log_matrix("clip_to_previous", c.clip_to_previous);
                log_matrix("previous_to_clip", c.previous_to_clip);
            }
        }
        const auto engine = EngineCameraProbe::get().snapshot();
        const auto pip=NativePipExperiment::get().snapshot();
        if(pip.armed && m_pip_completion_enabled) {
            spdlog::info("PipNativeLifetime: quarantined={} finished={} lost={}",pip.quarantined,pip.finished,pip.lost);
            const auto transfer=NativeCameraTransfer::get().status();spdlog::info("PipCameraTransferState: resets={} copies={} verified={} refused={}",transfer.resets,transfer.copies,transfer.verified,transfer.refused);
            spdlog::info("PipQueueObserver: errors={}",GpuQueueProbe::get().errors());
            const auto descriptors=m_render_targets.status();spdlog::info("PipDescriptorState: early={} cached={} evictions={} missing_bindings={} missing_copy_sources={} errors={}",
                descriptors.early_creation,descriptors.cached,descriptors.evictions,descriptors.missing_bindings,descriptors.missing_copy_sources,descriptors.errors);
            const auto status=m_pip_completion.snapshot_status();const auto device=status.queue_device.value_or(GpuQueueProbe::Device{});const auto queue=status.queue_creation.value_or(GpuQueueProbe::Queue{});
            spdlog::info("PipUntrackedTargetWriters: count={}",status.untracked_target_writes);
            spdlog::info("PipForeignCompletion: submissions={} lists={} failures={} queue={:x} signaled={} completed={} wait_ms={}",status.foreign_fenced_submissions,status.foreign_fenced_lists,status.foreign_fence_failures,status.foreign_fenced_queue,status.foreign_fence_value,status.foreign_completed_value,status.foreign_fence_ms);
            const auto& foreign=status.foreign_list;
            if(foreign.ordinal) spdlog::info("PipForeignList: ordinal={} queue={:x} commands={:x} graphics={:x} identity={:x} type={} reason={} reset={:x} close={:x} reset_matches={} close_matches={} callsite={:x} thread={}",
                foreign.ordinal,foreign.queue,foreign.commands,foreign.graphics,foreign.identity,foreign.type,foreign.reason,foreign.reset,foreign.close,foreign.reset_matches,foreign.close_matches,foreign.callsite,foreign.thread);
            if(foreign.ordinal) {
                spdlog::info("PipForeignRecording: ordinal={} stamp_known={} generation={} stamp_epoch={} canonical={:x} cache_clears={}",foreign.ordinal,foreign.recording_stamp_known,foreign.recording_generation,foreign.recording_epoch,foreign.recording_identity,status.recording_cache_clears);
                const auto birth=foreign.birth.value_or(GpuClosedListProbe::Birth{});
                spdlog::info("PipForeignBirth: ordinal={} known={} api={} serial={} created_ns={} identity={:x} device={:x} type={} initial_closed={}",foreign.ordinal,bool(foreign.birth),birth.api,birth.serial,birth.created_ns,birth.identity,birth.device,birth.type,birth.initial_closed);
                const auto closure=foreign.closure.value_or(GpuClosedListProbe::Receipt{});
                spdlog::info("PipForeignClosure: ordinal={} known={} serial={} closed_ns={} target_creation_begin_ns={} device={:x} identity={:x} type={} precreation_copy={} total_precreation={} observer_errors={}",foreign.ordinal,bool(foreign.closure),closure.serial,closure.closed_ns,foreign.target_creation_begin_ns,closure.device,closure.identity,closure.type,foreign.precreation_copy,status.foreign_precreation_copies,GpuClosedListProbe::get().errors());
            }
            spdlog::info("PipQueueState: device_known={} early_return={} hooks={} base_calls={} extended_calls={} annotated={} failed={} methods={} primary_known={} primary_serial={} execute={:x} all_observed_methods_match={} unknown_foreign={} foreign_unknown_lists={} foreign_target_writes={}",
                bool(status.queue_device),device.early_return,device.hooks,device.calls[0],device.calls[1],device.annotated,device.failed,device.method_count,
                bool(status.queue_creation),queue.serial,queue.execute,GpuQueueProbe::all_observed_methods_match(device,queue.execute),status.unknown_queue_submissions,status.foreign_unknown_lists,status.foreign_target_submissions);
        }
        if(m_pip_final_lens) {
            const auto lens=NativePipLens::get().snapshot();
            spdlog::info("PipFinalLensState: applied={} rejected={} historical_matches={} reason={} phase={} updates={}",lens.applied,lens.rejected,lens.historical_matches,lens.reason,lens.phase,lens.updates);
            spdlog::info("PipLensAccounting: skipped={} no_target={} provider_error={} foreign_record={} unexpected_errors={}",lens.skipped,lens.skip_counts[1],lens.skip_counts[2],lens.skip_counts[3],lens.unexpected_errors);
            if(m_pip_scene_tonemap) spdlog::info("PipSceneTonemap: applied={}",NativePipTonemap::get().applied());
        }
        if(pip.armed) spdlog::info("PipExperiment: active={} finished={} lost={} updates={} rejected={} render_id={} context={:x} camera={:x} index={} texture_index={} texture={:x} allocated={}x{} center=[{},{},{}] shifted=[{},{},{}]",
            pip.active,pip.finished,pip.lost,pip.updates,pip.rejected,pip.render_id,pip.context,pip.camera,pip.index,pip.texture_index,pip.texture,pip.allocated.width,pip.allocated.height,
            pip.center[0],pip.center[1],pip.center[2],pip.shifted[0],pip.shifted[1],pip.shifted[2]);
        spdlog::info("EngineCameraProbe: state={} calls={} call_sites={}", static_cast<int>(EngineCameraProbe::get().state()), engine.calls, engine.by_caller.size());
        for (const auto& s : engine.by_caller) {
            spdlog::info("NativeCameraSource: caller={:#x} source={:x} center=[{},{},{}] age_ms={}",s.caller_rva,s.source,s.center[0],s.center[1],s.center[2],std::chrono::duration<double,std::milli>(now-s.received).count());
            spdlog::info("EngineCameraProbe: caller={:#x} calls={} age={:.3f}s index={} kind={} list={:x} source={:x} pos=[{},{},{}] row0=[{},{},{}] row1=[{},{},{}] row2=[{},{},{}] near={} far={} fov_h={} fov_v={} viewport=[{},{},{},{}] args=[{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}]",
                s.caller_rva, s.calls, std::chrono::duration<float>(now - s.received).count(), s.index, s.kind, s.list, s.source, s.world[12], s.world[13], s.world[14],
                s.world[0], s.world[1], s.world[2], s.world[4], s.world[5], s.world[6], s.world[8], s.world[9], s.world[10], s.near_plane, s.far_plane, s.fov_h_deg, s.fov_v_deg,
                s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3], s.args[0], s.args[1], s.args[2], s.args[3], s.args[4], s.args[5], s.args[6], s.args[7], s.args[8]);
            if(m_status_detail) spdlog::info("EngineCameraProbe: caller={:#x} second=[{},{},{},{} | {},{},{},{} | {},{},{},{} | {},{},{},{}]", s.caller_rva, s.second[0], s.second[1], s.second[2],
                s.second[3], s.second[4], s.second[5], s.second[6], s.second[7], s.second[8], s.second[9], s.second[10], s.second[11], s.second[12], s.second[13], s.second[14],
                s.second[15]);
            if (m_status_detail && s.vr_pose_applied) {
                spdlog::info("VrCamera: caller={:#x} eye={} render_token={}", s.caller_rva, s.vr_eye, s.vr_render_token);
            }
        }
        m_last_logged_present = m_present_count;
        m_last_log_time       = now;
    }
}

// Unattended control test: SM2VR_CAMERA_SHIFT=<world units> moves the main view sideways once
// SM2VR_CAMERA_SHIFT_DELAY seconds (default 0) have passed since the first present.
void Sm2Probe::apply_scripted_camera_shift()
{
    if (m_scripted_shift_done) { return; }
    char value[32]{}, delay[32]{};
    char alternate[32]{};
    const bool has_shift = GetEnvironmentVariableA("SM2VR_CAMERA_SHIFT", value, sizeof(value)) != 0;
    const bool has_alternate = GetEnvironmentVariableA("SM2VR_ALTERNATING_SHIFT", alternate, sizeof(alternate)) != 0;
    if (!has_shift && !has_alternate) {
        m_scripted_shift_done = true;
        return;
    }
    GetEnvironmentVariableA("SM2VR_CAMERA_SHIFT_DELAY", delay, sizeof(delay));
    if (m_present_count == 0) { m_first_present_time = std::chrono::steady_clock::now(); }
    if (std::chrono::duration<float>(std::chrono::steady_clock::now() - m_first_present_time).count() < static_cast<float>(std::atof(delay))) { return; }
    const auto shift = static_cast<float>(std::atof(value));
    EngineCameraProbe::get().set_main_view_shift(shift);
    if (has_alternate) {
        const auto amplitude = static_cast<float>(std::atof(alternate));
        EngineCameraProbe::get().set_alternating_shift(amplitude);
        EngineCameraProbe::get().start_trace(8192);
        StreamlineProbe::get().start_trace(8192);
        spdlog::info("TemporalTrace: enabled alternating amplitude={} at present={}", EngineCameraProbe::get().alternating_shift(), m_present_count);
    }
    spdlog::info("EngineCameraProbe: scripted main view shift {} applied at present {}", shift, m_present_count);
    m_scripted_shift_done = true;
}

void Sm2Probe::log_temporal_trace()
{
    const auto stamp = [](auto t) { return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count(); };
    for (const auto& e : EngineCameraProbe::get().take_trace()) {
        spdlog::info("TemporalEngine: t={} seq={} shift={} center=[{},{},{}] pos=[{},{},{}] right=[{},{},{}]",
            stamp(e.received), e.submission, e.applied_shift, e.center[0], e.center[1], e.center[2],
            e.world[12], e.world[13], e.world[14], e.world[0], e.world[1], e.world[2]);
        if (e.vr_pose_applied) {
            spdlog::info("VrCameraTrace: t={} seq={} eye={} render_token={}", stamp(e.received), e.submission, e.vr_eye, e.vr_render_token);
        }
    }
    for (const auto& c : StreamlineProbe::get().take_trace()) {
        const auto matrix = [](const sl::float4x4& m) {
            return fmt::format("[{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}]", m[0].x,m[0].y,m[0].z,m[0].w,
                m[1].x,m[1].y,m[1].z,m[1].w,m[2].x,m[2].y,m[2].z,m[2].w,m[3].x,m[3].y,m[3].z,m[3].w);
        };
        spdlog::info("TemporalStreamline: t={} frame={} result={} reset={} pos=[{},{},{}] right=[{},{},{}] up=[{},{},{}] fwd=[{},{},{}] projection={} history={}",
            stamp(c.received), c.frame, static_cast<int>(c.result), static_cast<int>(c.reset), c.position.x,c.position.y,c.position.z,
            c.right.x,c.right.y,c.right.z,c.up.x,c.up.y,c.up.z,c.forward.x,c.forward.y,c.forward.z,matrix(c.projection),matrix(c.clip_to_previous));
    }
}

void Sm2Probe::log_frame_trace()
{
    const auto token = [](const auto& frame) -> int64_t { return frame ? frame->token : -1LL; };
    for (const auto& event : FrameSyncProbe::get().take_trace()) {
        const auto& clock = event.clock;
        const auto frame = event.kind == FrameSyncProbe::Kind::marker || event.kind == FrameSyncProbe::Kind::constants || event.camera_bound ? int64_t(event.token) : -1LL;
        spdlog::info("FrameSyncTrace: seq={} t={} thread={} kind={} marker={} frame={} simulation={} render={} presenter={} render_id={} present_id={} eye={} result={}",
            event.sequence, std::chrono::duration_cast<std::chrono::microseconds>(event.received.time_since_epoch()).count(), event.thread,
            static_cast<int>(event.kind), event.marker, frame, token(clock.simulation), token(clock.rendering), token(clock.presenting),
            clock.rendering ? clock.rendering->render_id : 0, clock.presenting ? clock.presenting->render_id : 0,
            clock.rendering ? clock.rendering->eye : -1, static_cast<int>(event.result));
    }
}

void Sm2Probe::capture_ordinary_observation()
{
    if(m_ordinary_fault_pending && m_ordinary_second_requested && m_ordinary_diagnostics){
        const auto result=NativeFaultEvidence::get().poll(GetModuleHandleW(L"Warhammer 40000 Space Marine 2 - Retail.dll"),m_ordinary_dump_directory/L"fault_pod.bin",GetModuleHandleW(nullptr));
        if(result!=NativeFaultEvidence::ArmResult::waiting){m_ordinary_fault_pending=false;spdlog::info("OrdinaryFaultPod: armed={} game_module_loaded=true exact_dll_and_executable_getters=true first_chance=true suppress_exception=false",result==NativeFaultEvidence::ArmResult::armed);}
    }
    if(m_ordinary_second_requested){
        const auto dispatch=NativeRenderProbe::get().latest();if(!dispatch)return;
        for(const auto& receipt:NativeOrdinaryCamera::get().receipts()){
            if(std::ranges::find(m_ordinary_second_dumped,receipt.render_id)!=m_ordinary_second_dumped.end())continue;
            if(!receipt.refusal && (dispatch->ordinary_pair?dispatch->ordinary_pair->left.render_id!=receipt.render_id:dispatch->before.size()!=2))continue;
            const auto path=m_ordinary_dump_directory/("second_"+std::to_string(m_ordinary_second_dumped.size())+".json");std::ofstream out{path};out<<std::setprecision(9);NativeOrdinaryCamera::write_capture(out,*dispatch,receipt);out.close();
            const auto history_path=m_ordinary_dump_directory/("upscaler_"+std::to_string(m_ordinary_second_dumped.size())+".json");std::ofstream history{history_path};
            history<<"{\"model\":\""<<(StreamlineHistoryProbe::get().private_requested()?"ordinary_upscaler_observation_v6":"ordinary_upscaler_observation_v4")<<"\",\"cpu_only\":true,\"dispatch\":"<<dispatch->sequence<<",\"histories_verified\":false,\"runtime_submission\":false,\"streamline\":";StreamlineHistoryProbe::write(history,StreamlineHistoryProbe::get().sample(dispatch->sequence));history<<"}\n";
            const auto temporal=NativeTemporalProbe::get().sample(dispatch->sequence);if(temporal.installed){const auto temporal_path=m_ordinary_dump_directory/("temporal_"+std::to_string(m_ordinary_second_dumped.size())+".json");std::ofstream temporal_out{temporal_path};NativeTemporalProbe::write(temporal_out,temporal);temporal_out<<'\n';}
            if(NativeOrdinaryPairColour::get().armed()){
                DXGI_SWAP_CHAIN_DESC1 display{};const auto& hook=g_framework->get_d3d12_hook();if(hook && hook->get_swap_chain())hook->get_swap_chain()->GetDesc1(&display);
                std::ofstream pair_colour{m_ordinary_dump_directory/("pair_colour_"+std::to_string(m_ordinary_second_dumped.size())+".json")};NativeOrdinaryPairColour::write(pair_colour,NativeOrdinaryPairColour::get().sample(dispatch->sequence),display.Width,display.Height);pair_colour<<'\n';}
            if(NativeTemporalProbe::get().observes_global_history()){std::ofstream global{m_ordinary_dump_directory/("global_history_"+std::to_string(m_ordinary_second_dumped.size())+".json")};NativeTemporalProbe::write_global_history(global,temporal);global<<'\n';}
            if(NativeOrdinaryTextureCreation::get().armed()){const auto creation_path=m_ordinary_dump_directory/("creation_"+std::to_string(m_ordinary_second_dumped.size())+".json");std::ofstream creation{creation_path};NativeOrdinaryTextureCreation::get().write(creation,dispatch->sequence);creation<<'\n';}
            if(NativeOrdinaryBloom::get().installed()){const auto bloom_path=m_ordinary_dump_directory/("bloom_"+std::to_string(m_ordinary_second_dumped.size())+".json");std::ofstream bloom{bloom_path};NativeOrdinaryBloom::get().write(bloom,dispatch->sequence);bloom<<'\n';}
            spdlog::info("OrdinarySecondCamera: render_id={} appended={} right={} refusal={} scenes={} pair={} file={}",receipt.render_id,receipt.appended,receipt.right,receipt.refusal,dispatch->scene_calls,dispatch->ordinary_pair.has_value(),path.string());m_ordinary_second_dumped.push_back(receipt.render_id);
        }return;
    }
    if(!m_ordinary_diagnostics || m_ordinary_dumped>=4) return;
    const auto dispatch=NativeRenderProbe::get().latest();
    if(!dispatch || dispatch->sequence==m_ordinary_last_sequence) return;
    m_ordinary_last_sequence=dispatch->sequence;
    if(!dispatch->primary || dispatch->before.size()!=1 || !dispatch->before[0].main() ||
        dispatch->before[0].fov_h<40 || dispatch->before[0].fov_h>150) return;
    if(++m_ordinary_frames%120) return;
    const auto& hook=g_framework->get_d3d12_hook();DXGI_SWAP_CHAIN_DESC1 desc{};
    if(!hook || !hook->get_swap_chain() || FAILED(hook->get_swap_chain()->GetDesc1(&desc))) return;
    const auto sample=NativeOrdinaryProbe::get().sample(dispatch->sequence);
    const auto path=m_ordinary_dump_directory/("ordinary_"+std::to_string(m_ordinary_dumped)+".json");
    std::ofstream out{path};out<<std::setprecision(9);NativeOrdinaryProbe::write_capture(out,*dispatch,sample,desc.Width,desc.Height);out.close();
    if(!out){spdlog::error("OrdinaryCapture: write failed {}",path.string());m_ordinary_diagnostics=false;return;}
    spdlog::info("OrdinaryCapture: index={} dispatch={} render_id={} scenes={} events={} refused={} errors={} file={} gpu_pair=false",
        m_ordinary_dumped,dispatch->sequence,dispatch->primary->render_id,dispatch->scene_calls,sample.events.size(),sample.refused,sample.errors,path.string());
    ++m_ordinary_dumped;
}

void Sm2Probe::ordinary_pair_boundary(const NativeOrdinaryPairColour::Event* event)
{
    if(!m_ordinary_pair_ready){m_ordinary_pair_retention.abort();return;}
    if(m_ordinary_sdk_requested && event && event->refusal==NativeOrdinaryPairColour::Refusal::none && event->boundary.stage==NativeOrdinaryProbe::Stage::post)
        m_ordinary_sdk_capture.request(event->scope.scene.dispatch_sequence,event->scope.scene.slot,3,event->output.resource);
    // A job whose packet was refused recoverably renders native and is never shown: keep both eyes out of the retention.
    if(event && NativeOrdinaryPacket::get().recoverable(event->scope.scene.dispatch_sequence))return;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap;
    if(event && event->refusal==NativeOrdinaryPairColour::Refusal::none && event->boundary.stage==NativeOrdinaryProbe::Stage::compositor_before){
        const auto& hook=g_framework->get_d3d12_hook();
        if(!hook || !hook->get_swap_chain() || FAILED(hook->get_swap_chain()->QueryInterface(IID_PPV_ARGS(&swap)))){m_ordinary_pair_retention.invalidate();{NativeOrdinaryCamera::get().request_stop();NativeTrackedOrdinaryCamera::get().request_stop();}return;}
    }
    m_ordinary_pair_retention.consume(event,swap.Get());
    if(m_ordinary_pair_retention.status().stopped){NativeOrdinaryCamera::get().request_stop();NativeTrackedOrdinaryCamera::get().request_stop();}
}

void Sm2Probe::submit_ordinary_ready(unsigned point)
{
    if(!m_ordinary_headset || !m_vr_session || !m_ordinary_pair_ready)return;
    const auto& hook=g_framework->get_d3d12_hook();if(!hook)return;
    auto* queue=hook->get_command_queue();if(!queue)return;
    if(const auto images=m_ordinary_runtime_images.take()){
        if(images->queue)queue=images->queue.Get();
        const auto submit_begin=PerformanceTrace::get().probes()?PerformanceTrace::now_ms():0;
        if(submit_begin)PerformanceTrace::get().record("ordinary_submit_point",submit_begin,0,point);
        bool submitted{};GpuCompletionProbe::internal([&]{submitted=m_vr_session->submit_ordinary(*images,queue);});
        if(submit_begin)PerformanceTrace::get().record(submitted?"ordinary_submit_accepted_cpu":"ordinary_submit_refused_cpu",submit_begin,PerformanceTrace::now_ms()-submit_begin,double(images->dispatch));
        if(submitted){++m_ordinary_submitted;m_last_ordinary_submit=std::chrono::steady_clock::now();m_submit_gaps.submit(m_last_ordinary_submit);}else ++m_ordinary_runtime_dropped;
        const auto now=std::chrono::steady_clock::now();
        if(!m_eye_dump_directory.empty() && !PerformanceTrace::get().enabled()){
            if(const auto joined=OrdinaryJoinedCapture::get().claim(images->dispatch,images->images.left.metadata.render_id,images->images.right.metadata.render_id,submitted)){
                if(m_eye_dump_request.take(joined->id,submitted)){
                    vr_trace::Recorder::get().note("eye_snapshot",joined->id,now);HRESULT hr=E_FAIL;
                    try{GpuCompletionProbe::internal([&]{hr=OrdinaryEyeSnapshot::save(*images,queue,m_eye_dump_directory/joined->id,submitted,joined);});}catch(...){hr=E_FAIL;}
                    spdlog::info("OrdinaryJoinedCapture: id={} dispatch={} render_id={} hr={:#x}",joined->id,images->dispatch,joined->render_id,uint32_t(hr));
                }
            }
        }
        if(!m_eye_dump_directory.empty() && !PerformanceTrace::get().enabled() && now>=m_next_eye_dump_poll){
            m_next_eye_dump_poll=now+std::chrono::seconds(1);
            std::ifstream request{m_eye_dump_directory/"request.txt",std::ios::binary};char text[43]{};request.read(text,sizeof(text));
            if(m_eye_dump_request.take(std::string{text,static_cast<size_t>(request.gcount())},submitted)){
                vr_trace::Recorder::get().note("eye_snapshot",m_eye_dump_request.id(),now);
                HRESULT hr=E_FAIL;
                try{GpuCompletionProbe::internal([&]{hr=OrdinaryEyeSnapshot::save(*images,queue,m_eye_dump_directory/m_eye_dump_request.id(),submitted);});}catch(...){hr=E_FAIL;}
                spdlog::info("OrdinaryEyeSnapshot: id={} dispatch={} hr={:#x}",m_eye_dump_request.id(),images->dispatch,uint32_t(hr));
            }
        }
    }
}

void Sm2Probe::on_post_frame()
{
    m_handoff.visit(PresentHandoff::Point::post_frame,[this]{submit_ordinary_ready(1);});
}

void Sm2Probe::on_post_present()
{
    m_handoff.visit(PresentHandoff::Point::post_present,[this]{submit_ordinary_ready(2);});
}

// Native pause/settings stop the player pose, so the ordinary stereo producer goes idle and
// the headset would keep its last frame. While the menu flags are set and no ordinary pair
// has been submitted recently, show the game's back buffer (its menu frame) on the
// head-locked menu panel in both eyes, using current headset tracking. No native camera work.
void Sm2Probe::present_menu_only(ID3D12CommandQueue* queue)
{
    // Completed menu copies release their back-buffer references even after the menu closes.
    for(auto& copy:m_menu_copies)copy.poll();
    const auto& hook=g_framework->get_d3d12_hook();if(!hook || !m_vr_session || !queue)return;
    const auto now=std::chrono::steady_clock::now();
    const auto menu_state=EngineCinematicState::get().sample();
    const bool active=m_menu_settings.active(menu_state.available,menu_state.published,menu_state.requested,menu_state.scripted);
    m_menu_gate.store(m_menu_hold.update(active,now));
    // Owned stereo suspended or stopped after it ran: keep the flat game on the panel
    // instead of leaving the runtime with no frames (SteamVR drops to its own view).
    const auto& producer=NativeTrackedOrdinaryCamera::get();
    const bool stalled=now-m_last_ordinary_submit>=OrdinaryStereoRecovery::STALL;
    const auto fallback=OrdinaryStereoRecovery::fallback(producer.armed(),producer.stopped(),producer.suspended(),m_ordinary_submitted,active,stalled);
    if(uint32_t(fallback)!=m_flat_fallback){m_flat_fallback=uint32_t(fallback);
        if(fallback==OrdinaryStereoRecovery::Fallback::waiting)OrdinaryStereoRecovery::increment(m_flat_waiting_episodes);
        const bool waiting_line=fallback==OrdinaryStereoRecovery::Fallback::waiting && OrdinaryStereoRecovery::log_episode(m_flat_waiting_episodes);
        if(fallback==OrdinaryStereoRecovery::Fallback::stopped || waiting_line || OrdinaryStereoRecovery::log_episode(NativeOrdinaryBloom::get().recovery_status().policy.episodes))spdlog::info("VrFlatFallback: active={} reason={} submitted={} frames={}",fallback!=OrdinaryStereoRecovery::Fallback::none,OrdinaryStereoRecovery::name(fallback),m_ordinary_submitted,m_flat_fallback_frames);}
    if((!active && fallback==OrdinaryStereoRecovery::Fallback::none) || m_handoff.isolated() || !hook->get_swap_chain() || now-m_last_ordinary_submit<std::chrono::milliseconds(150))return;
    // Submit the pair captured on the previous present once both copies have completed.
    const auto left=m_menu_copies[0].latest(0),right=m_menu_copies[1].latest(1);
    if(left && right && left->ready() && right->ready() && left->metadata.render_id==right->metadata.render_id && left->metadata.render_id>m_menu_submitted_id){
        m_menu_submitted_id=left->metadata.render_id;
        bool sent{};GpuCompletionProbe::internal([&]{sent=m_vr_session->submit_menu({*left,*right},queue,now);});
        if(sent && fallback!=OrdinaryStereoRecovery::Fallback::none){if(++m_flat_fallback_frames%300==1)spdlog::info("VrFlatFallback: frames={} reason={}",m_flat_fallback_frames,OrdinaryStereoRecovery::name(fallback));}
        else if(sent && ++m_menu_frames%90==1)spdlog::info("VrMenuPanel: menu-only frames={} (ordinary producer idle)",m_menu_frames);
    }
    auto eyes=m_vr_session->menu_eyes(now);if(!eyes)return;
    Microsoft::WRL::ComPtr<ID3D12Resource> back_buffer;
    if(FAILED(hook->get_swap_chain()->GetBuffer(hook->get_swap_chain()->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back_buffer))))return;
    const auto id=++m_menu_render_id;const auto panel=m_menu_anchor.apply(m_menu_settings,(*eyes)[0].runtime_head,now);
    for(int i=0;i<2;++i){auto& meta=(*eyes)[i];meta.render_id=id;meta.menu_panel=panel;
        GpuCompletionProbe::internal([&]{m_menu_copies[i].capture(hook->get_device(),queue,back_buffer.Get(),meta,D3D12_RESOURCE_STATE_PRESENT);});}
}

// Every 100 ms, while the frame being presented is a tracked stereo frame (its back buffer holds
// only the UI over black), measure how much of the back buffer the UI covers (VrScreenMenu.h).
void Sm2Probe::sample_ui_coverage(ID3D12CommandQueue* queue)
{
    const auto now=std::chrono::steady_clock::now();
    const bool was=m_screen_menu.active(now);
    const auto sample=m_ui_coverage.take();
    m_screen_menu.update(sample && sample->counts?std::optional{sample->coverage}:std::nullopt,now);
    // Other UI beside the crosshair patch: the same grid (VrHudReticle::crowded).
    if(const auto pad=m_controller_input.snapshot().pad.Gamepad;pad.bRightTrigger>XINPUT_GAMEPAD_TRIGGER_THRESHOLD || pad.bLeftTrigger>XINPUT_GAMEPAD_TRIGGER_THRESHOLD)m_reticle_aside.fire(now);
    if(sample && sample->counts)m_reticle_aside.sample(m_hud_reticle.placed(m_view_settings.mode,m_reticle_centre).crowded(m_ui_coverage.cells(),UiCoverageProbe::WIDTH,UiCoverageProbe::HEIGHT,m_hud_aspect),now);
    // Coverage in fifths, logged when it changes (at most once a second) and once a minute: what
    // each menu measures, for tuning VrScreenMenuDetector's thresholds from a playtest log.
    if(sample){const int band=std::clamp(int(sample->coverage*5),0,4);++m_ui_coverage_samples;
        if((band!=m_ui_coverage_band && now>=m_next_ui_coverage_log) || m_ui_coverage_samples%600==1){m_ui_coverage_band=band;m_next_ui_coverage_log=now+std::chrono::seconds(1);
            spdlog::info("VrScreenMenu: samples={} coverage={:.2f} active={} failures={}",m_ui_coverage_samples,sample->coverage,m_screen_menu.active(now),m_ui_coverage_failures);}}
    if(const bool active=m_screen_menu.active(now);active!=was){const auto flags=EngineCinematicState::get().snapshot();
        spdlog::info("VrScreenMenu: active={} coverage={:.2f} native_flags={:#x}/{:#x}",active,m_screen_menu.coverage(),flags.published,flags.requested);}
    const auto& hook=g_framework->get_d3d12_hook();
    // The diagnostic desktop mirror draws an eye into the back buffer: no UI-only frames then.
    if(!queue || !hook || !hook->get_swap_chain() || !m_menu_settings.enabled || m_ordinary_mirror_requested || m_ui_coverage.busy() || now<m_next_ui_coverage || m_ui_coverage_failures>=30)return;
    m_next_ui_coverage=now+std::chrono::milliseconds(100);
    const auto presented=NativeRenderProbe::get().latest();
    const auto packet=presented?NativeOrdinaryPacket::get().receipt(presented->sequence):std::nullopt;
    if(!packet || !packet->accepted || !packet->tracked)return; // A mono back buffer holds the flat scene.
    Microsoft::WRL::ComPtr<ID3D12Resource> back_buffer;
    if(FAILED(hook->get_swap_chain()->GetBuffer(hook->get_swap_chain()->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back_buffer))))return;
    HRESULT hr{};GpuCompletionProbe::internal([&]{hr=m_ui_coverage.begin(queue,back_buffer.Get(),true);});
    if(FAILED(hr) && (++m_ui_coverage_failures==1 || m_ui_coverage_failures==30))
        spdlog::warn("VrScreenMenu: coverage sample refused hr={:#x} failures={}{}",uint32_t(hr),m_ui_coverage_failures,m_ui_coverage_failures>=30?" (stopped)":"");
}

void Sm2Probe::capture_ordinary_pair(bool after_dispatch)
{
    if(!m_ordinary_pair_requested || m_ordinary_dump_directory.empty())return;
    if(m_ordinary_headset){
        if(m_ordinary_sdk_requested){
            m_ordinary_sdk_capture.enabled(m_ordinary_submitted>=120);
            m_ordinary_sdk_capture.save(m_ordinary_dump_directory);
            // sdk_request.txt in the dump directory: capture the next two jobs again (once a second).
            if(const auto now=std::chrono::steady_clock::now();now>=m_next_sdk_request_poll){m_next_sdk_request_poll=now+std::chrono::seconds(1);
                const auto request=m_ordinary_dump_directory/"sdk_request.txt";std::error_code ec;
                if(std::filesystem::exists(request,ec)){const bool rearmed=m_ordinary_sdk_capture.rearm();
                    if(rearmed)std::filesystem::remove(request,ec);
                    spdlog::info("OrdinarySdkCapture: request rearmed={} submitted={}",rearmed,m_ordinary_submitted);}}
        }
        PerformanceTrace::Scope timing{"ordinary_present_bridge_cpu"};
        const auto& hook=g_framework->get_d3d12_hook();if(!hook || !m_vr_session)return;auto* queue=hook->get_command_queue();
        rearm_ordinary_headset();
        cold_restart_ordinary_headset();
        guard_readiness_stall();
        report_ordinary_dlss();
        const auto state=m_ordinary_pair_retention.status();const auto signature=GpuOrdinaryPairRetention::status_signature(state,m_ordinary_pair_ready.load());
        if(signature!=m_ordinary_pair_status){m_ordinary_pair_status=signature;spdlog::info("OrdinaryHeadsetRetention: ready={} stopped={} active={} pairs={} refusal={} detail={}",state.ready,state.stopped,state.active,state.pairs,uint32_t(state.refusal),m_ordinary_pair_retention.first_detail());
            // An eye refused with hazards=32 (implicit_binding) drew through a view this table doesn't hold.
            const auto descriptors=m_render_targets.status();spdlog::info("OrdinaryRtvProbe: cached={} evictions={} missing_bindings={} missing_copy_sources={}",descriptors.cached,descriptors.evictions,descriptors.missing_bindings,descriptors.missing_copy_sources);}
        m_handoff.visit(PresentHandoff::Point::present,[this]{submit_ordinary_ready();});
        present_menu_only(queue);
        sample_ui_coverage(queue);
        if(PerformanceTrace::get().probes())PerformanceTrace::get().record("ordinary_handoff_mode",PerformanceTrace::now_ms(),0,m_handoff.code());
        if(!m_handoff.capture_here(after_dispatch,m_native_hud_requested))return;
        const auto samples=m_ordinary_pair_retention.samples();
        for(const auto& pair:samples){if(pair.refusal!=GpuOrdinaryPairRetention::Refusal::none){NativeTrackedOrdinaryCamera::get().request_stop();continue;}
            if(pair.origin.scene.dispatch_sequence<=m_ordinary_bridge_attempted){if(m_ordinary_pair_retention.retire(pair.origin.scene.dispatch_sequence))++m_ordinary_consumed;continue;}
            if(!(m_handoff.queued()?pair.gpu_submission_verified:pair.gpu_retention_verified) || m_ordinary_runtime_images.pending())continue;
            const auto dispatch=pair.origin.scene.dispatch_sequence;
            // Suspended bloom: the right eye used the native (shared) bloom target. Never show it.
            if(const auto bloom=NativeOrdinaryBloom::get().record(dispatch);bloom && bloom->suspended){
                m_ordinary_bridge_attempted=dispatch;++m_ordinary_runtime_dropped;++m_ordinary_suspended_pairs;if(m_ordinary_pair_retention.retire(dispatch))++m_ordinary_consumed;continue;}
            const auto packet=NativeOrdinaryPacket::get().receipt(dispatch);bool captured{};
            Microsoft::WRL::ComPtr<ID3D12Resource> hud;
            if(m_native_hud_requested && packet && packet->tracked){
                DWORD foreground{};GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
                const auto actor=EngineBodyProbe::get().active_actor();const auto menu=EngineCinematicState::get().sample();
                const auto epoch=packet->tracked->prepared.eyes[0].tracking.tracking_epoch;
                const auto presented=NativeRenderProbe::get().latest();
                const auto presented_packet=presented?NativeOrdinaryPacket::get().receipt(presented->sequence):std::nullopt;
                const bool stereo_buffer=VrNativeHudSource::stereo(dispatch,m_ordinary_bridge_attempted,presented?std::optional{presented->sequence}:std::nullopt,
                    presented_packet && presented_packet->accepted && presented_packet->tracked.has_value());
                if(!stereo_buffer && ++m_native_hud_mono_skips%120==1)spdlog::info("NativeHud: skipped a mono back buffer (skips={} dispatch={} previous={} presented={})",m_native_hud_mono_skips,dispatch,m_ordinary_bridge_attempted,presented?presented->sequence:0);
                if(stereo_buffer && foreground==GetCurrentProcessId() && scene_of(menu).hud() && VrCompactHud::get().allows(actor,epoch) && !m_ordinary_mirror_requested && hook->get_swap_chain())
                    hook->get_swap_chain()->GetBuffer(hook->get_swap_chain()->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&hud));
            }
            // Native pause/settings menus are UI-only: show the back buffer on the VR menu panel.
            Microsoft::WRL::ComPtr<ID3D12Resource> menu_image;
            const auto menu_state=EngineCinematicState::get().sample();
            const auto scene=scene_of(menu_state,m_screen_menu.active(std::chrono::steady_clock::now()));
            const bool pause_menu=m_menu_settings.enabled && scene.pause_menu();
            // A full-screen menu that keeps the world rendering (no pause mask): same panel.
            const bool screen_menu=m_menu_settings.enabled && scene.screen_menu();
            const bool menu_active=pause_menu || screen_menu;
            if(menu_active && !m_handoff.isolated() && hook->get_swap_chain()){hud.Reset();hook->get_swap_chain()->GetBuffer(hook->get_swap_chain()->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&menu_image));}
            if(menu_active!=m_menu_was_active){m_menu_was_active=menu_active;
                spdlog::info("VrMenuPanel: active={} path=ordinary source={} image={} native_flags={:#x}/{:#x} coverage={:.2f} width_degrees={} distance_metres={}",menu_active,pause_menu?"pause":screen_menu?"screen":"none",menu_image!=nullptr,
                    menu_state.published,menu_state.requested,m_screen_menu.coverage(),m_menu_settings.width_degrees,m_menu_settings.distance_metres);}
            // Guard guide: gameplay only (no menu, title, cutscene or pause), like the native HUD.
            const auto guide=m_vr_session && !menu_active && scene.hud()?m_vr_session->guard_guide(std::chrono::steady_clock::now()):std::nullopt;
            // Comfort vignette while a takeover rides along with the jump pack, same scenes.
            const auto riding=EngineBodyFollowing::get().installed()?EngineBodyFollowing::get().snapshot().follow:VrBodyFollowing::Snapshot{};
            const float vignette=!menu_active && scene.hud() && riding.camera_mode==VrBodyFollowing::CameraMode::ride?riding.vignette:0.f;
            const auto panel=menu_image && packet && packet->tracked?m_menu_anchor.apply(m_menu_settings,packet->tracked->prepared.eyes[0].tracking.runtime_head,std::chrono::steady_clock::now()):m_menu_settings;
            // Aim dot at the game's aim ray hit (VrAimDot), gameplay only like the guide. Third-person views keep
            // the HUD canvas head-locked: there the dot marks the aim and a camera-locked canvas is metres off.
            const bool first_person=m_view_settings.mode==VrViewMode::first_person;
            // First person locks the canvas to the game camera in plain play only; the hub keeps it head-locked
            // so a pop-up stays in sight whatever the game camera does (VrNativePrompt::camera_lock).
            const bool hud_camera_lock=VrNativePrompt::camera_lock(first_person,scene);
            // The game's own crosshair moves there too (VrHudReticle), first person where the canvas locks;
            // without a hit point it stays on the canvas.
            std::optional<VrAimDot::Draw> aim_dot;std::optional<std::array<float,3>> reticle_target;bool reticle_cut{true};
            const bool dot_wanted=m_aim_dot.shows(m_view_settings.mode),reticle_wanted=hud!=nullptr && m_hud_reticle.shows(m_view_settings.mode,scene);
            if(!menu_active && scene.hud() && (dot_wanted || reticle_wanted)){
                const auto target=EngineWeaponProbe::get().installed()?EngineWeaponProbe::get().owned_target(std::chrono::steady_clock::now()):std::nullopt;
                if(target && dot_wanted)aim_dot=m_aim_dot.draw(target->point);
                if(target && reticle_wanted){reticle_target=target->point;++m_reticle_pairs;if(target->steered)++m_reticle_steered;
                    m_hud_aspect=float(hud->GetDesc().Width)/float(hud->GetDesc().Height);
                    if(m_hud_reticle.stands_aside() && m_reticle_aside.active(std::chrono::steady_clock::now())){reticle_cut=false;++m_reticle_aside_pairs;}
                    // The crosshair's place on the canvas: the hit point through the game camera, while the game's own ray aims.
                    const auto& prepared=packet->tracked->prepared;const auto& t=prepared.native_tangents;
                    if(!target->steered && prepared.native_valid)if(const auto uv=VrHudReticle::canvas_point(target->point,VrHudCamera{prepared.native_world,t[0],t[1],t[2],t[3],1}))m_reticle_centre=uv;}
                if(target)++m_aim_dot_pairs;else ++m_aim_dot_missing;
            }
            if(packet)GpuCompletionProbe::internal([&]{captured=m_ordinary_runtime_images.capture(pair,*packet,queue,hud.Get(),m_native_hud_crop,VrCompactHud::get().settings(),m_handoff.queued(),m_handoff.isolated() && !m_native_hud_requested,
                menu_image.Get(),panel,m_native_prompt,guide,m_native_hud_corners,vignette,aim_dot,hud_camera_lock,m_hud_reticle.placed(m_view_settings.mode,m_reticle_centre),reticle_target,reticle_cut);});
            m_ordinary_bridge_attempted=dispatch;
            if(captured && PerformanceTrace::get().probes())PerformanceTrace::get().record("ordinary_bridge_point",PerformanceTrace::now_ms(),0,after_dispatch?1:0);
            if(hud){
                if(captured){m_native_hud_refusals.record(true);++m_native_hud_frames;if(m_native_hud_frames==1 || m_native_hud_frames%120==0)spdlog::info("NativeHud: composed={} dispatch={} source={}x{} refusals={}",m_native_hud_frames,dispatch,hud->GetDesc().Width,hud->GetDesc().Height,m_native_hud_refusals.total);}
                else if(!m_native_hud_refusals.record(false)){m_native_hud_requested=false;spdlog::error("NativeHud: {} consecutive capture refusals; status graphics disabled for subsequent frames",m_native_hud_refusals.consecutive);}
                else if(m_native_hud_refusals.consecutive==1)spdlog::warn("NativeHud: capture refused at dispatch={} (refusals={}); kept for the next pair",dispatch,m_native_hud_refusals.total);
            }
            if(!captured)++m_ordinary_runtime_dropped;
            if(m_ordinary_pair_retention.retire(dispatch))++m_ordinary_consumed;
            if((m_ordinary_consumed==1 || m_ordinary_consumed%1200==0) && packet && packet->tracked){const auto& t=packet->tracked->prepared.native_tangents;
                spdlog::info("HudCamera: locked={} tangents=[{:.3f},{:.3f},{:.3f},{:.3f}] (the HUD canvas follows the game camera in first-person play, not the hub; THIRD_PERSON.md)",
                    packet->tracked->prepared.native_valid && hud_camera_lock,t[0],t[1],t[2],t[3]);
                spdlog::info("AimDot: mode={} pairs={} no_target={} reticle_pairs={} reticle_steered={} reticle_aside={} reticle_centre=[{:.3f},{:.3f}] dot={}",VrViewSettings::name(m_view_settings.mode),m_aim_dot_pairs,m_aim_dot_missing,m_reticle_pairs,m_reticle_steered,m_reticle_aside_pairs,
                    m_reticle_centre?(*m_reticle_centre)[0]:-1.f,m_reticle_centre?(*m_reticle_centre)[1]:-1.f,m_aim_dot.enabled());}
            if(m_ordinary_consumed==1 || m_ordinary_consumed%120==0){
                const auto stats=m_vr_session?m_vr_session->statistics():VrSession::Statistics{};
                // submit_gap_*: since the previous line, the longest time between two pairs sent to the
                // runtime and how many gaps were over 100 ms / over the flat-panel stall (what the eyes get).
                const auto gaps=m_submit_gaps.take();
                spdlog::info("OrdinaryHeadset: consumed={} submitted={} dropped={} dispatch={} gpu_verified=true eye_source={} eye_source_since={} native_camera_id={} second_camera_id={} submit_gap_max_ms={} submit_gaps_over_100ms={} submit_gaps_over_stall={} dropped_by=[{}]",m_ordinary_consumed,m_ordinary_submitted,m_ordinary_runtime_dropped,dispatch,VrEyeSourceDiagnostic::name(stats.eye_source),stats.eye_source_since_render_id,
                    NativeTrackedOrdinaryCamera::get().native_camera_id_arg(),NativeTrackedOrdinaryCamera::get().second_camera_id(),gaps.longest.count(),gaps.over_100ms,gaps.over_stall,m_ordinary_runtime_images.refusal_summary());
            }
        }
        if(m_present_count%600==0){const auto mirror=ordinary_adaptation_mirror_line();if(!mirror.empty())spdlog::info("NativeAdaptationMirror: {}",mirror);}
        if(m_present_count%600==0){const auto share=ordinary_adaptation_share_line();if(!share.empty())spdlog::info("OrdinaryAdaptationShare: {}",share);}
        if(m_present_count%600==0){const auto flares=ordinary_flare_probe_line();if(!flares.empty())spdlog::info("NativeFlareProbe: {}",flares);}
        if(m_present_count%600==0){const auto tone=ordinary_tone_source_stats();if(tone.mask || tone.substituted)spdlog::info("OrdinaryToneSource: mask={} substituted={} missing={} shared={}/{}/{}",tone.mask,tone.substituted,tone.missing,tone.shared[0],tone.shared[1],tone.shared[2]);}
        if(m_present_count%120==0){const auto receipts=NativeTrackedOrdinaryCamera::get().receipts();if(!receipts.empty()){const auto& r=receipts.back();const auto packet=NativeOrdinaryPacket::get().receipt(NativeRenderProbe::get().latest().value_or(NativeRenderProbe::Snapshot{}).sequence);
            const auto ready=NativeTrackedOrdinaryCamera::get().readiness();
            spdlog::info("OrdinaryOwnedReadiness: reason={} consecutive={} checks={} blocked={} ready_jobs={} owner_changes={} resumes={} job={} actor={} controller={} instance={}",OwnedStereoReadiness::name(ready.reason),ready.consecutive,ready.checks,ready.blocked,ready.ready_jobs,ready.owner_changes,ready.resumes,ready.last_job,ready.owner[0],ready.owner[2],ready.owner[6]);
            if(const auto foreign=ForeignViewHold::get().stats();foreign.sightings)
                spdlog::info("ForeignViewHold: held={} sightings={} holds={} behind_pair={} slot={} held_jobs={}",ForeignViewHold::get().held(std::chrono::steady_clock::now()),foreign.sightings,foreign.holds,foreign.behind_pair,foreign.slot,NativeTrackedOrdinaryCamera::get().foreign_jobs());
            // packet_refusal=999: no packet receipt for the latest native dispatch (a mono dispatch: stopped, suspended, skipped).
            const auto suspension=NativeTrackedOrdinaryCamera::get().suspension();
            spdlog::info("OrdinaryHeadsetProducer: job={} accepted={} refusal={} packet_refusal={} submitted={} volume_shared={} clock_skips={} provider_returned={} provider_ms={} tracking_age_ms={} expired_pre_add_retry={} producer_stopped={} producer_suspended={} suspensions={} suspended_jobs={} resumes={}",r.job.render_id,r.accepted,uint32_t(r.refusal),packet?packet->refusal:999,m_ordinary_submitted,NativeOrdinaryVolumeFrame::get().shared(),NativeTrackedOrdinaryCamera::get().clock_skips(),r.provider_returned,r.provider_ms,r.tracking_age_ms,r.expired_pre_add_retry,NativeTrackedOrdinaryCamera::get().stopped(),
                NativeTrackedOrdinaryCamera::get().suspended(),suspension.suspensions,suspension.suspended_jobs,suspension.resumes);
            if(NativeOrdinaryBloom::get().installed()){const auto recovery=NativeOrdinaryBloom::get().recovery_status();const auto& p=recovery.policy;
                auto signature=fmt::format("suspended={} exhausted={} episodes={} attempts={} refused_attempts={} recoveries={} probe_refusals={} probe_difference={:#x} suspended_records={} dropped_pairs={} bloom_stopped={}",
                    p.suspended,p.exhausted,p.episodes,p.attempts,p.refused_attempts,p.recoveries,p.probe_refusals,recovery.probe_difference,recovery.suspended_records,m_ordinary_suspended_pairs,recovery.stopped);
                // On a state change (rate-limited like the episode lines), else at most once a minute.
                const auto flags=uint32_t(p.suspended)|uint32_t(p.exhausted)<<1|uint32_t(recovery.stopped)<<2;const auto at=std::chrono::steady_clock::now();
                if(signature!=m_ordinary_recovery_signature && ((flags!=m_ordinary_recovery_flags && (OrdinaryStereoRecovery::log_episode(p.episodes) || p.exhausted || recovery.stopped)) || at-m_ordinary_recovery_logged>=std::chrono::minutes(1))){
                    spdlog::info("OrdinaryStereoRecovery: {}",signature);m_ordinary_recovery_signature=std::move(signature);m_ordinary_recovery_logged=at;}
                m_ordinary_recovery_flags=flags;}
            const auto shading=NativeOrdinaryShadingControl::get().stats();spdlog::info("OrdinaryShading: mask={} ssr={} ssao={} screen_shadows={}",shading[0],shading[1],shading[2],shading[3]);
            const auto light=NativeOrdinaryLightVisibility::get().stats();spdlog::info("OrdinaryLightVisibility: jobs={} left={} right={}",light[0],light[1],light[2]);
            {const auto& ls=NativeOrdinaryLightSection::get();const auto c=ls.stats();
                spdlog::info("LightSection: installed={} mode={} calls={} site_calls={} slot0={} slot1={} swapped={}",ls.installed(),ls.mode()==NativeOrdinaryLightSection::Mode::slot?"slot":"native",c[0],c[1],c[2],c[3],c[4]);}
            if(NativeOcclusionHarvestOnce::get().installed()){const auto h=NativeOcclusionHarvestOnce::get().stats();
                spdlog::info("OcclusionHarvest: mode={} calls={} scene_calls={} slot0={} slot1={} skipped={}",NativeOcclusionHarvestOnce::get().mode()==NativeOcclusionHarvestOnce::Mode::once?"once":"native",h[0],h[1],h[2],h[3],h[4]);}
            {const auto s=NativeOrdinaryLightVisibility::get().stereo_stats();
                spdlog::info("StereoLights: mode={} shared={} copied={} fallback_copy={} refused={} right_only_added={} over_cap={} differing_copies={} largest={}",
                    stereo_light_list::name(NativeOrdinaryLightVisibility::get().stereo_mode()),s[0],s[1],s[2],s[3],s[4],s[5],s[6],s[7]);}
            if(NativeOrdinaryLightVisibility::get().copy_configured()){const auto [on,c]=NativeOrdinaryLightVisibility::get().copy_stats();
                spdlog::info("LightListCopy: mode={} copied={} refused={}",on?"copy":"native",c[0],c[1]);}
            if(NativeOrdinaryLightVisibility::get().clamp_configured()){const auto [on,c]=NativeOrdinaryLightVisibility::get().clamp_stats();
                spdlog::info("LightClamp: mode={} clamped eye0={} eye1={}",on?"clamp":"native",c[0],c[1]);}
            if(NativeSecondCameraPlainLightTest::get().installed()){const auto p=NativeSecondCameraPlainLightTest::get().stats();
                spdlog::info("SecondCameraPlainLightTest: mode={} calls={} camera1_stereo={} cleared={} already_plain={} refused={}",NativeSecondCameraPlainLightTest::get().mode()==NativeSecondCameraPlainLightTest::Mode::plain?"plain":"native",p[0],p[1],p[2],p[3],p[4]);}
            if(NativeOrdinaryLightVisibility::get().hold_configured()){const auto [setting,hold]=NativeOrdinaryLightVisibility::get().hold_stats();
                spdlog::info("OrdinaryLightHold: frames={} kind={} eye0 queries={} missing={} zero={} low={} visible={} held={} tracked={} eye1 queries={} missing={} zero={} low={} visible={} held={} tracked={}",
                    setting.frames,setting.kind==LightVisibilityHold::Kind::drops?"drops":"missing",
                    hold[0].queries,hold[0].missing,hold[0].zero,hold[0].low,hold[0].visible,hold[0].held,hold[0].tracked,
                    hold[1].queries,hold[1].missing,hold[1].zero,hold[1].low,hold[1].visible,hold[1].held,hold[1].tracked);}
            const auto volumetric=NativeOrdinaryLightVisibility::get().scene_stats();spdlog::info("OrdinaryVolumetricVisibility: unioned shafts={} fog_sun={} fog_volume={} refused shafts={} fog_sun={} fog_volume={} changed shafts={} fog_sun={} fog_volume={}",volumetric[0],volumetric[1],volumetric[2],volumetric[3],volumetric[4],volumetric[5],volumetric[6],volumetric[7],volumetric[8]);
            const auto selection=NativeOrdinaryReflectionSelection::get().stats();spdlog::info("OrdinaryReflectionSelection: calls={} refused={} different={} applied={}",selection[0],selection[1],selection[2],selection[3]);
            for(const auto& e:NativeOrdinaryShadowFit::get().evidence())if(e.dispatch)spdlog::info("OrdinaryShadowFit: dispatch={} eye={} complete={} camera={} cache_camera={} matrix=[{}] cascade=[{}]",e.dispatch,e.eye,e.complete,e.camera,e.cache_camera,fmt::join(e.matrix,","),fmt::join(e.cascade,","));
            const auto fit=NativeOrdinaryShadowFit::get().stats();spdlog::info("OrdinaryShadowFitCalls: total={} caller={} scope={} owned_scope={} receipts={} last_caller={:x}",fit[0],fit[1],fit[2],fit[3],fit[4],fit[5]);
            {const auto hzb=NativeHzbControl::get().stats();if(hzb.calls)spdlog::info("NativeHzbControl: mode={} calls={} second_eye={} checked={} inherited={} lwi={} lwi_changes={} wide_gap={} edge={:.3f}",native_hzb_culling::name(hzb.mode),hzb.calls,hzb.second_eye,hzb.checked,hzb.inherited,hzb.lwi,hzb.lwi_changes,hzb.wide_gap,hzb.edge);}
            const auto cache=NativeOrdinaryShadowCache::get().stats();spdlog::info("OrdinaryShadowRefresh: total={} caller={} pair={} owned={} left={} right={}",cache[0],cache[1],cache[2],cache[3],cache[4],cache[5]);
            const auto& shadow_lights=NativeOrdinaryShadowLights::get();const auto lights=shadow_lights.stats();
            if(NativeOrdinaryTaa::get().enabled()){const auto taa=NativeOrdinaryTaa::get().status();const auto& creation=NativeOrdinaryTextureCreation::taa();spdlog::info("OrdinaryTaa: jobs={} owned={} resets={} bootstraps={} failures={} previous_mismatches={} renewals={} difference={:#x} stalled={} creation_refusal={} generation={}",taa.history.jobs,taa.history.owned,taa.history.resets,taa.history.bootstraps,taa.history.failures,taa.history.previous_mismatches,taa.renewals,taa.difference,taa.stalled,uint32_t(creation.refusal()),creation.generation());}
            spdlog::info("OrdinaryShadowLights: installed={} calls={} second_eye={} same={} copied={} refused={}",shadow_lights.installed(),lights[0],lights[1],lights[2],lights[3],lights[4]);
            const auto& spot_share=NativeOrdinarySpotShadowShare::get();const auto spots=spot_share.stats();
            spdlog::info("OrdinarySpotShadowShare: installed={} calls={} second_eye={} reused={} rendered={}",spot_share.installed(),spots[0],spots[1],spots[2],spots[3]);
            for(const auto& e:NativeOrdinaryShadingControl::get().evidence())if(e.dispatch)spdlog::info("OrdinaryLighting: dispatch={} slot={} complete={} lights=[{},{},{},{}] shadows=[{},{},{},{}]",e.dispatch,e.slot,e.complete,e.lights[0],e.lights[1],e.lights[2],e.lights[3],e.shadows[0],e.shadows[1],e.shadows[2],e.shadows[3]);}}
        return;
    }
    if(m_ordinary_sdk_requested)m_ordinary_sdk_capture.save(m_ordinary_dump_directory);
    const auto state=m_ordinary_pair_retention.status();const auto signature=GpuOrdinaryPairRetention::status_signature(state,m_ordinary_pair_ready.load());
    if(m_ordinary_pair_status!=signature){std::ofstream status{m_ordinary_dump_directory/"pair_retention_status.json"};
        status<<"{\"model\":\"ordinary_pair_gpu_status_v1\",\"configured\":"<<(state.ready?"true":"false")<<",\"enabled\":"<<(m_ordinary_pair_ready?"true":"false")<<",\"stopped\":"<<(state.stopped?"true":"false")<<",\"active\":"<<(state.active?"true":"false")<<",\"pairs\":"<<state.pairs<<",\"refusal\":"<<uint32_t(state.refusal)<<",\"headset_ready\":false}\n";
        if(status)m_ordinary_pair_status=signature;
        spdlog::info("OrdinaryPairRetentionStatus: configured={} pairs={} active={} stopped={} refusal={} headset_ready=false",state.ready,state.pairs,state.active,state.stopped,uint32_t(state.refusal));}
    const auto samples=m_ordinary_pair_retention.samples();
    for(size_t index=0;index<samples.size();++index){const auto& pair=samples[index];const auto dispatch=pair.origin.scene.dispatch_sequence;
        if(std::ranges::find(m_ordinary_pair_dumped,dispatch)!=m_ordinary_pair_dumped.end() || (!pair.gpu_retention_verified && pair.refusal==GpuOrdinaryPairRetention::Refusal::none))continue;
        const auto stem="pair_retention_"+std::to_string(index);HRESULT readback=E_PENDING;std::array<bool,2> saved{};
        if(pair.gpu_retention_verified){const std::array<GpuTextureReadback::Source,2> sources{{{pair.eyes[0].copy->texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE},{pair.eyes[1].copy->texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE}}};
            std::vector<GpuTextureReadback::Pixels> pixels;const auto& hook=g_framework->get_d3d12_hook();
            if(hook)GpuCompletionProbe::internal([&]{readback=GpuTextureReadback::capture(hook->get_command_queue(),sources,pixels);});
            if(SUCCEEDED(readback) && pixels.size()==2)for(size_t slot=0;slot<2;++slot)saved[slot]=GpuTextureReadback::save(m_ordinary_dump_directory/(stem+(slot?"_right.ppm":"_left.ppm")),pixels[slot]);}
        std::ofstream out{m_ordinary_dump_directory/(stem+".json")};out<<"{\"model\":\"ordinary_pair_gpu_capture_v1\",\"actual_game_run\":true,\"pip_enabled\":false,\"gpu_pair_verified\":false,\"runtime_submission\":false,\"headset_ready\":false,\"readback_result\":"<<readback<<",\"pixels_saved\":["<<(saved[0]?"true":"false")<<','<<(saved[1]?"true":"false")<<"],\"retention\":";
        GpuOrdinaryPairRetention::write(out,pair);out<<"}\n";out.close();
        if(!out){spdlog::error("OrdinaryPairRetention: receipt write failed dispatch={}",dispatch);{NativeOrdinaryCamera::get().request_stop();NativeTrackedOrdinaryCamera::get().request_stop();}continue;}
        m_ordinary_pair_dumped.push_back(dispatch);spdlog::info("OrdinaryPairRetentionResult: index={} dispatch={} completed={} refusal={} readback={} left_saved={} right_saved={} native_histories_verified=false runtime_submission=false",index,dispatch,pair.gpu_retention_verified,uint32_t(pair.refusal),readback,saved[0],saved[1]);
    }
}

void Sm2Probe::ordinary_boundary(const NativeOrdinaryProbe::Event& event)
{
    if(!m_ordinary_inline_ready || event.stage==NativeOrdinaryProbe::Stage::post)return;
    if(event.stage==NativeOrdinaryProbe::Stage::compositor_before) {
        if(++m_ordinary_compositor_depth!=1){m_ordinary_inline.invalidate_active(GpuInlineCopy::Refusal::busy);return;}
        if(m_ordinary_private_stopped || m_ordinary_inline_attempts>=4 || ++m_ordinary_inline_frames%120)return;
        const auto views=NativeRenderProbe::read_views(event.scope.scene.list);
        if(!views || views->size()!=1 || !views->front().main() || views->front().fov_h<40 || views->front().fov_h>150)return;
        const auto& hook=g_framework->get_d3d12_hook();Microsoft::WRL::ComPtr<IDXGISwapChain3> swap;Microsoft::WRL::ComPtr<ID3D12Resource> source;
        if(!hook || !hook->get_swap_chain() || FAILED(hook->get_swap_chain()->QueryInterface(IID_PPV_ARGS(&swap))) || FAILED(swap->GetBuffer(swap->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&source))))return;
        const auto tag=event.scope.scene.dispatch_sequence;
        auto* capture_source=m_ordinary_private_requested?m_ordinary_private_output.texture():source.Get();
        if(m_ordinary_inline.begin(tag,capture_source)){m_ordinary_inline_tag=tag;++m_ordinary_inline_attempts;
            if(m_ordinary_private_requested && !m_ordinary_private_output.begin(tag,source.Get())){m_ordinary_inline.invalidate_active(GpuInlineCopy::Refusal::configuration);m_ordinary_private_stopped=true;}}
        else spdlog::warn("OrdinaryInlineCopy: begin refused dispatch={}",tag);
    } else {
        if(!m_ordinary_compositor_depth || --m_ordinary_compositor_depth)return;
        if(m_ordinary_inline_tag){const auto result=m_pip_completion.finish_inline(m_ordinary_inline_tag);m_ordinary_inline_tag=0;
            if(m_ordinary_private_requested){const auto receipt=m_ordinary_private_output.end(result.tag,result.appended,m_render_targets);{std::lock_guard lock{m_ordinary_private_mutex};m_ordinary_private_receipts.emplace(result.tag,receipt);}
                if(receipt.refusal!=GpuCompositorOutput::Refusal::none)m_ordinary_private_stopped=true;
                spdlog::info("OrdinaryPrivateRestore: dispatch={} bindings={} restored={} restores={} refusal={} generation={} source={:x} target={:x}",receipt.tag,receipt.bindings,receipt.restored,receipt.restores,uint32_t(receipt.refusal),receipt.generation,receipt.source,receipt.target);}
            spdlog::info("OrdinaryInlineAppend: dispatch={} appended={} refusal={} generation={} writes={} state={} hazards={}",result.tag,result.appended,uint32_t(result.refusal),result.generation,result.writes,result.state,result.hazards);
            if(result.hazards){const auto& a=result.access;spdlog::info("OrdinaryInlineHazard: dispatch={} aliases={} null={} target={} other={} before={:x} after={:x} extended_kind={} resource={:x} allocation_known={} allocation_kind={} allocation_flags={} callsite={:x}",
                result.tag,a.aliases,a.alias_null,a.alias_target,a.alias_other,a.alias_before,a.alias_after,a.first_extended,a.extended_resource,bool(a.extended_allocation),a.extended_allocation?uint32_t(a.extended_allocation->range.kind):0,a.extended_allocation?a.extended_allocation->range.heap_flags:0,a.extended_callsite);}}
    }
}

void Sm2Probe::mirror_ordinary_output()
{
    if(!m_ordinary_inline_ready || !m_ordinary_mirror_requested)return;
    const auto dispatch=NativeRenderProbe::get().latest();if(!dispatch)return;
    for(const auto& sample:m_ordinary_inline.samples()) {
        if(sample.tag!=dispatch->sequence || !sample.submitted || sample.refusal!=GpuInlineCopy::Refusal::none || m_ordinary_mirror.sample(sample.tag))continue;
        std::optional<GpuCompositorOutput::Receipt> redirect;{std::lock_guard lock{m_ordinary_private_mutex};const auto found=m_ordinary_private_receipts.find(sample.tag);if(found!=m_ordinary_private_receipts.end())redirect=found->second;}
        if(!redirect || !redirect->restored || redirect->refusal!=GpuCompositorOutput::Refusal::none)continue;
        const auto& hook=g_framework->get_d3d12_hook();Microsoft::WRL::ComPtr<IDXGISwapChain3> swap;Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        if(!hook || !hook->get_swap_chain() || FAILED(hook->get_swap_chain()->QueryInterface(IID_PPV_ARGS(&swap))) || FAILED(swap->GetBuffer(swap->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer))))continue;
        HRESULT result=E_PENDING;GpuCompletionProbe::internal([&]{result=m_ordinary_mirror.submit(hook->get_command_queue(),sample,dispatch->sequence,buffer.Get(),redirect->source);});
        spdlog::info("OrdinaryMirror: dispatch={} result={} producing_queue={:x} destination={:x} runtime_submission=false",sample.tag,result,sample.queue,GpuObjectIdentity::get(buffer.Get()));
    }
}

void Sm2Probe::capture_ordinary_inline()
{
    if(!m_ordinary_inline_ready)return;
    const auto samples=m_ordinary_inline.samples();const auto dispatch=NativeRenderProbe::get().latest();
    for(size_t index=0;index<samples.size();++index){const auto& sample=samples[index];
        if(!m_ordinary_inline_receipts.contains(sample.tag) && dispatch && dispatch->sequence==sample.tag){std::ostringstream receipt;receipt<<std::setprecision(9);
            NativeOrdinaryProbe::write_capture(receipt,*dispatch,NativeOrdinaryProbe::get().sample(sample.tag),sample.width,sample.height);m_ordinary_inline_receipts.emplace(sample.tag,receipt.str());}
        if((sample.appended && !sample.completed && sample.refusal==GpuInlineCopy::Refusal::none) || std::ranges::find(m_ordinary_inline_dumped,sample.tag)!=m_ordinary_inline_dumped.end())continue;
        std::optional<GpuCompositorOutput::Receipt> redirect;if(m_ordinary_private_requested){std::lock_guard lock{m_ordinary_private_mutex};const auto found=m_ordinary_private_receipts.find(sample.tag);if(found==m_ordinary_private_receipts.end())continue;redirect=found->second;}
        const auto stem="inline_"+std::to_string(index);HRESULT readback=E_PENDING;bool pixels_saved=false;
        if(sample.completed && sample.texture){const std::array<GpuTextureReadback::Source,1> source{{{sample.texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE}}};std::vector<GpuTextureReadback::Pixels> pixels;
            const auto& hook=g_framework->get_d3d12_hook();if(hook)readback=GpuTextureReadback::capture(hook->get_command_queue(),source,pixels);
            if(SUCCEEDED(readback) && pixels.size()==1)pixels_saved=GpuTextureReadback::save(m_ordinary_dump_directory/(stem+".ppm"),pixels.front());}
        const auto mirror=m_ordinary_mirror.sample(sample.tag);const bool mirrored=mirror && mirror->completed && SUCCEEDED(mirror->result);
        std::ofstream out{m_ordinary_dump_directory/(stem+".json")};out<<"{\"model\":\""<<(m_ordinary_mirror_requested?"ordinary_private_compositor_mirror_v1":m_ordinary_private_requested?"ordinary_private_compositor_v1":"ordinary_inline_copy_v1")<<"\",\"pip_enabled\":false,\"gpu_pair_verified\":false,\"runtime_submission\":false,\"copy\":";
        GpuInlineCopy::write_sample(out,sample);out<<",\"readback_result\":"<<readback<<",\"pixels_saved\":"<<(pixels_saved?"true":"false")<<",\"cpu_boundary\":";
        const auto receipt=m_ordinary_inline_receipts.find(sample.tag);if(receipt!=m_ordinary_inline_receipts.end())out<<receipt->second;else out<<"null";
        if(redirect){out<<",\"redirect\":";GpuCompositorOutput::write_receipt(out,*redirect);out<<",\"shader_override\":false,\"desktop_mirror_verified\":"<<(mirrored?"true":"false")<<",\"retirement\":\"process_lifetime\"";}
        if(m_ordinary_mirror_requested){out<<",\"mirror\":";if(mirror)GpuPresentMirror::write_sample(out,*mirror);else out<<"null";}out<<"}\n";out.close();
        spdlog::info("OrdinaryInlineResult: index={} dispatch={} appended={} completed={} refusal={} readback={} pixels_saved={} cpu_receipt={} file={}",index,sample.tag,sample.appended,sample.completed,uint32_t(sample.refusal),readback,pixels_saved,receipt!=m_ordinary_inline_receipts.end(),(m_ordinary_dump_directory/(stem+".json")).string());
        m_ordinary_inline_dumped.push_back(sample.tag);
    }
}

void Sm2Probe::capture_pip_experiment()
{
    const auto state=NativePipExperiment::get().snapshot();
    if(!state.active) {if(m_pip_completion_enabled) m_pip_completion.clear_target();return;}
    if(m_pip_dump_directory.empty() || (m_pip_dump_attempted && !(m_pip_final_lens && m_pip_lens_sweep)) || m_vr_session || PerformanceTrace::get().enabled()) return;
    if(state.updates<120) return;
    const auto lens=NativePipLens::get().snapshot();
    const auto phase=m_pip_final_lens && m_pip_lens_sweep?std::min(uint32_t((state.updates-1)/512),3u):0u;
    const auto directory=m_pip_final_lens && m_pip_lens_sweep?m_pip_dump_directory/("phase_"+std::to_string(phase)):m_pip_dump_directory;
    if(m_pip_dumped_phases&(1u<<phase)) return;
    if(m_pip_final_lens && (lens.reason || !lens.applied || lens.phase!=phase || state.updates<uint64_t(phase)*512+120)) {
        if(m_present_count%120==0) spdlog::info("PipLensGate: reason={} applied={} rejected={} phase={}/{} updates={}/{}",lens.reason,lens.applied,lens.rejected,lens.phase,phase,lens.updates,state.updates);
        return;
    }
    const auto native=EngineCinematicState::get().sample();
    if(!native.available || (native.published|native.requested)!=0) return;
    const auto presenting=FrameSyncProbe::get().snapshot().clock.presenting;
    const auto constants=StreamlineProbe::get().frame_camera(presenting?presenting->token:0);
    const bool diagnose=m_present_count%120==0;
    if(!presenting || presenting->render_id<2 || !constants) {
        if(diagnose) spdlog::info("PipCaptureGate: presenting={} id={} token={} constants={}",bool(presenting),presenting?presenting->render_id:0,presenting?presenting->token:0,bool(constants));
        return;
    }
    const auto main=EngineCameraProbe::get().rendered_frame(presenting->render_id-1);
    const auto pip=EngineCameraProbe::get().additional_frame(presenting->render_id-1);
    if(!main || !pip || !main->projection || !pip->projection || main->game_frame->token!=pip->game_frame->token ||
        uint32_t(main->game_frame->token+1)!=presenting->token) {
        if(diagnose) spdlog::info("PipCaptureGate: present={}/{} main={} pip={} projections={}/{} tokens={}/{}",presenting->render_id,presenting->token,bool(main),bool(pip),main&&main->projection.has_value(),pip&&pip->projection.has_value(),main?main->game_frame->token:0,pip?pip->game_frame->token:0);
        return;
    }
    const vr_camera::Matrix world{constants->right.x,constants->right.y,constants->right.z,0,constants->up.x,constants->up.y,constants->up.z,0,
        constants->forward.x,constants->forward.y,constants->forward.z,0,constants->position.x,constants->position.y,constants->position.z,1};
    const auto now=std::chrono::steady_clock::now();
    if(now<main->received || now<pip->received || now-main->received>std::chrono::milliseconds(250) || now-pip->received>std::chrono::milliseconds(250)) return;
    const auto dispatch=NativeRenderProbe::get().latest();
    if(!dispatch || now<dispatch->end || now-dispatch->end>std::chrono::milliseconds(250)) return;
    if(m_pip_final_lens && (now<lens.received || now-lens.received>std::chrono::milliseconds(250))) return;
    for(size_t i=0;i<16;++i) if(std::abs(main->world[i]-world[i])>=1e-4f) {
        if(diagnose) spdlog::info("PipCaptureGate: world index={} main={} streamline={}",i,main->world[i],world[i]);
        return;
    }
    const auto& hook=g_framework->get_d3d12_hook();
    if(!hook || !hook->get_swap_chain() || !hook->get_command_queue()) return;
    uintptr_t context{};SIZE_T bytes{};
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(base+game_build::rva(0x4ff0c10)),&context,sizeof(context),&bytes) || !context) return;
    const bool visited=NativePipExperiment::get().visit_resource(context,[&](const auto& experiment,const auto& binding) {
        Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer;
        const auto swap=hook->get_swap_chain();
        auto hr=swap->GetBuffer(swap->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&backbuffer));
        const auto resource=reinterpret_cast<ID3D12Resource*>(binding.resource);
        if(!m_pip_completion_enabled || !m_pip_completion.target(resource)) return;
        auto captured=m_pip_completion.take_snapshot(uint64_t(phase)+1);
        if(!captured) {
            m_pip_completion.request_snapshot(uint64_t(phase)+1);
            if(diagnose) {const auto status=m_pip_completion.snapshot_status();spdlog::info("PipSnapshotWait: phase={} requested={} writes={} lists={} unknown_lists={} transition_lists={} raster_state_lists={} state={:x} barrier_calls={} target_transitions={} untracked_transitions={} foreign_submissions={}",phase,status.requested,status.writes,status.lists,status.unknown_lists,status.transition_lists,status.raster_state_lists,status.state,status.barrier_calls,status.target_transitions,status.untracked_transitions,status.foreign_submissions);}
            if(diagnose) {const auto status=m_pip_completion.snapshot_status();spdlog::info("PipBarrierHazards: mask={} aliases={} uavs={} malformed={} enhanced_calls={} untracked_hazards={} foreign_hazard_submissions={}",
                status.barrier_hazards,status.aliasing_barriers,status.uav_barriers,status.malformed_barriers,status.enhanced_calls,status.untracked_hazards,status.foreign_hazard_submissions);}
            if(diagnose) {const auto status=m_pip_completion.snapshot_status();spdlog::info("PipUnsupportedWriters: dispatch={} indirect={} bundle={} uav_uint={} uav_float={} last_kind={} last_callsite={:x} last_thread={}",
                status.unsupported_writer_calls[0],status.unsupported_writer_calls[1],status.unsupported_writer_calls[2],status.unsupported_writer_calls[3],status.unsupported_writer_calls[4],
                status.unsupported_writer_kind,status.unsupported_writer_callsite,status.unsupported_writer_thread);}
            if(diagnose) {const auto status=m_pip_completion.snapshot_status();spdlog::info("PipNestedWriters: dispatch={} indirect={} bundle={} uav_uint={} uav_float={}",
                status.nested_writer_calls[0],status.nested_writer_calls[1],status.nested_writer_calls[2],status.nested_writer_calls[3],status.nested_writer_calls[4]);}
            if(diagnose) {const auto status=m_pip_completion.snapshot_status();spdlog::info("PipAccessExclusions: aliases={} uavs={} dispatch={} indirect={} bundle={} uav_uint={} uav_float={}",
                status.excluded_aliases,status.excluded_uavs,status.excluded_writer_calls[0],status.excluded_writer_calls[1],status.excluded_writer_calls[2],status.excluded_writer_calls[3],status.excluded_writer_calls[4]);}
            if(diagnose) {
                const auto status=m_pip_completion.snapshot_status();const auto observer=m_pip_completion.allocation_status();
                const auto signature=status.signature.value_or(GpuAllocationProbe::SignatureRecord{});
                spdlog::info("PipIndirectSignatures: unknown={} draw={} indexed={} dispatch={} rays={} mesh={} creation_calls={} annotated={} hook={}",
                    status.indirect_actions[0],status.indirect_actions[1],status.indirect_actions[2],status.indirect_actions[3],status.indirect_actions[4],status.indirect_actions[5],observer.signature_calls,observer.signatures_annotated,bool(observer.hook_mask&32));
                spdlog::info("PipIndirectSample: known={} command={:x} interface={:x} canonical={:x} serial={} device={:x} stride={} arguments={} action={} max_count={} root={:x} creation_callsite={:x} thread={}",
                    bool(status.signature),status.indirect_command,status.signature_interface,signature.signature,signature.serial,signature.device,
                    signature.shape.stride,signature.shape.count,uint32_t(signature.shape.action()),status.indirect_max_count,signature.root,signature.callsite,signature.thread);
                spdlog::info("PipIndirectAccess: compute_excluded={} raster_excluded={} potential={} unknown_bindings={} uncovered_bindings={} clear_state={} begin_pass={} end_pass={}",
                    status.indirect_exclusions[0],status.indirect_exclusions[1],status.indirect_potential,status.indirect_binding_unknown,status.indirect_binding_uncovered,
                    status.binding_calls[0],status.binding_calls[1],status.binding_calls[2]);
                spdlog::info("PipExtendedWriters: calls=[{}] excluded=[{}] last_kind={} last_callsite={:x}",fmt::join(status.extended_calls,","),fmt::join(status.extended_exclusions,","),status.extended_kind,status.extended_callsite);
                const auto device=status.queue_device.value_or(GpuQueueProbe::Device{});const auto queue=status.queue_creation.value_or(GpuQueueProbe::Queue{});
                spdlog::info("PipQueueCreation: device_known={} early_return={} hooks={} base_calls={} extended_calls={} annotated={} failed={} methods={} primary_known={} primary_serial={} execute={:x} all_observed_methods_match={} unknown_foreign={} foreign_unknown_lists={} foreign_target_writes={}",
                    bool(status.queue_device),device.early_return,device.hooks,device.calls[0],device.calls[1],device.annotated,device.failed,device.method_count,
                    bool(status.queue_creation),queue.serial,queue.execute,GpuQueueProbe::all_observed_methods_match(device,queue.execute),status.unknown_queue_submissions,status.foreign_unknown_lists,status.foreign_target_submissions);
            }
            if(diagnose) {
                const auto status=m_pip_completion.snapshot_status();const auto allocations=m_pip_completion.allocation_status();
                spdlog::info("PipAllocationObserver: hook_mask={} committed={} placed={} committed1={} heaps={} heaps1={} annotated={} failed={}",
                    allocations.hook_mask,allocations.calls[0],allocations.calls[1],allocations.calls[2],allocations.heap_calls[0],allocations.heap_calls[1],allocations.annotated,allocations.failed);
                const auto log_allocation=[](const char* label,const std::optional<GpuAllocationProbe::Record>& observed) {
                    const auto value=observed.value_or(GpuAllocationProbe::Record{});const auto& range=value.range;
                    spdlog::info("{}: known={} kind={} serial={} device={:x} resource={:x} heap={:x} heap_serial={} offset={} size={} heap_size={} alignment={} heap_flags={} api={} initial_state={} callsite={:x} thread={}",
                        label,bool(observed),uint32_t(range.kind),range.serial,range.device,range.resource,range.heap,range.heap_serial,range.offset,range.size,range.heap_size,range.alignment,range.heap_flags,value.api,value.initial_state,value.callsite,value.thread);
                    spdlog::info("{}Heap: known={} type={} cpu_page={} memory_pool={} creation_node={} visible_node={} flags={} private_target={}",label,value.properties_known,
                        uint32_t(value.properties.Type),uint32_t(value.properties.CPUPageProperty),uint32_t(value.properties.MemoryPoolPreference),value.properties.CreationNodeMask,value.properties.VisibleNodeMask,
                        uint32_t(value.actual_heap_flags),bool(observed) && value.access().private_render_target());
                };
                log_allocation("PipTargetAllocation",status.allocation);
                const auto& alias=status.alias_sample;
                spdlog::info("PipAliasSample: ordinal={} target_named={} both_null={} other={} command={:x} before={:x} after={:x} callsite={:x} thread={} sampled_only=true",
                    alias.ordinal,status.alias_target_named,status.alias_both_null,status.alias_other,alias.command,alias.before,alias.after,alias.callsite,alias.thread);
                log_allocation("PipAliasBeforeAllocation",alias.before_allocation);log_allocation("PipAliasAfterAllocation",alias.after_allocation);
            }
            return;
        }
        if(FAILED(captured->result)) {
            m_pip_dump_attempted=true;m_pip_dumped_phases|=uint8_t(1u<<phase);
            spdlog::info("PipSnapshot: phase={} result={:x}",phase,uint32_t(captured->result));return;
        }
        const auto completion=std::optional{captured->writer};
        bool source_matches=!m_pip_pose_tags;
        if(completion->origin && completion->origin->render_id==pip->game_frame->render_id && completion->origin->token==pip->game_frame->token) {
            source_matches=true;
            for(size_t i=0;i<16;++i) if(std::abs(completion->origin->render_world[i]-pip->world[i])>1e-4f) source_matches=false;
        }
        if(!source_matches) {
            ++m_pip_readback_retries;
            if(m_pip_readback_retries<=4 || diagnose) spdlog::info("PipSnapshotGate: source={} cpu_pip={} phase={} retries={}",completion->origin?completion->origin->render_id:0,pip->game_frame->render_id,phase,m_pip_readback_retries);
            m_pip_completion.request_snapshot(uint64_t(phase)+1);return;
        }
        const auto snapshot_now=std::chrono::steady_clock::now();
        if(!completion || completion->resource!=binding.resource || completion->queue!=reinterpret_cast<uintptr_t>(hook->get_command_queue()) ||
           snapshot_now<completion->submitted || snapshot_now-completion->submitted>std::chrono::milliseconds(250)) return;
        const auto desc=resource->GetDesc();
        Microsoft::WRL::ComPtr<ID3D12Device> resource_device;
        D3D12_FEATURE_DATA_FORMAT_INFO format_info{desc.Format,0};
        if(FAILED(resource->GetDevice(IID_PPV_ARGS(&resource_device))) ||
            FAILED(resource_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,&format_info,sizeof(format_info)))) return;
        const auto views=m_render_targets.views(binding.resource);
        if(std::ranges::none_of(views,[](const auto& view){return view.bound!=0;})) {
            if(diagnose) spdlog::info("PipCaptureGate: no observed bound RTV for resource={:x}",binding.resource);
            return;
        }
        std::vector<GpuTextureReadback::Pixels> pixels;
        const auto& readback=captured->boundary;
        const auto queue_status=m_pip_completion.snapshot_status();
        if(SUCCEEDED(hr) && (desc.Width!=experiment.allocated.width || desc.Height!=experiment.allocated.height)) hr=E_INVALIDARG;
        const std::array<GpuTextureReadback::Source,1> sources{{{backbuffer.Get(),D3D12_RESOURCE_STATE_PRESENT}}};
        if(SUCCEEDED(hr)) hr=GpuTextureReadback::capture(hook->get_command_queue(),sources,pixels);
        if(SUCCEEDED(hr)) {
            if(captured->pixels.size()==1 && pixels.size()==1) pixels.push_back(std::move(captured->pixels[0]));else hr=E_UNEXPECTED;
        }
        m_pip_dump_attempted=true;
        m_pip_dumped_phases|=uint8_t(1u<<phase);
        bool saved=false;
        if(SUCCEEDED(hr) && pixels.size()==2 && GpuTextureReadback::save(directory/"main.ppm",pixels[0]) && GpuTextureReadback::save(directory/"pip.ppm",pixels[1])) {
            const auto native_rva=[base](uintptr_t address) {
                HMODULE owner{};return address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(address),&owner) && reinterpret_cast<uintptr_t>(owner)==base?address-base:uintptr_t{};
            };
            const auto& kinds=completion->kinds;
            const auto allocation=readback.allocation.value_or(GpuAllocationProbe::Record{});const auto& range=allocation.range;
            std::ostringstream access_receipt;
            access_receipt<<"{\"model\":\"private_committed_gpu_rt_v3\",\"aliases\":"<<readback.excluded_aliases<<",\"uavs\":"<<readback.excluded_uavs
                <<",\"indirect_compute\":"<<readback.indirect_exclusions[0]<<",\"indirect_raster\":"<<readback.indirect_exclusions[1]<<",\"writers\":[";
            for(size_t i=0;i<readback.excluded_writers.size();++i) access_receipt<<(i?",":"")<<readback.excluded_writers[i];
            access_receipt<<"],\"extended\":[";
            for(size_t i=0;i<readback.extended_exclusions.size();++i) access_receipt<<(i?",":"")<<readback.extended_exclusions[i];
            access_receipt<<"],\"allocation\":";
            if(!readback.allocation) access_receipt<<"null";
            else access_receipt<<"{\"creation_begin_ns\":"<<allocation.creation_begin_ns<<",\"api\":"<<allocation.api<<",\"kind\":"<<uint32_t(range.kind)<<",\"serial\":"<<range.serial<<",\"device\":"<<range.device
                <<",\"resource\":"<<range.resource<<",\"resource_interface\":"<<readback.resource<<",\"heap\":"<<range.heap<<",\"heap_serial\":"<<range.heap_serial
                <<",\"offset\":"<<range.offset<<",\"size\":"<<range.size<<",\"heap_size\":"<<range.heap_size<<",\"alignment\":"<<range.alignment<<",\"heap_flags\":"<<range.heap_flags
                <<",\"properties_known\":"<<(allocation.properties_known?"true":"false")<<",\"actual_heap_flags\":"<<allocation.actual_heap_flags
                <<",\"heap_type\":"<<allocation.properties.Type<<",\"cpu_page\":"<<allocation.properties.CPUPageProperty<<",\"memory_pool\":"<<allocation.properties.MemoryPoolPreference
                <<",\"creation_node\":"<<allocation.properties.CreationNodeMask<<",\"visible_node\":"<<allocation.properties.VisibleNodeMask<<'}';
            access_receipt<<'}';
            std::ofstream evidence{directory/"capture.json"};
            evidence<<std::setprecision(9)<<"{\"presenting_render_id\":"<<presenting->render_id<<",\"presenting_token\":"<<presenting->token
                <<",\"camera_job_render_id\":"<<main->game_frame->render_id<<",\"camera_job_token\":"<<main->game_frame->token
                <<",\"pip_camera_job_render_id\":"<<pip->game_frame->render_id<<",\"pip_camera_job_token\":"<<pip->game_frame->token
                <<",\"pip_resource_state\":"<<binding.state<<",\"pip_tracker_index\":"<<binding.tracker_index<<",\"pip_record_index\":"<<experiment.index
                <<",\"runtime_submission\":false,\"pose_tags_enabled\":"<<(m_pip_pose_tags?"true":"false")
                <<",\"readback_mode\":\"pip_at_writer_batch_main_at_present\",\"paired_readback\":false"
                <<",\"pip_native_pool\":{\"model\":\"present_slot_observation_v1\",\"context\":"<<binding.lease.context<<",\"manager\":"<<binding.lease.manager
                <<",\"records\":"<<binding.lease.records<<",\"record_index\":"<<binding.lease.index<<",\"camera\":"<<binding.lease.camera
                <<",\"pool\":"<<binding.lease.pool<<",\"slots\":"<<binding.lease.slots<<",\"slot\":"<<binding.lease.slot<<",\"texture_index\":"<<binding.lease.texture_index
                <<",\"texture\":"<<binding.lease.texture<<",\"resource\":"<<binding.resource<<",\"width\":"<<binding.lease.allocated.width<<",\"height\":"<<binding.lease.allocated.height
                <<",\"record_recycle\":"<<uint32_t(binding.lease.record_recycle)<<",\"record_deferred_release\":"<<uint32_t(binding.lease.record_deferred_release)
                <<",\"pool_release_marked\":"<<uint32_t(binding.lease.pool_release_marked)<<",\"owner_lock_held\":true,\"writer_boundary_lease_verified\":false,\"gpu_retirement_verified\":false}"
                <<",\"pip_resource_desc\":{\"dimension\":"<<desc.Dimension<<",\"width\":"<<desc.Width<<",\"height\":"<<desc.Height
                <<",\"depth_or_array_size\":"<<desc.DepthOrArraySize<<",\"mip_levels\":"<<desc.MipLevels<<",\"format\":"<<desc.Format
                <<",\"sample_count\":"<<desc.SampleDesc.Count<<",\"sample_quality\":"<<desc.SampleDesc.Quality<<",\"layout\":"<<desc.Layout<<",\"flags\":"<<desc.Flags
                <<",\"plane_count\":"<<uint32_t(format_info.PlaneCount)<<",\"subresource_count\":"<<uint32_t(desc.MipLevels)*desc.DepthOrArraySize*format_info.PlaneCount<<'}'
                <<",\"readback_boundary\":{\"writer_serial\":"<<readback.writer_serial<<",\"writer_submission\":"<<readback.writer_submission
                <<",\"copy_submission\":"<<readback.copy_submission<<",\"executions\":"<<readback.executions<<",\"queue\":"<<readback.queue
                <<",\"resource\":"<<readback.resource<<",\"commands\":"<<readback.commands<<",\"fence_completed\":"<<(readback.fence_completed?"true":"false")
                <<",\"copy_source_state\":"<<readback.source_state<<",\"copy_state_source\":\"observed_raster_and_legacy_transitions\""
                <<",\"state_from_raster\":"<<(readback.raster_state?"true":"false")<<",\"common_state_decay\":"<<(readback.common_decay?"true":"false")
                <<",\"barrier_observation\":{\"model\":\"legacy_with_recording_identity_v6\",\"capabilities_known\":"<<(readback.barrier_capabilities_known?"true":"false")
                <<",\"enhanced_supported\":"<<(readback.enhanced_supported?"true":"false")<<",\"enhanced_hook_installed\":"<<(readback.enhanced_hook_installed?"true":"false")
                <<",\"base_writer_hook_mask\":"<<readback.base_writer_hook_mask<<",\"hazards\":"<<readback.barrier_hazards
                <<",\"binding_capabilities_known\":"<<(readback.binding_capabilities_known?"true":"false")<<",\"list4_supported\":"<<(readback.list4_supported?"true":"false")
                <<",\"binding_hook_mask\":"<<readback.binding_hook_mask<<",\"signature_hook_installed\":"<<(readback.signature_hook_installed?"true":"false")
                <<",\"recording_binding_coverage_verified\":"<<(completion->all_bindings_covered?"true":"false")
                <<",\"command_object_receipts_verified\":"<<(completion->all_lists_tracked?"true":"false")
                <<",\"base_implementations_verified\":"<<(completion->all_bindings_covered?"true":"false")
                <<",\"extended_capabilities_known\":"<<(readback.extended.known?"true":"false")<<",\"list1_supported\":"<<(readback.extended.list1?"true":"false")
                <<",\"list6_supported\":"<<(readback.extended.list6?"true":"false")<<",\"extended_hook_mask\":"<<readback.extended.hook_mask<<'}'
                <<",\"access_exclusions\":"<<access_receipt.str()
                <<",\"retries\":"<<m_pip_readback_retries<<",\"observed_queue_boundary_verified\":true,\"all_writer_coverage_verified\":false,\"source_version_verified\":false}"
                <<",\"final_lens_enabled\":"<<(m_pip_final_lens?"true":"false")<<",\"lens_phase\":"<<phase
                <<",\"gpu_observed_batch\":{\"serial\":"<<completion->serial<<",\"resource\":"<<completion->resource<<",\"queue\":"<<completion->queue<<",\"commands\":"<<completion->commands
                <<",\"generation\":"<<completion->generation<<",\"writes\":"<<completion->writes<<",\"lists\":"<<completion->lists<<",\"submission_sequence\":"<<completion->submission_sequence
                <<",\"observed_target_lists\":"<<completion->target_lists<<",\"all_lists_tracked\":"<<(completion->all_lists_tracked?"true":"false")
                <<",\"barrier_hazards\":"<<completion->barrier_hazards
                <<",\"writer_counts\":{\"draw\":"<<kinds[0]<<",\"indexed\":"<<kinds[1]<<",\"clear\":"<<kinds[2]
                <<",\"copy\":"<<kinds[3]<<",\"copy_texture\":"<<kinds[4]<<",\"resolve\":"<<kinds[5]<<",\"indirect_possible\":"<<kinds[6]<<'}'
                <<",\"last_write_callsite\":"<<completion->last_write_callsite<<",\"last_write_rva\":"<<native_rva(completion->last_write_callsite)
                <<",\"last_write_thread\":"<<completion->last_write_thread<<",\"execute_callsite\":"<<completion->execute_callsite
                <<",\"execute_rva\":"<<native_rva(completion->execute_callsite)
                <<",\"fence_completed\":true,\"gpu_begin\":"<<completion->gpu_begin<<",\"gpu_end\":"<<completion->gpu_end<<",\"frequency\":"<<completion->frequency
                <<",\"gpu_ms\":"<<completion->gpu_ms<<",\"completion_upper_ms\":"<<completion->completion_upper_ms
                <<",\"simulation_frame_verified\":false,\"observed_writer_origin\":";
            if(completion->origin) {
                const auto& origin=*completion->origin;
                evidence<<"{\"source_render_id\":"<<origin.render_id<<",\"source_token\":"<<origin.token<<",\"record\":"<<origin.record<<",\"source_world\":[";
                for(size_t i=0;i<16;++i) {if(i) evidence<<',';evidence<<origin.source_world[i];}
                evidence<<"],\"render_world\":[";
                for(size_t i=0;i<16;++i) {if(i) evidence<<',';evidence<<origin.render_world[i];}
                evidence<<"],\"identity_model\":\"native_copy_slot_v1\",\"camera_revision\":"<<origin.camera_revision
                    <<",\"camera_render_list\":"<<origin.camera_render_list<<",\"camera_pip_entry\":"<<origin.camera_pip_entry<<'}';
                spdlog::info("PipWriterOrigin: source_id={} token={} record={} batch={} pose_tags={}",origin.render_id,origin.token,origin.record,completion->serial,m_pip_pose_tags);
            } else evidence<<"null";
            evidence<<"},\"scene_tonemap\":"<<(m_pip_scene_tonemap && (!m_pip_lens_sweep || phase%2==1)?"true":"false")
                <<",\"scene_tonemap_applications\":"<<NativePipTonemap::get().applied()
                <<",\"main_grading_applications\":"<<NativePipTonemap::get().grading_applied()
                <<",\"scene_effects_applications\":"<<NativePipTonemap::get().effects_applied()
                <<",\"outer_grading_applications\":"<<NativePipTonemap::get().outer_grading_applied()<<",\"pip_rtvs\":[";
            uintptr_t vtable{},colour_method{};SIZE_T read_bytes{};
            if(ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(dispatch->renderer),&vtable,8,&read_bytes) &&
               ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(vtable+0x98),&colour_method,8,&read_bytes))
                spdlog::info("PipColourMethod: renderer={:x} vtable_rva={:x} method_rva={:x}",dispatch->renderer,vtable-base,colour_method-base);
            spdlog::info("PipCompletion: batch={} writes={} lists={} gpu_ms={} completion_upper_ms={}",completion->serial,completion->writes,completion->lists,completion->gpu_ms,completion->completion_upper_ms);
            spdlog::info("PipReadbackBoundary: batch={} writer_submission={} copy_submission={} executions={} fence_completed={} retries={}",readback.writer_serial,readback.writer_submission,readback.copy_submission,readback.executions,readback.fence_completed,m_pip_readback_retries);
            spdlog::info("PipWriter: draw={} indexed={} clear={} copy={} texture_copy={} resolve={} writer_rva={:x} execute_rva={:x} thread={}",
                kinds[0],kinds[1],kinds[2],kinds[3],kinds[4],kinds[5],native_rva(completion->last_write_callsite),native_rva(completion->execute_callsite),completion->last_write_thread);
            for(size_t i=0;i<views.size();++i) {
                if(i) evidence<<',';const auto& view=views[i];
                evidence<<"{\"format\":"<<view.format<<",\"storage\":"<<view.storage<<",\"dimension\":"<<view.dimension
                    <<",\"width\":"<<view.width<<",\"height\":"<<view.height<<",\"created\":"<<view.created<<",\"bound\":"<<view.bound
                    <<",\"resource_dimension\":"<<view.resource_dimension<<",\"flags\":"<<view.flags<<",\"depth_or_array\":"<<view.depth_or_array
                    <<",\"mips\":"<<view.mips<<",\"samples\":"<<view.samples<<",\"planes\":"<<view.planes<<",\"subresources\":"<<view.subresources
                    <<",\"cpu_only\":true}";
            }
            evidence<<"],\"images\":[";
            const EngineCameraProbe::Sample* cameras[]{&*main,&*pip};
            for(size_t eye=0;eye<2;++eye) {
                if(eye) evidence<<',';
                evidence<<"{\"file\":\""<<(eye?"pip.ppm":"main.ppm")<<"\",\"width\":"<<pixels[eye].width<<",\"height\":"<<pixels[eye].height
                    <<",\"format\":"<<pixels[eye].format<<",\"world\":[";
                for(size_t i=0;i<16;++i) {if(i) evidence<<',';evidence<<cameras[eye]->world[i];}
                evidence<<"],\"projection_at_add\":[";
                for(size_t i=0;i<16;++i) {if(i) evidence<<',';evidence<<(*cameras[eye]->projection)[i];}
                evidence<<"]}";
            }
            const auto queue_device=queue_status.queue_device.value_or(GpuQueueProbe::Device{});const auto queue_creation=queue_status.queue_creation.value_or(GpuQueueProbe::Queue{});
            evidence<<"],\"queue_observation\":{\"device_known\":"<<(queue_status.queue_device?"true":"false")<<",\"early_device_return\":"<<(queue_device.early_return?"true":"false")
                <<",\"device_canonical\":"<<queue_device.identity
                <<",\"creation_hook_mask\":"<<queue_device.hooks<<",\"device9\":"<<(queue_device.device9?"true":"false")<<",\"base_calls\":"<<queue_device.calls[0]<<",\"extended_calls\":"<<queue_device.calls[1]
                <<",\"annotated\":"<<queue_device.annotated<<",\"failed\":"<<queue_device.failed<<",\"observer_errors\":"<<GpuQueueProbe::get().errors()<<",\"method_count\":"<<queue_device.method_count
                <<",\"primary_known\":"<<(queue_status.queue_creation?"true":"false")<<",\"primary_serial\":"<<queue_creation.serial<<",\"primary_canonical\":"<<queue_creation.identity
                <<",\"primary_execute\":"<<queue_creation.execute<<",\"primary_type\":"<<uint32_t(queue_creation.desc.Type)<<",\"requested_node\":"<<queue_creation.requested_node<<",\"actual_node\":"<<queue_creation.desc.NodeMask
                <<",\"all_observed_methods_match\":"<<(GpuQueueProbe::all_observed_methods_match(queue_device,queue_creation.execute)?"true":"false")
                <<",\"unknown_foreign\":"<<queue_status.unknown_queue_submissions<<",\"foreign_unknown_lists\":"<<queue_status.foreign_unknown_lists<<",\"foreign_target_writes\":"<<queue_status.foreign_target_submissions
                <<",\"foreign_fenced_submissions\":"<<queue_status.foreign_fenced_submissions<<",\"foreign_fenced_lists\":"<<queue_status.foreign_fenced_lists
                <<",\"foreign_fence_failures\":"<<queue_status.foreign_fence_failures<<",\"foreign_fenced_queue\":"<<queue_status.foreign_fenced_queue
                <<",\"foreign_fence_value\":"<<queue_status.foreign_fence_value<<",\"foreign_completed_value\":"<<queue_status.foreign_completed_value<<",\"foreign_fence_ms\":"<<queue_status.foreign_fence_ms
                <<",\"close_observer_errors\":"<<GpuClosedListProbe::get().errors()<<",\"untracked_target_writes\":"<<queue_status.untracked_target_writes<<",\"foreign_precreation_copies\":"<<queue_status.foreign_precreation_copies<<",\"last_foreign_closure\":";
            const auto& foreign=queue_status.foreign_list;
            if(!foreign.ordinal) evidence<<"null";
            else {
                const auto closure=foreign.closure.value_or(GpuClosedListProbe::Receipt{});
                evidence<<"{\"ordinal\":"<<foreign.ordinal<<",\"queue\":"<<foreign.queue<<",\"commands\":"<<foreign.commands<<",\"identity\":"<<foreign.identity
                    <<",\"type\":"<<foreign.type<<",\"reason\":"<<foreign.reason<<",\"reset\":"<<foreign.reset<<",\"close\":"<<foreign.close
                    <<",\"reset_matches\":"<<(foreign.reset_matches?"true":"false")<<",\"close_matches\":"<<(foreign.close_matches?"true":"false")
                    <<",\"known\":"<<(foreign.closure?"true":"false")<<",\"serial\":"<<closure.serial<<",\"closed_ns\":"<<closure.closed_ns<<",\"target_creation_begin_ns\":"<<foreign.target_creation_begin_ns
                    <<",\"closure_identity\":"<<closure.identity<<",\"device\":"<<closure.device<<",\"closure_type\":"<<closure.type<<",\"precreation_copy\":"<<(foreign.precreation_copy?"true":"false")<<'}';
            }
            evidence<<",\"foreign_completions\":[";
            for(size_t n=0;n<queue_status.foreign_completions.size();++n) {
                if(n) evidence<<',';const auto& done=queue_status.foreign_completions[n];
                evidence<<"{\"submission\":"<<done.submission<<",\"lists\":"<<done.lists<<",\"signaled\":"<<done.signaled<<",\"completed\":"<<done.completed
                    <<",\"queue_canonical\":"<<done.queue<<",\"device\":"<<done.device<<",\"wait_ms\":"<<done.wait_ms<<'}';
            }
            evidence<<"]}";
            evidence<<",\"native_dispatch\":{\"sequence\":"<<dispatch->sequence<<",\"list\":"<<dispatch->list<<",\"renderer\":"<<dispatch->renderer<<",\"cpu_only\":true,\"camera_list_receipt\":";
            if(dispatch->camera_entries) {
                const auto& entry=*dispatch->camera_entries;
                evidence<<"{\"model\":\"native_add_slot_v1\",\"cpu_producing_frame_verified\":true,\"render_id\":"<<entry.render_id<<",\"token\":"<<entry.token
                    <<",\"revision\":"<<entry.revision<<",\"add_list\":"<<entry.add_list<<",\"render_list\":"<<entry.render_list
                    <<",\"main_entry\":"<<entry.main_entry<<",\"pip_entry\":"<<entry.pip_entry<<",\"main_slot\":"<<entry.main_slot<<",\"pip_slot\":"<<entry.pip_slot<<'}';
                spdlog::info("PipCameraListReceipt: id={} token={} revision={} add_list={:x} render_list={:x} main_entry={:x} pip_entry={:x} slots={}/{}",entry.render_id,entry.token,entry.revision,entry.add_list,entry.render_list,entry.main_entry,entry.pip_entry,entry.main_slot,entry.pip_slot);
            } else {
                evidence<<"null";spdlog::info("PipCameraListReceipt: known=false main_list={:x} pip_list={:x} dispatch_list={:x} slots={}/{}",main->list,pip->list,dispatch->list,main->index,pip->index);
                for(const auto& entry:NativeCameraEntries::get().status()) spdlog::info("PipCameraListStatus: list={:x} revision={} inflight={} main_calls={} pip_calls={} missing_frames={} id={} token={} valid={} entries={}",
                    entry.list,entry.revision,entry.inflight,entry.main_calls,entry.pip_calls,entry.missing_frames,entry.render_id,entry.token,entry.valid,entry.entries);
            }
            evidence<<",\"scene_observer\":"<<(dispatch->scene_observer?"true":"false")<<",\"scene_calls\":"<<dispatch->scene_calls
                <<",\"scene_refused\":"<<dispatch->scene_refused<<",\"scene_dropped\":"<<dispatch->scene_dropped<<",\"camera_revision\":";
            if(dispatch->camera_revision) evidence<<*dispatch->camera_revision;else evidence<<"null";
            evidence<<",\"scenes\":[";
            const auto write_output=[&](const NativeRenderProbe::Output& output) {
                evidence<<"{\"resource\":"<<output.resource<<",\"status\":"<<uint32_t(output.status)
                    <<",\"lease_verified\":false,\"epoch\":"<<output.descriptors.epoch<<",\"serial\":"<<output.descriptors.serial<<",\"rtvs\":[";
                for(size_t i=0;i<output.descriptors.views.size();++i) {
                    if(i) evidence<<',';const auto& view=output.descriptors.views[i];
                    evidence<<"{\"resource\":"<<view.resource<<",\"identity\":"<<view.identity<<",\"handle\":"<<view.handle
                        <<",\"known\":"<<(view.known?"true":"false")<<",\"created\":"<<view.created<<",\"bound\":"<<view.bound<<",\"command_list\":"<<view.command_list
                        <<",\"width\":"<<view.width<<",\"height\":"<<view.height<<",\"storage\":"<<view.storage<<",\"format\":"<<view.format
                        <<",\"dimension\":"<<view.dimension<<",\"resource_dimension\":"<<view.resource_dimension<<",\"flags\":"<<view.flags
                        <<",\"depth_or_array\":"<<view.depth_or_array<<",\"mips\":"<<view.mips<<",\"samples\":"<<view.samples
                        <<",\"planes\":"<<view.planes<<",\"subresources\":"<<view.subresources<<'}';
                }
                evidence<<"]}";
            };
            for(size_t n=0;n<dispatch->scenes.size();++n) {
                if(n) evidence<<',';const auto& scene=dispatch->scenes[n];
                evidence<<"{\"ordinal\":"<<scene.ordinal<<",\"dispatch_sequence\":"<<scene.dispatch_sequence<<",\"flags\":"<<scene.flags<<",\"slot\":"<<scene.slot
                    <<",\"renderer\":"<<scene.renderer<<",\"list\":"<<scene.list<<",\"entry\":"<<scene.entry<<",\"active_entry_after\":"<<scene.active_entry_after
                    <<",\"output_wrapper_before\":"<<scene.output_before<<",\"output_wrapper_after\":"<<scene.output_after
                    <<",\"render_counter_before\":"<<scene.render_counter_before<<",\"render_counter_after\":"<<scene.render_counter_after
                    <<",\"view_flags_before\":"<<scene.view_flags_before<<",\"view_flags_after\":"<<scene.view_flags_after
                    <<",\"valid\":"<<(scene.valid()?"true":"false")<<",\"refusal\":"<<uint32_t(scene.refusal)<<",\"camera_revision_before\":";
                if(scene.camera_revision_before) evidence<<*scene.camera_revision_before;else evidence<<"null";
                evidence<<",\"camera_revision_after\":";if(scene.camera_revision_after) evidence<<*scene.camera_revision_after;else evidence<<"null";
                evidence<<",\"resource_before\":";write_output(scene.resource_before);
                evidence<<",\"resource_after\":";write_output(scene.resource_after);
                evidence<<'}';
                spdlog::info("PipSceneReceipt: dispatch={} ordinal={} slot={} flags={} list={:x} entry={:x} active={:x} output={:x}/{:x} counter={}/{} refusal={} cpu_only=true",
                    scene.dispatch_sequence,scene.ordinal,scene.slot,scene.flags,scene.list,scene.entry,scene.active_entry_after,scene.output_before,scene.output_after,
                    scene.render_counter_before,scene.render_counter_after,uint32_t(scene.refusal));
                spdlog::info("PipSceneResource: dispatch={} slot={} resource={:x}/{:x} status={}/{} epoch={}/{} serial={}/{} descriptors={}/{} lease=false",
                    scene.dispatch_sequence,scene.slot,scene.resource_before.resource,scene.resource_after.resource,uint32_t(scene.resource_before.status),uint32_t(scene.resource_after.status),
                    scene.resource_before.descriptors.epoch,scene.resource_after.descriptors.epoch,scene.resource_before.descriptors.serial,scene.resource_after.descriptors.serial,
                    scene.resource_before.descriptors.views.size(),scene.resource_after.descriptors.views.size());
            }
            spdlog::info("PipSceneSummary: dispatch={} installed={} calls={} refused={} dropped={} gpu_pair=false",dispatch->sequence,dispatch->scene_observer,dispatch->scene_calls,dispatch->scene_refused,dispatch->scene_dropped);
            evidence<<"],\"before\":[";
            const auto write_views=[&](const auto& views) {
                for(size_t n=0;n<views.size();++n) {
                    if(n) evidence<<',';const auto& view=views[n];
                    evidence<<"{\"slot\":"<<view.slot<<",\"record\":"<<view.record<<",\"flags\":"<<view.flags<<",\"pip\":"<<(view.pip()?"true":"false")
                        <<",\"fov_h\":"<<view.fov_h<<",\"fov_v\":"<<view.fov_v<<",\"aspect\":"<<view.aspect<<",\"world\":[";
                    for(size_t i=0;i<16;++i) {if(i) evidence<<',';evidence<<view.world[i];}
                    evidence<<"],\"projection_cache\":[";for(size_t i=0;i<16;++i) {if(i) evidence<<',';evidence<<view.projection[i];}
                    evidence<<"],\"inverse_projection_cache\":[";for(size_t i=0;i<16;++i) {if(i) evidence<<',';evidence<<view.inverse_projection[i];}
                    evidence<<"],\"combined_cache\":[";for(size_t i=0;i<16;++i) {if(i) evidence<<',';evidence<<view.combined[i];}
                    evidence<<"],\"rectangle\":[";for(size_t i=0;i<4;++i) {if(i) evidence<<',';evidence<<view.rectangle[i];}
                    evidence<<"],\"corners\":[";for(size_t i=0;i<8;++i) {if(i) evidence<<',';evidence<<view.corners[i];}
                    evidence<<"],\"planes\":[";for(size_t i=0;i<24;++i) {if(i) evidence<<',';evidence<<view.planes[i];}
                    evidence<<"],\"cull_radius\":"<<view.cull_radius<<",\"cull_distance\":"<<view.cull_distance<<",\"cull_directions\":[";
                    for(size_t i=0;i<12;++i) {if(i) evidence<<',';evidence<<view.cull_directions[i];}
                    evidence<<"],\"cull_sides\":[";for(size_t i=0;i<12;++i) {if(i) evidence<<',';evidence<<view.cull_sides[i];}
                    evidence<<"],\"cull_normal\":[";for(size_t i=0;i<3;++i) {if(i) evidence<<',';evidence<<view.cull_normal[i];}
                    evidence<<"],\"cull_origin\":[";for(size_t i=0;i<3;++i) {if(i) evidence<<',';evidence<<view.cull_origin[i];}evidence<<"]}";
                }
            };
            write_views(dispatch->before);evidence<<"],\"after\":[";write_views(dispatch->after);
            evidence<<"]}}\n";saved=bool(evidence);
        }
        spdlog::info("PipPixelCapture: hr={:#x} saved={} present_id={} camera_job={} pip_job={} native_state={:#x} tracker={} size={}x{} format={} path={}",
            uint32_t(hr),saved,presenting->render_id,main->game_frame->render_id,pip->game_frame->render_id,binding.state,binding.tracker_index,desc.Width,desc.Height,static_cast<int>(desc.Format),directory.string());
    });
    if(!visited && diagnose) spdlog::info("PipCaptureGate: resource rejected context={:x} texture={:x}",context,state.texture);
}

void Sm2Probe::capture_eye_image()
{
    if (!m_gpu_capture_enabled) { return; }
    PerformanceTrace::Scope capture_scope{"capture_eye_cpu"};
    m_eye_capture.poll();
    const auto presenting = FrameSyncProbe::get().snapshot().clock.presenting;
    if (!presenting) { ++m_gpu_missing; return; }
    const auto constants = StreamlineProbe::get().frame_camera(presenting->token);
    if (!constants || std::chrono::steady_clock::now()-constants->received > std::chrono::milliseconds(250)) { ++m_gpu_missing; return; }
    const auto& hook = g_framework->get_d3d12_hook();
    if (!hook || !hook->get_swap_chain()) { ++m_gpu_missing; return; }
    Microsoft::WRL::ComPtr<ID3D12Resource> source;
    const auto swap = hook->get_swap_chain();
    if (FAILED(swap->GetBuffer(swap->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&source)))) { ++m_gpu_errors; return; }
    EyeTextureCapture::Metadata metadata{};
    metadata.game_token = presenting->token; metadata.render_id = presenting->render_id; metadata.eye = presenting->eye;
    metadata.world = {constants->right.x,constants->right.y,constants->right.z,0,
        constants->up.x,constants->up.y,constants->up.z,0,constants->forward.x,constants->forward.y,constants->forward.z,0,
        constants->position.x,constants->position.y,constants->position.z,1};
    std::memcpy(metadata.projection.data(),&constants->projection,sizeof(metadata.projection));
    const auto matching = EngineCameraProbe::get().matching_camera(metadata.world,metadata.projection);
    if (matching && matching->game_frame->render_id == presenting->render_id) { ++m_gpu_pose_matches; }
    else if (matching && matching->game_frame->render_id+1 == presenting->render_id) { ++m_gpu_previous_pose_matches; }
    else { ++m_gpu_pose_missing; }
    // The measured one-job delay supplies explicit ownership even if a stationary head
    // produces identical matrices in several historical frames. Global pose uniqueness is
    // useful for discovery, but must not stop valid stationary headset images.
    const auto preceding = EngineCameraProbe::get().rendered_frame(presenting->render_id-1);
    if (preceding) { bind_image_camera(metadata,*presenting,*preceding,std::chrono::steady_clock::now()); }
    if (m_vr_session && !metadata.tracked_stereo) { ++m_gpu_missing; return; }
    const auto menu_state=EngineCinematicState::get().sample();
    metadata.screen_menu=m_vr_session && metadata.tracked_stereo &&
        m_menu_settings.active(menu_state.available,menu_state.published,menu_state.requested,menu_state.scripted);
    metadata.menu_panel=m_menu_settings;
    if(metadata.screen_menu!=m_menu_was_active) {
        m_menu_was_active=metadata.screen_menu;
        spdlog::info("VrMenuPanel: active={} native_flags={:#x}/{:#x} width_degrees={} distance_metres={}",metadata.screen_menu,menu_state.published,menu_state.requested,m_menu_settings.width_degrees,m_menu_settings.distance_metres);
    }
    const auto result = m_eye_capture.capture(hook->get_device(),hook->get_command_queue(),source.Get(),metadata);
    if (result == S_OK) {
        ++m_gpu_copied;
        auto& performance=PerformanceTrace::get();
        if(performance.enabled()) performance.record("eye_copied",PerformanceTrace::now_ms(),0,1);
    }
    else if (result == E_PENDING) { ++m_gpu_busy; }
    else if (FAILED(result)) { ++m_gpu_errors; if (m_gpu_errors <= 3) { spdlog::error("EyeCapture: copy failed hr={:#x}",uint32_t(result)); } }
    if (const auto pair = m_eye_capture.pair(false); pair && pair->right.metadata.render_id != m_last_gpu_pair) {
        ++m_gpu_pairs; m_last_gpu_pair = pair->right.metadata.render_id;
    }
    const auto now=std::chrono::steady_clock::now();
    if(!m_eye_dump_directory.empty() && !PerformanceTrace::get().enabled() && now>=m_next_eye_dump_poll) {
        m_next_eye_dump_poll=now+std::chrono::seconds(1);
        std::ifstream request{m_eye_dump_directory/"request.txt",std::ios::binary};
        char text[43]{};request.read(text,sizeof(text));
        if(m_eye_dump_request.take(std::string{text,static_cast<size_t>(request.gcount())},m_eye_capture.pair(false).has_value())) {
            const auto state=EngineCinematicState::get().sample();
            const auto hr=m_eye_capture.dump_pair(m_eye_dump_directory/m_eye_dump_request.id(),false);
            if(hr==S_OK) {
                std::ofstream evidence{m_eye_dump_directory/m_eye_dump_request.id()/"state.json"};
                evidence<<"{\"native_available\":"<<(state.available?"true":"false")<<",\"paused\":"<<(state.paused?"true":"false")
                    <<",\"scripted\":"<<(state.scripted?"true":"false")<<",\"screen_movies\":[";
                const auto screen=EngineScreenUi::get().snapshot();
                for(size_t n=0;n<screen.movies.size();++n) {
                    if(n) evidence<<',';const auto& movie=screen.movies[n];
                    evidence<<"{\"size\":["<<movie.size[0]<<','<<movie.size[1]<<"],\"origin\":["<<movie.origin[0]<<','<<movie.origin[1]<<"]}";
                }
                evidence<<"]}\n";
            }
            spdlog::info("EyeDump: id={} hr={:#x} paused={} scripted={} path={}",m_eye_dump_request.id(),uint32_t(hr),state.paused,state.scripted,m_eye_dump_directory.string());
        }
    }
}

bool Sm2Probe::first_person_features() const
{
    return m_view_settings.mode==VrViewMode::first_person && !(m_vr_session && m_vr_session->viewer_engaged());
}

void Sm2Probe::log_viewer(uint32_t native_flags)
{
    const auto status=m_vr_session->viewer_status();
    if(status.state==m_viewer_logged.state && status.failures==m_viewer_logged.failures && status.refusals==m_viewer_logged.refusals &&
       status.rig.presets==m_viewer_logged.rig.presets && status.rig.grounded==m_viewer_logged.rig.grounded && status.plays==m_viewer_logged.plays &&
       status.refreezes==m_viewer_logged.refreezes && status.play_end==m_viewer_logged.play_end) return;
    const auto& rig=status.rig;
    const auto driver=EngineArmDriver::get().snapshot();
    const auto publications=m_viewer_release_from?int64_t(driver.updates-*m_viewer_release_from):-1;
    // Hands the arm driver posed since the head release: two per publication while the player's
    // pose is kept, so the publication the freeze holds was posed when this grows up to active.
    const auto posed_hands=m_viewer_release_from?int64_t(driver.hands-m_viewer_release_hands):-1;
    spdlog::info("VrViewer: state={} engaged={} native_flags={:#x} requests={} refusals={} failures={} unreleased={} publications_since_release={} keep_pose={} posed_hands_since_release={} units_per_metre={} anchor=[{},{},{}] grabs={} snaps={} preset={} grounded={} life_units_per_metre={} preset_changes={} plays={} refreezes={} play_end={} play_seconds={:.2f} play_moved={} play_speed={:.2f}",
        VrViewerSequencer::name(status.state),rig.engaged,native_flags,status.requests,status.refusals,status.failures,status.unreleased,publications,m_viewer_keep_pose,posed_hands,rig.units_per_metre,
        rig.anchor[12],rig.anchor[13],rig.anchor[14],rig.grabs,rig.snaps,rig.preset,rig.grounded,rig.life_units_per_metre,rig.presets,
        status.plays,status.refreezes,vr_viewer::name(status.play_end),status.play_seconds,status.play_moved,status.play_speed);
    m_viewer_logged=status;
    // Each photo entry: did the left thumb keep its lifted pose (lift the thumb, then hold B)?
    std::lock_guard lock{m_thumbs_up_mutex};
    if(m_thumbs_up.photo_requests()!=m_thumbs_up_requests_logged) {
        m_thumbs_up_requests_logged=m_thumbs_up.photo_requests();
        spdlog::info("VrThumbsUp: photo_requests={} photo_latches={} mode={}",m_thumbs_up.photo_requests(),m_thumbs_up.photo_latches(),vr_thumbs_up::name(m_thumbs_up.mode()));
    }
}

vr_camera::Matrix Sm2Probe::camera_anchor(const vr_camera::Matrix& game,const EngineCameraProbe::VrEye& eye)
{
    if(eye.viewer_anchor) {trace_camera(game,*eye.viewer_anchor,eye);return *eye.viewer_anchor;}
    if(eye.view_settings.mode==VrViewMode::first_person) return m_first_person_anchor?m_first_person_anchor(game,eye):game;
    const auto now=std::chrono::steady_clock::now();
    const auto actor=EngineBodyProbe::get().active_actor(now);
    std::optional<VrViewCamera::Focus> focus;
    for(const auto& body:EngineBodyProbe::get().snapshot().bodies) {
        if(actor && body.actor==actor && now>=body.received && now-body.received<=std::chrono::milliseconds(100)) {
            focus=VrViewCamera::Focus{actor,body.position};break;
        }
    }
    const auto cinematic=EngineCinematicState::get().authority();
    return m_view_camera.camera(game,eye.view_settings,eye.reference_epoch,eye.units_per_metre,focus,!cinematic || *cinematic);
}

bool Sm2Probe::apply_view_settings(const VrViewSettings& value,bool persist)
{
    if(!value.valid() || !VrViewSettings::allowed(value.mode,m_chase_launch) || (m_vr_session && !m_vr_session->set_view_settings(value))) return false;
    m_view_settings=value;
    m_view_camera.reset();
    const bool first_person=value.mode==VrViewMode::first_person;
    m_controller_input.native_passthrough(!first_person);
    if(!first_person) {
        EngineBodyFollowing::get().allow(false);
        EngineArmDriver::get().allow(false,m_vr_session?m_vr_session->epoch():0);
        EngineWeaponProbe::get().aim_driver().allow(false,0);
    }
    spdlog::info("VrView: mode={} diorama_scale={} distance={} height={} pitch={} follow={} recenter={} shoulder_distance={} height={} side={} chase_scale={}",
        VrViewSettings::name(value.mode),value.diorama_scale,value.diorama_distance,value.diorama_height,value.diorama_pitch,value.diorama_follow,value.diorama_recenter,
        value.shoulder_distance,value.shoulder_height,value.shoulder_side,value.chase_scale);
    if(persist && !m_view_settings_path.empty() && !m_view_file.to_save(value).save(m_view_settings_path)) spdlog::warn("VrView: could not save camera settings");
    return true;
}

void Sm2Probe::poll_view_settings()
{
    const auto now=std::chrono::steady_clock::now();
    if(m_view_settings_path.empty() || now<m_next_view_settings_poll) return;
    m_next_view_settings_poll=now+std::chrono::seconds(1);
    if(const auto value=m_view_file.changed(VrViewSettings::load(m_view_settings_path),m_chase_launch);value && *value!=m_view_settings) apply_view_settings(*value);
}

void Sm2Probe::poll_calibration()
{
    const auto now = std::chrono::steady_clock::now();
    if (m_calibration_path.empty() || now < m_next_calibration_poll) { return; }
    m_next_calibration_poll = now+std::chrono::seconds(1); if (!m_calibration_stamp.changed(m_calibration_path)) { return; }
    const auto value = VrCalibration::load(m_calibration_path);
    if (!value) {
        if (!m_calibration_read_failed) { spdlog::warn("VrCalibration: unreadable/invalid file; keeping active calibration"); }
        m_calibration_read_failed = true;
        return;
    }
    m_calibration_read_failed = false;
    if (*value != m_vr_calibration && m_vr_session->set_calibration(*value)) {
        m_vr_calibration = *value;
        spdlog::info("VrCalibration: applied units_per_metre={} height_offset_metres={} head_forward_offset_metres={} sprint_forward_offset_metres={} sprint_blend_seconds={} first_person_units_per_metre={}",value->units_per_metre,value->height_offset_metres,value->head_forward_offset_metres,value->sprint_forward_offset_metres,value->sprint_blend_seconds,value->first_person_units_per_metre.value_or(value->units_per_metre));
    }
}

void Sm2Probe::save_calibration()
{
    spdlog::info("VrCalibration: applied units_per_metre={} height_offset_metres={} head_forward_offset_metres={} sprint_forward_offset_metres={} sprint_blend_seconds={} first_person_units_per_metre={}",m_vr_calibration.units_per_metre,m_vr_calibration.height_offset_metres,m_vr_calibration.head_forward_offset_metres,m_vr_calibration.sprint_forward_offset_metres,m_vr_calibration.sprint_blend_seconds,m_vr_calibration.first_person_units_per_metre.value_or(m_vr_calibration.units_per_metre));
    if (!m_calibration_path.empty() && !m_vr_calibration.save(m_calibration_path)) {
        spdlog::error("VrCalibration: could not persist active calibration");
    }
}

void Sm2Probe::poll_menu_settings() {
    const auto now=std::chrono::steady_clock::now();
    if(m_menu_settings_path.empty() || now<m_next_menu_settings_poll) return;
    m_next_menu_settings_poll=now+std::chrono::seconds(1);if(!m_menu_settings_stamp.changed(m_menu_settings_path)) return;
    if(const auto loaded=VrMenuPanel::load(m_menu_settings_path);loaded && *loaded!=m_menu_settings) {
        m_menu_settings=*loaded;
        spdlog::info("VrMenuPanel: reloaded enabled={} width_degrees={} distance_metres={}",loaded->enabled,loaded->width_degrees,loaded->distance_metres);
    }
}
void Sm2Probe::poll_arm_settings() {
    const auto now=std::chrono::steady_clock::now();
    if (m_arm_settings_path.empty() || now<m_next_arm_settings_poll) { return; }
    m_next_arm_settings_poll=now+std::chrono::seconds(1);if(!m_arm_settings_stamp.changed(m_arm_settings_path)) return;
    const auto settings=EngineArmDriver::Settings::load(m_arm_settings_path);
    if (settings && *settings!=EngineArmDriver::get().settings()) { EngineArmDriver::get().set_settings(*settings); spdlog::info("ArmSettings: reloaded {}",m_arm_settings_path.string()); }
}
void Sm2Probe::poll_swing_settings() {
    const auto now=std::chrono::steady_clock::now();
    if(!m_vr_session || m_swing_settings_path.empty() || now<m_next_swing_settings_poll) return;
    m_next_swing_settings_poll=now+std::chrono::seconds(1);if(!m_swing_settings_stamp.changed(m_swing_settings_path)) return;
    const auto settings=VrSwingMelee::Settings::load(m_swing_settings_path);
    if(settings && *settings!=m_vr_session->swing_settings()) { m_vr_session->set_swing_settings(*settings);spdlog::info("VrSwing: reloaded {}",m_swing_settings_path.string()); }
}
void Sm2Probe::poll_camera_guard() {
    const auto now=std::chrono::steady_clock::now();
    if(m_camera_guard_path.empty() || now<m_next_camera_guard_poll) return;
    m_next_camera_guard_poll=now+std::chrono::seconds(1);if(!m_camera_guard_stamp.changed(m_camera_guard_path)) return;
    const auto loaded=VrBodyFollowing::Settings::load_camera_guard(m_camera_guard_path);
    if(!loaded) return;
    auto settings=EngineBodyFollowing::get().settings();bool changed=false;
    for(const auto& p:VrBodyFollowing::Settings::CAMERA_PARAMETERS) if(settings.*p.field!=(*loaded).*p.field) {settings.*p.field=(*loaded).*p.field;changed=true;}
    if(!changed) return;
    if(EngineBodyFollowing::get().set_settings(settings)) spdlog::info("CameraGuard: reloaded {} limits=[{},{}] turn_threshold_degrees={} ride_speed_metres_per_second={} ride_vignette={} release_speed_metres_per_second={} release_seconds={}",m_camera_guard_path.string(),
        settings.camera_radius_metres,settings.camera_height_metres,settings.turn_threshold_degrees,settings.ride_speed_metres_per_second,settings.ride_vignette,
        settings.release_speed_metres_per_second,settings.release_seconds);
}
// Diagnostic (-TakeoverControl): 'takeover' feeds a pulled-back, bobbing animation camera into
// body following, 'off' the real one (docs/ANIMATION_CAMERA.md). Polled once a second.
void Sm2Probe::poll_takeover_control() {
    const auto now=std::chrono::steady_clock::now();
    if(m_takeover_control_path.empty() || now<m_next_takeover_control_poll) return;
    m_next_takeover_control_poll=now+std::chrono::seconds(1);if(!m_takeover_control_stamp.changed(m_takeover_control_path)) return;
    std::ifstream control{m_takeover_control_path};std::string word;
    if(!(control>>word) || (word!="takeover" && word!="off")) return;
    const bool value=word=="takeover";
    if(value==m_takeover_injected) return;
    m_takeover_injected=value;EngineBodyFollowing::get().inject_takeover(value);
    spdlog::info("TakeoverControl: injected={}",value);
}
void Sm2Probe::poll_melee_sweep_control() {
    const auto now=std::chrono::steady_clock::now();
    if(m_melee_sweep_control_path.empty() || now<m_next_melee_sweep_control_poll) return;
    m_next_melee_sweep_control_poll=now+std::chrono::seconds(1);if(!m_melee_sweep_control_stamp.changed(m_melee_sweep_control_path)) return;
    std::ifstream control{m_melee_sweep_control_path};std::string word;
    if(!(control>>word)) return;
    const auto mode=vr_melee_sweep::parse(word);
    if(!mode || *mode==EngineMeleeSweep::get().mode()) return;
    EngineMeleeSweep::get().set_mode(*mode);
    spdlog::info("MeleeSweep: mode={} (control)",vr_melee_sweep::name(*mode));
}
void Sm2Probe::poll_weapon_settings() {
    const auto now=std::chrono::steady_clock::now();
    if(m_weapon_settings_path.empty() || now<m_next_weapon_settings_poll) return;
    m_next_weapon_settings_poll=now+std::chrono::seconds(1);if(!m_weapon_settings_stamp.changed(m_weapon_settings_path)) return;
    const auto settings=vr_weapon::Settings::load(m_weapon_settings_path);
    if(settings && *settings!=EngineArmDriver::get().weapon_settings()) {
        EngineArmDriver::get().set_weapon_settings(*settings);
        spdlog::info("WeaponGrip: reloaded {} values=[{},{},{},{},{},{}]",m_weapon_settings_path.string(),settings->values[0],settings->values[1],settings->values[2],settings->values[3],settings->values[4],settings->values[5]);
    }
}
void Sm2Probe::on_draw_ui()
{
    ++m_ui_draw_count;
    if (!ImGui::CollapsingHeader("Space Marine 2 probe", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    if (ImGui::Button("Capture frame-sync phases (bounded)")) { FrameSyncProbe::get().start_trace(16384); }
    if (ImGui::Checkbox("Reset DLSS history every frame (diagnostic)",&m_temporal_reset)) {
        StreamlineProbe::get().set_force_temporal_reset(m_temporal_reset);
        spdlog::info("TemporalReset: diagnostic_reset={}",m_temporal_reset);
    }
    if (m_vr_session) {
        if (ImGui::Checkbox(m_openxr_selected?"OpenXR enabled":"OpenVR enabled",&m_runtime_enabled)) { m_vr_session->enable(m_runtime_enabled); }
        ImGui::TextWrapped("%s",m_vr_session->status().c_str());
        if (ImGui::Button("Recenter headset")) { m_vr_session->recenter(); }
        {
            const auto body=m_vr_session->body_scale();
            if(!body.reference_eye_height) ImGui::TextWrapped("Body scale: off (no reference_eye_height_metres in the calibration)");
            else if(!body.player_eye_height) ImGui::TextWrapped("Body scale: stand straight and recenter to measure (factor 1)");
            else ImGui::TextWrapped("Body scale: eye height %.2f m, reference %.2f m, factor %.3f",*body.player_eye_height,*body.reference_eye_height,body.factor);
            ImGui::Text("Eyes and hands: %.3f game units per metre",body.units_per_metre);
            if(body.last && *body.last!=VrBodyScale::Reading::accepted)
                ImGui::TextColored({1,.6f,.2f,1},"Last recenter was %s: height kept. Stand straight and recenter.",VrBodyScale::name(*body.last));
        }
        if(m_chase_launch && ImGui::TreeNode("Camera mode")) {
            ImGui::TextWrapped("Chase camera launch (-ChaseCam): the game's own over-the-shoulder camera in stereo. F9 or gamepad Back/View recenters; hold Back and press D-pad up/down to make the world bigger/smaller; all other gamepad controls stay native.");
            auto settings=m_view_settings;
            if(ImGui::SliderFloat("chase_scale",&settings.chase_scale,.5f,20)) apply_view_settings(settings,true);
            ImGui::TreePop();
        } else if(!m_chase_launch && ImGui::TreeNode("Camera mode")) {
            auto settings=m_view_settings;
            int mode=static_cast<int>(settings.mode);
            bool changed=ImGui::Combo("View",&mode,"First person / motion controls\0Diorama / gamepad\0Over shoulder / gamepad\0");
            settings.mode=static_cast<VrViewMode>(mode);
            for(const auto& p:VrViewSettings::PARAMETERS) if(p.field!=&VrViewSettings::chase_scale) changed|=ImGui::SliderFloat(p.name,&(settings.*p.field),p.minimum,p.maximum);
            if(ImGui::Button("Reset camera modes")) {settings={};changed=true;}
            if(changed) apply_view_settings(settings,true);
            if(ImGui::Button("Save camera mode preset") && !m_view_settings_path.empty()) {auto path=m_view_settings_path;path+=L".preset";m_view_settings.save(path);}
            const auto state=m_view_camera.snapshot();
            ImGui::Text("Fixed anchor: %s; captures: %llu; follows: %llu; automatic recenters: %llu",state.anchored?"yes":"no",static_cast<unsigned long long>(state.captures),static_cast<unsigned long long>(state.follows),static_cast<unsigned long long>(state.recenters));
            ImGui::TextWrapped("F8 cycles camera modes. F9 recenters. In third person, gamepad Back/View recenters, and Back held with D-pad up/down makes the diorama world bigger/smaller; all other gamepad controls stay native. The diorama also recenters by itself once the Marine walks diorama_recenter metres (at the default scale; 0 off). Camera offsets do not perform wall collision tests.");
            ImGui::TreePop();
        }
        if(ImGui::TreeNode("VR menu panel")) {
            bool changed=ImGui::Checkbox("Project campaign menus for each eye",&m_menu_settings.enabled);
            changed|=ImGui::SliderFloat("Menu width (degrees)",&m_menu_settings.width_degrees,40,100);
            changed|=ImGui::SliderFloat("Menu distance (metres)",&m_menu_settings.distance_metres,1,5);
            if(ImGui::Button("Reset menu panel")) {m_menu_settings={};changed=true;}
            if(changed && !m_menu_settings_path.empty()) m_menu_settings.save(m_menu_settings_path);
            if(ImGui::Button("Save menu panel preset") && !m_menu_settings_path.empty()) {auto preset=m_menu_settings_path;preset+=L".preset";m_menu_settings.save(preset);}
            ImGui::Text("Native campaign menu: %s",m_menu_was_active?"active":"inactive");
            ImGui::TreePop();
        }
        auto controllers = m_controller_input.snapshot();
        controllers.enabled=m_controller_input.motion_preference();
        if (ImGui::Checkbox("Index locomotion (first person)",&controllers.enabled)) { m_controller_input.enable(controllers.enabled); }
        if (ImGui::SliderFloat("Snap turn (degrees)",&controllers.snap_degrees,15,90,"%.0f")) { m_controller_input.set_snap_degrees(controllers.snap_degrees); }
        auto melee=m_controller_input.melee_enabled();
        if(ImGui::Checkbox("Right grip melee",&melee)) m_controller_input.set_melee_enabled(melee);
        if(ImGui::TreeNode("Weapon holsters")) {
            auto enabled=m_vr_session->holsters_enabled();
            if(ImGui::Checkbox("Shoulder = primary, hip = sidearm, raised fist = emote",&enabled)) m_vr_session->enable_holsters(enabled);
            auto settings=m_vr_session->holster_settings();bool changed=false;
            for(size_t i=0;i<settings.values.size();++i) {
                const auto& p=VrHolster::PARAMETERS[i];
                changed|=ImGui::SliderFloat(p.name,&settings.values[i],p.minimum,p.maximum,"%.2f m");
            }
            if(ImGui::Button("Reset holster zones")) { settings={};changed=true; }
            if(changed && m_vr_session->set_holster_settings(settings) && !m_holster_settings_path.empty()) settings.save(m_holster_settings_path);
            const auto holster=m_vr_session->holster_statistics();
            const char* zone=holster.zone==VrHolster::Slot::primary?"shoulder":holster.zone==VrHolster::Slot::secondary?"hip":holster.zone==VrHolster::Slot::salute?"raised fist":"none";
            ImGui::Text("Hand: height %.2f, lateral %.2f, forward %.2f m; zone %s",holster.height,holster.lateral,holster.forward,zone);
            ImGui::Text("Primary %llu, sidearm %llu, raised fist emotes %llu, missed squeezes %llu",static_cast<unsigned long long>(holster.primary),static_cast<unsigned long long>(holster.secondary),
                static_cast<unsigned long long>(holster.salutes),static_cast<unsigned long long>(holster.misses));
            ImGui::TextWrapped("Diagnostic: send one D-pad direction to the game (watch what it does).");
            for(const auto& [label,button]:{std::pair{"Test D-pad up",WORD(XINPUT_GAMEPAD_DPAD_UP)},std::pair{"Test D-pad left",WORD(XINPUT_GAMEPAD_DPAD_LEFT)},
                std::pair{"Test D-pad down",WORD(XINPUT_GAMEPAD_DPAD_DOWN)},std::pair{"Test D-pad right",WORD(XINPUT_GAMEPAD_DPAD_RIGHT)}}) {
                if(ImGui::Button(label)) {m_controller_input.test_pulse(button);spdlog::info("VrDpadTest: sent {}",label);}
                ImGui::SameLine();
            }
            ImGui::NewLine();
            ImGui::TreePop();
        }
        draw_gesture_overlay();
        if(ImGui::TreeNode("Physical melee requests (prototype)")) {
            auto enabled=m_vr_session->swing_melee_enabled();
            if(ImGui::Checkbox("Physical right-hand swings",&enabled)) m_vr_session->enable_swing_melee(enabled);
            auto lifted=m_vr_session->trigger_lift_required();
            if(ImGui::Checkbox("Require trigger finger lifted (touch gate)",&lifted)) m_vr_session->require_trigger_lifted(lifted);
            auto settings=m_vr_session->swing_settings();bool changed=false;
            for(size_t i=0;i<settings.values.size();++i) {
                const auto& p=VrSwingMelee::PARAMETERS[i];
                changed|=ImGui::SliderFloat(p.name,&settings.values[i],p.minimum,p.maximum);
            }
            if(ImGui::Button("Reset swing thresholds")) { settings={};changed=true; }
            if(changed && m_vr_session->set_swing_settings(settings) && !m_swing_settings_path.empty()) settings.save(m_swing_settings_path);
            if(ImGui::Button("Save swing preset") && !m_swing_settings_path.empty()) { auto preset=m_swing_settings_path;preset+=L".preset";m_vr_session->swing_settings().save(preset); }
            const auto swing=m_vr_session->swing_statistics();
            ImGui::Text("Swings: %llu, speed %.2f m/s, travel %.2f m",static_cast<unsigned long long>(swing.swings),swing.speed,swing.travel);
            ImGui::TextWrapped("Sword loadout only: swing with trigger released for light, held for power. Native animation/damage and menu behavior still need combat validation.");
            ImGui::TreePop();
        }
        if(ImGui::TreeNode("Two-hand support aim")) {
            auto settings=m_vr_session->support_settings();bool changed=false;
            for(size_t i=0;i<VrSupportGrip::PARAMETERS.size();++i) {
                const auto& p=VrSupportGrip::PARAMETERS[i];changed|=ImGui::SliderFloat(p.name,&settings.values.values[i],p.minimum,p.maximum);
            }
            if(ImGui::Button("Reset support grip")){settings={};changed=true;}
            if(changed && m_vr_session->set_support_settings(settings) && !m_support_settings_path.empty())settings.save(m_support_settings_path);
            if(ImGui::Button("Save support preset") && !m_support_settings_path.empty()){auto path=m_support_settings_path;path+=L".preset";settings.save(path);}
            const auto support=m_vr_session->support_statistics();
            ImGui::Text("Support: %s, forward %.2fm, radial %.2fm",support.held?"aiming":"released",support.forward,support.radial);
            ImGui::TreePop();
        }
        if(ImGui::TreeNode("Compact status HUD")) {
            auto settings=VrCompactHud::get().settings();bool changed=false;
            for(size_t i=0;i<VrCompactHud::PARAMETERS.size();++i){const auto& p=VrCompactHud::PARAMETERS[i];changed|=ImGui::SliderFloat(p.name,&settings.values.values[i],p.minimum,p.maximum);}
            if(ImGui::Button("Reset HUD placement")){settings={};changed=true;}
            if(changed && VrCompactHud::get().set_settings(settings) && !m_hud_settings_path.empty())settings.save(m_hud_settings_path);
            if(ImGui::Button("Save HUD preset") && !m_hud_settings_path.empty()){auto path=m_hud_settings_path;path+=L".preset";settings.save(path);}
            ImGui::Text("Native status: %s",VrCompactHud::get().data()?"fresh":"unavailable");
            ImGui::Checkbox("Native status graphics candidate",&m_native_hud_requested);
            auto crop=m_native_hud_crop;bool crop_changed=false;
            for(size_t i=0;i<VrNativeHud::PARAMETERS.size();++i){const auto& p=VrNativeHud::PARAMETERS[i];crop_changed|=ImGui::SliderFloat(p.name,&crop.values.values[i],p.minimum,p.maximum);}
            if(ImGui::Button("Reset native HUD regions")){crop={};crop_changed=true;}
            if(crop_changed && crop.valid()){m_native_hud_crop=crop;if(!m_native_hud_settings_path.empty())crop.save(m_native_hud_settings_path);}
            if(ImGui::Button("Save native HUD preset") && !m_native_hud_settings_path.empty()){auto path=m_native_hud_settings_path;path+=L".preset";crop.save(path);}
            // Centre of the native canvas: the focused "hold X" prompt (docs/INTERACTION.md).
            auto prompt=m_native_prompt;bool prompt_changed=false;
            for(size_t i=0;i<VrNativePrompt::PARAMETERS.size();++i){const auto& p=VrNativePrompt::PARAMETERS[i];prompt_changed|=ImGui::SliderFloat(p.name,&prompt.values.values[i],p.minimum,p.maximum);}
            prompt.values.values[VrNativePrompt::ENABLED]=prompt.values.values[VrNativePrompt::ENABLED]>=.5f?1.f:0.f;
            if(ImGui::Button("Reset interaction prompt")){prompt={};prompt_changed=true;}
            if(prompt_changed && prompt.valid()){m_native_prompt=prompt;if(!m_native_prompt_settings_path.empty())prompt.save(m_native_prompt_settings_path);}
            // Right-hand corners of the canvas (objective, squad, mission card) on a low panel.
            auto corners=m_native_hud_corners;bool corners_changed=false;
            ImGui::PushID("hud_corners");
            for(size_t i=0;i<VrNativeHudCorners::PARAMETERS.size();++i){const auto& p=VrNativeHudCorners::PARAMETERS[i];corners_changed|=ImGui::SliderFloat(p.name,&corners.values.values[i],p.minimum,p.maximum);}
            ImGui::PopID();
            corners.values.values[VrNativeHudCorners::ENABLED]=corners.values.values[VrNativeHudCorners::ENABLED]>=.5f?1.f:0.f;
            if(ImGui::Button("Reset HUD corners panel")){corners={};corners_changed=true;}
            if(corners_changed && corners.valid()){m_native_hud_corners=corners;if(!m_native_hud_corners_path.empty())corners.save(m_native_hud_corners_path);}
            ImGui::TreePop();
        }
        if (ImGui::Checkbox("Stereo eye offsets",&m_vr_stereo)) { m_vr_session->set_stereo(m_vr_stereo); }
        bool calibration_changed = ImGui::SliderFloat("World units per metre",&m_vr_calibration.units_per_metre,VrCalibration::SCALE.minimum,VrCalibration::SCALE.maximum,"%.3f",ImGuiSliderFlags_Logarithmic);
        {float first_person=m_vr_calibration.first_person_units_per_metre.value_or(m_vr_calibration.units_per_metre);
         if(ImGui::SliderFloat("First-person units per metre",&first_person,VrCalibration::FIRST_PERSON_SCALE.minimum,VrCalibration::FIRST_PERSON_SCALE.maximum,"%.3f")) {m_vr_calibration.first_person_units_per_metre=first_person;calibration_changed=true;}}
        calibration_changed |= ImGui::SliderFloat("Height offset (metres)",&m_vr_calibration.height_offset_metres,VrCalibration::HEIGHT.minimum,VrCalibration::HEIGHT.maximum,"%.3f");
        calibration_changed |= ImGui::SliderFloat("Head forward offset (metres)",&m_vr_calibration.head_forward_offset_metres,VrCalibration::FORWARD.minimum,VrCalibration::FORWARD.maximum,"%.3f");
        calibration_changed |= ImGui::SliderFloat("Sprint forward offset (metres)",&m_vr_calibration.sprint_forward_offset_metres,VrCalibration::SPRINT_FORWARD.minimum,VrCalibration::SPRINT_FORWARD.maximum,"%.3f");
        calibration_changed |= ImGui::SliderFloat("Sprint blend (seconds)",&m_vr_calibration.sprint_blend_seconds,VrCalibration::SPRINT_BLEND.minimum,VrCalibration::SPRINT_BLEND.maximum,"%.2f");
        {const auto sprint=m_vr_session->sprint_camera(std::chrono::steady_clock::now());
         ImGui::Text("Sprint camera: %s, offset %.3f m, native %s",sprint.active?"active":"idle",sprint.offset_metres,sprint.native_known?(sprint.confirmed?"confirmed":"not sprinting"):"unobserved");}
        if (ImGui::Button("Reset VR calibration")) { m_vr_calibration = {}; calibration_changed = true; }
        if (calibration_changed) { m_vr_session->set_calibration(m_vr_calibration); save_calibration(); }
        if (ImGui::Button("Save calibration preset")) {
            if (!m_calibration_path.empty()) {
                auto preset = m_calibration_path; preset += L".preset";
                if (!m_vr_calibration.save(preset)) { spdlog::error("VrCalibration: could not save preset"); }
            } else { spdlog::info("VrCalibrationPreset: units_per_metre={} height_offset_metres={} head_forward_offset_metres={}",m_vr_calibration.units_per_metre,m_vr_calibration.height_offset_metres,m_vr_calibration.head_forward_offset_metres); }
        }
    }

    if(m_vr_session && EngineBodyFollowing::get().installed() && ImGui::TreeNode("External camera guard (candidate)")) {
        auto settings=EngineBodyFollowing::get().settings();bool changed=false;
        for(const auto& p:VrBodyFollowing::Settings::CAMERA_PARAMETERS) changed|=ImGui::SliderFloat(p.name,&(settings.*p.field),p.minimum,p.maximum);
        if(ImGui::Button("Reset camera guard")) {const VrBodyFollowing::Settings defaults;for(const auto& p:VrBodyFollowing::Settings::CAMERA_PARAMETERS) settings.*p.field=defaults.*p.field;changed=true;}
        if(changed && EngineBodyFollowing::get().set_settings(settings) && !m_camera_guard_path.empty()) settings.save_camera_guard(m_camera_guard_path);
        if(ImGui::Button("Save camera guard preset") && !m_camera_guard_path.empty()) {auto path=m_camera_guard_path;path+=L".preset";settings.save_camera_guard(path);}
        const auto state=EngineBodyFollowing::get().snapshot().follow;
        ImGui::Text("Mode: %d, transitions: %llu",static_cast<int>(state.camera_mode),static_cast<unsigned long long>(state.camera_transitions));
        const auto cinematic=EngineCinematicState::get().snapshot();
        ImGui::Text("Native cinematic: %s, paused: %s",cinematic.available?(cinematic.scripted?"active":"inactive"):"unknown",cinematic.paused?"yes":"no");
        ImGui::TextWrapped("Native cinematic state also releases close shots. External cameras keep their native origin; gameplay reacquires its heading without a recenter. Stereo and head tracking remain active.");
        ImGui::TreePop();
    }
    if(m_vr_session && EnginePlayerOpacity::get().installed() && ImGui::TreeNode("Player visibility (prototype)")) {
        const auto fade=EnginePlayerOpacity::get().params_snapshot();
        int fade_scope=fade.enabled?(fade.all_objects?2:1):0;
        if(ImGui::Combo("Camera-distance fade fix",&fade_scope,"Off\0Player only\0All objects\0"))
            EnginePlayerOpacity::get().distance_control(fade_scope==2?"all":fade_scope==1?"opaque":"native");
        auto opaque=EnginePlayerOpacity::get().snapshot().enabled;
        if(!EnginePlayerOpacity::get().opacity_installed()) ImGui::TextUnformatted("Keep own character opaque: launch with SM2VR_PLAYER_OPAQUE=1");
        else if(ImGui::Checkbox("Keep own character opaque",&opaque)) EnginePlayerOpacity::get().enable(opaque);
        if(EngineArmProbe::get().installed() && EngineBodyFollowing::get().installed()) {
            auto hidden=EngineArmDriver::get().snapshot().head_hidden;
            if(ImGui::Checkbox("Hide own head and helmet",&hidden)) { EngineArmDriver::get().configure_native();EngineArmDriver::get().hide_head(hidden); }
            ImGui::TextWrapped("Head bone scale prototype; head shadows also shrink. Native animation restores on the next publication.");
        }
        ImGui::TextWrapped("Opacity comparison affects owned setter, renderer and mesh draw calls. Turning off restores native behavior on the next call. The draw bypass is experimental; no shared mesh resources are changed.");
        ImGui::TreePop();
    }
    if(EngineWeaponProbe::get().installed() && EngineArmProbe::get().installed() && ImGui::TreeNode("Right-hand firearm aiming (candidate)")) {
        auto enabled=EngineArmDriver::get().weapon_enabled();
        if(ImGui::Checkbox("Gun follows right wrist",&enabled)) {
            EngineArmDriver::get().set_weapon_observer([](const auto& pose){EngineWeaponProbe::get().aim_driver().publish(pose);});
            EngineWeaponProbe::get().aim_driver().enable(enabled);EngineArmDriver::get().follow_weapon(enabled);
        }
        auto settings=EngineArmDriver::get().weapon_settings();bool changed=false;
        for(size_t i=0;i<settings.values.size();++i) {const auto& parameter=vr_weapon::PARAMETERS[i];changed|=ImGui::SliderFloat(parameter.name,&settings.values[i],parameter.minimum,parameter.maximum);}
        if(ImGui::Button("Reset gun grip")) {settings={};changed=true;}
        if(changed && EngineArmDriver::get().set_weapon_settings(settings) && !m_weapon_settings_path.empty()) settings.save(m_weapon_settings_path);
        if(ImGui::Button("Save gun preset") && !m_weapon_settings_path.empty()) {auto path=m_weapon_settings_path;path+=L".preset";settings.save(path);}
        ImGui::TextWrapped("One committed gun pose drives native attachment and targeting. Grip/shot fit needs a headset comparison. Left trigger retains native aim/zoom.");
        ImGui::TreePop();
    }
    if (m_vr_session && EngineArmProbe::get().installed() && EngineBodyFollowing::get().installed() && ImGui::TreeNode("Tracked marine arms (prototype)")) {
        auto enabled=EngineArmDriver::get().snapshot().enabled;
        if (ImGui::Checkbox("Controller-driven arms",&enabled)) { if (enabled) { EngineArmDriver::get().configure_native(); } EngineArmDriver::get().enable(enabled); }
        auto fingers_enabled=EngineArmDriver::get().snapshot().fingers_enabled;
        if(ImGui::Checkbox("Index finger tracking",&fingers_enabled)) { EngineArmDriver::get().enable_fingers(fingers_enabled); }
        auto settings=EngineArmDriver::get().settings(); bool changed{};
        for (size_t hand=0;hand<2;++hand) {
            ImGui::PushID(static_cast<int>(hand)); ImGui::TextUnformatted(hand==0?"Left wrist":"Right wrist");
            changed|=ImGui::SliderFloat3("Grip offset (metres)",settings.grips[hand].metres.data(),-0.25f,0.25f,"%.3f");
            changed|=ImGui::SliderFloat3("Grip rotation (XYZ degrees)",settings.grips[hand].degrees.data(),-180,180,"%.1f");
            changed|=ImGui::SliderFloat3("Elbow direction (character axes)",settings.elbow_poles[hand].data(),-2,2,"%.2f"); ImGui::PopID();
        }
        if (ImGui::Button("Reset arm calibration")) { settings={}; changed=true; }
        if (changed && EngineArmDriver::get().set_settings(settings) && !m_arm_settings_path.empty()) { settings.save(m_arm_settings_path); }
        if (ImGui::Button("Save arm preset") && !m_arm_settings_path.empty()) { auto preset=m_arm_settings_path; preset+=L".preset"; settings.save(preset); }
        const auto state=EngineArmDriver::get().snapshot(); ImGui::Text("Hands solved: %llu  Bone writes: %llu  Failures: %llu",state.hands,state.bone_writes,state.failures);
        ImGui::TreePop();
    }
    const auto& hook = g_framework->get_d3d12_hook();
    ImGui::Text("Presents: %llu (%.1f fps)", m_present_count, m_fps);
    if (hook) {
        ImGui::Text("Display: %ux%u  Render: %ux%u", hook->get_display_width(), hook->get_display_height(), hook->get_render_width(), hook->get_render_height());
    }
    const auto engine_state = EngineCameraProbe::get().state();
    const auto engine       = EngineCameraProbe::get().snapshot();
    ImGui::Text("Engine cameras: %s, %llu adds from %zu call sites",
        engine_state == EngineCameraProbe::State::installed ? "hook installed" : engine_state == EngineCameraProbe::State::wrong_build ? "different game build, not hooked" :
        engine_state == EngineCameraProbe::State::idle ? "not installed yet" : "hook failed (see log)", engine.calls, engine.by_caller.size());
    auto shift = EngineCameraProbe::get().main_view_shift();
    if (ImGui::SliderFloat("Main view sideways shift (test)", &shift, -1.0f, 1.0f, "%.3f")) { EngineCameraProbe::get().set_main_view_shift(shift); }
    auto alternate = EngineCameraProbe::get().alternating_shift();
    if (ImGui::SliderFloat("Alternating half-separation (test)", &alternate, 0.0f, 0.1f, "%.3f")) { EngineCameraProbe::get().set_alternating_shift(alternate); }
    if (ImGui::Button("Capture 8192 temporal samples")) {
        EngineCameraProbe::get().start_trace(8192);
        StreamlineProbe::get().start_trace(8192);
    }
    if (!engine.by_caller.empty() && ImGui::TreeNode("Engine camera list (latest per call site)")) {
        for (const auto& s : engine.by_caller) {
            ImGui::Text("+%llx  slot %lld  %.0fx%.0f  fov %.1f/%.1f  pos %.2f %.2f %.2f  near %.2f", static_cast<unsigned long long>(s.caller_rva), static_cast<long long>(s.index),
                s.viewport[2], s.viewport[3], s.fov_h_deg, s.fov_v_deg, s.world[12], s.world[13], s.world[14], s.near_plane);
        }
        ImGui::TreePop();
    }
    const auto state = StreamlineProbe::get().state();
    ImGui::Text("Streamline: %s", state == StreamlineProbe::State::installed ? "hook installed" :
        state == StreamlineProbe::State::waiting ? "waiting for interposer" : "hook unavailable (see log)");
    const auto sample = StreamlineProbe::get().snapshot();
    ImGui::Text("Camera calls: %llu  Invalid: %llu  Rejected: %llu", sample.calls, sample.invalid, sample.rejected);
    if (!sample.latest) {
        ImGui::TextWrapped("Waiting for camera constants. Select DLSS and enter a rendered scene.");
        return;
    }
    const auto& c = *sample.latest;
    ImGui::Text("Frame: %u  Viewport: %u  Age: %.2fs", c.frame, c.viewport,
        std::chrono::duration<float>(std::chrono::steady_clock::now() - c.received).count());
    ImGui::Text("Position: %.3f %.3f %.3f", c.position.x, c.position.y, c.position.z);
    ImGui::Text("Forward: %.3f %.3f %.3f", c.forward.x, c.forward.y, c.forward.z);
    ImGui::Text("Jitter: %.3f %.3f  Near/far: %.4f / %.1f", c.jitter.x, c.jitter.y, c.near_plane, c.far_plane);
    ImGui::Text("FOV: %.3f rad  Aspect: %.3f  Depth inverted: %d", c.fov, c.aspect, static_cast<int>(c.depth_inverted));
    if (ImGui::TreeNode("Projection (row major, no jitter)")) {
        for (const auto& row : c.projection.row) { ImGui::Text("% .5f % .5f % .5f % .5f", row.x, row.y, row.z, row.w); }
        ImGui::TreePop();
    }
}
