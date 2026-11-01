#include <windows.h>
#define XInputGetState HeaderXInputGetState
#define XInputGetCapabilities HeaderXInputGetCapabilities
#define XInputSetState HeaderXInputSetState
#include <Xinput.h>
#undef XInputGetState
#undef XInputGetCapabilities
#undef XInputSetState
#include <cstring>
extern "C" __declspec(dllexport) DWORD WINAPI XInputGetState(DWORD,XINPUT_STATE* state) {
    if (!state) { return ERROR_BAD_ARGUMENTS; }
    std::memset(state,0,sizeof(*state)); state->dwPacketNumber = 123; state->Gamepad.sThumbRY = 7654;
    return ERROR_DEVICE_NOT_CONNECTED;
}
extern "C" __declspec(dllexport) DWORD WINAPI XInputGetCapabilities(DWORD,DWORD,XINPUT_CAPABILITIES* capabilities) {
    if (!capabilities) { return ERROR_BAD_ARGUMENTS; }
    *capabilities = {}; return ERROR_DEVICE_NOT_CONNECTED;
}
// Rumble: records what reached the "device" (none is connected, as on the VR player's PC).
static XINPUT_VIBRATION last_vibration{}; static DWORD vibration_calls{};
extern "C" __declspec(dllexport) DWORD WINAPI XInputSetState(DWORD,XINPUT_VIBRATION* vibration) {
    if (!vibration) { return ERROR_BAD_ARGUMENTS; }
    last_vibration = *vibration; ++vibration_calls; return ERROR_DEVICE_NOT_CONNECTED;
}
extern "C" __declspec(dllexport) DWORD WINAPI FixtureLastVibration(XINPUT_VIBRATION* out) { *out = last_vibration; return vibration_calls; }
