@echo off
setlocal
set "ROOT=%~dp0.."
set "DEPS=%~1"
if "%DEPS%"=="" set "DEPS=%ROOT%\build\mod\_deps"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S "%ROOT%\cmake\two-hand-offline" -B "%ROOT%\build\two-hand-offline" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded -DCMAKE_C_FLAGS_DEBUG=/Z7 -DCMAKE_CXX_FLAGS_DEBUG=/Z7 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DSM2VR_OFFLINE_DEPS="%DEPS%" || exit /b 1
cmake --build "%ROOT%\build\two-hand-offline" --parallel 4 || exit /b 1
ctest --test-dir "%ROOT%\build\two-hand-offline" --output-on-failure || exit /b 1
