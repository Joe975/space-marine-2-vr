#pragma once

#include "VrCameraPose.h"
#include "VrMenuPanel.h"
#include "MenuPanelRenderer.h"
#include "NativeHudRenderer.h"
#include "GuardGuideRenderer.h"
#include "ComfortVignetteRenderer.h"
#include "VrGuardGuide.h"
#include "VrAimDot.h"
#include "GpuTimestampSpan.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <optional>
#include <chrono>
#include <filesystem>

// Retained GPU copies only. VrSession owns opt-in runtime submission and transfer leases.
// Called on the Present thread. A separate direct queue is allowed only when
// the caller proves producer completion and retains an immutable source lease.
class EyeTextureCapture {
public:
    struct Metadata {
        uint32_t game_token{};
        uint64_t render_id{};
        int eye{};
        bool tracked_stereo{}; // true only when matching tracked pose AND eye projection were applied
        bool screen_menu{};
        VrMenuPanel menu_panel;
        bool native_hud{};
        VrNativeHud hud_crop;
        VrCompactHud::Settings hud_placement;
        VrNativePrompt hud_prompt=VrNativePrompt::off(); // Centre prompt crop, drawn with the native HUD.
        // The game's camera for this image: when set the prompt canvas (crosshair and world markers) is
        // locked to it instead of the head (VrNativePrompt::camera_locked); unset keeps it head-locked.
        std::optional<VrHudCamera> hud_camera;
        bool hud_camera_lock{true}; // false: the canvas stays head-locked (third person); hud_camera still sizes the reticle.
        // The game's crosshair patch at the aim point (VrHudReticle), drawn with the native HUD when
        // reticle_target and hud_camera are set; the canvas then leaves the patch out. reticle_target is
        // unset after capture when it was not drawn.
        VrHudReticle hud_reticle=VrHudReticle::off();
        std::optional<std::array<float,3>> reticle_target;
        bool reticle_cut{true}; // false: other UI is beside the patch (VrHudReticle::crowded), so the canvas stays whole.
        VrNativeHudCorners hud_corners=VrNativeHudCorners::off(); // Right-hand HUD corners on a low panel, drawn with the native HUD.
        // Guard guide quads (head-yaw frame of runtime_head). Drawn on tracked gameplay eyes
        // only, never on the menu panel; a failure skips the guide, never the eye image.
        std::optional<VrGuardGuide::Draw> guard_guide;
        // Comfort vignette strength [0,1] (vr_vignette; VrBodyFollowing while riding along). Drawn
        // on tracked gameplay eyes only, over the scene and the guide, under the HUD; a failure
        // skips it, never the eye image. 0 after capture when it was not drawn.
        float comfort_vignette{};
        // Aim dot at the aim ray's hit point (game world, VrAimDot). Drawn on tracked gameplay eyes only,
        // over everything else; a failure skips the dot, never the eye image. Unset after capture when
        // it was not drawn.
        std::optional<VrAimDot::Draw> aim_dot;
        vr_camera::Matrix world{vr_camera::IDENTITY}, projection{};
        vr_camera::Matrix runtime_head{vr_camera::IDENTITY};
        vr_camera::Matrix runtime_eye_to_head{vr_camera::IDENTITY};
        uint64_t tracking_epoch{};
        std::chrono::steady_clock::time_point pose_captured{};
    };
private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture, source,hud_source,menu_source,tap_target;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
        uint64_t pending{};
        MenuPanelRenderer::Bindings menu_bindings;
        NativeHudRenderer::Bindings hud_bindings;
        GuardGuideRenderer::Bindings guide_bindings;
        ComfortVignetteRenderer::Bindings vignette_bindings;
        GpuTimestampSpan probe;
        bool probe_pending{};int probe_eye{};uint64_t probe_render{};double probe_enqueue{};
    };
public:
    struct Image {
        Metadata metadata;
        uint64_t generation{}, fence_value{};
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        std::shared_ptr<const Slot> lease;
        ID3D12Resource* texture() const { return lease ? lease->texture.Get() : nullptr; }
        bool ready() const {
            if (!fence || !lease) { return false; }
            const auto completed = fence->GetCompletedValue();
            return completed != UINT64_MAX && completed >= fence_value;
        }
    };
    struct Pair { Image left, right; };
    // A camera mode photo (EyePhoto): the next capture of a plain eye also copies its source into this
    // readback buffer, in the same command list and before any panel is drawn, so the picture has no
    // HUD, guide or menu in it. Nothing waits: Tapped says when the copy will be done.
    struct Tap { Microsoft::WRL::ComPtr<ID3D12Resource> buffer; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; };
    struct Tapped { Microsoft::WRL::ComPtr<ID3D12Fence> fence; uint64_t value{}; uint64_t render_id{}; };
    void tap(Tap value) { m_tap=std::move(value); }
    // The tap's fence once a capture has submitted it. An unused tap ends here too: it never
    // outlives the frame it was set for.
    std::optional<Tapped> take_tapped() { m_tap.reset(); auto out=std::move(m_tapped); m_tapped.reset(); return out; }
    HRESULT capture(ID3D12Device*, ID3D12CommandQueue*, ID3D12Resource*, const Metadata&,
                    D3D12_RESOURCE_STATES input_state=D3D12_RESOURCE_STATE_PRESENT,ID3D12Resource* native_hud=nullptr,
                    ID3D12Resource* menu_image=nullptr); // Optional separate screen-menu image (PRESENT state), drawn as the panel.
    std::optional<Image> latest(int eye) const;
    std::optional<Pair> pair(bool require_tracked = true) const;
    void poll();
    // Explicit diagnostic only: synchronous, bounded GPU readback of the completed pair.
    // Writes RGB PPMs and metadata; never called by normal rendering or benchmarks.
    HRESULT dump_pair(const std::filesystem::path& directory, bool require_tracked = true);
    // Refuse a nonblocking reset while copies are pending; optional bounded drain for device reset.
    bool reset(bool wait = false);
    // Images per eye (2 to MAX_SLOTS, created only when needed). A capture with every slot held by a
    // consumer or in flight is refused with E_PENDING and that pair is lost. The OpenXR submit thread
    // holds up to three per eye at once (queued, being transferred, retained until its queue finishes),
    // so two slots dropped 13-40 % of pairs on 29 Oct (MOD_REVIEW_2026-10-29).
    static constexpr size_t MAX_SLOTS=4;
    void slots(size_t count) { m_slot_count=count<2?2:count>MAX_SLOTS?MAX_SLOTS:count; }
    size_t slots() const { return m_slot_count; }
    ~EyeTextureCapture();
    EyeTextureCapture() = default;
    EyeTextureCapture(const EyeTextureCapture&) = delete;
    EyeTextureCapture& operator=(const EyeTextureCapture&) = delete;
private:
    HRESULT configure(ID3D12Device*,ID3D12CommandQueue*,const D3D12_RESOURCE_DESC&);
    HRESULT make_slot(std::shared_ptr<Slot>&);
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    D3D12_RESOURCE_DESC m_desc{};
    MenuPanelRenderer m_menu_renderer;
    NativeHudRenderer m_hud_renderer;
    GuardGuideRenderer m_guide_renderer;bool m_guide_failed{};
    ComfortVignetteRenderer m_vignette_renderer;bool m_vignette_failed{};
    std::array<std::array<std::shared_ptr<Slot>,MAX_SLOTS>,2> m_slots;size_t m_slot_count{2};
    std::array<std::optional<Image>,2> m_latest;
    std::optional<Pair> m_candidate_pair;
    std::optional<Tap> m_tap;
    std::optional<Tapped> m_tapped;
    uint64_t m_generation{}, m_fence_value{}, m_last_render_id{};
    bool m_poisoned{};
};
