@echo off
setlocal
call "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat" >nul
if errorlevel 1 exit /b 1
for %%i in ("%~dp0..") do set "TASK_ROOT=%%~fi"
set "TASK_OUT=%TASK_ROOT%/build/replay-extract"
set "TASK_HEADERS=C:/Repos/sm2-vr/build/tools/renderdoc-replay-1.46/renderdoc/api/replay"
set "TASK_DLL=C:/Repos/sm2-vr/build/tools/renderdoc-1.46/RenderDoc_1.46_64/renderdoc.dll"
set "TASK_JSON=C:/Repos/sm2-vr/build/mod/_deps/streamline-src/external/json/include"
if not exist "%TASK_OUT%" mkdir "%TASK_OUT%"
lib /nologo /def:"%TASK_ROOT%/scripts/replay_extract_renderdoc.def" /out:"%TASK_OUT%/renderdoc.lib" /machine:x64
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DRENDERDOC_PLATFORM_WIN32 /DRENDERDOC_STABLE_BUILD=1 /I"%TASK_HEADERS%" /I"%TASK_JSON%" "%TASK_ROOT%/scripts/native_replay_extract.cpp" /Fo"%TASK_OUT%/native_replay_extract.obj" /Fe"%TASK_OUT%/native_replay_extract.exe" /link "%TASK_OUT%/renderdoc.lib"
if errorlevel 1 exit /b 1
copy /y "C:\Repos\sm2-vr\build\tools\renderdoc-1.46\RenderDoc_1.46_64\renderdoc.dll" "%TASK_ROOT%\build\replay-extract\renderdoc.dll" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I"%TASK_ROOT%/scripts" /I"%TASK_JSON%" "%TASK_ROOT%/tests/test_replay_extract_support.cpp" /Fo"%TASK_OUT%/test_replay_extract_support.obj" /Fe"%TASK_OUT%/test_replay_extract_support.exe"
if errorlevel 1 exit /b 1
rem This test has NO RenderDoc dependency and never initialises replay.
"%TASK_OUT%/test_replay_extract_support.exe"

if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DRENDERDOC_PLATFORM_WIN32 /DRENDERDOC_STABLE_BUILD=1 /I"%TASK_HEADERS%" /I"%TASK_JSON%" "%TASK_ROOT%/tests/test_replay_extract_paths.cpp" /Fo"%TASK_OUT%/test_replay_extract_paths.obj" /Fe"%TASK_OUT%/test_replay_extract_paths.exe"
if errorlevel 1 exit /b 1
"%TASK_OUT%/test_replay_extract_paths.exe"

if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DRENDERDOC_PLATFORM_WIN32 /DRENDERDOC_STABLE_BUILD=1 /I"%TASK_HEADERS%" /I"%TASK_JSON%" "%TASK_ROOT%/tests/test_replay_extract_scan.cpp" /Fo"%TASK_OUT%/test_replay_extract_scan.obj" /Fe"%TASK_OUT%/test_replay_extract_scan.exe"
if errorlevel 1 exit /b 1
"%TASK_OUT%/test_replay_extract_scan.exe"

if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DRENDERDOC_PLATFORM_WIN32 /DRENDERDOC_STABLE_BUILD=1 /I"%TASK_ROOT%/scripts" /I"%TASK_HEADERS%" /I"%TASK_JSON%" "%TASK_ROOT%/tests/test_replay_output_preview.cpp" /Fo"%TASK_OUT%/test_replay_output_preview.obj" /Fe"%TASK_OUT%/test_replay_output_preview.exe"
if errorlevel 1 exit /b 1
"%TASK_OUT%/test_replay_output_preview.exe"
