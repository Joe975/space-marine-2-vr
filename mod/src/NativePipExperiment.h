#pragma once
#include <Windows.h>
#include "GameBuild.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <chrono>
#include <vector>
#include <span>
#include "VrCameraPose.h"
#include "PipPoseEvidence.h"
#include "NativeCameraEntries.h"

// Desktop-only bounded feasibility experiment, NOT a stereo submission mode.
// Owns one stable camera; registers/releases it through the shipping PIP API.
// Native contracts and limitations: docs/SINGLE_FRAME_STEREO.md.
class NativePipExperiment {
public:
    using Time=std::chrono::steady_clock::time_point;
    using Now=std::function<Time()>;
    static constexpr uint32_t BUILD_TIMESTAMP=0x6ac3e716;
    static constexpr uintptr_t GET_RVA=0x1887d50,REGISTER_RVA=0x1887940,RELEASE_RVA=0x1887a60,MANAGER_RVA=0x4f17768;
    inline static constexpr game_build::Code GET_BYTES=game_build::hashed(0x1887d50,16,"2cc963ffa977321115db426fb27c3d03afc9e9b206e704b5fe97f1b9a47ede55");
    inline static constexpr game_build::Code REGISTER_BYTES=game_build::hashed(0x1887940,20,"36f250054f3aa2353af32b7587322f291d8929dfbd9a8f7ecea508c315ed84fc");
    inline static constexpr game_build::Code RELEASE_BYTES=game_build::hashed(0x1887a60,16,"e35305931da317644d5969f91dae5fc2a7ca563af2955e91e9d236b70c671b7c");
    struct Array {uintptr_t data{};int32_t count{},capacity{};};
    struct Size {int32_t width{640},height{360};};
    struct Request {
        // 0x40 expanded effects; 0x10 uses native current exposure ratio instead
        // of request+0x10 (renderer 0x180ac7b..0x180ac99 on this build).
        uint8_t flags{0x50};
        uint8_t padding[7]{};
        uintptr_t camera{};
        float exposure{-1},fxaa_edge_threshold{-1},fxaa_edge_threshold_min{-1},fxaa_subpix{-1};
    };
    static_assert(sizeof(Request)==0x20 && offsetof(Request,camera)==8);
    struct Config {
        Size size;
        uint32_t frames{600};
        uint32_t warmup_frames{120};
        float shift{0.1f};
        float lens_shift{}; // Diagnostic near-rectangle offset as a fraction of width.
        bool pose_tags{}; // Desktop-only 4096-position pattern, radius <0.013 world units.
        bool retain_until_exit{}; // One native slot stays unrecycled for a bounded future playtest.
        // Larger requests corrupted the main image in the 2026-10-16 4K control.
        // Keep the diagnostic within the exercised envelope until native scratch
        // target allocation and request retirement are independently understood.
        bool valid() const {return size.width>=64 && size.width<=1280 && size.height>=64 && size.height<=720 &&
            frames>0 && frames<=4096 && warmup_frames<=600 && std::isfinite(shift) && std::abs(shift)<=1 &&
            std::isfinite(lens_shift) && std::abs(lens_shift)<=.25f;}
    };
    struct Snapshot {
        bool bound{},armed{},active{},finished{},lost{},quarantined{};
        uint64_t updates{},rejected{},render_id{};
        uintptr_t context{},manager{},pool{},texture{},camera{};
        int32_t index{-1},texture_index{-1};
        Size requested,allocated;
        std::array<float,3> center{},shifted{};
    };
    struct Api {
        std::function<uintptr_t()> context,manager;
        std::function<void(uintptr_t,int32_t*,const Size*,const Request*,bool)> create;
        std::function<void(uintptr_t,int32_t,bool,bool)> release;
        Now now{[]{return std::chrono::steady_clock::now();}};
    };
    static NativePipExperiment& get() {static auto* instance=new NativePipExperiment;return *instance;}
    static bool matches(uint32_t stamp,std::span<const uint8_t> get,std::span<const uint8_t> create,std::span<const uint8_t> release) {
        return game_build::code(stamp,GET_RVA,get,GET_BYTES) && game_build::code(stamp,REGISTER_RVA,create,REGISTER_BYTES) && game_build::code(stamp,RELEASE_RVA,release,RELEASE_BYTES);
    }
    // Binding/arming is startup-only, before installing the main-camera observer.
    bool bind(Api api) {
        std::lock_guard lock{m_mutex};
        if(m_state.active || m_retention_exposed || !api.context || !api.manager || !api.create || !api.release || !api.now) return false;
        m_api=std::move(api);m_state={};m_state.bound=true;m_origins.clear();m_history={};m_history_cursor=0;return true;
    }
    bool bind_in_game(HMODULE module) {
        auto base=reinterpret_cast<uintptr_t>(module);IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
        std::array<uint8_t,GET_BYTES.size()> get{};std::array<uint8_t,REGISTER_BYTES.size()> create{};std::array<uint8_t,RELEASE_BYTES.size()> release{};
        if(!read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 ||
           !read(base+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
           !game_build::supported(nt.FileHeader.TimeDateStamp) || nt.OptionalHeader.SizeOfImage<MANAGER_RVA+8 || !read(base+game_build::rva(GET_RVA),get) || !read(base+game_build::rva(REGISTER_RVA),create) || !read(base+game_build::rva(RELEASE_RVA),release) ||
           !matches(nt.FileHeader.TimeDateStamp,get,create,release)) return false;
        using Get=uintptr_t(*)();using Create=void(*)(uintptr_t,int32_t*,const Size*,const Request*,bool);using Release=void(*)(uintptr_t,int32_t,bool,bool);
        return bind({reinterpret_cast<Get>(base+game_build::rva(GET_RVA)),[base]{uintptr_t result{};read(base+game_build::rva(MANAGER_RVA),result);return result;},
            reinterpret_cast<Create>(base+game_build::rva(REGISTER_RVA)),reinterpret_cast<Release>(base+game_build::rva(RELEASE_RVA))});
    }
    bool arm(const Config& config) {
        std::lock_guard lock{m_mutex};
        if(!m_state.bound || m_state.armed || m_state.active || m_state.finished || !config.valid()) return false;
        m_origins.reserve(config.frames);m_config=config;m_state.armed=true;m_state.requested=config.size;return true;
    }
    // Called only after the real main camera add/restoration on its native job thread.
    void on_main(const void* source,uint64_t render_id,bool gameplay,std::optional<uint32_t> token={}) {
        std::lock_guard lock{m_mutex};
        if(!m_state.bound || !m_state.armed || m_state.finished) return;
        const auto context=m_api.context(),manager=m_api.manager();Array records{};
        if(!context || !manager || !read(context,records) || records.count<0 || records.count>64 || records.capacity<records.count) {++m_state.rejected;return;}
        if(m_state.active && (context!=m_state.context || manager!=m_state.manager || !owns(records))) {
            // Never release an index in a different scene or one now owned by another caller.
            m_state.active=false;m_state.finished=true;m_state.lost=true;return;
        }
        if(m_state.active && (!gameplay || m_state.updates>=m_config.frames)) {
            if(m_config.retain_until_exit) {
                // Native Release also marks the pool slot for reuse. Stop only
                // our source on its camera-job thread; retain that native slot.
                // Copies already queued keep stable camera/resource storage.
                const auto lease=pool_lease(records);const uintptr_t zero{};SIZE_T bytes{};
                const auto source_pointer=records.data+size_t(m_state.index)*0x48+8;
                m_state.quarantined=lease && WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(source_pointer),&zero,sizeof(zero),&bytes) && bytes==sizeof(zero);
                m_state.lost=!m_state.quarantined;m_state.active=false;m_state.finished=true;return;
            }
            m_api.release(manager,m_state.index,true,false);m_state.active=false;m_state.finished=true;return;
        }
        if(!gameplay || !source || !render_id || render_id<=m_last_seen) {
            if(!gameplay) m_warmup=0;
            ++m_state.rejected;return;
        }
        m_last_seen=render_id;
        std::array<uint8_t,0x388> camera{};
        if(!read(reinterpret_cast<uintptr_t>(source),camera)) {++m_state.rejected;return;}
        vr_camera::Matrix world{},view{};
        std::memcpy(world.data(),camera.data(),sizeof(world));std::memcpy(view.data(),camera.data()+0x40,sizeof(view));
        if(!vr_camera::rigid(world,false) || !vr_camera::rigid(view,false)) {++m_state.rejected;return;}
        float fov{};std::memcpy(&fov,camera.data()+0x2d0,4);
        if(!std::isfinite(fov) || fov<40 || fov>150) {m_warmup=0;++m_state.rejected;return;}
        if(!m_state.active && m_warmup++<m_config.warmup_frames) {++m_state.rejected;return;}
        m_camera=camera;
        for(size_t axis=0;axis<3;++axis) {
            m_state.center[axis]=world[12+axis];world[12+axis]+=world[axis]*m_config.shift;m_state.shifted[axis]=world[12+axis];
        }
        if(m_config.pose_tags) {
            // Use the accepted update ordinal, not a wrapping game-frame ID.
            // The pattern never repeats within the entire 4096-update budget.
            const auto tag=m_state.updates;
            const float right=(float(tag&15)-7.5f)*.001f,up=(float((tag>>4)&15)-7.5f)*.001f,forward=(float((tag>>8)&15)-7.5f)*.001f;
            for(size_t axis=0;axis<3;++axis) {world[12+axis]+=world[axis]*right+world[4+axis]*up+world[8+axis]*forward;m_state.shifted[axis]=world[12+axis];}
        }
        view=vr_camera::inverse(world);
        std::memcpy(m_camera.data(),world.data(),sizeof(world));std::memcpy(m_camera.data()+0x40,view.data(),sizeof(view));
        if(m_config.lens_shift!=0) {
            std::array<float,4> rect{};std::memcpy(rect.data(),m_camera.data()+0x12c,sizeof(rect));
            if(!std::ranges::all_of(rect,[](float x){return std::isfinite(x);}) || rect[2]<=0 || rect[3]<=0) {++m_state.rejected;return;}
            rect[0]+=rect[2]*m_config.lens_shift;std::memcpy(m_camera.data()+0x12c,rect.data(),sizeof(rect));
        }
        Request request;request.camera=reinterpret_cast<uintptr_t>(m_camera.data());
        if(!m_state.active && records.count>=63) {++m_state.rejected;return;}
        auto index=m_state.active?m_state.index:-1;
        m_state.finished=true; // Exceptions/partial native failure must never retry allocations indefinitely.
        m_retention_exposed|=m_config.retain_until_exit;
        m_api.create(manager,&index,&m_config.size,&request,false); // Append; never steal the native free list.
        if(!read(context,records) || index<0 || index>=records.count) {m_state.finished=true;++m_state.rejected;return;}
        m_state.index=index;m_state.context=context;m_state.manager=manager;m_state.camera=request.camera;
        m_state.finished=false;
        m_state.active=true;m_state.render_id=render_id;++m_state.updates;
        if(!owns(records)) {m_state.active=false;m_state.finished=true;m_state.lost=true;return;}
        const auto record=records.data+size_t(index)*0x48;
        m_state.texture_index=-1;m_state.texture=0;m_state.pool=0;m_state.allocated={0,0};
        read(record+0x40,m_state.texture_index);
        uintptr_t pool{};Array textures{};
        if(read(manager+0x30,pool) && read(pool,textures) && textures.count>0 && textures.count<=1024 &&
            textures.capacity>=textures.count && m_state.texture_index>=0 && m_state.texture_index<textures.count &&
            textures.data && textures.data<=UINTPTR_MAX-size_t(textures.count)*0x18) {
            m_state.pool=pool;
            const auto texture=textures.data+size_t(m_state.texture_index)*0x18;
            read(texture,m_state.texture);read(texture+8,m_state.allocated);
        }
        m_history[m_history_cursor++%m_history.size()]={world,m_api.now(),render_id,token,m_state.updates};
        m_origins.push_back(m_history[(m_history_cursor-1)%m_history.size()]);
    }
    Snapshot snapshot() const {std::lock_guard lock{m_mutex};return m_state;}
    struct RenderOrigin {int32_t record{};uint64_t render_id{};uint32_t token{};vr_camera::Matrix source_world{},render_world{};};
    // Resolve the camera entry actually being rendered, never the latest camera
    // job by proximity in time. Repeated stationary poses deliberately fail.
    std::optional<RenderOrigin> render_origin(uintptr_t renderer,std::optional<Time> at={},const NativeCameraEntries::Pair* receipt=nullptr) const {
        std::lock_guard lock{m_mutex};Array records{};uintptr_t entry{};uint64_t flags{};int32_t record{};vr_camera::Matrix world{};
        if(!m_state.active || m_state.finished || !renderer || renderer>UINTPTR_MAX-0x148 ||
           m_api.context()!=m_state.context || !read(m_state.context,records) || records.count!=1 || !owns(records) ||
           !read(renderer+0x140,entry) || !entry || entry>UINTPTR_MAX-0xa54 || !read(entry,flags) || !(flags&0x40) ||
           !read(entry+0xa50,record) || record!=m_state.index || !read(entry+0x34,world) || !vr_camera::rigid(world,false)) return std::nullopt;
        if(receipt && (entry!=receipt->pip_entry || !NativeCameraEntries::get().current(*receipt))) return std::nullopt;
        const auto now=at?*at:m_api.now(); // Sample only after acquiring and validating ownership.
        std::optional<RenderOrigin> result;
        // Compare against every accepted job, including expired poses. Otherwise
        // a delayed old entry could be relabelled when a newer pose coincides.
        for(const auto& pose:m_origins) {
            if(receipt && (pose.render_id!=receipt->render_id || pose.token!=receipt->token)) continue;
            bool match=true;for(size_t i=0;i<16;++i) if(std::abs(pose.world[i]-world[i])>1e-4f) {match=false;break;}
            if(!match) continue;
            if(!pose.render_id || !pose.token || now<pose.received || now-pose.received>std::chrono::milliseconds(250)) return std::nullopt;
            if(result && result->render_id!=pose.render_id) return std::nullopt;
            result=RenderOrigin{record,pose.render_id,*pose.token,pose.world,world};
        }
        return result;
    }
    struct OwnedCamera {
        int32_t record{-1};vr_camera::Matrix world{};float lens_shift{};uint64_t updates{};
        std::vector<vr_camera::Matrix> recent_worlds;
        std::vector<PipPoseEvidence> history;
        bool current_eligible{};double lock_wait_ms{};
    };
    // Inspect expired/future evidence for diagnostics without making it eligible.
    std::optional<OwnedCamera> inspect_owned_camera(std::optional<Time> at={}) const {
        const auto waiting=std::chrono::steady_clock::now();
        std::lock_guard lock{m_mutex};
        const auto acquired=std::chrono::steady_clock::now();Array records{};
        if(!m_state.active || m_state.finished || m_api.context()!=m_state.context || !read(m_state.context,records) || records.count!=1 || !owns(records)) return std::nullopt;
        const auto now=at?*at:m_api.now();
        OwnedCamera value;value.record=m_state.index;value.lens_shift=m_config.lens_shift;value.updates=m_state.updates;
        value.lock_wait_ms=std::chrono::duration<double,std::milli>(acquired-waiting).count();
        std::memcpy(value.world.data(),m_camera.data(),sizeof(value.world));
        for(const auto& pose:m_history) if(pose.render_id) {
            const bool eligible=now>=pose.received && now-pose.received<=std::chrono::milliseconds(250);
            value.history.push_back({pose.world,pose.render_id,pose.updates,pose.token,std::chrono::duration<double,std::milli>(now-pose.received).count(),eligible});
            if(eligible) value.recent_worlds.push_back(pose.world);
            if(pose.updates==m_state.updates) value.current_eligible=eligible;
        }
        return value;
    }
    std::optional<OwnedCamera> owned_camera(std::optional<Time> at={}) const {
        auto value=inspect_owned_camera(at);
        return value && !value->recent_worlds.empty()?std::move(value):std::nullopt;
    }
    std::optional<OwnedCamera> inspect_frame_camera(const NativeCameraEntries::Pair& receipt,std::optional<Time> at={}) const {
        if(!NativeCameraEntries::get().current(receipt)) return std::nullopt;
        auto value=inspect_owned_camera(at);if(!value) return std::nullopt;
        std::optional<PipPoseEvidence> selected;
        for(const auto& pose:value->history) if(pose.render_id==receipt.render_id && pose.token==receipt.token) {
            if(selected) return std::nullopt;selected=pose;
        }
        if(!selected || !NativeCameraEntries::get().current(receipt)) return std::nullopt;
        value->world=selected->world;value->updates=selected->updates;value->current_eligible=selected->eligible;
        value->recent_worlds.clear();if(selected->eligible) value->recent_worlds.push_back(selected->world);
        value->history={*selected};return value;
    }
    std::optional<OwnedCamera> frame_camera(const NativeCameraEntries::Pair& receipt,std::optional<Time> at={}) const {
        auto value=inspect_frame_camera(receipt,at);
        return value && value->current_eligible?std::move(value):std::nullopt;
    }
    struct PoolLease {
        uintptr_t context{},manager{},records{},pool{},slots{},slot{},camera{},texture{};
        int32_t index{-1},texture_index{-1};Size allocated{0,0};
        uint8_t record_recycle{},record_deferred_release{},pool_release_marked{};
    };
    struct Resource {uintptr_t texture{},resource{};uint32_t state{},tracker_index{};int32_t lane{-1};std::array<uint8_t,56> record{};PoolLease lease;};
    // Present is the existing validated graphics-queue boundary. Hold request ownership
    // across diagnostic readback so the camera job cannot release its texture mid-copy.
    bool visit_resource(uintptr_t command_context,const std::function<void(const Snapshot&,const Resource&)>& visitor) const {
        std::lock_guard lock{m_mutex};Array records{};
        if(!m_state.active || m_state.finished || !visitor || m_api.context()!=m_state.context || m_api.manager()!=m_state.manager ||
           !read(m_state.context,records) || records.count!=1 || !owns(records)) return false;
        const auto lease=pool_lease(records);if(!lease) return false;
        Resource resource;resource.texture=m_state.texture;resource.lease=*lease;
        uint32_t tracker_index{},subresources{};uintptr_t states{};
        if(!read(resource.texture+0x278,resource.resource) || !resource.resource || !read(command_context+0x109a0,tracker_index) || tracker_index>=4) return false;
        const auto tracker=resource.texture+0xd8+tracker_index*0x50;
        if(!read(tracker+8,states) || !read(tracker+0x10,subresources) || subresources!=1 ||
           !read(states?states:tracker+0x18,resource.record)) return false;
        resource.tracker_index=tracker_index;
        // Native barrier helper reads the active context stack, rather than
        // assuming the graphics lane when a parallel state record is active.
        size_t offset=0x10;
        if(resource.record[0] && resource.record[0x30]) {
            uintptr_t stack{};int32_t count{};
            if(!read(command_context+0x38,stack) || !read(command_context+0x40,count) || count<1 || count>64 ||
                !read(stack+size_t(count-1)*4,resource.lane) || resource.lane!=0) return false;
            offset=0x28; // Only the verified direct/graphics lane may use this readback.
        }
        std::memcpy(&resource.state,resource.record.data()+offset,4);
        visitor(m_state,resource);return true;
    }
private:
    template<class T> static bool read(uintptr_t address,T& value) {
        SIZE_T count{};return address && address<=UINTPTR_MAX-sizeof(T) && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(T),&count) && count==sizeof(T);
    }
    bool owns(const Array& records) const {
        uintptr_t camera{};return records.count>0 && records.count<=64 && records.capacity>=records.count &&
            m_state.index>=0 && m_state.index<records.count && records.data && records.data<=UINTPTR_MAX-size_t(records.count)*0x48 &&
            read(records.data+size_t(m_state.index)*0x48+8,camera) && camera==reinterpret_cast<uintptr_t>(m_camera.data());
    }
    std::optional<PoolLease> pool_lease(const Array& records) const {
        PoolLease value;Array textures{};int32_t output_count{};uintptr_t pool{};
        value.context=m_state.context;value.manager=m_state.manager;value.records=records.data;
        value.index=m_state.index;value.camera=m_state.camera;
        const auto record=records.data+size_t(m_state.index)*0x48;
        if(!m_state.manager || m_state.manager>UINTPTR_MAX-0x38 || !m_state.pool || !m_state.texture ||
           !read(record+0x30,output_count) || output_count!=0 ||
           !read(record+0x3c,value.record_recycle) || value.record_recycle ||
           !read(record+0x3d,value.record_deferred_release) || value.record_deferred_release ||
           !read(record+0x40,value.texture_index) || value.texture_index!=m_state.texture_index ||
           !read(m_state.manager+0x30,pool) || pool!=m_state.pool || !read(pool,textures) ||
           textures.count<1 || textures.count>1024 || textures.capacity<textures.count || !textures.data ||
           textures.data>UINTPTR_MAX-size_t(textures.count)*0x18 || value.texture_index<0 || value.texture_index>=textures.count) return std::nullopt;
        value.pool=pool;value.slots=textures.data;value.slot=textures.data+size_t(value.texture_index)*0x18;
        if(!read(value.slot,value.texture) || value.texture!=m_state.texture ||
           !read(value.slot+8,value.allocated) || value.allocated.width!=m_state.allocated.width || value.allocated.height!=m_state.allocated.height ||
           value.allocated.width<64 || value.allocated.width>1280 || value.allocated.height<64 || value.allocated.height>720 ||
           !read(value.slot+0x10,value.pool_release_marked) || value.pool_release_marked) return std::nullopt;
        return value;
    }
    mutable std::mutex m_mutex;Api m_api;Config m_config;Snapshot m_state;
    bool m_retention_exposed{}; // Native camera pointer may survive even a partial allocation failure.
    uint64_t m_last_seen{};uint32_t m_warmup{};
    struct OwnedPose {vr_camera::Matrix world{};Time received{};uint64_t render_id{};std::optional<uint32_t> token;uint64_t updates{};};
    std::array<OwnedPose,16> m_history{};uint64_t m_history_cursor{};
    std::vector<OwnedPose> m_origins; // At most Config::frames (4096), retained until release/rebind.
    alignas(16) std::array<uint8_t,0x388> m_camera{};
};
