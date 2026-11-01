@echo off
rem Builds a git worktree of this repo without fetching the dependencies again: it reuses the main
rem checkout's fetched sources (build\mod\_deps\*-src) and links its refs\vrframework-core.
rem Runs at below-normal priority with -j 6 so it doesn't starve a game or a headset session.
rem Usage (from the worktree): scripts\worktree-build.bat [cmake --build args, e.g. --target sm2vr vr_session_tests]
rem Before "git worktree remove", delete the junction: rmdir refs\vrframework-core (never rmdir /s).
setlocal
set NoDefaultCurrentDirectoryInExePath=
set "ROOT=%~dp0.."
for %%i in ("%ROOT%") do set "ROOT=%%~fi"
for /f "usebackq tokens=*" %%i in (`git -C "%ROOT%" rev-parse --path-format^=absolute --git-common-dir`) do set "COMMON=%%i"
for %%i in ("%COMMON%\..") do set "MAIN=%%~fi"
if /i "%MAIN%"=="%ROOT%" (echo This is the main checkout: use scripts\build-mod.bat. & exit /b 1)
set "D=%MAIN%\build\mod\_deps"
if not exist "%D%\imgui-src" (echo Build the main checkout once first ^(scripts\build-mod.bat^): %D% is missing. & exit /b 1)
if not exist "%ROOT%\refs" mkdir "%ROOT%\refs"
if not exist "%ROOT%\refs\vrframework-core\CMakeLists.txt" mklink /J "%ROOT%\refs\vrframework-core" "%MAIN%\refs\vrframework-core" >nul || exit /b 1
set "B=%ROOT%\build\mod"
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSROOT=%%i"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem scripts\worktree-build.bat test "^(vr_session|vr_holster)$"  runs those ctest cases (build them first).
if /i "%~1"=="test" (ctest --test-dir "%B%" --output-on-failure -R "%~2" & exit /b)
if not exist "%B%\build.ninja" (
cmake -S "%ROOT%" -B "%B%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ^
 -DFETCHCONTENT_SOURCE_DIR_BDDISASM=%D%\bddisasm-src -DFETCHCONTENT_SOURCE_DIR_DIRECTXTK=%D%\directxtk-src ^
 -DFETCHCONTENT_SOURCE_DIR_DIRECTXTK12=%D%\directxtk12-src -DFETCHCONTENT_SOURCE_DIR_FIDELITYFX=%D%\fidelityfx-src ^
 -DFETCHCONTENT_SOURCE_DIR_GLM=%D%\glm-src -DFETCHCONTENT_SOURCE_DIR_IMGUI=%D%\imgui-src ^
 -DFETCHCONTENT_SOURCE_DIR_IMGUIZMO=%D%\imguizmo-src -DFETCHCONTENT_SOURCE_DIR_KANANLIB=%D%\kananlib-src ^
 -DFETCHCONTENT_SOURCE_DIR_MINHOOK=%D%\minhook-src -DFETCHCONTENT_SOURCE_DIR_OPENXR=%D%\openxr-src ^
 -DFETCHCONTENT_SOURCE_DIR_SAFETYHOOK=%D%\safetyhook-src -DFETCHCONTENT_SOURCE_DIR_SPDLOG=%D%\spdlog-src ^
 -DFETCHCONTENT_SOURCE_DIR_STREAMLINE=%D%\streamline-src -DFETCHCONTENT_SOURCE_DIR_XBYAK=%D%\xbyak-src ^
 -DFETCHCONTENT_SOURCE_DIR_ZYDIS=%D%\zydis-src || exit /b 1
)
start "" /belownormal /wait /b cmake --build "%B%" -j 6 %*
if errorlevel 1 exit /b 1
