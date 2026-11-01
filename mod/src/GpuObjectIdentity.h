#pragma once
#include <unknwn.h>
#include <wrl/client.h>
#include <cstdint>

struct GpuObjectIdentity {
    static uintptr_t get(IUnknown* object) {
        Microsoft::WRL::ComPtr<IUnknown> canonical;
        return object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&canonical)))?reinterpret_cast<uintptr_t>(canonical.Get()):0;
    }
};
