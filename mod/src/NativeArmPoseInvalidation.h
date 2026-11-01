#pragma once
#include <Windows.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include "GameBuild.h"

namespace vr_arm_cache {
struct Receipt {
    bool called{},valid{};std::optional<uint32_t> before,after;
    const char* status{"not_attempted"};
    bool flush_called{},resolved{};
    std::optional<float> world_error;
    uint32_t verified_hands{};
};
using Read=std::function<bool(uintptr_t,uint32_t&)>;
using Invoke=std::function<void(uintptr_t,uint32_t)>;
// Called once after a complete D0 batch, under its existing instance CS. This
// invokes the native invalidation API, never an evaluator or a guessed flag OR.
inline Receipt invalidate(uintptr_t instance,const Read& read,const Invoke& invoke) {
    const DWORD error=GetLastError();struct Preserve{DWORD error;~Preserve(){SetLastError(error);}} preserve{error};
    Receipt result;
    if(!read||!invoke){result.status="api_unavailable";return result;}
    try {
        uint32_t value{};
        if(!read(instance,value)){result.status="dirty_before_unavailable";return result;}
        result.before=value;result.called=true;invoke(instance,1);
        if(!read(instance,value)){result.status="dirty_after_unavailable";return result;}
        result.after=value;result.valid=(value&1)!=0;
        result.status=result.valid?"invalidated":"native_did_not_invalidate";
    }catch(...){result.status="native_adapter_exception";}
    return result;
}
inline constexpr uintptr_t RVA=0x1d92e40;
inline constexpr uintptr_t FLUSH_RVA=0x1d93150;
using ReadWord=std::function<bool(uintptr_t,uint32_t&)>;
// Mask1 also services pending mask2 in the engine. Refuse that state rather than
// recursively invoking its animation callback. Caller already holds instance CS.
inline Receipt resolve(uintptr_t instance,const ReadWord& read,const Invoke& invalidate_native,const Invoke& flush_native) {
    const DWORD error=GetLastError();struct Preserve{DWORD error;~Preserve(){SetLastError(error);}} preserve{error};
    Receipt result;
    if(!read||!invalidate_native||!flush_native){result.status="resolve_api_unavailable";return result;}
    try {
        uint32_t dirty{},flags{};
        if(!read(instance+0x2f0,dirty)||!read(instance+0x30,flags)){result.status="resolve_before_unavailable";return result;}
        result.before=dirty;
        if(dirty&2){result.status="animation_callback_pending";return result;}
        if(flags&4){result.status="world_evaluation_disabled";return result;}
        result.called=true;invalidate_native(instance,1);
        if(!read(instance+0x2f0,dirty)||!(dirty&1)||(dirty&2)){result.status="resolve_invalidation_refused";return result;}
        result.flush_called=true;flush_native(instance,1);
        if(!read(instance+0x2f0,dirty)){result.status="resolve_after_unavailable";return result;}
        result.after=dirty;result.resolved=(dirty&3)==0;result.valid=result.resolved;
        result.status=result.valid?"world_resolved":"world_remained_dirty";
    }catch(...){result.status="resolve_adapter_exception";}
    return result;
}
inline bool matches_flush(std::span<const uint8_t> bytes) {
    constexpr game_build::Code signature=game_build::hashed(0x1d93150,16,"48f1b91e3835e56b2983fffa6541a8ef67a261f7ff6e8b290afe6bce973e825e");
    return game_build::code(FLUSH_RVA,bytes,signature);
}
inline bool matches(uint32_t stamp,size_t image_size,std::span<const uint8_t> bytes) {
    constexpr game_build::Code signature=game_build::hashed(0x1d92e40,16,"175d2b1429552c48190f1cdeca323842c92004432aa1ca97b4db9a66bbd52ce7");
    const auto at=game_build::rva(stamp,RVA);
    return at&&image_size>=at+signature.size()&&game_build::code(stamp,RVA,bytes,signature);
}
inline std::function<Receipt(uintptr_t)> bind(HMODULE game,bool resolve_world=false) {
    auto* base=reinterpret_cast<const uint8_t*>(game);if(!base)return {};
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>4096)return {};
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto stamp=nt->Signature==IMAGE_NT_SIGNATURE?nt->FileHeader.TimeDateStamp:0;
    const auto rva=game_build::rva(stamp,RVA),flush_rva=game_build::rva(stamp,FLUSH_RVA);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||!game_build::supported(stamp)||!rva||!flush_rva||nt->OptionalHeader.SizeOfImage<rva+16||
        !matches(stamp,nt->OptionalHeader.SizeOfImage,{base+rva,16}))return {};
    if(resolve_world&&(nt->OptionalHeader.SizeOfImage<flush_rva+16||!matches_flush({base+flush_rva,16})))return {};
    const auto native=reinterpret_cast<void(*)(uintptr_t,uint32_t)>(const_cast<uint8_t*>(base)+rva);
    if(resolve_world) {
        const auto flush=reinterpret_cast<void(*)(uintptr_t,uint32_t)>(const_cast<uint8_t*>(base)+flush_rva);
        return [native,flush](uintptr_t instance){return resolve(instance,
            [](uintptr_t address,uint32_t& value){SIZE_T got{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(value),&got)&&got==sizeof(value);},native,flush);};
    }
    return [native](uintptr_t instance){return invalidate(instance,
        [](uintptr_t p,uint32_t& value){SIZE_T got{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p+0x2f0),&value,sizeof(value),&got)&&got==sizeof(value);},native);};
}
}
