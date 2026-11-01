#include "EngineControllerRegistration.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <spdlog/spdlog.h>
#include <array>
#include <cstring>
#include <algorithm>

namespace {
template<typename T> T read(const void* object,size_t offset) {
    T value{}; std::memcpy(&value,static_cast<const uint8_t*>(object)+offset,sizeof(T)); return value;
}
constexpr game_build::Code UPDATE=game_build::hashed(0x1d6eca0,12,"9dd4d1d6c1c85560ad7318dcb5612b9c7191b121d78dc754b19dcc094afc6520");
constexpr game_build::Code CONSTRUCT=game_build::hashed(0x1d6c300,12,"5396f7ca0db023e3ba4474e9687bb4dc86692b19f971d116bffe6e774d1abdf5");
constexpr game_build::Code ALLOCATE=game_build::hashed(0x26e0840,12,"716bf23e971ffb2fa4394e588e7716d8b507e8e0ec172f70cb39ad196dcbcfb8");
constexpr game_build::Code REGISTER=game_build::hashed(0x1d63870,12,"1941f83175f966a6ba618fbb47319840f340d5f6c603b160ded4c6bcbc2bc2a6");
constexpr game_build::Code INITIALIZE=game_build::hashed(0x1d6c380,12,"dc5539ec1e14d178b38c8341be9b64f62d7ca0ad0fbc6a0b5cdf8dba4b70d7cb");
bool match(std::span<const uint8_t> actual,const auto& expected) {
    return actual.size() >= expected.size() && std::equal(expected.begin(),expected.end(),actual.begin());
}
bool executable(const void* address) {
    MEMORY_BASIC_INFORMATION region{};
    return address && VirtualQuery(address,&region,sizeof(region)) && region.State==MEM_COMMIT &&
        !(region.Protect & (PAGE_GUARD|PAGE_NOACCESS)) &&
        (region.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
}
HMODULE module_of(const void* address) {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,static_cast<LPCWSTR>(address),&module);
    return module;
}
// The export-table address of a function, bypassing any GetProcAddress hook. Forwarded
// exports (address inside the export directory) are not followed.
void* raw_export(HMODULE module,const char* name) {
    __try {
        const auto base=reinterpret_cast<const uint8_t*>(module);
        const auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if(!base || dos->e_magic!=IMAGE_DOS_SIGNATURE) return nullptr;
        const auto nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        if(nt->Signature!=IMAGE_NT_SIGNATURE) return nullptr;
        const auto& entry=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if(!entry.VirtualAddress || !entry.Size) return nullptr;
        const auto exports=reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base+entry.VirtualAddress);
        const auto names=reinterpret_cast<const uint32_t*>(base+exports->AddressOfNames);
        const auto ordinals=reinterpret_cast<const uint16_t*>(base+exports->AddressOfNameOrdinals);
        const auto functions=reinterpret_cast<const uint32_t*>(base+exports->AddressOfFunctions);
        for(uint32_t i=0;i<exports->NumberOfNames;++i) {
            if(std::strcmp(reinterpret_cast<const char*>(base+names[i]),name)!=0) continue;
            if(ordinals[i]>=exports->NumberOfFunctions) return nullptr;
            const auto rva=functions[ordinals[i]];
            if(rva>=entry.VirtualAddress && rva<entry.VirtualAddress+entry.Size) return nullptr;
            return const_cast<uint8_t*>(base+rva);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}
}
namespace {
template<class Function> std::array<Function,2> resolve_export(void* manager,const char* name) {
    HMODULE module{};
    if(!manager) return {};
    std::memcpy(&module,static_cast<uint8_t*>(manager)+0x58,sizeof(module));
    // Only a module that is actually loaded in this process; never dereference a stale handle.
    if(!module || module_of(module)!=module) return {};
    return {reinterpret_cast<Function>(GetProcAddress(module,name)),reinterpret_cast<Function>(raw_export(module,name))};
}
}
std::array<EngineControllerRegistration::Api::GetState,2> EngineControllerRegistration::resolve_native(void* manager) {
    return resolve_export<Api::GetState>(manager,"XInputGetState");
}
std::array<EngineControllerRegistration::Api::SetState,2> EngineControllerRegistration::resolve_native_set(void* manager) {
    return resolve_export<Api::SetState>(manager,"XInputSetState");
}
std::string EngineControllerRegistration::describe(const void* address) {
    if(!address) return "null";
    if(const auto module=module_of(address)) {
        wchar_t path[MAX_PATH]{};const auto length=GetModuleFileNameW(module,path,MAX_PATH);
        std::wstring name{path,length};const auto slash=name.find_last_of(L"\\/");if(slash!=std::wstring::npos) name=name.substr(slash+1);
        std::string narrow;for(const auto c:name) narrow+=c<128?static_cast<char>(c):'?';
        return fmt::format("{}+{:#x}",narrow,reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(module));
    }
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(address,&region,sizeof(region))) return fmt::format("{:#x} (unmapped)",reinterpret_cast<uintptr_t>(address));
    return fmt::format("{:#x} ({} protect={:#x})",reinterpret_cast<uintptr_t>(address),region.Type==MEM_PRIVATE?"private":region.Type==MEM_MAPPED?"mapped":"image?",region.Protect);
}
EngineControllerRegistration& EngineControllerRegistration::get() {
    static auto* instance = new EngineControllerRegistration; return *instance;
}
bool EngineControllerRegistration::matches_build(uint32_t stamp,std::span<const uint8_t> update,
    std::span<const uint8_t> construct,std::span<const uint8_t> allocate,
    std::span<const uint8_t> registration,std::span<const uint8_t> initialize) {
    return game_build::code(stamp,UPDATE_RVA,update,UPDATE) && game_build::code(stamp,CONSTRUCT_RVA,construct,CONSTRUCT) &&
        game_build::code(stamp,ALLOCATE_RVA,allocate,ALLOCATE) && game_build::code(stamp,REGISTER_RVA,registration,REGISTER) &&
        game_build::code(stamp,INITIALIZE_RVA,initialize,INITIALIZE);
}
EngineControllerRegistration::Status EngineControllerRegistration::install_in_game(HMODULE game,VrControllerInput& input) {
    if (status() != Status::idle) { return status(); }
    const auto base = reinterpret_cast<uint8_t*>(game);
    if (!base) { m_status = Status::wrong_build; return status(); }
    const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto stamp=nt->FileHeader.TimeDateStamp;
    const auto r=[stamp](uintptr_t reference){return game_build::rva(stamp,reference);};
    const std::array<uintptr_t,8> at{r(UPDATE_RVA),r(CONSTRUCT_RVA),r(ALLOCATE_RVA),r(REGISTER_RVA),r(INITIALIZE_RVA),r(0x366bfe0),r(0x366bb20),r(STATE_SLOT_RVA)};
    if (std::ranges::any_of(at,[](uintptr_t v){return v==0;}) || nt->OptionalHeader.SizeOfImage < at[7]+sizeof(Api::GetState) ||
        !matches_build(stamp,{base+at[0],12},{base+at[1],12},{base+at[2],12},{base+at[3],12},{base+at[4],12})) {
        m_status = Status::wrong_build;
        spdlog::error("EngineControllerRegistration: unsupported executable {:#x}; leaving native input unchanged",stamp);
        return status();
    }
    Api api{reinterpret_cast<Api::Allocate>(base+at[2]),reinterpret_cast<Api::Construct>(base+at[1]),
        reinterpret_cast<Api::Register>(base+at[3]),reinterpret_cast<uintptr_t>(base+at[5]),
        reinterpret_cast<uintptr_t>(base+at[6]),reinterpret_cast<Api::GetState*>(base+at[7]),&resolve_native};
    // Rumble is optional: a build whose table lacks the slot keeps native rumble and full input.
    if (const auto set=r(SET_STATE_SLOT_RVA); set && nt->OptionalHeader.SizeOfImage >= set+sizeof(Api::SetState)) {
        api.set_slot=reinterpret_cast<Api::SetState*>(base+set); api.resolve_set=&resolve_native_set;
    } else { spdlog::warn("EngineControllerRegistration: no verified rumble slot for executable {:#x}; controller haptics unavailable",stamp); }
    install(base+at[0],api,input); return status();
}
bool EngineControllerRegistration::install(void* update,Api api,VrControllerInput& input) {
    if (status() == Status::installed) { return true; }
    if (!update || !api.allocate || !api.construct || !api.register_pad || !api.manager_vtable || !api.pad_vtable ||
        !api.state_slot || reinterpret_cast<uintptr_t>(api.state_slot)%alignof(void*) ||
        reinterpret_cast<uintptr_t>(api.set_slot)%alignof(void*)) {
        m_status = Status::hook_failed; return false;
    }
    auto result = MH_Initialize();
    if (result != MH_OK && result != MH_ERROR_ALREADY_INITIALIZED) { m_status = Status::hook_failed; return false; }
    m_api = api; m_input = &input; m_original_state = nullptr; m_dispatch_healthy = false;
    m_original_set = nullptr; m_rumble_healthy = false; m_last_rejected_set = nullptr;
    if (MH_CreateHook(update,reinterpret_cast<void*>(&update_hook),reinterpret_cast<void**>(&m_original)) != MH_OK) {
        m_status = Status::hook_failed; return false;
    }
    m_target = update;
    if (MH_EnableHook(update) != MH_OK) { uninstall(); m_status = Status::hook_failed; return false; }
    m_status = Status::installed;
    spdlog::info("EngineControllerRegistration: native input-update hook installed"); return true;
}
void EngineControllerRegistration::uninstall() {
    m_input = nullptr;
    if (m_api.state_slot && m_original_state.load()) {
        // Only remove our own pointer; a later replacement belongs to its installer.
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(m_api.state_slot),
            reinterpret_cast<void*>(m_original_state.load()),reinterpret_cast<void*>(&state_hook));
    }
    if (m_api.set_slot && m_original_set.load()) {
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(m_api.set_slot),
            reinterpret_cast<void*>(m_original_set.load()),reinterpret_cast<void*>(&set_state_hook));
    }
    m_dispatch_healthy = false; m_rumble_healthy = false;
    if (m_target) { MH_DisableHook(m_target); MH_RemoveHook(m_target); }
    m_target = nullptr; m_status = Status::idle;
}
void EngineControllerRegistration::update_hook(void* manager) {
    auto& self = get(); ++self.m_updates;
    self.ensure_dispatch(manager); self.ensure_rumble_dispatch(manager);
    // Device re-enumeration occurs in this update. Append only after it releases its SRW lock.
    self.m_original(manager);
    // Re-enumeration resolves XInput again (e.g. after a focus change). Rebind before the pad polls.
    self.ensure_dispatch(manager); self.ensure_rumble_dispatch(manager); self.ensure_pad(manager);
}
void EngineControllerRegistration::ensure_dispatch(void* manager) {
    const auto input = m_input.load();
    if (!input || !input->needs_dispatch()) { return; }
    auto slot = reinterpret_cast<PVOID volatile*>(m_api.state_slot);
    const auto current = reinterpret_cast<Api::GetState>(InterlockedCompareExchangePointer(slot,nullptr,nullptr));
    if (current == &state_hook) { m_dispatch_healthy = m_original_state.load()!=nullptr; return; }
    m_dispatch_healthy = false;
    if (!current) { // Startup may not have resolved the native slot yet.
        // After a bind, a cleared slot means the engine dropped XInput itself; say so once,
        // since its pad then skips polling exactly like an unadopted route would.
        if (m_original_state.load() && !m_reported_cleared) { m_reported_cleared = true;
            spdlog::warn("EngineControllerRegistration: native GetState slot cleared after binding (captured original {})",describe(reinterpret_cast<void*>(m_original_state.load()))); }
        return;
    }
    auto original = m_original_state.load();
    if (!original) {
        if (!executable(reinterpret_cast<void*>(current))) { return; }
        original = current; m_original_state = original;
    }
    if (current != original) {
        // The engine re-ran its own resolution. Adopt the new route only when it is exactly
        // what that resolution yields now (GetProcAddress or the raw export of the engine's
        // XInput module), is executable and is not ours. Anything else is a foreign hook and
        // stays untouched (unhealthy) so its owner keeps working.
        std::array<Api::GetState,2> routes{};
        if (m_api.resolve && manager && read<uintptr_t>(manager,0) == m_api.manager_vtable) { routes = m_api.resolve(manager); }
        const bool resolved = current == routes[0] || current == routes[1];
        // (current == &state_hook returned above, so adopting can never chain into ourselves.)
        if (!resolved || !executable(reinterpret_cast<void*>(current))) {
            if (current != m_last_rejected) {
                m_last_rejected = current; ++m_dispatch_rejections;
                spdlog::warn("EngineControllerRegistration: GetState slot holds unrecognized {} (captured original {}, engine resolves {} / export {}); leaving it unchanged",
                    describe(reinterpret_cast<void*>(current)),describe(reinterpret_cast<void*>(original)),
                    describe(reinterpret_cast<void*>(routes[0])),describe(reinterpret_cast<void*>(routes[1])));
            }
            return;
        }
        spdlog::info("EngineControllerRegistration: native re-resolution replaced GetState {} with {}; adopting it",
            describe(reinterpret_cast<void*>(original)),describe(reinterpret_cast<void*>(current)));
        // Publish the new chain target before the slot points at us, so a poll never calls a stale route.
        m_original_state = current; original = current; ++m_dispatch_adoptions;
    }
    const auto previous = InterlockedCompareExchangePointer(slot,reinterpret_cast<void*>(&state_hook),reinterpret_cast<void*>(original));
    if (previous == reinterpret_cast<void*>(original)) {
        ++m_dispatch_repairs; m_dispatch_healthy = true; m_last_rejected = nullptr; m_reported_cleared = false;
        spdlog::info("EngineControllerRegistration: bound native GetState dispatch original={} repairs={} adoptions={}",
            describe(reinterpret_cast<void*>(original)),m_dispatch_repairs.load(),m_dispatch_adoptions.load());
    }
}
// Same adoption rules as the GetState slot (see ensure_dispatch), for the rumble route.
void EngineControllerRegistration::ensure_rumble_dispatch(void* manager) {
    const auto input = m_input.load();
    if (!m_rumble.load() || !m_api.set_slot || !input || !input->needs_dispatch()) { return; }
    auto slot = reinterpret_cast<PVOID volatile*>(m_api.set_slot);
    const auto current = reinterpret_cast<Api::SetState>(InterlockedCompareExchangePointer(slot,nullptr,nullptr));
    if (current == &set_state_hook) { m_rumble_healthy = m_original_set.load()!=nullptr; return; }
    m_rumble_healthy = false;
    if (!current) { return; }
    auto original = m_original_set.load();
    if (!original) {
        if (!executable(reinterpret_cast<void*>(current))) { return; }
        original = current; m_original_set = original;
    }
    if (current != original) {
        std::array<Api::SetState,2> routes{};
        if (m_api.resolve_set && manager && read<uintptr_t>(manager,0) == m_api.manager_vtable) { routes = m_api.resolve_set(manager); }
        if ((current != routes[0] && current != routes[1]) || !executable(reinterpret_cast<void*>(current))) {
            if (current != m_last_rejected_set) {
                m_last_rejected_set = current;
                spdlog::warn("EngineControllerRegistration: SetState slot holds unrecognized {} (captured original {}); leaving rumble native",
                    describe(reinterpret_cast<void*>(current)),describe(reinterpret_cast<void*>(original)));
            }
            return;
        }
        m_original_set = current; original = current;
    }
    if (InterlockedCompareExchangePointer(slot,reinterpret_cast<void*>(&set_state_hook),reinterpret_cast<void*>(original)) == reinterpret_cast<void*>(original)) {
        ++m_rumble_binds; m_rumble_healthy = true; m_last_rejected_set = nullptr;
        spdlog::info("EngineControllerRegistration: bound native SetState (rumble) original={} binds={}",
            describe(reinterpret_cast<void*>(original)),m_rumble_binds.load());
    }
}
DWORD WINAPI EngineControllerRegistration::set_state_hook(DWORD index,XINPUT_VIBRATION* vibration) {
    auto& self = get(); ++self.m_rumble_calls;
    const auto original = self.m_original_set.load();
    // The native call and its result are unchanged: the game already tolerates a missing pad.
    const DWORD result = original ? original(index,vibration) : ERROR_DEVICE_NOT_CONNECTED;
    if (index == 0 && vibration) {
        if (const auto mailbox = self.m_rumble.load()) {
            mailbox->publish({vibration->wLeftMotorSpeed,vibration->wRightMotorSpeed},std::chrono::steady_clock::now());
        }
    }
    return result;
}
DWORD WINAPI EngineControllerRegistration::state_hook(DWORD index,XINPUT_STATE* state) {
    auto& self = get(); ++self.m_state_polls;
    const auto original = self.m_original_state.load();
    uint32_t result = original ? original(index,state) : ERROR_DEVICE_NOT_CONNECTED;
    const auto input = self.m_input.load();
    if (input) { input->get_state(&result,index,state);input->native_actions(result,index,state); }
    return result;
}
bool EngineControllerRegistration::ensure_pad(void* manager) {
    const auto input = m_input.load();
    if (!input || !input->snapshot().enabled || !manager) { return false; }
    if (m_registering.test_and_set()) { return false; }
    const auto result = ensure_pad_impl(manager);
    m_registering.clear(); return result;
}
bool EngineControllerRegistration::ensure_pad_impl(void* manager) {
    if (read<uintptr_t>(manager,0) != m_api.manager_vtable) { ++m_failures; return false; }
    auto lock = reinterpret_cast<PSRWLOCK>(static_cast<uint8_t*>(manager)+0x38);
    AcquireSRWLockShared(lock);
    const auto count = read<int32_t>(manager,0x30),capacity = read<int32_t>(manager,0x34);
    const auto devices = read<void**>(manager,0x28);
    if (count < 0 || count > 128 || capacity < count || (count && !devices)) {
        ReleaseSRWLockShared(lock); ++m_failures; return false;
    }
    bool exists{};
    for (int i=0;i<count;++i) {
        if (devices[i] && read<uintptr_t>(devices[i],0) == m_api.pad_vtable && read<uint32_t>(devices[i],0x1240) == 0) { exists = true; break; }
    }
    ReleaseSRWLockShared(lock);
    if (exists) { return true; }
    auto pad = m_api.allocate(PAD_SIZE,0);
    if (!pad) { ++m_failures; return false; }
    m_api.construct(pad); // Existing constructor initializes slot zero and native base fields.
    const auto table = read<uintptr_t*>(pad,0);
    using Initialize = bool(*)(void*);
    using Destroy = void(*)(void*,uint32_t);
    if (!reinterpret_cast<Initialize>(table[1])(pad)) {
        reinterpret_cast<Destroy>(table[0])(pad,1); ++m_failures; return false;
    }
    m_api.register_pad(manager,&pad); // Transfers the allocated object; engine now owns lifetime.
    if (pad) { // Fail closed if the ownership contract was not honored.
        reinterpret_cast<Destroy>(table[0])(pad,1); ++m_failures; return false;
    }
    ++m_registrations;
    spdlog::info("EngineControllerRegistration: registered native XInput slot zero"); return true;
}
