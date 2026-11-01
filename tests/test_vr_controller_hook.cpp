#include "VrControllerHook.h"
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <MinHook.h>
void require(bool ok,const char* message) { if (!ok) { throw std::runtime_error{message}; } }
int main(int argc,char** argv) {
    try {
        require(argc == 2,"fixture path required");
        const auto module = LoadLibraryA(argv[1]); require(module != nullptr,"fixture must load");
        const auto state = reinterpret_cast<DWORD(WINAPI*)(DWORD,XINPUT_STATE*)>(GetProcAddress(module,"XInputGetState"));
        const auto capabilities = reinterpret_cast<DWORD(WINAPI*)(DWORD,DWORD,XINPUT_CAPABILITIES*)>(GetProcAddress(module,"XInputGetCapabilities"));
        require(state && capabilities,"fixture must expose actual legacy XInput signatures");
        VrControllerInput input;
        std::array<uint8_t,5> original{};
        std::memcpy(original.data(),reinterpret_cast<void*>(state),original.size());
        const auto replace = [&](const auto& bytes) {
            DWORD previous{}; auto target = reinterpret_cast<void*>(state);
            require(VirtualProtect(target,bytes.size(),PAGE_EXECUTE_READWRITE,&previous),"fixture code must become writable");
            std::memcpy(target,bytes.data(),bytes.size()); FlushInstructionCache(GetCurrentProcess(),target,bytes.size());
            DWORD ignored{}; require(VirtualProtect(target,bytes.size(),previous,&ignored),"fixture execute protection must restore");
        };
        auto& hook = VrControllerHook::get();
        require(hook.install(input,module),"production API detours must install on loaded fixture exports");
        XINPUT_STATE pad{};
        require(state(0,&pad) == ERROR_DEVICE_NOT_CONNECTED && pad.dwPacketNumber == 123 && pad.Gamepad.sThumbRY == 7654,"disabled detour must preserve original return and bytes");
        input.configure(true,true,false,true); // Legacy combat fixture explicitly opts in.
        XINPUT_CAPABILITIES caps{};
        require(capabilities(0,0,&caps) == ERROR_SUCCESS && caps.Type == XINPUT_DEVTYPE_GAMEPAD,"legacy API startup discovery must see neutral virtual controller");
        const auto now = std::chrono::steady_clock::now();
        input.update(VrControllerSample{},0,true,now);
        VrControllerSample forward{}; forward.move_y = 1; input.update(forward,0,true,now);
        require(state(0,&pad) == ERROR_SUCCESS && pad.Gamepad.sThumbLY == 32767,"game-call ABI must receive mapped movement through real detour");
        forward.fire=1;forward.aim=0.5f;forward.interact=true;forward.melee=1;input.update(forward,0,true,now);
        require(state(0,&pad)==ERROR_SUCCESS && pad.Gamepad.bRightTrigger==255 && pad.Gamepad.bLeftTrigger==128 &&
            pad.Gamepad.wButtons==(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_RIGHT_SHOULDER),"actual XInput DLL detour must forward fire, aim, interact and melee");
        input.set_melee_enabled(false);input.update(forward,0,true,now);
        require(state(0,&pad)==ERROR_SUCCESS && pad.Gamepad.bRightTrigger==255 && pad.Gamepad.bLeftTrigger==128 &&
            pad.Gamepad.sThumbLY==32767 && pad.Gamepad.wButtons==XINPUT_GAMEPAD_B,
            "real XInput detour must suppress grip melee while preserving the gun and walking");
        // Swing requests are gated (opt-in, trigger finger lifted) in VrSession, not by grip melee.
        forward.swing_melee=true;input.update(forward,0,true,now);
        require(state(0,&pad)==ERROR_SUCCESS && pad.Gamepad.wButtons==(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_RIGHT_SHOULDER),
            "real XInput detour must forward a session swing request with grip melee disabled");
        forward.swing_melee=false;input.update(forward,0,true,now);
        replace(original);
        require(state(0,&pad) == ERROR_DEVICE_NOT_CONNECTED,"external startup restore must reproduce loss of the owned movement hook");
        const auto refreshed = hook.refreshes();
        require(hook.install(input,module) && hook.healthy() && hook.refreshes() == refreshed+1,
            "periodic production install must detect original-byte restoration and re-arm the owned hook");
        // Repatching can exceed the 100 ms tracking deadline under game/build load. Feed a
        // fresh neutral/forward pair before testing mapping; stale-cache release is intentional.
        input.update(VrControllerSample{},0,true,std::chrono::steady_clock::now());
        input.update(forward,0,true,std::chrono::steady_clock::now());
        require(state(0,&pad) == ERROR_SUCCESS && pad.Gamepad.sThumbLY == 32767,
            "re-armed export must receive fresh mapped walking");
        require(hook.install(input,module) && hook.refreshes() == refreshed+1,"healthy exports must not be repatched");
        auto unknown = original; unknown[0] ^= 1; replace(unknown);
        require(!hook.install(input,module) && !hook.healthy() &&
            std::memcmp(reinterpret_cast<void*>(state),unknown.data(),unknown.size()) == 0,
            "unknown external modification must remain untouched and report unhealthy");
        replace(original);
        require(hook.install(input,module) && hook.healthy(),"return of exact original bytes must permit owned recovery");
        require(state(1,&pad) == ERROR_DEVICE_NOT_CONNECTED && pad.Gamepad.sThumbRY == 7654,"detour must preserve unrelated controller slots");
        input.update(std::nullopt,0,true,now);
        require(state(0,&pad) == ERROR_SUCCESS && pad.Gamepad.sThumbLY == 0 && pad.Gamepad.bRightTrigger==0 && pad.Gamepad.bLeftTrigger==0 && pad.Gamepad.wButtons==0,"controller loss must release movement and combat via real hooked API");
        hook.uninstall();
        require(state(0,&pad) == ERROR_DEVICE_NOT_CONNECTED && pad.dwPacketNumber == 123,"uninstall must restore original SDK API");
        require(hook.install(input,module),"fixture must allow a fresh healthy hook installation");
        replace(unknown); hook.uninstall();
        require(std::memcmp(reinterpret_cast<void*>(state),unknown.data(),unknown.size()) == 0,
            "uninstall must not replace unfamiliar external code with its saved original bytes");
        // The fixture owns this synthetic replacement. Restore it and retire retained MinHook
        // metadata here; production leaves unknown replacements alone until process teardown.
        replace(original); MH_DisableHook(reinterpret_cast<void*>(state)); MH_RemoveHook(reinterpret_cast<void*>(state));
        require(state(0,&pad) == ERROR_DEVICE_NOT_CONNECTED,"fixture must restore native function after unknown replacement test");
        FreeLibrary(module);
        std::cout << "legacy XInput detour discovery, movement, passthrough and release checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
