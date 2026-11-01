#pragma once
#ifdef USE_DXGI_HOOK
#include <dxgi1_3.h>
#include <intrin.h>
#include "DxgiProxyBridge.h"

#pragma comment(linker,"/EXPORT:CreateDXGIFactory=PX_CreateDXGIFactory")
extern "C" __declspec(dllexport) HRESULT WINAPI PX_CreateDXGIFactory(REFIID riid,void** out){return sm2_dxgi_proxy::forward<sm2_dxgi_proxy::Symbol::factory,decltype(&CreateDXGIFactory)>(reinterpret_cast<uintptr_t>(_ReturnAddress()),riid,out);}
#pragma comment(linker,"/EXPORT:CreateDXGIFactory1=PX_CreateDXGIFactory1")
extern "C" __declspec(dllexport) HRESULT WINAPI PX_CreateDXGIFactory1(REFIID riid,void** out){return sm2_dxgi_proxy::forward<sm2_dxgi_proxy::Symbol::factory1,decltype(&CreateDXGIFactory1)>(reinterpret_cast<uintptr_t>(_ReturnAddress()),riid,out);}
#pragma comment(linker,"/EXPORT:CreateDXGIFactory2=PX_CreateDXGIFactory2")
extern "C" __declspec(dllexport) HRESULT WINAPI PX_CreateDXGIFactory2(UINT flags,REFIID riid,void** out){return sm2_dxgi_proxy::forward<sm2_dxgi_proxy::Symbol::factory2,decltype(&CreateDXGIFactory2)>(reinterpret_cast<uintptr_t>(_ReturnAddress()),flags,riid,out);}
#pragma comment(linker,"/EXPORT:DXGIGetDebugInterface1=PX_DXGIGetDebugInterface1")
extern "C" __declspec(dllexport) HRESULT WINAPI PX_DXGIGetDebugInterface1(UINT flags,REFIID riid,void** out){return sm2_dxgi_proxy::forward<sm2_dxgi_proxy::Symbol::debug1,decltype(&DXGIGetDebugInterface1)>(reinterpret_cast<uintptr_t>(_ReturnAddress()),flags,riid,out);}
#endif
