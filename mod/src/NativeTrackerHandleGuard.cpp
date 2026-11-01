#include "NativeTrackerHandleGuard.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <algorithm>
#include <cstring>

ReadablePages::ReadablePages(Query query):m_slots(std::make_unique<std::atomic<uint64_t>[]>(SLOTS)),m_query(query) {}

void ReadablePages::clear() noexcept {
    for(size_t i=0;i<SLOTS;++i) m_slots[i].store(0,std::memory_order_relaxed);
}

bool ReadablePages::readable_protection(const MEMORY_BASIC_INFORMATION& memory) noexcept {
    constexpr DWORD READ=PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY;
    return memory.State==MEM_COMMIT && (memory.Protect&READ) && !(memory.Protect&(PAGE_GUARD|PAGE_NOACCESS));
}

bool ReadablePages::readable(uintptr_t address,size_t size) noexcept {
    if(!size) return true;
    // User-mode x64 addresses stay below 2^47; anything else (including wrap) faults.
    constexpr uintptr_t LIMIT=uintptr_t{1}<<47;
    if(!address || address>=LIMIT || size>LIMIT-address) return false;
    const uintptr_t end=address+size;
    uintptr_t current=address;
    while(current<end) {
        const uint64_t chunk=current>>CHUNK_SHIFT;
        const uintptr_t chunk_end=uintptr_t(chunk+1)<<CHUNK_SHIFT;
        auto& entry=m_slots[slot(chunk)];
        if(entry.load(std::memory_order_relaxed)==chunk+1) {current=chunk_end;continue;}
        MEMORY_BASIC_INFORMATION memory{};
        m_queries.fetch_add(1,std::memory_order_relaxed);
        if(m_query(reinterpret_cast<LPCVOID>(current),&memory,sizeof(memory))!=sizeof(memory) || !readable_protection(memory)) return false;
        const auto base=reinterpret_cast<uintptr_t>(memory.BaseAddress);
        const auto region_end=base+memory.RegionSize;
        if(region_end<=current) return false;
        if(base<=(uintptr_t(chunk)<<CHUNK_SHIFT) && region_end>=chunk_end) entry.store(chunk+1,std::memory_order_relaxed);
        current=region_end; // All of [current, region_end) is readable; continue past it.
    }
    return true;
}

namespace {
template<class T> T load(uintptr_t address) {T value{};std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(T));return value;}
constexpr std::array<float,16> IDENTITY{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
}

NativeTrackerHandleGuard& NativeTrackerHandleGuard::get() {static auto* value=new NativeTrackerHandleGuard;return *value;}

bool NativeTrackerHandleGuard::matches(const Build& build,uint32_t timestamp,std::span<const uint8_t> getter,std::span<const uint8_t> pair) {
    // build holds the reference image's values; timestamp may be that image or a verified port of it.
    const auto image=build.timestamp==game_build::REFERENCE_DLL?game_build::Image::dll:game_build::Image::exe;
    return build.timestamp==game_build::reference_timestamp(image) &&
        game_build::code(timestamp,build.getter_rva,getter,build.getter_bytes,image) && game_build::code(timestamp,build.pair_rva,pair,build.pair_bytes,image);
}

bool NativeTrackerHandleGuard::packed_readable(uint64_t packed,uint64_t sentinel,ReadablePages& pages) noexcept {
    if(packed==sentinel) return true; // The native "no geometry" branch; not this guard's concern.
    // Native: table=*(low48+0xc0); matrix=table+int16(packed>>48)*0x40; reads 64 bytes.
    const uintptr_t geometry=uintptr_t(packed&0xffffffffffffull);
    if(!pages.readable(geometry+0xc0,8)) return false;
    const auto table=load<uintptr_t>(geometry+0xc0);
    const auto index=int64_t(int16_t(uint16_t(packed>>48)));
    const auto matrix=table+uintptr_t(index*0x40);
    return pages.readable(matrix,0x40);
}

bool NativeTrackerHandleGuard::owner_readable(uintptr_t object,ReadablePages& pages) noexcept {
    // Native sentinel branch: actor=**(object+0xd0); geometry=*(actor+0xd8); if geometry,
    // reads geometry+0x340..0x38c. A null geometry returns the native identity constant.
    const auto wrapper=load<uintptr_t>(object+OWNER_OFFSET);
    if(!wrapper || !pages.readable(wrapper,8)) return false;
    const auto actor=load<uintptr_t>(wrapper);
    if(!actor || !pages.readable(actor+0xd8,8)) return false;
    const auto geometry=load<uintptr_t>(actor+0xd8);
    return !geometry || pages.readable(geometry+0x340,0x50);
}

bool NativeTrackerHandleGuard::pair_readable(uintptr_t object,ReadablePages& pages) noexcept {
    // Native: record=*(object+0xd0 / +0xd8); if record, tests byte record+0xe.
    for(const auto offset:{OWNER_OFFSET,HANDLE_OFFSET}) {
        const auto record=load<uintptr_t>(object+offset);
        if(record && !pages.readable(record+0xe,1)) return false;
    }
    return true;
}

void NativeTrackerHandleGuard::record(Image image,Kind kind,uintptr_t object,uintptr_t value) noexcept {
    const auto index=m_trip_count.fetch_add(1,std::memory_order_relaxed);
    if(index>=TRIP_SLOTS) return;
    auto& trip=m_trips[index];
    trip.image=image;trip.kind=kind;trip.thread=GetCurrentThreadId();trip.object=object;trip.value=value;
    trip.vtable=load<uintptr_t>(object);
    SIZE_T copied{};
    trip.bytes_valid=ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<LPCVOID>(object),trip.bytes.data(),trip.bytes.size(),&copied) && copied==trip.bytes.size();
    m_trip_ready[index].store(true,std::memory_order_release);
}

template<int I> void NativeTrackerHandleGuard::getter_hook(void* tracker,void* output) {
    auto& self=get();auto& slot=self.m_slots[I];
    const auto original=slot.getter.load(std::memory_order_acquire);
    const auto object=reinterpret_cast<uintptr_t>(tracker);
    const auto* sentinel=slot.sentinel.load(std::memory_order_acquire);
    const auto filtered=[&]{original(tracker,output);if(const auto filter=self.m_filters[I].load(std::memory_order_acquire)) filter(tracker,output);};
    if(!self.enabled() || !object || !sentinel) {filtered();return;}
    const auto packed=load<uint64_t>(object+HANDLE_OFFSET);
    if(packed_readable(packed,*sentinel,self.m_pages)) {filtered();return;}
    slot.getter_trips.fetch_add(1,std::memory_order_relaxed);
    self.record(Image(I),Kind::getter,object,uintptr_t(packed));
    if(!owner_readable(object,self.m_pages)) {
        slot.fallback_identity.fetch_add(1,std::memory_order_relaxed);
        std::memcpy(output,IDENTITY.data(),sizeof(IDENTITY));return;
    }
    // The game's own branch for a tracker without geometry, run on a copy so the
    // corrupted native object is left exactly as found.
    alignas(16) std::array<uint8_t,OBJECT_SIZE> copy{};
    std::memcpy(copy.data(),tracker,copy.size());
    const uint64_t value=*sentinel;std::memcpy(copy.data()+HANDLE_OFFSET,&value,sizeof(value));
    original(copy.data(),output);
}

template<int I> uint8_t NativeTrackerHandleGuard::pair_hook(void* tracker) {
    auto& self=get();auto& slot=self.m_slots[I];
    const auto original=slot.pair.load(std::memory_order_acquire);
    const auto object=reinterpret_cast<uintptr_t>(tracker);
    if(!self.enabled() || !object || pair_readable(object,self.m_pages)) return original(tracker);
    slot.pair_trips.fetch_add(1,std::memory_order_relaxed);
    self.record(Image(I),Kind::pair,object,load<uintptr_t>(object+HANDLE_OFFSET));
    return 0;
}

template void NativeTrackerHandleGuard::getter_hook<0>(void*,void*);
template void NativeTrackerHandleGuard::getter_hook<1>(void*,void*);
template uint8_t NativeTrackerHandleGuard::pair_hook<0>(void*);
template uint8_t NativeTrackerHandleGuard::pair_hook<1>(void*);

bool NativeTrackerHandleGuard::install(Image image,void* getter,void* pair,const uint64_t* sentinel,uintptr_t module) {
    auto& slot=m_slots[size_t(image)];
    if(slot.installed.load()) return true;
    if(!getter || !pair || !sentinel) return false;
    const auto init=MH_Initialize();
    if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    Getter original_getter{};Pair original_pair{};
    // A stale MinHook entry can remain from an earlier mapping of the same address (server DLL
    // reloaded at its old base). Removing it restores bytes identical to the new image.
    MH_RemoveHook(getter);MH_RemoveHook(pair);
    void* getter_detour=image==Image::executable?reinterpret_cast<void*>(&getter_hook<0>):reinterpret_cast<void*>(&getter_hook<1>);
    void* pair_detour=image==Image::executable?reinterpret_cast<void*>(&pair_hook<0>):reinterpret_cast<void*>(&pair_hook<1>);
    if(MH_CreateHook(getter,getter_detour,reinterpret_cast<void**>(&original_getter))!=MH_OK) return false;
    if(MH_CreateHook(pair,pair_detour,reinterpret_cast<void**>(&original_pair))!=MH_OK) {MH_RemoveHook(getter);return false;}
    slot.getter.store(original_getter,std::memory_order_release);slot.pair.store(original_pair,std::memory_order_release);
    slot.sentinel.store(sentinel,std::memory_order_release);
    slot.getter_target=getter;slot.pair_target=pair;slot.module=module;
    if(MH_EnableHook(getter)!=MH_OK || MH_EnableHook(pair)!=MH_OK) {
        MH_DisableHook(getter);MH_DisableHook(pair);MH_RemoveHook(getter);MH_RemoveHook(pair);
        slot.getter_target=slot.pair_target=nullptr;return false;
    }
    slot.installed.store(true,std::memory_order_release);return true;
}

bool NativeTrackerHandleGuard::install_in_module(Image image,HMODULE module) {
    auto& slot=m_slots[size_t(image)];
    if(slot.installed.load()) return true;
    if(slot.refused.load() || !module) return false;
    const auto& build=image==Image::executable?EXE:DLL;
    const auto base=reinterpret_cast<uint8_t*>(module);
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto size=nt->OptionalHeader.SizeOfImage;
    const auto stamp=nt->FileHeader.TimeDateStamp;const auto which=image==Image::dll?game_build::Image::dll:game_build::Image::exe;
    const auto getter=game_build::rva(stamp,build.getter_rva,which),pair=game_build::rva(stamp,build.pair_rva,which),sentinel=game_build::rva(stamp,build.sentinel_rva,which);
    if(!getter || !pair || !sentinel || size<getter+build.getter_bytes.size() || size<pair+build.pair_bytes.size() || size<sentinel+8 ||
       !matches(build,stamp,{base+getter,build.getter_bytes.size()},{base+pair,build.pair_bytes.size()})) {
        slot.refused.store(true);return false;
    }
    if(install(image,base+getter,base+pair,reinterpret_cast<const uint64_t*>(base+sentinel),reinterpret_cast<uintptr_t>(module))) return true;
    slot.refused.store(true);return false;
}

void NativeTrackerHandleGuard::poll() {
    if(!m_polled_disabled) {
        m_polled_disabled=true;char value[8]{};
        m_disabled=GetEnvironmentVariableA("SM2VR_TRACKER_GUARD",value,sizeof(value)) && std::string_view(value)=="0";
        enable(!m_disabled);
    }
    if(m_disabled) return;
    const auto tick=m_poll_count++;
    if(poll_steps(tick,m_slots[1].installed.load(),false).revalidate) revalidate(Image::dll,GetModuleHandleW(L"Warhammer 40000 Space Marine 2 - Retail.dll"));
    const auto decided=[this](size_t i){return m_slots[i].installed.load() || m_slots[i].refused.load();};
    if(!poll_steps(tick,false,decided(0) && decided(1)).install) return;
    // Both images are hooked once the late-loaded server DLL exists (about 15 s after the
    // first present, minutes before any second view). Sm2Probe polls this after the
    // fault pod, so the pod's own exact-prologue check of both getters sees native bytes.
    const auto dll=GetModuleHandleW(L"Warhammer 40000 Space Marine 2 - Retail.dll");
    if(!dll) return;
    if(!decided(1)) install_in_module(Image::dll,dll);
    if(!decided(0)) install_in_module(Image::executable,GetModuleHandleW(nullptr));
}

bool NativeTrackerHandleGuard::revalidate(Image image,HMODULE current) {
    auto& slot=m_slots[size_t(image)];
    if(!slot.installed.load()) return false;
    if(current && reinterpret_cast<uintptr_t>(current)==slot.module) {
        for(auto* target:{slot.getter_target,slot.pair_target}) {
            if(*static_cast<const uint8_t*>(target)==0xe9) continue; // MinHook's rel32 jump is in place.
            MH_DisableHook(target);
            if(MH_EnableHook(target)!=MH_OK) {slot.installed.store(false);slot.refused.store(true);return false;}
            slot.reapplied.fetch_add(1,std::memory_order_relaxed);
        }
        return true;
    }
    slot.installed.store(false);slot.getter_target=slot.pair_target=nullptr;slot.module=0;
    slot.remapped.fetch_add(1,std::memory_order_relaxed);return false;
}

void NativeTrackerHandleGuard::uninstall() {
    for(auto& slot:m_slots) {
        for(auto* target:{slot.getter_target,slot.pair_target}) if(target) {MH_DisableHook(target);MH_RemoveHook(target);}
        slot.getter_target=slot.pair_target=nullptr;slot.installed.store(false);slot.refused.store(false);
        slot.getter.store(nullptr);slot.pair.store(nullptr);slot.sentinel.store(nullptr);
        slot.getter_trips.store(0);slot.pair_trips.store(0);slot.fallback_identity.store(0);slot.reapplied.store(0);slot.remapped.store(0);slot.module=0;
    }
    for(auto& ready:m_trip_ready) ready.store(false);
    m_trip_count.store(0);m_trips_taken=0;m_pages.clear();
}

NativeTrackerHandleGuard::Stats NativeTrackerHandleGuard::stats() const {
    Stats s;
    for(size_t i=0;i<2;++i) {
        s.installed[i]=m_slots[i].installed.load();s.refused[i]=m_slots[i].refused.load();
        s.getter_trips[i]=m_slots[i].getter_trips.load();s.pair_trips[i]=m_slots[i].pair_trips.load();
        s.fallback_identity[i]=m_slots[i].fallback_identity.load();
        s.reapplied[i]=m_slots[i].reapplied.load();s.remapped[i]=m_slots[i].remapped.load();
    }
    s.queries=m_pages.queries();return s;
}

size_t NativeTrackerHandleGuard::take_trips(std::array<Trip,TRIP_SLOTS>& out) {
    size_t count=0;
    while(m_trips_taken<TRIP_SLOTS && m_trip_ready[m_trips_taken].load(std::memory_order_acquire)) out[count++]=m_trips[m_trips_taken++];
    return count;
}

std::string NativeTrackerHandleGuard::hex(std::span<const uint8_t> bytes) {
    static constexpr char DIGITS[]="0123456789abcdef";
    std::string text;text.reserve(bytes.size()*2);
    for(const auto b:bytes) {text.push_back(DIGITS[b>>4]);text.push_back(DIGITS[b&15]);}
    return text;
}
