#pragma once

#include "VrCameraPose.h"

namespace vr_projection {
// Signed slopes: x/z left/right, y/z bottom/top in the game's +Z-forward view space.
// Adapters convert OpenXR angles with tan; OpenVR vertical raw bounds need their Y sign flipped.
struct Tangents {
    float left{}, right{}, bottom{}, top{};
    bool valid() const {
        return std::isfinite(left) && std::isfinite(right) && std::isfinite(bottom) && std::isfinite(top) &&
            left < 0 && right > 0 && bottom < 0 && top > 0 && right-left > 1e-5f && top-bottom > 1e-5f;
    }
};

// Row vectors, D3D [0,1] depth, reversed Z, w=z. Preserve the engine's finite near/far planes.
inline bool build(const Tangents& fov, float near_z, float far_z, vr_camera::Matrix& out)
{
    if (!fov.valid() || !std::isfinite(near_z) || !std::isfinite(far_z) || near_z <= 0 || far_z <= near_z) { return false; }
    vr_camera::Matrix result{};
    result[0] = 2 / (fov.right-fov.left);
    result[5] = 2 / (fov.top-fov.bottom);
    result[8] = -(fov.right+fov.left) / (fov.right-fov.left);
    result[9] = -(fov.top+fov.bottom) / (fov.top-fov.bottom);
    result[10] = near_z / (near_z-far_z);
    result[11] = 1;
    result[14] = -result[10] * far_z;
    for (float value : result) { if (!std::isfinite(value)) { return false; } }
    const float rectangle[]{near_z*fov.left, near_z*fov.bottom, near_z*(fov.right-fov.left), near_z*(fov.top-fov.bottom)};
    for (float value : rectangle) { if (!std::isfinite(value)) { return false; } }
    if (!std::isfinite((fov.top-fov.bottom)/(fov.right-fov.left))) { return false; }
    if (result[0] <= 0 || result[5] <= 0 || result[14] <= 0) { return false; }
    out = result;
    return true;
}
} // namespace vr_projection
