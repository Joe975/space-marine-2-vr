@echo off
rem Configures and builds the mod (build\mod\bin\dxgi.dll) with MSVC + Ninja.
rem Usage: scripts\build-mod.bat [RelWithDebInfo|Debug]
setlocal
set "CFG=%~1"
if "%CFG%"=="" set "CFG=RelWithDebInfo"
set "ROOT=%~dp0.."
set "B=%ROOT%\build\mod"

rem DirectXTK's build runs CompileShaders.cmd by bare name from its own folder. Some shells (Claude
rem Code's among them) set this variable, which makes that fail with "no such file or directory".
set NoDefaultCurrentDirectoryInExePath=

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSROOT=%%i"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S "%ROOT%" -B "%B%" -G Ninja -DCMAKE_BUILD_TYPE=%CFG% -DCMAKE_POLICY_VERSION_MINIMUM=3.5 || exit /b 1
cmake --build "%B%" --target sm2vr || exit /b 1
rem tests\test_replay_melee_trace.py runs build\mod\melee_replay.exe; rebuild it with the rules it replays.
cmake --build "%B%" --target melee_replay || exit /b 1
cmake --build "%B%" --target streamline_history_probe_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^streamline_history_probe$" || exit /b 1
cmake --build "%B%" --target native_ordinary_camera_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^native_ordinary_camera$" || exit /b 1
cmake --build "%B%" --target gpu_state_evidence_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^gpu_state_evidence$" || exit /b 1
cmake --build "%B%" --target gpu_signature_probe_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^gpu_signature_probe$" || exit /b 1
cmake --build "%B%" --target gpu_allocation_probe_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^gpu_allocation_probe$" || exit /b 1
cmake --build "%B%" --target native_pip_tonemap_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^native_pip_tonemap$" || exit /b 1
cmake --build "%B%" --target gpu_completion_probe_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^gpu_completion_probe$" || exit /b 1
cmake --build "%B%" --target gpu_inline_copy_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^gpu_inline_copy$" || exit /b 1
cmake --build "%B%" --target native_pip_lens_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^native_pip_lens$" || exit /b 1
cmake --build "%B%" --target native_render_probe_tests || exit /b 1
cmake --build "%B%" --target native_ordinary_probe_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^native_ordinary_probe$" || exit /b 1
cmake --build "%B%" --target render_target_probe_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^render_target_probe$" || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^native_render_probe$" || exit /b 1
cmake --build "%B%" --target gpu_texture_readback_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^gpu_texture_readback$" || exit /b 1
cmake --build "%B%" --target native_pip_experiment_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^native_pip_experiment$" || exit /b 1
cmake --build "%B%" --target view_mode_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^view_mode$" || exit /b 1
cmake --build "%B%" --target desktop_window_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^desktop_window$" || exit /b 1
cmake --build "%B%" --target engine_output_size_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^engine_output_size$" || exit /b 1
cmake --build "%B%" --target openxr_backend_tests openxr_input_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^openxr_(backend|input)$" || exit /b 1
cmake --build "%B%" --target performance_trace_tests || exit /b 1
cmake --build "%B%" --target validated_queue_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^validated_queue$" || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^performance_trace$" || exit /b 1
cmake --build "%B%" --target weapon_aim_tests engine_weapon_probe_tests swing_melee_tests screen_ui_tests streamline_probe_tests engine_camera_probe_tests world_markers_tests frame_sync_probe_tests engine_projection_tests eye_texture_capture_tests vr_screen_menu_tests scene_mode_tests native_call_census_tests bounded_table_tests ordinary_stereo_recovery_tests ordinary_dlss_presence_tests ordinary_taa_history_tests vr_session_tests vr_controller_input_tests vr_combat_tests vr_controller_hook_tests engine_controller_registration_tests engine_body_probe_tests body_following_tests engine_arm_probe_tests vr_arm_pose_tests vr_finger_pose_tests vr_thumbs_up_tests engine_arm_driver_tests player_visibility_tests vr_sprint_camera_tests vr_gaze_camera_tests vr_trace_tests native_hud_tests vr_guard_guide_tests owned_stereo_readiness_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^(weapon_aim|engine_weapon_probe|swing_melee|screen_ui|streamline_probe|engine_camera_probe|world_markers|frame_sync_probe|engine_projection|eye_texture_capture|vr_screen_menu|scene_mode|native_call_census|bounded_table|ordinary_stereo_recovery|ordinary_dlss_presence|ordinary_taa_history|vr_session|vr_controller_input|vr_combat|vr_controller_hook|engine_controller_registration|engine_body_probe|body_following|engine_arm_probe|vr_arm_pose|vr_finger_pose|vr_thumbs_up|engine_arm_driver|player_visibility|vr_sprint_camera|vr_gaze_camera|vr_trace|native_hud|vr_guard_guide|owned_stereo_readiness)$" || exit /b 1
cmake --build "%B%" --target native_tracker_handle_guard_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^native_tracker_handle_guard$" || exit /b 1
cmake --build "%B%" --target native_pool_release_guard_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^native_pool_release_guard$" || exit /b 1
cmake --build "%B%" --target game_build_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^game_build$" || exit /b 1
cmake --build "%B%" --target native_temporal_probe_tests || exit /b 1
ctest --test-dir "%B%" --output-on-failure -R "^native_temporal_probe$" || exit /b 1
rem Every other target, then every suite: ctest runs whatever binary is on disk, so a suite this
rem script didn't build ran stale (27 Oct: 41 suites, vr_rumble among them, were never rebuilt).
rem That includes the framework patch and pair retention suites (docs/FREEZE_REHOOK_2026-10-27.md).
cmake --build "%B%" || exit /b 1
ctest --test-dir "%B%" --output-on-failure -j 4 || exit /b 1
