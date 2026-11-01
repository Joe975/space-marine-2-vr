#pragma once
#include "VrCameraPose.h"
#include <cstdint>
#include <optional>

// Diagnostic history, including ineligible entries. Eligibility remains 0..250 ms.
struct PipPoseEvidence {
    vr_camera::Matrix world{};
    uint64_t render_id{},updates{};
    std::optional<uint32_t> token;
    double age_ms{};
    bool eligible{};
};
