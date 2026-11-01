#include "NativePipLens.h"
#include "GameBuild.h"
#include "NativeRenderProbe.h"
#include <MinHook.h>
#include <cstring>

namespace {
constexpr game_build::Code CACHE_BYTES=game_build::hashed(0x1d75d30,15,"db3faf5fa9a00ab6991d95d3a0bb0ec484096ac76039c52be7fcfad21776bf58");
constexpr game_build::Code FRUSTUM_BYTES=game_build::hashed(0x2224490,14,"88e317b63b35e0f5ba3e396002cbafdc6a225035fbecef65a044bccb6053f893");
template<class T> bool read(uintptr_t address,T& value) {
    SIZE_T bytes{};return address && address<=UINTPTR_MAX-sizeof(T) &&
        ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(T),&bytes) && bytes==sizeof(T);
}
bool writable(uintptr_t address,size_t bytes) {
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(reinterpret_cast<void*>(address),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
        (region.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || !(region.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) return false;
    const auto begin=reinterpret_cast<uintptr_t>(region.BaseAddress);
    return address>=begin && address-begin<=region.RegionSize && bytes<=region.RegionSize-(address-begin);
}
template<class T> T field(const std::array<uint8_t,0xa60>& entry,size_t offset) {T result;std::memcpy(&result,entry.data()+offset,sizeof(T));return result;}
}
NativePipLens& NativePipLens::get() {static auto* instance=new NativePipLens;return *instance;}
bool NativePipLens::matches(uint32_t stamp,std::span<const uint8_t> render,std::span<const uint8_t> cache,std::span<const uint8_t> frustum) {
    return game_build::code(stamp,RENDER_RVA,render,RENDER_BYTES) && game_build::code(stamp,CACHE_RVA,cache,CACHE_BYTES) && game_build::code(stamp,FRUSTUM_RVA,frustum,FRUSTUM_BYTES);
}
bool NativePipLens::install_in_game(HMODULE module,std::function<std::optional<Target>()> target) {
    const auto base=reinterpret_cast<uintptr_t>(module);IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    std::array<uint8_t,RENDER_BYTES.size()> render{};std::array<uint8_t,CACHE_BYTES.size()> cache{};std::array<uint8_t,FRUSTUM_BYTES.size()> frustum{};
    if(!read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 || !read(base+dos.e_lfanew,nt) ||
        nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || !game_build::supported(nt.FileHeader.TimeDateStamp) || nt.OptionalHeader.SizeOfImage<LIST_RVA+8 ||
        !read(base+game_build::rva(RENDER_RVA),render) || !read(base+game_build::rva(CACHE_RVA),cache) || !read(base+game_build::rva(FRUSTUM_RVA),frustum) ||
        !matches(nt.FileHeader.TimeDateStamp,render,cache,frustum)) return false;
    return install(reinterpret_cast<void*>(base+game_build::rva(RENDER_RVA)),reinterpret_cast<Cache>(base+game_build::rva(CACHE_RVA)),reinterpret_cast<Frustum>(base+game_build::rva(FRUSTUM_RVA)),
        [base]{uintptr_t list{};read(base+game_build::rva(LIST_RVA),list);return list;},std::move(target));
}
bool NativePipLens::install(void* target,Cache cache,Frustum frustum,std::function<uintptr_t()> list,std::function<std::optional<Target>()> source) {
    std::lock_guard lock{m_install};if(m_original || !target || !cache || !frustum || !list || !source) return false;
    const auto initialized=MH_Initialize();if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED) return false;
    void* original{};if(MH_CreateHook(target,reinterpret_cast<void*>(&on_render),&original)!=MH_OK) return false;
    m_cache=cache;m_frustum=frustum;m_list=std::move(list);m_target=std::move(source);m_original=reinterpret_cast<Render>(original);
    if(MH_EnableHook(target)!=MH_OK) {m_original=nullptr;MH_RemoveHook(target);return false;}
    NativeRenderProbe::get().attach_scene_hook(target);return true;
}
void NativePipLens::prepare(int32_t slot) {
    const auto list=m_list();int32_t count{};
    if(!list || list>UINTPTR_MAX-0x2996c || !read(list+0x29968,count) || count<1 || count>64 || slot<0 || slot>=count) return;
    const auto address=list+0x168+size_t(slot)*0xa60;std::array<uint8_t,0xa60> backup{};
    if(!read(address,backup) || !(field<uint64_t>(backup,0)&0x40)) return;
    const auto world=field<vr_camera::Matrix>(backup,0x34),view=field<vr_camera::Matrix>(backup,0x74);
    const auto record=field<int32_t>(backup,0xa50);
    std::optional<Target> target;
    try {target=m_target();} catch(...) {skip(2,slot,record,world);return;}
    if(!target) {skip(1,slot,record,world);return;}
    if(record!=target->record) {skip(3,slot,record,world);return;}
    Sample sample;
    sample.phase=target->phase;sample.updates=target->updates;sample.slot=slot;sample.reason=1;
    const auto pose_error=[&](const auto& candidate) {
        float error{};
        for(size_t i=0;i<16;++i) {
            if(!std::isfinite(candidate[i]) || !std::isfinite(world[i])) return INFINITY;
            error=std::max(error,std::abs(world[i]-candidate[i]));
        }
        return error;
    };
    Rejection event;event.updates=target->updates;event.phase=target->phase;event.slot=slot;event.record=target->record;
    event.thread=GetCurrentThreadId();event.candidates=target->recent_worlds.size()+(target->current_eligible?1:0);
    event.current_eligible=target->current_eligible;event.lock_wait_ms=target->lock_wait_ms;
    event.world=world;event.target_world=target->world;event.current_pose_error=pose_error(target->world);event.nearest_pose_error=event.current_pose_error;
    for(const auto& candidate:target->recent_worlds) event.nearest_pose_error=std::min(event.nearest_pose_error,pose_error(candidate));
    const auto reject=[&](int reason) {sample.reason=reason;event.reason=reason;event.history=target->history;publish(std::move(sample),false,std::move(event));};
    const auto matches_pose=[&](const auto& candidate) {
        for(size_t i=0;i<16;++i) if(!std::isfinite(candidate[i]) || !std::isfinite(world[i]) || std::abs(world[i]-candidate[i])>1e-4f) return false;
        return true;
    };
    if(!target->current_eligible && target->recent_worlds.empty()) {reject(12);return;}
    const bool current_pose=target->current_eligible && matches_pose(target->world);
    if(!current_pose && std::ranges::none_of(target->recent_worlds,matches_pose)) {reject(2);return;}
    if(!vr_camera::rigid(view,false) || backup[0x3cc] || !writable(address,backup.size())) {reject(3);return;}
    const auto rectangle=field<std::array<float,4>>(backup,0x160);
    const float near_z=field<float>(backup,0xb4),far_z=field<float>(backup,0xb8);
    const std::array<float,4> centred{-rectangle[2]*.5f,-rectangle[3]*.5f,rectangle[2],rectangle[3]};
    if(!pip_lens::build(centred,world,near_z,far_z,target->horizontal,target->vertical,sample.geometry)) {reject(4);return;}
    // The native reset must have produced the expected four near-plane corners.
    const auto old_corners=field<std::array<float,8>>(backup,0xc0);
    const std::array<float,8> expected{rectangle[0],rectangle[1],rectangle[0]+rectangle[2],rectangle[1],
        rectangle[0]+rectangle[2],rectangle[1]+rectangle[3],rectangle[0],rectangle[1]+rectangle[3]};
    if(field<uint32_t>(backup,0xbc)!=4) {reject(5);return;}
    for(size_t i=0;i<8;++i) if(!std::isfinite(old_corners[i]) || std::abs(old_corners[i]-expected[i])>1e-5f) {reject(5);return;}
    // No other PIP may masquerade as this record. Recheck the active renderer list.
    for(int32_t i=0;i<count;++i) if(i!=slot) {
        uint64_t flags{};int32_t record{};const auto other=list+0x168+size_t(i)*0xa60;
        if(!read(other,flags) || !read(other+0xa50,record) || ((flags&0x40) && record==target->record)) {reject(6);return;}
    }
    if(m_list()!=list) {reject(6);return;}
    if(target->camera_entries && (target->camera_entries->render_list!=list || target->camera_entries->pip_entry!=address ||
        target->camera_entries->pip_slot!=slot || !NativeCameraEntries::get().current(*target->camera_entries))) {reject(6);return;}
    auto* entry=reinterpret_cast<uint8_t*>(address);auto* camera=entry+0x34;
    try {
        std::memcpy(camera+0x12c,sample.geometry.rectangle.data(),sizeof(sample.geometry.rectangle));
        std::memcpy(camera+0x8c,sample.geometry.corners.data(),sizeof(sample.geometry.corners));
        // Use the shipping cache writer for projection, combined and inverse matrices.
        m_cache(entry,view.data(),sample.geometry.projection.data(),nullptr);
        m_frustum(camera);
        float native_radius{};std::memcpy(&native_radius,camera+0x2f8,4);
        if(!std::isfinite(native_radius) || native_radius<0) throw 7;
        const auto radius=std::max(native_radius,sample.geometry.radius);std::memcpy(camera+0x2f8,&radius,4);sample.geometry.radius=radius;
        // Native compact near-cap distance multiplies the symmetric radius by
        // the first unit corner's cosine. Off-centre corners have unequal lengths.
        float cap_distance{};for(size_t axis=0;axis<3;++axis) cap_distance-=world[8+axis]*world[8+axis]*near_z;
        std::memcpy(camera+0x30c,&cap_distance,4);
        std::array<float,12> directions{},sides{};std::array<float,3> normal{},origin{};
        std::memcpy(directions.data(),camera+0x310,sizeof(directions));std::memcpy(sides.data(),camera+0x340,sizeof(sides));
        std::memcpy(normal.data(),camera+0x370,sizeof(normal));std::memcpy(origin.data(),camera+0x37c,sizeof(origin));
        if(!pip_lens::compact_matches(sample.geometry,world,near_z,directions,sides,normal,origin,cap_distance)) throw 11;
        uint32_t planes{};std::memcpy(&planes,camera+0x15c,4);
        for(size_t i=0;i<6;++i) std::memcpy(sample.planes.data()+i*4,camera+0x160+i*0x14,4*sizeof(float));
        vr_camera::Matrix cached{};std::memcpy(cached.data(),entry+0x45c,sizeof(cached));
        for(size_t i=0;i<16;++i) if(!std::isfinite(cached[i]) || std::abs(cached[i]-sample.geometry.projection[i])>1e-5f) throw 8;
        vr_camera::Matrix inverse{},combined{};std::memcpy(inverse.data(),entry+0x4dc,sizeof(inverse));std::memcpy(combined.data(),entry+0x55c,sizeof(combined));
        if(!pip_lens::caches_match(cached,inverse,view,combined)) throw 8;
        if(planes!=6 || !pip_lens::culling_matches(sample.geometry,world,near_z,far_z,sample.planes)) throw 9;
        sample.reason=0;publish(std::move(sample),!current_pose);
    } catch(int reason) {
        std::memcpy(entry,backup.data(),backup.size());reject(reason);
    } catch(...) {
        std::memcpy(entry,backup.data(),backup.size());reject(10); // Restore every dependent native cache on failure.
    }
}
void NativePipLens::publish(Sample sample,bool historical,std::optional<Rejection> event) {
    std::lock_guard lock{m_mutex};
    sample.applied=m_sample.applied;sample.rejected=m_sample.rejected;sample.historical_matches=m_sample.historical_matches;
    sample.reason_counts=m_sample.reason_counts;sample.rejection_events=std::move(m_sample.rejection_events);
    sample.skipped=m_sample.skipped;sample.skip_counts=m_sample.skip_counts;sample.skip_events=std::move(m_sample.skip_events);sample.unexpected_errors=m_sample.unexpected_errors;
    if(event) {
        event->serial=++sample.rejected;
        if(event->reason>0 && size_t(event->reason)<sample.reason_counts.size()) ++sample.reason_counts[event->reason];
        if(sample.rejection_events.size()==64) sample.rejection_events.erase(sample.rejection_events.begin());
        sample.rejection_events.push_back(*event);
    } else {++sample.applied;if(historical) ++sample.historical_matches;}
    sample.received=std::chrono::steady_clock::now();m_sample=std::move(sample);
}
void NativePipLens::skip(int reason,int32_t slot,int32_t record,const vr_camera::Matrix& world) {
    std::lock_guard lock{m_mutex};
    const auto serial=++m_sample.skipped;++m_sample.skip_counts[reason];
    if(m_sample.skip_events.size()==64) m_sample.skip_events.erase(m_sample.skip_events.begin());
    m_sample.skip_events.push_back({serial,reason,GetCurrentThreadId(),slot,record,world});
}
void NativePipLens::on_render(void* renderer,uint32_t flags,int32_t slot) {
    auto& lens=get();try {lens.prepare(slot);} catch(...) {std::lock_guard lock{lens.m_mutex};++lens.m_sample.unexpected_errors;}
    NativeRenderProbe::observe_scene(renderer,flags,slot,lens.m_original.load());
}
NativePipLens::Sample NativePipLens::snapshot() const {std::lock_guard lock{m_mutex};return m_sample;}
