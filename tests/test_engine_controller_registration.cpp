#include "synthetic_code.h"
#include "EngineControllerRegistration.h"
#include "game_build_ported.h"
#include <array>
#include <vector>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* message) { if (!value) { throw std::runtime_error{message}; } }
template<typename T> void write(void* object,size_t offset,T value) {
    std::memcpy(static_cast<uint8_t*>(object)+offset,&value,sizeof(value));
}
template<typename T> T read(void* object,size_t offset) {
    T value{}; std::memcpy(&value,static_cast<uint8_t*>(object)+offset,sizeof(value)); return value;
}
std::array<uintptr_t,2> pad_table{};
std::array<uintptr_t,1> manager_table{};
std::array<uint8_t,0x60> manager{};
std::vector<void*> devices;
uint64_t allocations{},destructions{},native_updates{},native_polls{};
uint64_t wrapper_polls{};
bool allocation_fails{},initialization_fails{},enumerate_again{};
XINPUT_STATE observed{};
XINPUT_STATE physical{};
VrControllerInput* generic_wrapper{};
DWORD WINAPI physical_state(DWORD slot,XINPUT_STATE* state) {
    if(!state) return ERROR_BAD_ARGUMENTS;
    if(slot) return ERROR_DEVICE_NOT_CONNECTED;
    *state=physical;uint32_t result=ERROR_SUCCESS;
    if(generic_wrapper) generic_wrapper->get_state(&result,slot,state);
    return result;
}
DWORD(WINAPI* get_state)(DWORD,XINPUT_STATE*){};
decltype(get_state) original_state{};
DWORD WINAPI resolver_wrapper(DWORD slot,XINPUT_STATE* state) { ++wrapper_polls; return original_state(slot,state); }
DWORD WINAPI foreign_state(DWORD,XINPUT_STATE*) { return ERROR_ACCESS_DENIED; }
// What the engine's own GetProcAddress currently answers (an overlay wrapper, or nothing
// extra), and what the next native re-enumeration writes into the slot.
decltype(get_state) resolved_route{},reresolve_to{};
// The engine's rumble slot, resolved by the same native function just before GetState.
DWORD(WINAPI* set_state)(DWORD,XINPUT_VIBRATION*){};
decltype(set_state) original_set{};
DWORD WINAPI foreign_set(DWORD,XINPUT_VIBRATION*) { return ERROR_ACCESS_DENIED; }
std::array<decltype(set_state),2> fixture_set_routes(void* owner) {
    require(owner == manager.data(),"rumble resolver must receive the native manager");
    return {original_set,original_set};
}
std::array<decltype(get_state),2> fixture_routes(void* owner) {
    require(owner == manager.data(),"resolver must receive the native manager");
    return {resolved_route?resolved_route:original_state,original_state};
}
void sync_devices() {
    write(manager.data(),0x28,devices.data()); write(manager.data(),0x30,int32_t(devices.size()));
    write(manager.data(),0x34,int32_t(devices.capacity()));
}
void destroy(void* pad,uint32_t flag) {
    require(flag == 1,"failed native initialization must use deleting destructor");
    ++destructions; delete[] static_cast<uint8_t*>(pad);
}
bool initialize(void*) { return !initialization_fails; }
void clear_devices() { for (auto pad:devices) { destroy(pad,1); } devices.clear(); sync_devices(); }
void* allocate(size_t size,uint32_t flags) {
    require(size == 0x1248 && flags == 0,"registration must use exact game allocator size/alignment ABI");
    if (allocation_fails) { return nullptr; }
    ++allocations; return new uint8_t[size]{};
}
void* construct(void* pad) {
    write(pad,0,reinterpret_cast<uintptr_t>(pad_table.data())); write(pad,0x1240,uint32_t(0)); return pad;
}
void register_pad(void* owner,void** pad) {
    require(owner == manager.data(),"registration must transfer to the input manager received by the update");
    auto lock = reinterpret_cast<PSRWLOCK>(static_cast<uint8_t*>(owner)+0x38);
    // Production ensure must not retain the manager read lock across native registration.
    require(TryAcquireSRWLockExclusive(lock),"native registration must acquire manager lock without recursive deadlock");
    devices.push_back(*pad); *pad = nullptr; sync_devices(); ReleaseSRWLockExclusive(lock);
}
__declspec(noinline) void native_update(void* owner) {
    require(owner == manager.data(),"native update must retain original manager argument"); ++native_updates;
    if (enumerate_again) { clear_devices(); get_state = reresolve_to ? reresolve_to : original_state; set_state = original_set; enumerate_again = false; }
    for (auto pad:devices) {
        ++native_polls; get_state(read<uint32_t>(pad,0x1240),&observed);
    }
}
}
int main(int argc,char** argv) {
    try {
        require(argc == 2,"XInput fixture required");
        const auto sdk = LoadLibraryA(argv[1]); require(sdk != nullptr,"fixture DLL must load");
        get_state = reinterpret_cast<decltype(get_state)>(GetProcAddress(sdk,"XInputGetState"));
        require(get_state != nullptr,"fixture state ABI must exist");
        original_state = get_state;
        set_state = reinterpret_cast<decltype(set_state)>(GetProcAddress(sdk,"XInputSetState")); original_set = set_state;
        const auto delivered = reinterpret_cast<DWORD(WINAPI*)(XINPUT_VIBRATION*)>(GetProcAddress(sdk,"FixtureLastVibration"));
        require(set_state && delivered,"fixture rumble ABI must exist");
        {
            // The in-game resolver reads the engine's XInput module from manager+0x58 and returns
            // GetProcAddress plus the raw export (parsed from the real DLL's export table).
            write(manager.data(),0x58,sdk);
            const auto routes=EngineControllerRegistration::resolve_native(manager.data());
            require(routes[0]==get_state && routes[1]==get_state,"real module: GetProcAddress and raw export agree when nothing hooks the resolver");
            const auto set_routes=EngineControllerRegistration::resolve_native_set(manager.data());
            require(set_routes[0]==set_state && set_routes[1]==set_state,"real module: the rumble export resolves the same way");
            write(manager.data(),0x58,reinterpret_cast<HMODULE>(uintptr_t{0x1234}));
            const auto stale=EngineControllerRegistration::resolve_native(manager.data());
            require(!stale[0] && !stale[1],"a handle that is not a loaded module resolves to nothing");
            require(EngineControllerRegistration::resolve_native(nullptr)[0]==nullptr,"no manager, no route");
            write(manager.data(),0x58,HMODULE{});
            const auto where=EngineControllerRegistration::describe(reinterpret_cast<void*>(get_state));
            require(where.find("+0x")!=std::string::npos && where.find(".dll")!=std::string::npos,"diagnostics name the module and offset of a route");
            require(EngineControllerRegistration::describe(nullptr)=="null","null route diagnostic");
        }
        // A resolver wrapper can exist while the engine holds the original export address.
        // Hooking that unrelated route cannot make a native pad receive VR movement.
        auto resolved_elsewhere = &resolver_wrapper;
        require(resolved_elsewhere != get_state,"fixture must reproduce distinct resolver/native routes");
        VrControllerInput input;
        pad_table = {reinterpret_cast<uintptr_t>(&destroy),reinterpret_cast<uintptr_t>(&initialize)};
        write(manager.data(),0,reinterpret_cast<uintptr_t>(manager_table.data()));
        InitializeSRWLock(reinterpret_cast<PSRWLOCK>(manager.data()+0x38)); sync_devices();
        EngineControllerRegistration::Api api{allocate,construct,register_pad,
            reinterpret_cast<uintptr_t>(manager_table.data()),reinterpret_cast<uintptr_t>(pad_table.data()),&get_state,&fixture_routes,&set_state,&fixture_set_routes};
        auto& registration = EngineControllerRegistration::get();
        require(registration.install(reinterpret_cast<void*>(&native_update),api,input),"native update detour must install");
        void(*volatile tick)(void*) = native_update;
        tick(manager.data()); require(devices.empty() && native_updates == 1,"disabled mode must preserve native update and register nothing");
        require(get_state == original_state,"disabled startup must not alter native dispatch");
        input.configure(true,true,false);
        get_state = nullptr;
        tick(manager.data()); require(devices.size() == 1 && registration.registrations() == 1,"enabled input must register a native slot zero without any HID gamepad");
        require(!registration.dispatch_healthy(),"unresolved startup dispatch must remain untouched");
        get_state = original_state;
        const auto now = std::chrono::steady_clock::now();
        input.update(VrControllerSample{},0,true,now);
        VrControllerSample move{}; move.move_y = 1; input.update(move,0,true,now);
        tick(manager.data()); require(native_polls == 1 && observed.Gamepad.sThumbLY == 32767,"native pad must receive mapped walking even when resolver wrapper is bypassed");
        require(wrapper_polls == 0 && registration.state_polls() == 1 && registration.dispatch_healthy(),"telemetry must count the native route, not an unrelated wrapper");
        const auto bound_state = get_state;
        XINPUT_STATE passthrough{};
        require(get_state(1,&passthrough) == ERROR_DEVICE_NOT_CONNECTED && passthrough.dwPacketNumber == 123 && passthrough.Gamepad.sThumbRY == 7654,"other slots must keep original result and state");
        require(get_state(0,nullptr) == ERROR_BAD_ARGUMENTS,"invalid state pointer must preserve native error");
        for (int i=0;i<10;++i) { tick(manager.data()); }
        require(devices.size() == 1 && registration.registrations() == 1,"repeated updates must not duplicate slot zero");
        require(registration.dispatch_repairs() == 1,"healthy dispatch must not be repeatedly replaced");
        input.update(std::nullopt,0,true,now); tick(manager.data());
        require(observed.Gamepad.sThumbLY == 0 && devices.size() == 1,"tracking loss must keep registered pad neutral without destruction");
        enumerate_again = true; tick(manager.data());
        require(devices.size() == 1 && registration.registrations() == 2,"device re-enumeration must restore native pad after original update completes");
        input.update(VrControllerSample{},0,true,now); input.update(move,0,true,now); tick(manager.data());
        require(get_state == bound_state && observed.Gamepad.sThumbLY == 32767 && registration.dispatch_repairs() == 2,"native resolver reset must recover actual walking");
        get_state = foreign_state; tick(manager.data());
        require(get_state == foreign_state && !registration.dispatch_healthy(),"unknown replacement must be preserved rather than chained or overwritten");
        get_state = original_state; tick(manager.data());
        require(observed.Gamepad.sThumbLY == 32767 && registration.dispatch_healthy(),"known original route must rebind before native pad polling");
        {
            // Playtest 18 Oct 14:19: focus moved to the SteamVR mirror and back. The engine dropped
            // its pads, re-enumerated, and its GetProcAddress now answered with an overlay wrapper.
            // Before the fix the slot stayed on that wrapper, polls stopped and walking died.
            const auto regs=registration.registrations(),adoptions=registration.dispatch_adoptions(),rejections=registration.dispatch_rejections();
            resolved_route=&resolver_wrapper;reresolve_to=&resolver_wrapper;enumerate_again=true;wrapper_polls=0;
            input.update(VrControllerSample{},0,true,now);input.update(move,0,true,now);
            tick(manager.data());
            require(registration.registrations()==regs+1,"re-enumeration re-registers slot zero");
            require(get_state==bound_state && registration.dispatch_healthy() && registration.dispatch_adoptions()==adoptions+1,
                "the engine's own re-resolved route is adopted after the update that re-enumerated");
            tick(manager.data());
            require(wrapper_polls>0 && observed.Gamepad.sThumbLY==32767 && registration.dispatch_rejections()==rejections,
                "walking resumes, chained through the adopted overlay route");
            // The overlay goes away: the engine resolves the raw export again; adopted back.
            resolved_route=nullptr;reresolve_to=nullptr;enumerate_again=true;wrapper_polls=0;
            tick(manager.data());tick(manager.data());
            require(get_state==bound_state && observed.Gamepad.sThumbLY==32767 && wrapper_polls==0 && registration.dispatch_adoptions()==adoptions+2,
                "a return to the raw export is adopted too");
            // A pointer the engine did not resolve stays untouched, even with a resolver, and is reported once.
            get_state=foreign_state;tick(manager.data());tick(manager.data());
            require(get_state==foreign_state && !registration.dispatch_healthy() && registration.dispatch_rejections()==rejections+1,
                "a foreign hook is preserved, not adopted, and reported once");
            // A wrapper the resolver does not vouch for is foreign too.
            get_state=&resolver_wrapper;tick(manager.data());
            require(get_state==&resolver_wrapper && !registration.dispatch_healthy() && registration.dispatch_rejections()==rejections+2,
                "an unvouched wrapper is not adopted");
            get_state=original_state;tick(manager.data());
            require(observed.Gamepad.sThumbLY==32767 && registration.dispatch_healthy(),"recovery to the captured route");
        }
        {
            // Rumble (docs/HAPTICS.md): slot-zero motor speeds reach the mailbox; the native call is unchanged.
            vr_rumble::Mailbox mailbox; XINPUT_VIBRATION vibration{100,200},seen{};
            require(set_state == original_set && registration.rumble_available() && !registration.rumble_healthy(),"the rumble slot stays native until forwarding is requested");
            registration.forward_rumble(&mailbox); tick(manager.data());
            require(set_state != original_set && registration.rumble_healthy() && registration.rumble_binds() == 1,"forwarding binds the engine's SetState slot");
            require(set_state(0,&vibration) == ERROR_DEVICE_NOT_CONNECTED,"the native SetState result is returned unchanged");
            require(delivered(&seen) == 1 && seen.wLeftMotorSpeed == 100 && seen.wRightMotorSpeed == 200,"the native SetState still receives the speeds");
            require(mailbox.read().calls == 1 && mailbox.read().motors == vr_rumble::Motors{100,200},"slot-zero speeds reach the mailbox");
            vibration = {5,6}; set_state(1,&vibration);
            require(mailbox.read().calls == 1,"other pad slots are not forwarded");
            require(set_state(0,nullptr) == ERROR_BAD_ARGUMENTS && mailbox.read().calls == 1,"a null vibration keeps the native error and is not forwarded");
            enumerate_again = true; tick(manager.data());
            require(set_state != original_set && registration.rumble_healthy() && registration.rumble_binds() == 2,"re-enumeration re-resolves rumble and it is rebound");
            set_state = foreign_set; tick(manager.data());
            require(set_state == foreign_set && !registration.rumble_healthy(),"a foreign SetState replacement is left alone");
            set_state = original_set; tick(manager.data());
            require(set_state != original_set && registration.rumble_healthy() && registration.rumble_binds() == 3,"the known route rebinds");
            registration.forward_rumble(nullptr); vibration = {1,1}; set_state(0,&vibration);
            require(mailbox.read().calls == 1 && delivered(&seen) == 3,"stopped forwarding publishes nothing while the native call continues");
        }
        input.enable(false);
        require(get_state(0,&passthrough) == ERROR_DEVICE_NOT_CONNECTED && passthrough.dwPacketNumber == 123 && passthrough.Gamepad.sThumbRY == 7654,"an installed dispatch must pass through unchanged when disabled");
        input.enable(true);
        input.enable(false); enumerate_again = true; tick(manager.data());
        require(devices.empty(),"disabled input must not re-register after re-enumeration");
        require(get_state == original_state && get_state(0,&passthrough) == ERROR_DEVICE_NOT_CONNECTED && passthrough.Gamepad.sThumbRY == 7654,"disabled mode must retain original input result and bytes");
        input.native_passthrough(true);tick(manager.data());
        require(devices.empty() && registration.dispatch_healthy() && get_state!=original_state,"third person binds recenter dispatch without creating virtual pad");
        require(get_state(0,&passthrough)==ERROR_DEVICE_NOT_CONNECTED && passthrough.Gamepad.sThumbRY==7654,"actual native dispatch preserves disconnected physical pad in third person");
        get_state=original_state;tick(manager.data());require(registration.dispatch_healthy(),"third-person dispatch survives native resolver re-enumeration");
        input.native_passthrough(false);
        input.enable(true);
        auto other = construct(allocate(0x1248,0)); write(other,0x1240,uint32_t(1)); devices.push_back(other); sync_devices();
        tick(manager.data()); require(devices.size() == 2 && read<uint32_t>(devices[0],0x1240) == 1,"native slot one must remain untouched while missing slot zero is added");
        clear_devices();
        initialization_fails = true; const auto before = destructions;
        tick(manager.data()); require(devices.empty() && destructions == before+1,"failed initialization must free unowned pad via native destructor");
        initialization_fails = false; allocation_fails = true; const auto before_alloc = allocations;
        tick(manager.data()); require(devices.empty() && allocations == before_alloc,"allocation failure must leave manager unchanged");
        allocation_fails = false;
        write(manager.data(),0,uintptr_t(123)); tick(manager.data());
        require(devices.empty(),"unrecognized manager vtable must fail closed");
        write(manager.data(),0,reinterpret_cast<uintptr_t>(manager_table.data()));
        write(manager.data(),0x30,int32_t(-1)); tick(manager.data());
        require(devices.empty(),"malformed native device list must fail closed"); sync_devices();
        // Verify every native ABI guard, including the constructor/init/ownership helpers.
        using Registration=EngineControllerRegistration;
        std::array<std::array<uint8_t,12>,5> bytes{synthetic_at<12>(Registration::UPDATE_RVA),synthetic_at<12>(Registration::CONSTRUCT_RVA),
            synthetic_at<12>(Registration::ALLOCATE_RVA),synthetic_at<12>(Registration::REGISTER_RVA),synthetic_at<12>(Registration::INITIALIZE_RVA)};
        auto matches = [&] (uint32_t stamp) { return registration.matches_build(stamp,bytes[0],bytes[1],bytes[2],bytes[3],bytes[4]); };
        require(matches(0x6ac3e716) && !matches(0),"native registration must require exact build timestamp");
        { using R=EngineControllerRegistration;namespace p=game_build_ported;
          require(registration.matches_build(p::EXE,p::code(R::UPDATE_RVA),p::code(R::CONSTRUCT_RVA),p::code(R::ALLOCATE_RVA),p::code(R::REGISTER_RVA),p::code(R::INITIALIZE_RVA)),"the 2026-10-21 registration functions must match");
          require(!matches(p::EXE),"reference rip/call bytes must not pass on the 2026-10-21 build"); }
        for (auto& function:bytes) { function[0] ^= 1; require(!matches(0x6ac3e716),"changed native function bytes must reject integration"); function[0] ^= 1; }
        registration.uninstall(); tick(manager.data()); require(devices.empty(),"uninstall must restore original update with no registration");
        require(get_state == original_state && set_state == original_set,"uninstall must restore owned native dispatch and rumble");
        require(registration.install(reinterpret_cast<void*>(&native_update),api,input),"native bridge must support reinstall");
        allocation_fails = true; tick(manager.data()); allocation_fails = false;
        get_state = foreign_state; registration.uninstall();
        require(get_state == foreign_state,"uninstall must preserve a foreign dispatch replacement");
        require(allocations == destructions,"all fixture allocations must have native ownership or deleting-destructor cleanup");
        input.native_passthrough(true);generic_wrapper=&input;original_state=physical_state;get_state=physical_state;
        require(registration.install(reinterpret_cast<void*>(&native_update),api,input),"third-person physical pad fixture installs native dispatch");
        tick(manager.data());require(devices.empty(),"physical-only startup must not manufacture a VR pad");
        physical.dwPacketNumber=5;physical.Gamepad.sThumbLX=123;physical.Gamepad.sThumbRX=456;physical.Gamepad.bRightTrigger=200;
        physical.Gamepad.wButtons=XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_A;
        require(get_state(0,&passthrough)==ERROR_SUCCESS && passthrough.Gamepad.sThumbLX==123 && passthrough.Gamepad.sThumbRX==456 && passthrough.Gamepad.bRightTrigger==200 && passthrough.Gamepad.wButtons==XINPUT_GAMEPAD_A,
            "real dispatch must pass native movement/camera/fire while consuming recenter");
        require(input.take_native_recenter(),"native Back reaches frame-loop request");
        get_state(0,&passthrough);require(!input.take_native_recenter(),"nested generic wrapper must not rearm held Back");
        physical.Gamepad.wButtons=0;get_state(0,&passthrough);physical.Gamepad.wButtons=XINPUT_GAMEPAD_BACK;get_state(0,&passthrough);
        require(input.take_native_recenter(),"released physical Back rearms through actual dispatch");
        registration.uninstall();generic_wrapper=nullptr;
        FreeLibrary(sdk);
        std::cout << "native controller discovery, real XInput walking, ownership, re-enumeration and ABI guards passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
