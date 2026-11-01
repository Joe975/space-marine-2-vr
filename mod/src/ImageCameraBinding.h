#pragma once
#include "EngineCameraProbe.h"
#include "EyeTextureCapture.h"

// SM2 presents the preceding camera job. Establish that relationship from a unique,
// full pose/projection match, not a fixed parity offset or a worker's TLS history.
inline bool bind_image_camera(EyeTextureCapture::Metadata& image, const FrameSyncClock::Frame& presenting,
    const EngineCameraProbe::Sample& camera, std::chrono::steady_clock::time_point now) {
    image.tracked_stereo = false;
    if (image.game_token != presenting.token || !camera.game_frame || !camera.projection || camera.frame_submissions != 1 ||
        camera.game_frame->render_id+1 != presenting.render_id || uint32_t(camera.game_frame->token+1) != presenting.token ||
        now < camera.received || now-camera.received > std::chrono::milliseconds(250) ||
        !camera.vr_pose_applied || !camera.vr_projection_requested || !camera.vr_projection_applied ||
        camera.vr_render_token != camera.game_frame->render_id || camera.vr_eye != camera.game_frame->eye) { return false; }
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(image.world[i]) || !std::isfinite(image.projection[i]) ||
            std::abs(camera.world[i]-image.world[i]) >= 1e-4f || std::abs((*camera.projection)[i]-image.projection[i]) >= 1e-4f) { return false; }
    }
    image.render_id = camera.vr_render_token;
    image.eye = camera.vr_eye;
    image.runtime_head = camera.runtime_head; image.tracking_epoch = camera.tracking_epoch; image.pose_captured = camera.pose_captured;
    image.runtime_eye_to_head = camera.runtime_eye_to_head;
    image.tracked_stereo = !camera.vr_synthetic && camera.tracking_epoch > 0 && vr_camera::rigid(camera.runtime_head);
    return true;
}
