#include "FrameworkCaptureCompatibility.h"
#include <d3d12.h>
#include <optional>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <type_traits>
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static bool throw_diagnostic{};
namespace spdlog {template<class... A>void info(const char* text,A&&... ){if(throw_diagnostic && std::strstr(text,"FrameworkCaptureCompat")){SetLastError(99);throw std::runtime_error("diagnostic failure");}} template<class... A>void error(A&&... ){} }
struct State {bool capture{},loaded{},owner{},bytes{},fail{},throw_native{},applied{};unsigned inspections{},copies{},protects{},calls{};} state;
const IID expected_iid=__uuidof(ID3D12Device);
static void** expected_out{};
static HRESULT WINAPI native(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** out){++state.calls;check(!adapter && level==D3D_FEATURE_LEVEL_11_0 && &iid==&expected_iid && out==expected_out,"actual native dummy call retains complete ABI arguments");check(GetLastError()==43,"guard preserves native incoming error");check(state.applied==state.bytes && !(state.capture && state.loaded && state.owner && state.applied),"capture original hook is never rewritten");SetLastError(84);if(state.throw_native)throw std::runtime_error("native");if(state.fail)return E_FAIL;*out=reinterpret_cast<void*>(0x1234);return S_OK;}
using Create=decltype(&native);
namespace utility {std::optional<std::vector<uint8_t>> get_original_bytes(Create){++state.inspections;return state.bytes?std::optional{std::vector<uint8_t>{1,2}}:std::nullopt;} }
struct ProtectionOverride {template<class T>ProtectionOverride(T,size_t,DWORD){++state.protects;}};
template<class D,class S>void fixture_memcpy(D destination,S source,size_t n){++state.copies;if constexpr(std::is_function_v<std::remove_pointer_t<D>>){if(state.copies==2)state.applied=true;else state.applied=false;}else if constexpr(std::is_function_v<std::remove_pointer_t<S>>){std::memset(destination,9,n);}else{std::memcpy(destination,source,n);} }
static bool active(){SetLastError(97);return state.capture && state.loaded;}
static bool owner(uintptr_t target){check(target==reinterpret_cast<uintptr_t>(&native),"guard receives exact resolved creation target");SetLastError(98);return state.owner;}
static bool attempt(){auto d3d12_create_device=&native;const auto feature_level=D3D_FEATURE_LEVEL_11_0;void* device{};expected_out=&device;
#undef IID_PPV_ARGS
#define IID_PPV_ARGS(value) expected_iid,reinterpret_cast<void**>(value)
#define memcpy fixture_memcpy
#include "framework_capture_dummy.inc"
#undef memcpy
#undef IID_PPV_ARGS
check(device==reinterpret_cast<void*>(0x1234),"native output survives actual branch");return true;}
int main(){try{sm2_dxgi_proxy::api().active=active;sm2_dxgi_proxy::api().renderdoc_origin=owner;
 for(unsigned flags=0;flags<16;++flags){state={};state.capture=flags&1;state.loaded=flags&2;state.owner=flags&4;state.bytes=flags&8;const bool retain=state.capture && state.loaded && state.owner;const auto wanted_bytes=state.bytes;SetLastError(43);if(retain)state.bytes=false; // no legacy inspection is permitted in this case
 check(attempt() && state.calls==1,"actual generated branch calls native exactly once");check(state.inspections==(retain?0:1),"only exact explicit capture hook skips inspection");check(state.protects==(!retain && wanted_bytes?1:0),"legacy protection path preserved");check(state.copies==(!retain && wanted_bytes?3:0) && !state.applied,"capture no writes; legacy bytes restored");if(retain)check(GetLastError()==84,"capture native outgoing error preserved");}
 state={};state.capture=state.loaded=state.owner=state.fail=true;SetLastError(43);check(!attempt() && state.calls==1 && !state.inspections && !state.copies && GetLastError()==84,"capture creation failure propagates without byte writes");
 state={};state.bytes=state.fail=true;SetLastError(43);check(!attempt() && state.calls==1 && state.copies==3 && !state.applied,"legacy failure restores original hooked bytes");
 state={};state.capture=state.loaded=state.owner=state.throw_native=true;SetLastError(43);bool threw=false;try{attempt();}catch(...){threw=true;}check(threw && state.calls==1 && !state.inspections && !state.copies && GetLastError()==84,"capture native exceptions propagate unchanged");
 state={};state.capture=state.loaded=state.owner=true;SetLastError(43);check(!sm2_capture_compat::dummy_original_bytes(reinterpret_cast<uintptr_t>(&native),[]{++state.inspections;return std::optional<std::vector<uint8_t>>{};}) && !state.inspections && GetLastError()==43,"capture guard preserves incoming error");
 state={};state.capture=state.loaded=state.owner=true;throw_diagnostic=true;SetLastError(43);check(attempt() && state.calls==1 && !state.inspections && !state.copies && GetLastError()==84,"throwing optional capture diagnostic cannot prevent exact native call");throw_diagnostic=false;
 std::cout<<"Framework capture CPU fixture PASS\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
