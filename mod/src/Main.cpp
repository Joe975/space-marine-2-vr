#include "Framework.hpp"
static void dxgi_capture_route_log(const char* name,unsigned depth,uintptr_t caller,const char* route,const char* module){spdlog::info("DxgiCaptureRoute: {} depth={} caller={:x} module={} route={}",name,depth,caller,module,route);}
#define SM2VR_DXGI_PROXY_LOG(name,depth) do { spdlog::info("{} proxy called",name);if(depth)spdlog::info("DxgiCaptureBridge: {} recursion={}",name,depth); } while(false)
#define SM2VR_DXGI_PROXY_ROUTE(name,depth,caller,route) sm2_dxgi_proxy::capture_route(name,depth,caller,route,dxgi_capture_route_log)
#include "DxgiProxy.h"
#undef SM2VR_DXGI_PROXY_LOG
#undef SM2VR_DXGI_PROXY_ROUTE

#include <ShlObj.h>
#include <spdlog/sinks/basic_file_sink.h>
#include "GpuSignatureProbe.h"
#include "EngineOutputSize.h"

namespace
{
    constexpr std::wstring_view GAME_EXE = L"Warhammer 40000 Space Marine 2 - Retail.exe";

    std::filesystem::path executable_path()
    {
        wchar_t buffer[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return buffer;
    }

    // The framework only logs to file in debug builds; this port always logs, outside the game folder.
    // Every process that loads the proxy gets its own file: several helper processes start from the
    // game's bin folder and would otherwise overwrite each other's lines.
    std::shared_ptr<spdlog::logger> make_file_logger(bool is_game)
    {
        wchar_t local_app_data[MAX_PATH]{};
        SHGetSpecialFolderPathW(nullptr, local_app_data, CSIDL_LOCAL_APPDATA, false);
        const auto dir = std::filesystem::path{ local_app_data } / "sm2vr";
        std::error_code ec{};
        std::filesystem::create_directories(dir, ec);

        const auto name   = is_game ? std::string{ "sm2vr.log" } : fmt::format("sm2vr_other_{}.log", GetCurrentProcessId());
        auto       logger = spdlog::basic_logger_mt("sm2vr", (dir / name).string(), true);
        logger->set_pattern("[%H:%M:%S.%e] [%L] [tid:%t] %v");
        // Warnings and errors reach the file at once. Info lines are flushed by spdlog's own thread
        // once a second (init_thread): flushing each one made the 5 s status block (150-450 lines)
        // a 2-3 ms stall on the render thread (MOD_REVIEW_2026-10-29). The flusher keeps running
        // while the game is frozen, so a hang still leaves its last lines in the file.
        logger->flush_on(spdlog::level::warn);
        return logger;
    }

    DWORD WINAPI init_thread(LPVOID param)
    {
        const auto module  = static_cast<HMODULE>(param);
        const auto exe     = executable_path();
        const auto is_game = _wcsicmp(exe.filename().c_str(), GAME_EXE.data()) == 0;
        const auto logger  = make_file_logger(is_game);
        spdlog::set_default_logger(logger);
        spdlog::flush_every(std::chrono::seconds(1));
        spdlog::info("sm2vr loaded as dxgi proxy: pid={} exe='{}' base={:x}", GetCurrentProcessId(), exe.string(), reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
        spdlog::info("command line: {}", GetCommandLineA());

        if (!is_game) {
            spdlog::info("not the game executable; staying a plain dxgi forwarder");
            return 0;
        }

        const auto environment=[](const char* name) {
            std::array<char,32> value{};const auto length=GetEnvironmentVariableA(name,value.data(),static_cast<DWORD>(value.size()));
            return length<value.size()?std::string{value.data(),length}:std::string{"invalid"};
        };
        if(GpuSignatureProbe::requested(environment("SM2VR_PIP_EXPERIMENT"),environment("SM2VR_OPENVR"),environment("SM2VR_OPENXR"),environment("SM2VR_SYNTHETIC_EYES"),environment("SM2VR_ORDINARY_OBSERVER"),environment("SM2VR_ORDINARY_HEADSET"))) {
            spdlog::info("PipEarlySignatureObserver: installed={}",GpuSignatureProbe::get().install_device_creation());
        }

        // Must precede the game's device: the swap chain is created a few seconds after start.
        engine_output_size::Hook::get().install_from_environment();

        // Let the game create its own device and swap chain before the framework probes D3D12.
        Sleep(5000);

        g_framework = std::make_unique<Framework>(module);
        spdlog::set_default_logger(logger);
        spdlog::info("framework constructed");
        return 0;
    }
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        CreateThread(nullptr, 0, init_thread, module, 0, nullptr);
    }
    return TRUE;
}
