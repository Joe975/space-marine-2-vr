#pragma once
#include <Windows.h>

// sl.interposer.dll, as every Streamline probe finds it. Diagnostic SM2VR_DIAG_NO_STREAMLINE=1: the mod
// sees no Streamline at all, as it may on a card without DLSS, so a TAA run can show that stereo doesn't
// lean on it (docs/TAA_STEREO.md).
inline HMODULE streamline_module(){
    static const bool hidden=[]{char flag[4]{};return GetEnvironmentVariableA("SM2VR_DIAG_NO_STREAMLINE",flag,sizeof(flag))==1 && flag[0]=='1';}();
    return hidden?nullptr:GetModuleHandleW(L"sl.interposer.dll");
}
