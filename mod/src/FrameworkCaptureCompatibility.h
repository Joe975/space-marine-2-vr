#pragma once
#include "DxgiProxyBridge.h"

namespace sm2_capture_compat {
// Inspecting/replacing disk bytes is inappropriate for RenderDoc's resolved
// hook. Keep its creation/wrapping path intact, only during explicit capture.
struct NoLog {void operator()(uintptr_t,bool,const char*)const{}};
template<class Inspect,class Log=NoLog> auto dummy_original_bytes(uintptr_t target,Inspect&& inspect,Log log={}){
    bool retain{};{
        sm2_dxgi_proxy::PreserveError error;const bool capture=sm2_dxgi_proxy::api().active();
        retain=target && capture && sm2_dxgi_proxy::api().renderdoc_origin(target);
        if(capture){HMODULE owner{};char path[MAX_PATH]{"<unknown>"};if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(target),&owner))if(!GetModuleFileNameA(owner,path,MAX_PATH))std::memcpy(path,"<unavailable>",14);path[MAX_PATH-1]=0;try{log(target,retain,path);}catch(...){/* Optional diagnostics cannot prevent device creation. */}}
    }
    if(retain)return decltype(inspect()){};
    return inspect();
}
}
