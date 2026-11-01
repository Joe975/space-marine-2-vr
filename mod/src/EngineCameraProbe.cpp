#include "EngineCameraProbe.h"
#include "NativeOrdinaryCamera.h"
#include "NativeTrackedOrdinaryCamera.h"
#include "ForeignViewHold.h"
#include "EngineProjectionHook.h"
#include "FrameSyncProbe.h"
#include "NativeCameraEntries.h"
#include "GameBuild.h"

#include <MinHook.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <intrin.h>
#include <spdlog/spdlog.h>

namespace
{
    template <typename T> T read(const void* base, size_t offset)
    {
        T value{};
        std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(T));
        return value;
    }
}

EngineCameraProbe& EngineCameraProbe::get()
{
    static auto* instance = new EngineCameraProbe;
    return *instance;
}

bool EngineCameraProbe::matches_build(uint32_t pe_timestamp, std::span<const uint8_t> function_start)
{
    return game_build::code(pe_timestamp, ADD_CAMERA_RVA, function_start, ADD_CAMERA_PROLOGUE);
}

EngineCameraProbe::State EngineCameraProbe::install_in_game(HMODULE game)
{
    if (state() != State::idle) { return state(); }
    const auto base = reinterpret_cast<const uint8_t*>(game);
    const auto dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt   = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto size = nt->OptionalHeader.SizeOfImage;
    const auto stamp = nt->FileHeader.TimeDateStamp;
    const auto add = game_build::rva(stamp, ADD_CAMERA_RVA);
    if (!add || add + ADD_CAMERA_PROLOGUE.size() > size ||
        !matches_build(stamp, { base + add, ADD_CAMERA_PROLOGUE.size() })) {
        m_state = State::wrong_build;
        spdlog::error("EngineCameraProbe: exe timestamp {:#x} ({}) or function bytes differ from the verified builds; not hooking", stamp, game_build::name(stamp));
        return state();
    }
    m_main_view_return_rva.store(game_build::rva(stamp, MAIN_VIEW_RETURN_RVA));
    m_additional_view_return_rva.store(game_build::rva(stamp, ADDITIONAL_VIEW_RETURN_RVA));
    spdlog::info("EngineCameraProbe: game build {:#x} ({}), add camera at rva {:#x}", stamp, game_build::name(stamp), add);
    return install(const_cast<uint8_t*>(base) + add, reinterpret_cast<uintptr_t>(base), size);
}

EngineCameraProbe::State EngineCameraProbe::install(void* target, uintptr_t module_base, size_t module_size)
{
    std::lock_guard lock{ m_install_mutex };
    if (state() != State::idle) { return state(); }
    m_module_base = module_base;
    m_module_size = module_size;

    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        m_state = State::hook_failed;
        spdlog::error("EngineCameraProbe: MinHook init: {}", MH_StatusToString(status));
        return state();
    }
    void* original{};
    status = MH_CreateHook(target, reinterpret_cast<void*>(&on_add_camera), &original);
    if (status == MH_OK) {
        // Publish the trampoline before enabling: render threads can call immediately.
        m_original.store(reinterpret_cast<AddCameraFn*>(original));
        status = MH_EnableHook(target);
        if (status != MH_OK) { MH_RemoveHook(target); }
    }
    if (status != MH_OK) {
        m_state = State::hook_failed;
        spdlog::error("EngineCameraProbe: hook: {}", MH_StatusToString(status));
        return state();
    }
    m_state = State::installed;
    spdlog::info("EngineCameraProbe: hooked camera-list add at {:x} (passive)", reinterpret_cast<uintptr_t>(target));
    return state();
}

void EngineCameraProbe::shift_camera(float* camera, float units)
{
    // Camera-to-world rows: 0 = right, 3 = position. World-to-view (+0x40) translation is
    // -dot(position, axis), so moving along the right axis only changes its x term.
    camera[12] += camera[0] * units;
    camera[13] += camera[1] * units;
    camera[14] += camera[2] * units;
    camera[16 + 12] -= units;
}

void EngineCameraProbe::set_alternating_shift(float units)
{
    if (!std::isfinite(units) || units < 0 || units > 1) { return; }
    m_alternating_shift.store(units);
}

void EngineCameraProbe::start_trace(unsigned count)
{
    std::lock_guard lock{m_snapshot_mutex};
    m_trace.clear();
    m_trace_remaining = std::min(count, 8192u);
}

std::vector<EngineCameraProbe::Sample> EngineCameraProbe::take_trace()
{
    std::lock_guard lock{m_snapshot_mutex};
    std::vector<Sample> result;
    result.swap(m_trace);
    return result;
}

bool EngineCameraProbe::publish_vr_eye(const VrEye& eye)
{
    std::lock_guard lock{ m_vr_mutex };
    if (!eye.valid_at(std::chrono::steady_clock::now())) {
        m_vr_eye.reset();
        std::lock_guard snapshot_lock{m_snapshot_mutex};m_ui_eyes.clear();
        return false;
    }
    if (m_vr_eye && eye.render_token <= m_vr_eye->render_token) { return false; }
    m_vr_eye = eye;
    std::lock_guard snapshot_lock{m_snapshot_mutex};m_ui_eyes.clear();
    return true;
}

void EngineCameraProbe::clear_vr_eye()
{
    std::lock_guard lock{ m_vr_mutex };
    m_vr_eye.reset();
    std::lock_guard snapshot_lock{m_snapshot_mutex};m_ui_eyes.clear();
}
void EngineCameraProbe::set_eye_provider(EyeProvider provider) {
    std::lock_guard lock{m_vr_mutex};
    m_eye_provider = std::move(provider);
    m_vr_eye.reset();
    std::lock_guard snapshot_lock{m_snapshot_mutex};m_ui_eyes.clear();
}
void EngineCameraProbe::set_anchor_provider(AnchorProvider provider) {
    std::lock_guard lock{m_vr_mutex}; m_anchor_provider = std::move(provider);
}
void EngineCameraProbe::set_main_observer(MainObserver observer) {
    std::lock_guard lock{m_vr_mutex}; m_main_observer = std::move(observer);
}

int64_t EngineCameraProbe::on_add_camera(void* list, void* source, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10,
    uint64_t a11)
{
    const auto return_address = reinterpret_cast<uintptr_t>(_ReturnAddress());
    auto&      probe          = get();
    const auto is_main_view   = source != nullptr && return_address - probe.m_module_base == probe.m_main_view_return_rva.load();
    const auto is_additional_view = source && return_address-probe.m_module_base==probe.m_additional_view_return_rva.load();
    const auto submission = is_main_view ? probe.m_main_submission.fetch_add(1) : 0;
    const auto amplitude = probe.m_alternating_shift.load();
    if(is_main_view && NativeTrackedOrdinaryCamera::get().armed())FrameSyncProbe::get().camera_job();
    auto game_frame = is_main_view || is_additional_view ? FrameSyncProbe::get().render_camera_frame() : std::nullopt;
    auto shift = is_main_view ? probe.m_main_shift.load() + (submission % 2 ? amplitude : -amplitude) : 0.0f;
    float center[3]{};
    if (source) { std::memcpy(center, static_cast<const float*>(source) + 12, sizeof(center)); }
    if(is_main_view && NativeTrackedOrdinaryCamera::get().armed()){
        // A separately armed tracked path owns BOTH native ordinary adds. Never
        // run the AFR eye provider, flat fixed-offset append or PIP observer on
        // top of it, or publish these jobs as adjacent AFR image provenance.
        probe.m_main_source=reinterpret_cast<uintptr_t>(source);
        FrameSyncProbe::get().record(FrameSyncProbe::Kind::camera);
        const NativeTrackedOrdinaryCamera::Args args{a3,a4,a5,a6,a7,a8,a9,a10,a11};
        auto& tracked=NativeTrackedOrdinaryCamera::get();const auto skipped=tracked.clock_skips();
        const auto result=tracked.on_main(list,source,game_frame?&*game_frame:nullptr,args,
            [&](void* target,void* camera,const auto& a){return probe.m_original.load()(target,camera,a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8]);});
        const auto count=tracked.clock_skips();
        if(count!=skipped && (count==1 || !(count&(count-1)))){
            const auto state=FrameSyncProbe::get().snapshot();const auto& clock=state.clock;
            spdlog::warn("OrdinaryClockGap: skipped={} markers={} errors={} simulation={} rendering={} presenting={} (snapshot after native fallback; waiting for a fresh unique job)",
                count,state.markers,clock.errors,clock.simulation?clock.simulation->render_id:0,clock.rendering?clock.rendering->render_id:0,clock.presenting?clock.presenting->render_id:0);
        }
        return result;
    }
    uint8_t saved[SOURCE_CAMERA_SIZE]{};
    bool modified = false;
    std::optional<VrEye> applied_eye;
    if (is_main_view) {
        probe.m_main_source=reinterpret_cast<uintptr_t>(source);
        std::memcpy(saved, source, sizeof(saved));
        // Do not unwind into the engine if a diagnostic/provider lock fails.
        try {
            const auto prepared=game_frame?probe.prepare_ui_eye(reinterpret_cast<uintptr_t>(source),*game_frame):std::nullopt;
            EyeProvider provider;
            AnchorProvider anchor_provider;
            std::optional<VrEye> selected;
            {
                std::lock_guard lock{probe.m_vr_mutex};
                provider = probe.m_eye_provider;
                anchor_provider = probe.m_anchor_provider;
                if (!provider) { selected = probe.m_vr_eye; }
            }
            if(prepared) selected=prepared->eye;
            else if (provider && game_frame) {
                selected = provider(*game_frame);
                if (selected && (selected->render_token != game_frame->render_id || selected->eye != game_frame->eye)) { selected.reset(); }
            }
            if (selected) {
                vr_camera::Matrix game{}, world{}, view{};
                std::memcpy(game.data(), saved, sizeof(game));
                const auto& eye = *selected;
                if (!prepared && eye.valid_at(std::chrono::steady_clock::now()) && anchor_provider) { game = anchor_provider(game,eye); }
                vr_camera::Matrix projection{};
                const bool lens_ready = !eye.fov || (EngineProjectionHook::get().state() == EngineProjectionHook::State::installed &&
                    vr_projection::build(*eye.fov, read<float>(source, 0x80), read<float>(source, 0x84), projection));
                if(prepared) {world=prepared->world;view=prepared->view;}
                if (eye.valid_at(std::chrono::steady_clock::now()) && lens_ready &&
                    (prepared || vr_camera::compose(game, eye.relative_eye, eye.units_per_metre, world, view,eye.view_settings.mode!=VrViewMode::diorama))) {
                    std::lock_guard preparation_lock{probe.m_preparation_mutex};
                    probe.m_native_inflight=reinterpret_cast<uintptr_t>(source);
                    std::memcpy(source, world.data(), sizeof(float) * 16);
                    std::memcpy(static_cast<float*>(source) + 16, view.data(), sizeof(float) * 16);
                    applied_eye = selected;
                    modified = true;
                    shift = 0; // Live eye pose supersedes flat-screen diagnostic alternation.
                    if (eye.fov) {
                        const auto& fov = *eye.fov;
                        const auto near_z = read<float>(source, 0x80);
                        const float rectangle[]{near_z*fov.left, near_z*fov.bottom, near_z*(fov.right-fov.left), near_z*(fov.top-fov.bottom)};
                        const float lens[]{(std::atan(fov.right)-std::atan(fov.left))*57.2957795f,
                            (std::atan(fov.top)-std::atan(fov.bottom))*57.2957795f, (fov.top-fov.bottom)/(fov.right-fov.left)};
                        std::memcpy(static_cast<uint8_t*>(source)+0x12c, rectangle, sizeof(rectangle));
                        std::memcpy(static_cast<uint8_t*>(source)+0x2d0, lens, sizeof(lens));
                    }
                }
            }
        } catch (...) {
        }
    }
    if (!modified && shift != 0.0f && is_main_view) {
        shift_camera(static_cast<float*>(source), shift);
        modified = true;
    }
    // An empty nested scope masks any outer main-view binding for helper/reentrant calls.
    std::optional<EngineProjectionHook::Binding> projection_binding;
    if (applied_eye && applied_eye->fov) { projection_binding = {*applied_eye->fov, applied_eye->eye, applied_eye->render_token}; }
    EngineProjectionHook::Scope projection_scope{projection_binding};
    if (is_main_view) { FrameSyncProbe::get().record(FrameSyncProbe::Kind::camera); }
    std::optional<NativeCameraEntries::Ticket> entry_ticket;
    try {entry_ticket=NativeCameraEntries::get().begin(reinterpret_cast<uintptr_t>(list),is_main_view?NativeCameraEntries::Role::main:is_additional_view?NativeCameraEntries::Role::pip:NativeCameraEntries::Role::other,game_frame);} catch(...) {}
    const auto index = probe.m_original.load()(list, source, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    // Diagnostics must never affect the result or unwind into the game.
    try {
        // A game PIP view: owned stereo stands down while one shows (ForeignViewHold). One added to
        // the list the pair was just added to sits behind it, with an HDR index past the game's table.
        if(!is_main_view && ForeignViewHold::foreign(a3)){
            const auto& tracked=NativeTrackedOrdinaryCamera::get();const auto slot=static_cast<int32_t>(index);
            const bool behind=tracked.armed() && slot>=2 && tracked.live_list()==reinterpret_cast<uintptr_t>(list) && !tracked.stopped();
            const auto at=std::chrono::steady_clock::now();
            if(ForeignViewHold::get().seen(at,slot,behind) || behind){const auto stats=ForeignViewHold::get().stats();
                if(behind && stats.behind_pair<=8)spdlog::error("StereoForeignView: slot={} flags={:#x} behind_pair=true sightings={} holds={} (a game PIP view landed behind the owned pair: its HDR index reads past the game's table this frame; stereo holds off while it shows)",slot,a3,stats.sightings,stats.holds);
                else if(!behind && stats.holds<=32)spdlog::info("ForeignViewHold: start slot={} flags={:#x} holds={} settle_ms={} (a game PIP view; owned stereo holds off while it shows)",slot,a3,stats.holds,ForeignViewHold::SETTLE.count());}
        }
        const auto completed_frame = is_main_view || is_additional_view ? FrameSyncProbe::get().render_camera_frame() : std::nullopt;
        if (!game_frame || !completed_frame || game_frame->render_id != completed_frame->render_id) { game_frame.reset(); }
        if(entry_ticket) {
            std::optional<vr_camera::Matrix> world;vr_camera::Matrix value{};SIZE_T bytes{};int32_t count{};
            const auto native_list=reinterpret_cast<uintptr_t>(list);const auto slot=static_cast<int32_t>(index);
            const auto address=NativeCameraEntries::address(native_list,slot);
            if(game_frame && native_list<=UINTPTR_MAX-0x2980c &&
               ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(native_list+0x29808),&count,sizeof(count),&bytes) && bytes==sizeof(count) && count>slot && count<=64 &&
               address && *address<=UINTPTR_MAX-0x74 && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(*address+0x34),value.data(),sizeof(value),&bytes) && bytes==sizeof(value)) world=value;
            NativeCameraEntries::get().finish(*entry_ticket,slot,world);entry_ticket.reset();
        }
        const uint64_t args[9]{ a3, a4, a5, a6, a7, a8, a9, a10, a11 };
        probe.observe(return_address, list, source, index, args, submission, shift, center, applied_eye ? &*applied_eye : nullptr,
            game_frame ? &*game_frame : nullptr, projection_scope.output(), projection_scope.applied());
    } catch (...) {
    }
    if(entry_ticket) try {NativeCameraEntries::get().finish(*entry_ticket,static_cast<int32_t>(index),std::nullopt);} catch(...) {}
    if (modified) {
        std::lock_guard preparation_lock{probe.m_preparation_mutex};
        std::memcpy(source, saved, sizeof(float)*32);
        if (applied_eye && applied_eye->fov) {
            std::memcpy(static_cast<uint8_t*>(source)+0x12c, saved+0x12c, sizeof(float)*4);
            std::memcpy(static_cast<uint8_t*>(source)+0x2d0, saved+0x2d0, sizeof(float)*3);
        }
        probe.m_native_inflight=0;
    }
    if (is_main_view) {
        try {
            const NativeOrdinaryCamera::Args native_args{a3,a4,a5,a6,a7,a8,a9,a10,a11};
            NativeOrdinaryCamera::get().on_main(list,source,game_frame?&*game_frame:nullptr,int32_t(index),native_args,[&](void* target,void* camera,const auto& a){return probe.m_original.load()(target,camera,a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8]);});
            MainObserver observer;
            { std::lock_guard lock{probe.m_vr_mutex}; observer=probe.m_main_observer; }
            if (observer) observer(list,source,game_frame ? &*game_frame : nullptr);
        } catch (...) {}
    }
    return index;
}

void EngineCameraProbe::observe(uintptr_t return_address, void* list, const void* source, int64_t index, const uint64_t (&args)[9], uint64_t submission, float shift, const float* center, const VrEye* vr_eye,
    const FrameSyncClock::Frame* game_frame, const std::optional<vr_camera::Matrix>& projection, bool projection_applied)
{
    if (source == nullptr) { return; }
    Sample s{};
    s.caller_rva = return_address >= m_module_base && return_address < m_module_base + m_module_size ? return_address - m_module_base : 0;
    s.list       = reinterpret_cast<uintptr_t>(list);
    s.source     = reinterpret_cast<uintptr_t>(source);
    s.index      = static_cast<int32_t>(index); // the engine returns a 32-bit int
    s.submission = submission;
    s.applied_shift = shift;
    if (vr_eye) { s.vr_pose_applied = true; s.vr_eye = vr_eye->eye; s.vr_render_token = vr_eye->render_token; s.vr_projection_requested = vr_eye->fov.has_value(); }
    s.vr_projection_applied = projection_applied;
    s.vr_synthetic = vr_eye && vr_eye->synthetic;
    if (vr_eye) { s.runtime_head = vr_eye->runtime_head; s.runtime_eye_to_head = vr_eye->runtime_eye_to_head; s.tracking_epoch = vr_eye->tracking_epoch; s.pose_captured = vr_eye->captured; }
    s.projection = projection;
    if (game_frame) { s.game_frame = *game_frame; }
    std::copy(center, center + 3, s.center);
    std::memcpy(s.world, source, sizeof(s.world));
    std::memcpy(s.second, static_cast<const uint8_t*>(source) + 0x40, sizeof(s.second));
    s.near_plane = read<float>(source, 0x80);
    s.far_plane  = read<float>(source, 0x84);
    std::memcpy(s.viewport, static_cast<const uint8_t*>(source) + 0x12c, sizeof(s.viewport));
    s.kind      = read<int32_t>(source, 0x2cc);
    s.fov_h_deg = read<float>(source, 0x2d0);
    s.fov_v_deg = read<float>(source, 0x2d4);
    std::copy(std::begin(args), std::end(args), s.args);
    s.received = std::chrono::steady_clock::now();

    std::lock_guard lock{ m_snapshot_mutex };
    if (s.game_frame && (s.caller_rva==m_main_view_return_rva.load() || s.caller_rva==m_additional_view_return_rva.load())) {
        auto& samples=s.caller_rva==m_additional_view_return_rva.load()?m_additional_samples:m_render_samples;
        const auto existing_frame = std::ranges::find_if(samples, [&](const auto& sample) { return sample.game_frame->render_id == s.game_frame->render_id; });
        s.frame_submissions = existing_frame == samples.end() ? 1 : existing_frame->frame_submissions+1;
        if (existing_frame != samples.end()) { *existing_frame = s; }
        else { if (samples.size() == 8) { samples.pop_front(); } samples.push_back(s); }
    }
    if (s.caller_rva == m_main_view_return_rva.load() && m_trace_remaining) {
        m_trace.push_back(s);
        --m_trace_remaining;
    }
    ++m_snapshot.calls;
    const auto existing = std::ranges::find(m_snapshot.by_caller, s.caller_rva, &Sample::caller_rva);
    if (existing != m_snapshot.by_caller.end()) {
        s.calls   = existing->calls + 1;
        *existing = s;
    } else if (m_snapshot.by_caller.size() < MAX_CALLERS) {
        s.calls = 1;
        m_snapshot.by_caller.push_back(s);
    }
}

EngineCameraProbe::Snapshot EngineCameraProbe::snapshot() const
{
    std::lock_guard lock{ m_snapshot_mutex };
    return m_snapshot;
}
std::optional<EngineCameraProbe::Sample> EngineCameraProbe::rendered_frame(uint64_t render_id) const {
    std::lock_guard lock{m_snapshot_mutex};
    for (const auto& sample : m_render_samples) {
        if (sample.game_frame->render_id == render_id && sample.frame_submissions == 1) { return sample; }
    }
    return std::nullopt;
}
std::optional<EngineCameraProbe::Sample> EngineCameraProbe::additional_frame(uint64_t render_id) const {
    std::lock_guard lock{m_snapshot_mutex};
    for(const auto& sample:m_additional_samples) if(sample.game_frame->render_id==render_id && sample.frame_submissions==1) return sample;
    return std::nullopt;
}
std::optional<EngineCameraProbe::UiEye> EngineCameraProbe::prepared_ui_eye(uint64_t render_id) const {
    std::lock_guard lock{m_snapshot_mutex};
    for(const auto& value:m_ui_eyes) if(value.frame.render_id==render_id) return value;
    return std::nullopt;
}
std::optional<EngineCameraProbe::UiEye> EngineCameraProbe::prepare_ui_eye(uintptr_t source,const FrameSyncClock::Frame& frame) {
    if(!source || source!=m_main_source || frame.invalid || !frame.render_id || frame.eye<0 || frame.eye>1) return std::nullopt;
    std::lock_guard preparation_lock{m_preparation_mutex};
    const auto now=std::chrono::steady_clock::now();
    if(const auto existing=prepared_ui_eye(frame.render_id)) {
        return existing->source==source && existing->eye.eye==frame.eye && existing->eye.valid_at(now) &&
            now-existing->eye.captured<=std::chrono::milliseconds(100) ? existing:std::nullopt;
    }
    // A missing binding must never cause us to compose an eye on top of a temporary VR camera.
    if(m_native_inflight) return std::nullopt;
    EyeProvider provider;AnchorProvider anchor_provider;std::optional<VrEye> selected;
    {std::lock_guard lock{m_vr_mutex};provider=m_eye_provider;anchor_provider=m_anchor_provider;if(!provider) selected=m_vr_eye;}
    if(provider) selected=provider(frame);
    if(!selected || !selected->fov || selected->render_token!=frame.render_id || selected->eye!=frame.eye ||
       !selected->valid_at(std::chrono::steady_clock::now())) return std::nullopt;
    UiEye result;result.source=source;result.frame=frame;result.eye=*selected;
    const auto camera=reinterpret_cast<const uint8_t*>(source);
    result.near_z=read<float>(camera,0x80);
    result.far_z=read<float>(camera,0x84);
    std::memcpy(result.native_lens.data(),camera+0x2d0,sizeof(result.native_lens));
    vr_camera::Matrix projection{},game{};
    if(EngineProjectionHook::get().state()!=EngineProjectionHook::State::installed ||
       !vr_projection::build(*selected->fov,result.near_z,read<float>(camera,0x84),projection)) return std::nullopt;
    std::memcpy(game.data(),camera,sizeof(game));
    if(anchor_provider) game=anchor_provider(game,*selected);
    if(!vr_camera::compose(game,selected->relative_eye,selected->units_per_metre,result.world,result.view,selected->view_settings.mode!=VrViewMode::diorama)) return std::nullopt;
    result.native_rectangle={read<float>(camera,0x8c),read<float>(camera,0x90),read<float>(camera,0x94)-read<float>(camera,0x8c),read<float>(camera,0xa8)-read<float>(camera,0x90)};
    std::lock_guard lock{m_snapshot_mutex};
    if(m_ui_eyes.size()==8) m_ui_eyes.pop_front();
    m_ui_eyes.push_back(result);
    return result;
}
std::optional<EngineCameraProbe::Sample> EngineCameraProbe::matching_camera(const vr_camera::Matrix& world, const vr_camera::Matrix& projection) const {
    std::lock_guard lock{m_snapshot_mutex};
    std::optional<Sample> result;
    for (const auto& sample : m_render_samples) {
        if (sample.frame_submissions != 1 || !sample.projection) { continue; }
        bool matches = true;
        for (int i = 0; i < 16; ++i) {
            matches &= std::isfinite(world[i]) && std::isfinite(projection[i]) &&
                std::abs(sample.world[i]-world[i]) < 1e-4f && std::abs((*sample.projection)[i]-projection[i]) < 1e-4f;
        }
        if (matches) {
            if (result) { return std::nullopt; }
            result = sample;
        }
    }
    return result;
}
