#include "synthetic_code.h"
#include "NativeTrackerHandleGuard.h"
#include "game_build_ported.h"
#include "NativeFaultEvidence.h"
#include <MinHook.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

// Deterministic fixtures for the tracker handle guard: the readable-page cache on real
// reserved/committed/guard pages, the three decision rules, and the actual MinHook
// detours on fixture functions that mimic the native getter and two-handle validity.
namespace {
void require(bool value,const char* message) {if(!value) throw std::runtime_error{message};}
template<class T> void put(void* object,size_t offset,const T& value) {std::memcpy(static_cast<uint8_t*>(object)+offset,&value,sizeof(T));}
template<class T> T get(const void* object,size_t offset) {T value{};std::memcpy(&value,static_cast<const uint8_t*>(object)+offset,sizeof(T));return value;}
constexpr uint64_t SENTINEL=0xffff000000000000ull;
constexpr size_t CHUNK=size_t{1}<<ReadablePages::CHUNK_SHIFT;
uint64_t g_sentinel=SENTINEL;

bool would_fault(uintptr_t address,size_t size) {
    MEMORY_BASIC_INFORMATION m{};
    return !VirtualQuery(reinterpret_cast<LPCVOID>(address),&m,sizeof(m)) || !ReadablePages::readable_protection(m) ||
        reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize<address+size;
}

// Mimics the native propGEOM getter's reads. Instead of faulting it records that the
// native code would have faulted, so a guard regression fails an assertion, not the run.
struct GetterCall {void* object{};uint64_t packed{};bool faulted{};unsigned calls{};} g_getter;
__declspec(noinline) void fixture_getter(void* tracker,void* output) {
    ++g_getter.calls;g_getter.object=tracker;const auto object=reinterpret_cast<uintptr_t>(tracker);
    const auto packed=get<uint64_t>(tracker,0xd8);g_getter.packed=packed;
    if(packed!=g_sentinel) {
        const auto geometry=uintptr_t(packed&0xffffffffffffull);
        if(would_fault(geometry+0xc0,8)) {g_getter.faulted=true;return;}
        const auto table=get<uintptr_t>(reinterpret_cast<void*>(geometry),0xc0);
        const auto matrix=table+uintptr_t(int64_t(int16_t(uint16_t(packed>>48)))*0x40);
        if(would_fault(matrix,0x40)) {g_getter.faulted=true;return;}
        std::memcpy(output,reinterpret_cast<void*>(matrix),0x40);return;
    }
    const auto wrapper=get<uintptr_t>(tracker,0xd0);
    const auto actor=get<uintptr_t>(reinterpret_cast<void*>(wrapper),0);
    const auto geometry=get<uintptr_t>(reinterpret_cast<void*>(actor),0xd8);
    if(!geometry) {std::memset(output,0x11,0x40);return;}
    std::memcpy(output,reinterpret_cast<void*>(geometry+0x340),0x40);
    (void)object;
}
struct PairCall {unsigned calls{};} g_pair;
__declspec(noinline) uint8_t fixture_pair(void* tracker) {
    ++g_pair.calls;
    for(const size_t offset:{size_t{0xd0},size_t{0xd8}}) {const auto record=get<uintptr_t>(tracker,offset);if(record) (void)get<uint8_t>(reinterpret_cast<void*>(record),0xe);}
    return 1;
}

struct Memory {
    uint8_t* base{};size_t size{};
    Memory(size_t bytes,DWORD type,DWORD protect,void* at=nullptr):size(bytes) {base=static_cast<uint8_t*>(VirtualAlloc(at,bytes,type,protect));}
    ~Memory() {if(base) VirtualFree(base,0,MEM_RELEASE);}
    uintptr_t address(size_t offset=0) const {return reinterpret_cast<uintptr_t>(base)+offset;}
};

void pages() {
    // Three chunks reserved; only the middle one committed.
    Memory region(3*CHUNK,MEM_RESERVE,PAGE_NOACCESS);require(region.base,"reserve fixture");
    require(VirtualAlloc(region.base+CHUNK,CHUNK,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"commit middle chunk");
    ReadablePages cache;
    require(cache.readable(region.address(CHUNK),8),"committed read/write memory is readable");
    const auto first=cache.queries();
    require(cache.readable(region.address(CHUNK+0x100),0x40) && cache.queries()==first,"a cached chunk answers without a system call");
    require(!cache.readable(region.address(0),8),"reserved, uncommitted memory is not readable");
    require(!cache.readable(region.address(2*CHUNK-4),8),"a range running from committed into reserved memory is not readable");
    require(!cache.readable(0,8) && !cache.readable(uintptr_t{1}<<47,8) && !cache.readable(UINTPTR_MAX-3,8),"null, kernel-half and wrapping ranges are not readable");
    require(cache.readable(region.address(0),0),"an empty range is trivially readable");
    // Adjacent regions with different protections: both readable, so the span is readable.
    Memory split(2*CHUNK,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);require(split.base,"split fixture");
    DWORD old{};require(VirtualProtect(split.base+CHUNK,CHUNK,PAGE_READONLY,&old),"read-only second half");
    require(cache.readable(split.address(CHUNK-8),16),"a range across two readable regions is readable");
    require(VirtualProtect(split.base+CHUNK,0x1000,PAGE_READONLY|PAGE_GUARD,&old),"guard page");
    ReadablePages fresh;
    require(!fresh.readable(split.address(CHUNK),8),"guard pages are not readable (reading them would raise)");
    require(VirtualProtect(split.base+CHUNK,0x1000,PAGE_NOACCESS,&old) && !fresh.readable(split.address(CHUNK),1),"no-access pages are not readable");
    require(fresh.readable(split.address(0),8),"the unprotected first half stays readable");
}

struct Scene {
    // Committed memory standing in for the native heap: a tracker, owner wrapper/actor,
    // geometry with its bone table, and owner geometry for the "no geometry" branch.
    // Four committed chunks followed by a reserved one, so reads past the end deterministically fault.
    Memory heap{5*CHUNK,MEM_RESERVE,PAGE_NOACCESS};
    uint8_t* tracker{};uint8_t* wrapper{};uint8_t* actor{};uint8_t* geometry{};uint8_t* table{};uint8_t* owner_geometry{};
    Scene() {
        require(heap.base && VirtualAlloc(heap.base,4*CHUNK,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"heap fixture");
        tracker=heap.base+0x1000;wrapper=heap.base+0x2000;actor=heap.base+0x3000;geometry=heap.base+0x4000;table=heap.base+0x6000;owner_geometry=heap.base+0x8000;
        put(tracker,0,uintptr_t(0x140003626b78));put(tracker,0xd0,reinterpret_cast<uintptr_t>(wrapper));
        put(wrapper,0,reinterpret_cast<uintptr_t>(actor));put(actor,0xd8,reinterpret_cast<uintptr_t>(owner_geometry));
        put(geometry,0xc0,reinterpret_cast<uintptr_t>(table));
        for(int i=0;i<0x40;++i) {table[0x40*3+i]=uint8_t(0xa0+i);owner_geometry[0x340+i]=uint8_t(0x50+i);}
        set_bone(3);
    }
    void set_bone(int16_t bone) {put(tracker,0xd8,(uint64_t(uint16_t(bone))<<48)|reinterpret_cast<uintptr_t>(geometry));}
    uint64_t packed() const {return get<uint64_t>(tracker,0xd8);}
};

// The observed corruption: a 4-byte small value written at object+0xdc.
uint64_t corrupt(uint64_t packed,uint32_t value) {return (packed&0xffffffffull)|(uint64_t(value)<<32);}

void decisions() {
    Scene s;ReadablePages cache;
    require(NativeTrackerHandleGuard::packed_readable(s.packed(),SENTINEL,cache),"an intact packed handle is readable");
    require(NativeTrackerHandleGuard::packed_readable(SENTINEL,SENTINEL,cache),"the native sentinel is left to the native branch");
    // Pin the 0x4d high-dword address range as reserved so the corruption pattern is deterministic.
    Memory pinned(CHUNK,MEM_RESERVE,PAGE_NOACCESS,reinterpret_cast<void*>(0x4d00000000ull));
    Memory spare(CHUNK,MEM_RESERVE,PAGE_NOACCESS);require(spare.base,"spare reserve");
    const uint64_t bad=pinned.base?corrupt(s.packed(),0x4d)&~0xffffffffull:spare.address(0);
    require(!NativeTrackerHandleGuard::packed_readable(bad,SENTINEL,cache),"a handle whose geometry is reserved memory is unreadable");
    if(pinned.base) {
        // Same low dword as the intact handle, as in the 20 Oct run (R8 0x4d26fc2500).
        const auto low=s.packed()&0xffffffffull;
        if(would_fault(0x4d00000000ull+low+0xc0,8)) require(!NativeTrackerHandleGuard::packed_readable(corrupt(s.packed(),0x4d),SENTINEL,cache),"the observed +0xdc corruption pattern is unreadable");
    }
    s.set_bone(7);require(NativeTrackerHandleGuard::packed_readable(s.packed(),SENTINEL,cache),"any bone inside the committed table is readable");
    put(s.geometry,0xc0,s.heap.address(4*CHUNK-0x20));s.set_bone(0);
    require(!NativeTrackerHandleGuard::packed_readable(s.packed(),SENTINEL,cache),"a bone matrix running past committed memory is unreadable");
    put(s.geometry,0xc0,reinterpret_cast<uintptr_t>(s.table)+0x40*4);s.set_bone(-1);
    require(NativeTrackerHandleGuard::packed_readable(s.packed(),SENTINEL,cache),"a negative bone index uses the native signed offset");
    require(NativeTrackerHandleGuard::owner_readable(reinterpret_cast<uintptr_t>(s.tracker),cache),"a live owner chain is readable");
    put(s.actor,0xd8,uintptr_t{0});
    require(NativeTrackerHandleGuard::owner_readable(reinterpret_cast<uintptr_t>(s.tracker),cache),"a null owner geometry takes the native identity branch");
    put(s.actor,0xd8,spare.address(0));
    require(!NativeTrackerHandleGuard::owner_readable(reinterpret_cast<uintptr_t>(s.tracker),cache),"an unreadable owner geometry is refused");
    put(s.tracker,0xd0,uintptr_t{0});
    require(!NativeTrackerHandleGuard::owner_readable(reinterpret_cast<uintptr_t>(s.tracker),cache),"a missing owner wrapper is refused");
    std::array<uint8_t,0xe0> pair{};std::array<uint8_t,0x20> record_a{},record_b{};
    put(pair.data(),0xd0,reinterpret_cast<uintptr_t>(record_a.data()));put(pair.data(),0xd8,reinterpret_cast<uintptr_t>(record_b.data()));
    require(NativeTrackerHandleGuard::pair_readable(reinterpret_cast<uintptr_t>(pair.data()),cache),"two readable weak-handle records pass");
    put(pair.data(),0xd8,uintptr_t{0});
    require(NativeTrackerHandleGuard::pair_readable(reinterpret_cast<uintptr_t>(pair.data()),cache),"a null record is left to native code");
    put(pair.data(),0xd8,spare.address(0x100));
    require(!NativeTrackerHandleGuard::pair_readable(reinterpret_cast<uintptr_t>(pair.data()),cache),"an unreadable +0xd8 record is refused");
    put(pair.data(),0xd8,reinterpret_cast<uintptr_t>(record_b.data()));put(pair.data(),0xd0,spare.address(0x100));
    require(!NativeTrackerHandleGuard::pair_readable(reinterpret_cast<uintptr_t>(pair.data()),cache),"an unreadable +0xd0 record is refused");
}

void builds() {
    using G=NativeTrackerHandleGuard;
    const auto same=[](const game_build::Code& a,const game_build::Code& b){return a.reference==b.reference && a.length==b.length && a.image==b.image && a.digest==b.digest;};
    require(same(G::EXE.getter_bytes,NativeFaultEvidence::EXE_FUNCTION_BYTES) && same(G::DLL.getter_bytes,NativeFaultEvidence::FUNCTION_BYTES),
        "getter guards match the fault-evidence signatures of the same two images");
    require(G::EXE.getter_rva==NativeFaultEvidence::EXE_FUNCTION_RVA && G::DLL.getter_rva==NativeFaultEvidence::FUNCTION_RVA,"getter RVAs agree");
    require(G::EXE.timestamp==NativeFaultEvidence::EXE_TIMESTAMP && G::DLL.timestamp==NativeFaultEvidence::TIMESTAMP,"image timestamps agree");
    // That the sentinel global is the getter's own rip-relative compare target needs the real
    // bytes: tests/test_game_code_hashes.py checks it against the reference images.
    auto getter=SYNTH(G::EXE.getter_bytes);auto pair=SYNTH(G::EXE.pair_bytes);
    require(G::matches(G::EXE,G::EXE.timestamp,getter,pair),"exact executable bytes match");
    require(!G::matches(G::EXE,G::DLL.timestamp,getter,pair),"a different image timestamp refuses");
    pair[27]^=1;require(!G::matches(G::EXE,G::EXE.timestamp,getter,pair),"a changed validity prologue refuses");
    pair=SYNTH(G::EXE.pair_bytes);getter[3]^=1;require(!G::matches(G::EXE,G::EXE.timestamp,getter,pair),"a changed getter prologue refuses");
    require(!G::matches(G::DLL,G::DLL.timestamp,SYNTH(G::EXE.getter_bytes),SYNTH(G::EXE.pair_bytes)),"executable bytes do not pass the DLL guard");
    {namespace p=game_build_ported;using I=game_build::Image;
     require(G::matches(G::EXE,p::EXE,p::code(G::EXE.getter_rva),p::code(G::EXE.pair_rva)),"the 2026-10-21 executable getter/validity match");
     require(G::matches(G::DLL,p::DLL,p::code(G::DLL.getter_rva,I::dll),p::code(G::DLL.pair_rva,I::dll)),"the 2026-10-21 server DLL getter/validity match");
     require(!G::matches(G::DLL,p::EXE,p::code(G::DLL.getter_rva,I::dll),p::code(G::DLL.pair_rva,I::dll)),"the exe timestamp does not unlock the DLL guard");
     require(!G::matches(G::EXE,p::EXE,SYNTH(G::EXE.getter_bytes),SYNTH(G::EXE.pair_bytes)),"reference rip/call bytes do not pass on the 2026-10-21 build");
     std::array<uint8_t,16> function{};std::array<uint8_t,7> fault{};
     std::memcpy(function.data(),p::code(NativeFaultEvidence::FUNCTION_RVA,I::dll).data(),16);std::memcpy(fault.data(),p::code(NativeFaultEvidence::RVA,I::dll).data(),7);
     require(NativeFaultEvidence::matches(p::DLL,function,fault,NativeFaultEvidence::Image::dll),"fault pod guard accepts the 2026-10-21 DLL");
     std::memcpy(function.data(),p::code(NativeFaultEvidence::EXE_FUNCTION_RVA).data(),16);std::memcpy(fault.data(),p::code(NativeFaultEvidence::EXE_RVA).data(),7);
     require(NativeFaultEvidence::matches(p::EXE,function,fault,NativeFaultEvidence::Image::executable),"fault pod guard accepts the 2026-10-21 exe");}
    auto& guard=G::get();
    require(!guard.install_in_module(G::Image::executable,GetModuleHandleW(nullptr)) && guard.stats().refused[0] && !guard.stats().installed[0],
        "a foreign executable image is refused and nothing is hooked");
    guard.uninstall();
}

void hooks() {
    using G=NativeTrackerHandleGuard;auto& guard=G::get();
    require(guard.install(G::Image::dll,reinterpret_cast<void*>(&fixture_getter),reinterpret_cast<void*>(&fixture_pair),&g_sentinel,0x7000),"actual detours install");
    void(*volatile getter)(void*,void*)=fixture_getter;uint8_t(*volatile pair)(void*)=fixture_pair;
    Scene s;std::array<uint8_t,0x40> out{};
    getter(s.tracker,out.data());
    require(g_getter.calls==1 && g_getter.object==s.tracker && !g_getter.faulted && out[0]==0xa0,"an intact handle reaches the native getter with the native object");
    {
        static void* filtered_tracker{};static uint8_t filtered_first{};
        guard.set_filter(G::Image::dll,[](void* tracker,void* output){filtered_tracker=tracker;filtered_first=*static_cast<uint8_t*>(output);*static_cast<uint8_t*>(output)=0x77;});
        out.fill(0);getter(s.tracker,out.data());
        require(g_getter.calls==2 && filtered_tracker==s.tracker && filtered_first==0xa0 && out[0]==0x77,"the image's filter runs after the native read and may replace its output");
        guard.set_filter(G::Image::executable,[](void*,void*){throw std::runtime_error("the executable's filter must not run for the DLL");});
        guard.set_filter(G::Image::dll,nullptr);out.fill(0);getter(s.tracker,out.data());
        require(g_getter.calls==3 && out[0]==0xa0,"no filter: the native output stands");
        guard.set_filter(G::Image::executable,nullptr);g_getter.calls=1;
    }
    Memory spare(CHUNK,MEM_RESERVE,PAGE_NOACCESS);require(spare.base,"spare reserve");
    put(s.tracker,0xd8,uint64_t(spare.address(0)));
    std::array<uint8_t,0xe0> before{};std::memcpy(before.data(),s.tracker,before.size());
    out.fill(0);getter(s.tracker,out.data());
    require(g_getter.calls==2 && !g_getter.faulted && g_getter.object!=s.tracker && g_getter.packed==SENTINEL,"a corrupted handle runs the native getter on a sentinel copy");
    require(out[0]==0x50 && out[0x3f]==0x8f,"the copy takes the native no-geometry branch through the real owner");
    require(std::memcmp(before.data(),s.tracker,before.size())==0,"the corrupted native object is left exactly as found");
    auto stats=guard.stats();require(stats.getter_trips[1]==1 && stats.fallback_identity[1]==0 && stats.getter_trips[0]==0,"one getter trip counted on the DLL slot");
    std::array<G::Trip,G::TRIP_SLOTS> trips{};
    require(guard.take_trips(trips)==1 && trips[0].object==reinterpret_cast<uintptr_t>(s.tracker) && trips[0].value==spare.address(0) &&
        trips[0].kind==G::Kind::getter && trips[0].image==G::Image::dll && trips[0].bytes_valid && trips[0].vtable==0x140003626b78,"the trip records object, value and bytes");
    require(std::memcmp(trips[0].bytes.data(),before.data(),before.size())==0 && guard.take_trips(trips)==0,"trip bytes are the object's and each trip is taken once");
    put(s.tracker,0xd0,spare.address(0x200));out.fill(0);
    getter(s.tracker,out.data());
    float identity[16]{};std::memcpy(identity,out.data(),sizeof(identity));
    require(g_getter.calls==2 && identity[0]==1 && identity[5]==1 && identity[10]==1 && identity[15]==1 && identity[1]==0 && guard.stats().fallback_identity[1]==1,
        "with an unreadable owner too, the native identity is written without calling native code");
    guard.enable(false);put(s.tracker,0xd0,reinterpret_cast<uintptr_t>(s.wrapper));
    getter(s.tracker,out.data());
    require(g_getter.calls==3 && g_getter.object==s.tracker && g_getter.faulted,"disabled, the native getter sees the corrupted object unchanged");
    guard.enable(true);g_getter.faulted=false;

    std::array<uint8_t,0xe0> two{};std::array<uint8_t,0x20> a{},b{};
    put(two.data(),0xd0,reinterpret_cast<uintptr_t>(a.data()));put(two.data(),0xd8,reinterpret_cast<uintptr_t>(b.data()));
    require(pair(two.data())==1 && g_pair.calls==1,"a readable two-handle tracker reaches native validity");
    put(two.data(),0xd8,spare.address(0x300));
    require(pair(two.data())==0 && g_pair.calls==1 && guard.stats().pair_trips[1]==1,"an unreadable record reports invalid without native code");
    require(guard.take_trips(trips)==2 && trips[1].kind==G::Kind::pair && trips[1].value==spare.address(0x300),"getter and pair trips are both recorded");
    // Server DLL reload: same mapping with fresh code is re-patched; a moved mapping is forgotten
    // and a new install over the stale MinHook entries works.
    const auto module=[](uintptr_t value){return reinterpret_cast<HMODULE>(value);};
    require(guard.revalidate(G::Image::dll,module(0x7000)) && guard.stats().reapplied[1]==0,"an unchanged mapping keeps its hooks");
    require(MH_DisableHook(reinterpret_cast<void*>(&fixture_getter))==MH_OK,"simulate fresh native code at the same base");
    const auto before_calls=g_getter.calls;getter(s.tracker,out.data());
    require(g_getter.calls==before_calls+1 && g_getter.faulted,"fresh code is unguarded until revalidated");
    g_getter.faulted=false;
    require(guard.revalidate(G::Image::dll,module(0x7000)) && guard.stats().reapplied[1]==1,"the same mapping is re-patched");
    getter(s.tracker,out.data());require(!g_getter.faulted && g_getter.packed==SENTINEL,"re-patched getter is guarded again");
    require(!guard.revalidate(G::Image::dll,module(0x8000)) && !guard.stats().installed[1] && guard.stats().remapped[1]==1,"a moved mapping is forgotten");
    require(!guard.revalidate(G::Image::dll,module(0x8000)),"a forgotten slot stays uninstalled");
    require(guard.install(G::Image::dll,reinterpret_cast<void*>(&fixture_getter),reinterpret_cast<void*>(&fixture_pair),&g_sentinel,0x8000),"install over stale entries");
    getter(s.tracker,out.data());require(!g_getter.faulted && g_getter.packed==SENTINEL && guard.revalidate(G::Image::dll,module(0x8000)),"the new mapping is guarded");
    require(!guard.revalidate(G::Image::dll,nullptr) && guard.stats().remapped[1]==2,"an unloaded DLL is forgotten");
    {   // The poll keeps revalidating after both images are decided (the count used to stop there).
        int revalidations{},installs{};
        for(uint32_t tick=0;tick<64;++tick){const auto steps=G::poll_steps(tick,true,true);revalidations+=steps.revalidate;installs+=steps.install;}
        require(revalidations==4 && installs==0,"an installed DLL slot is revalidated every 16th poll; nothing left to install");
        require(G::poll_steps(0,false,false).install && !G::poll_steps(0,false,false).revalidate && !G::poll_steps(5,true,false).install,"undecided images install on the 16-poll cadence; nothing to revalidate before install");
    }
    guard.install(G::Image::dll,reinterpret_cast<void*>(&fixture_getter),reinterpret_cast<void*>(&fixture_pair),&g_sentinel,0x8000);
    guard.uninstall();
    const auto after_calls=g_getter.calls;
    getter(s.tracker,out.data());require(g_getter.calls==after_calls+1 && g_getter.faulted,"uninstall restores the native function");
}
}

int main() {
    try {
        pages();decisions();builds();hooks();
        std::cout<<"native_tracker_handle_guard: PASS\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"native_tracker_handle_guard: FAIL "<<e.what()<<"\n";return 1;}
}
