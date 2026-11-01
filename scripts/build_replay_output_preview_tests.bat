@echo off
setlocal
call "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat" >nul
if errorlevel 1 exit /b 1
for %%i in ("%~dp0..") do set "TASK_PREVIEW_ROOT=%%~fi"
set "TASK_PREVIEW_OUT=%TASK_PREVIEW_ROOT%/build/replay-preview-tests"
if not exist "%TASK_PREVIEW_OUT%" mkdir "%TASK_PREVIEW_OUT%"
cl /nologo /std:c++17 /EHsc /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DRENDERDOC_PLATFORM_WIN32 /DRENDERDOC_STABLE_BUILD=1 /I"%TASK_PREVIEW_ROOT%/scripts" /I"C:/Repos/sm2-vr/build/tools/renderdoc-replay-1.46/renderdoc/api/replay" /I"C:/Repos/sm2-vr/build/mod/_deps/streamline-src/external/json/include" "%TASK_PREVIEW_ROOT%/tests/test_replay_output_preview.cpp" /Fo"%TASK_PREVIEW_OUT%/test.obj" /Fe"%TASK_PREVIEW_OUT%/test.exe"
if errorlevel 1 exit /b 1
rem CPU only: SDK allocation stubs, no RenderDoc DLL or graphics/replay calls.
"%TASK_PREVIEW_OUT%/test.exe"
exit /b %errorlevel%
