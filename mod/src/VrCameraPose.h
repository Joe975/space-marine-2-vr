#pragma once

#include <array>
#include <cmath>
#include <optional>

namespace vr_camera {
// Row-vector storage. XR rows are right/up/back; game rows are right/up/forward.
// Translation is row 3. Runtime adapters must transpose column-vector API matrices.
using Matrix = std::array<float, 16>;
inline constexpr Matrix IDENTITY{ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

inline bool rigid(const Matrix& m, bool xr = true)
{
    for (float v : m) { if (!std::isfinite(v)) { return false; } }
    if (std::abs(m[3]) > 1e-4f || std::abs(m[7]) > 1e-4f || std::abs(m[11]) > 1e-4f || std::abs(m[15] - 1) > 1e-4f) { return false; }
    for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) {
        float dot = 0;
        for (int j = 0; j < 3; ++j) { dot += m[a*4+j] * m[b*4+j]; }
        if (std::abs(dot - (a == b ? 1.0f : 0.0f)) > 1e-4f) { return false; }
    }
    const float det = m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]);
    return !xr || std::abs(det-1) < 1e-4f;
}

// A rotation scaled the same on all three axes, plus a translation: the player's entity world
// while some attack animations scale the marine (30 Oct 20:46, a Ravener attack: the axes
// shrank by about 1e-4 and rigid() refused the player's world for 8 s, so owned stereo stood
// down; docs/RAVENER_ATTACK_2026-10-30.md). Returns the scale and the matrix with it divided
// out (rigid by rigid()'s rule), or nothing for anything else: unequal axis lengths, axes not at
// right angles, a mirror, a projective column, or a scale outside [min_scale, max_scale].
struct Unscaled { Matrix matrix; float scale; };
inline std::optional<Unscaled> unscaled(const Matrix& m, float min_scale = 0.8f, float max_scale = 1.25f)
{
    for (float v : m) { if (!std::isfinite(v)) { return std::nullopt; } }
    if (std::abs(m[3]) > 1e-4f || std::abs(m[7]) > 1e-4f || std::abs(m[11]) > 1e-4f || std::abs(m[15] - 1) > 1e-4f) { return std::nullopt; }
    std::array<float, 3> length{};
    for (int a = 0; a < 3; ++a) { length[a] = std::sqrt(m[a*4]*m[a*4] + m[a*4+1]*m[a*4+1] + m[a*4+2]*m[a*4+2]); }
    const float scale = (length[0] + length[1] + length[2]) / 3;
    if (!(scale >= min_scale && scale <= max_scale)) { return std::nullopt; }
    for (float l : length) { if (std::abs(l - scale) > 1e-3f*scale) { return std::nullopt; } }
    Matrix out = m;
    for (int a = 0; a < 3; ++a) for (int j = 0; j < 3; ++j) { out[a*4+j] = m[a*4+j] / length[a]; }
    if (!rigid(out)) { return std::nullopt; } // Axes not at right angles, or a mirror (det -1).
    return Unscaled{out, scale};
}

// Caller validates rigidity before using this orthonormal inverse.
inline Matrix inverse(const Matrix& m)
{
    Matrix out = IDENTITY;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { out[i*4+j] = m[j*4+i]; }
    for (int j = 0; j < 3; ++j) { out[12+j] = -(m[12]*m[j*4] + m[13]*m[j*4+1] + m[14]*m[j*4+2]); }
    return out;
}

inline Matrix multiply(const Matrix& a, const Matrix& b)
{
    Matrix out{};
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) for (int k = 0; k < 4; ++k) { out[i*4+j] += a[i*4+k]*b[k*4+j]; }
    return out;
}

// Keep heading/position while using gravity up. A tilted head at recenter must not rotate the
// room's floor; the game camera's mouse pitch/animation roll must not tilt the VR reference.
// Preserve the input handedness (XR +det, the game's native camera commonly -det).
inline bool level_anchor(const Matrix& input,Matrix& out,bool xr = true)
{
    if (!rigid(input,xr)) { return false; }
    const float determinant = input[0]*(input[5]*input[10]-input[6]*input[9])-
        input[1]*(input[4]*input[10]-input[6]*input[8])+input[2]*(input[4]*input[9]-input[5]*input[8]);
    const float sign = determinant < 0 ? -1.0f : 1.0f;
    auto result = IDENTITY;
    const auto forward_length = std::hypot(input[8],input[10]);
    if (forward_length > 1e-5f) {
        result[8] = input[8]/forward_length; result[10] = input[10]/forward_length;
        result[0] = sign*result[10]; result[2] = -sign*result[8];
    } else {
        // Looking directly up/down: use projected right rather than an undefined yaw.
        const auto right_length = std::hypot(input[0],input[2]);
        if (right_length <= 1e-5f) { return false; }
        result[0] = input[0]/right_length; result[2] = input[2]/right_length;
        result[8] = -sign*result[2]; result[10] = sign*result[0];
    }
    result[12] = input[12]; result[13] = input[13]; result[14] = input[14];
    if (!rigid(result,xr)) { return false; }
    out = result; return true;
}

// Recenter at the reference head, not the room floor. Preserve eye-to-head translation/IPD.
inline bool relative_eye(const Matrix& reference_head, const Matrix& current_head, const Matrix& eye_to_head, Matrix& out)
{
    if (!rigid(reference_head) || !rigid(current_head) || !rigid(eye_to_head)) { return false; }
    const auto result = multiply(multiply(eye_to_head, current_head), inverse(reference_head));
    if (!rigid(result)) { return false; }
    out = result;
    return true;
}

// Map XR metres and -Z forward into the game's level heading basis. The game camera supplies
// eye height; adding absolute room height would count it twice. Commit only valid output.
inline bool compose(const Matrix& game, const Matrix& relative_eye, float units_per_metre, Matrix& world, Matrix& view, bool level = true)
{
    if (!rigid(game, false) || !rigid(relative_eye) || !std::isfinite(units_per_metre) || units_per_metre < 0.01f || units_per_metre > 100.0f) { return false; }
    Matrix anchor{};
    if (level) { if (!level_anchor(game,anchor,false)) { return false; } }
    else { anchor=game; }
    Matrix result = IDENTITY;
    for (int axis = 0; axis < 3; ++axis) for (int j = 0; j < 3; ++j) {
        const float direction = axis == 2 ? -1.0f : 1.0f;
        result[axis*4+j] = direction*(relative_eye[axis*4]*anchor[j] + relative_eye[axis*4+1]*anchor[4+j] - relative_eye[axis*4+2]*anchor[8+j]);
    }
    for (int j = 0; j < 3; ++j) {
        result[12+j] = anchor[12+j] + units_per_metre*(relative_eye[12]*anchor[j] + relative_eye[13]*anchor[4+j] - relative_eye[14]*anchor[8+j]);
    }
    if (!rigid(result, false)) { return false; }
    const auto inv = inverse(result);
    if (!rigid(inv, false)) { return false; }
    world = result;
    view = inv;
    return true;
}
} // namespace vr_camera
