#pragma once
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>

struct VrCalibration {
    struct Parameter { float initial, minimum, maximum; };
    static constexpr Parameter SCALE{1,0.01f,100}; // Game units per physical metre.
    static constexpr Parameter HEIGHT{0,-1,1}; // Metres relative to the game's camera height.
    static constexpr Parameter FORWARD{0,-0.5f,0.5f}; // Metres along level tracked-head forward; negative retracts.
    // Added to FORWARD only while sprinting (docs/SPRINT_CAMERA.md); 0 disables.
    static constexpr Parameter SPRINT_FORWARD{0.20f,0,0.4f}; // Joe, 21 Oct: 0.20 m (walking clearance).
    static constexpr Parameter SPRINT_BLEND{0.25f,0.05f,1.5f}; // Seconds for the sprint offset to ramp fully in or out.
    // Joe's standing eye height above the floor, metres (docs/BODY_SCALE.md). A player measured at
    // recenter gets the first-person scale multiplied by this over their own; absent: no body scale.
    static constexpr Parameter REFERENCE_EYE_HEIGHT{0,1.0f,2.1f};
    // Game units per physical metre of the first-person eyes and hands (docs/BODY_SCALE.md). Marine
    // eye height over the player's puts the real floor on the game's ground; absent: units_per_metre.
    // units_per_metre still measures the Marine: the offsets above keep their game-unit place.
    static constexpr Parameter FIRST_PERSON_SCALE{0,0.5f,3};
    float units_per_metre{SCALE.initial}, height_offset_metres{HEIGHT.initial},head_forward_offset_metres{FORWARD.initial};
    float sprint_forward_offset_metres{SPRINT_FORWARD.initial},sprint_blend_seconds{SPRINT_BLEND.initial};
    std::optional<float> reference_eye_height_metres;
    std::optional<float> first_person_units_per_metre;
    // How many times the first-person eyes' scale exceeds the Marine's.
    float first_person_factor() const { return first_person_units_per_metre ? *first_person_units_per_metre/units_per_metre : 1; }
    bool operator==(const VrCalibration&) const = default;
    // The fields that define the head reference. Changing only the sprint fields
    // must not recenter: they are blended live.
    bool same_placement(const VrCalibration& other) const {
        return units_per_metre==other.units_per_metre && height_offset_metres==other.height_offset_metres && head_forward_offset_metres==other.head_forward_offset_metres &&
            reference_eye_height_metres==other.reference_eye_height_metres && first_person_units_per_metre==other.first_person_units_per_metre;
    }
    bool valid() const {
        return std::isfinite(units_per_metre) && units_per_metre >= SCALE.minimum && units_per_metre <= SCALE.maximum &&
            std::isfinite(height_offset_metres) && height_offset_metres >= HEIGHT.minimum && height_offset_metres <= HEIGHT.maximum &&
            std::isfinite(head_forward_offset_metres) && head_forward_offset_metres >= FORWARD.minimum && head_forward_offset_metres <= FORWARD.maximum &&
            std::isfinite(sprint_forward_offset_metres) && sprint_forward_offset_metres >= SPRINT_FORWARD.minimum && sprint_forward_offset_metres <= SPRINT_FORWARD.maximum &&
            std::isfinite(sprint_blend_seconds) && sprint_blend_seconds >= SPRINT_BLEND.minimum && sprint_blend_seconds <= SPRINT_BLEND.maximum &&
            (!reference_eye_height_metres || (std::isfinite(*reference_eye_height_metres) &&
                *reference_eye_height_metres >= REFERENCE_EYE_HEIGHT.minimum && *reference_eye_height_metres <= REFERENCE_EYE_HEIGHT.maximum)) &&
            (!first_person_units_per_metre || (std::isfinite(*first_person_units_per_metre) &&
                *first_person_units_per_metre >= FIRST_PERSON_SCALE.minimum && *first_person_units_per_metre <= FIRST_PERSON_SCALE.maximum));
    }
    // Require scale/height; forward, the sprint fields, the reference eye height and the first-person scale are optional for compatibility
    // with older presets (an absent field keeps its default).
    // Unknown, duplicate, invalid or incomplete required fields reject the entire update.
    static std::optional<VrCalibration> read(std::istream& input) {
        VrCalibration value;
        bool scale{}, height{},forward{},sprint{},blend{};
        std::string line;
        while (std::getline(input,line)) {
            const auto first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos || line[first] == '#') { continue; }
            const auto split = line.find('=');
            if (split == std::string::npos) { return std::nullopt; }
            std::istringstream number{line.substr(split+1)};
            float parsed{};
            if (!(number >> parsed) || !(number >> std::ws).eof()) { return std::nullopt; }
            const auto name = line.substr(0,split);
            if (name == "units_per_metre" && !scale) { value.units_per_metre = parsed; scale = true; }
            else if (name == "height_offset_metres" && !height) { value.height_offset_metres = parsed; height = true; }
            else if (name == "head_forward_offset_metres" && !forward) { value.head_forward_offset_metres = parsed; forward = true; }
            else if (name == "sprint_forward_offset_metres" && !sprint) { value.sprint_forward_offset_metres = parsed; sprint = true; }
            else if (name == "sprint_blend_seconds" && !blend) { value.sprint_blend_seconds = parsed; blend = true; }
            else if (name == "reference_eye_height_metres" && !value.reference_eye_height_metres) { value.reference_eye_height_metres = parsed; }
            else if (name == "first_person_units_per_metre" && !value.first_person_units_per_metre) { value.first_person_units_per_metre = parsed; }
            else { return std::nullopt; }
        }
        return input.eof() && scale && height && value.valid() ? std::optional{value} : std::nullopt;
    }
    bool write(std::ostream& output) const {
        if (!valid()) { return false; }
        output << std::setprecision(9) << "units_per_metre=" << units_per_metre
            << "\nheight_offset_metres=" << height_offset_metres << "\nhead_forward_offset_metres=" << head_forward_offset_metres
            << "\nsprint_forward_offset_metres=" << sprint_forward_offset_metres << "\nsprint_blend_seconds=" << sprint_blend_seconds << '\n';
        if (reference_eye_height_metres) { output << "reference_eye_height_metres=" << *reference_eye_height_metres << '\n'; }
        if (first_person_units_per_metre) { output << "first_person_units_per_metre=" << *first_person_units_per_metre << '\n'; }
        return bool(output);
    }
    static std::optional<VrCalibration> load(const std::filesystem::path& path) {
        std::ifstream input{path};
        return read(input);
    }
    bool save(const std::filesystem::path& path) const {
        if (!valid()) { return false; }
        std::ofstream output{path,std::ios::trunc};
        if (!write(output)) { return false; }
        output.flush();
        return bool(output);
    }
};
