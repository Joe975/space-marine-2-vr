#pragma once
#include "VrArmPose.h"
#include <array>
#include <optional>
#include <span>
#include <string>

// Left-handed play (docs/LEFT_HANDED.md). Three small swaps, so nothing else has to know:
//
// - Bindings: every OpenXR action reads the other hand (OpenXrInput.h bindings()). "fire" is then
//   the left trigger, and all of the mod's logic keeps its meaning: "right" reads "weapon hand",
//   "left" reads "support hand".
// - Gestures: VrSession hands them a mirrored player, head and both hands (logical()). Holsters,
//   the belt, the guard and the swing detector all measure a hand against the head or the other
//   hand, so a mirrored player meets the zones tuned for the right hand. The head has to mirror
//   too: mirroring the hands about the moving head alone would add twice the head's turn to a
//   still hand's speed (2 x 0.3 m x 3.5 rad/s is about 2 m/s at a 200 deg/s head turn), a swing
//   the player never made. Outputs that go back to the real world (the grenade launch, the
//   guard guide) mirror back.
// - Weapons: the arms stay on their own sides, the physical left hand on the Marine's left arm.
//   The gun (Swap::moved) and the sword socket (sword_to_left) move to the other wrist instead.
namespace vr_hand {
using Matrix=vr_arm::Matrix;

// Read at start. keep_sticks: left-handed, but walking stays on the left stick and turning on the
// right one, with their clicks (Support left hand dominant play asks for the two separately).
struct Settings {
    bool left_handed{},keep_sticks{};
    bool operator==(const Settings&) const=default;
};

// Matrices are row-vector (v'=v*M) with +Y up in standing space, so the mirror through the X=0
// plane is F*M*F, F=diag(-1,1,1,1). flip_local is F*M (X in M's own frame), flip_parent is M*F.
inline Matrix flip_local(Matrix m) { for(int i:{0,1,2}) m[i]=-m[i];return m; }
inline Matrix flip_parent(Matrix m) { for(int i:{0,4,8,12}) m[i]=-m[i];return m; }
// A rigid pose seen in a mirror, still rigid. Applied to a controller it also flips the
// controller's own X axis: the left and right raw device frames are mirror images about X
// (config/openxr_index_grip.calibration: measured 8789 pairs per hand, rotation about X equal, Y
// and Z negated), so the mirrored left controller is what a right controller would read.
inline Matrix mirror(const Matrix& m) { return flip_local(flip_parent(m)); }

// The tracked player the gestures see: index 0 the support hand, 1 the weapon hand.
struct Player { Matrix head{vr_camera::IDENTITY};std::array<std::optional<Matrix>,2> hands; };
inline Player logical(const Matrix& head,const std::array<std::optional<Matrix>,2>& controllers,bool left_handed) {
    if(!left_handed) return {head,controllers};
    Player out{mirror(head),{}};
    for(size_t hand=0;hand<2;++hand) if(controllers[1-hand]) out.hands[hand]=mirror(*controllers[1-hand]);
    return out;
}

// The grip each physical hand squeezes, left then right: the actions are logical (melee is the
// weapon hand's), and the viewer's rig pairs a grip with its physical hand.
inline std::array<float,2> physical_grips(float parry,float melee,bool left_handed) {
    return left_handed?std::array{melee,parry}:std::array{parry,melee};
}

// Weapons change wrists. A weapon rests in the hand as `relative` (weapon to wrist). The same hold
// in the other hand is its mirror image: mirrored through the controller frame, which the grips
// connect to each wrist (wrist=grip*controller), and through the weapon's own X. X is the weapon's
// sideways axis on all three kinds (Gun_pose on the bolter and the pistol, GUN_R with the sword:
// their X axes lie 0.96-0.99 along the controller's X, 31 Oct 20:29 bone dumps). The mesh itself is
// not flipped: a weapon's ejection port stays on the right. test_vr_handedness.cpp checks the
// result against the game's own left-hand hold.
struct Swap {
    std::array<Matrix,2> fit; // fit[hand]: from hand's wrist frame to the other wrist's frame. A reflection.
    // Grips in metres, as EngineArmDriver::grip gives them; wrist_world scales them the same way.
    static std::optional<Swap> make(Matrix left_grip,Matrix right_grip,float units_per_metre) {
        if(!std::isfinite(units_per_metre) || units_per_metre<.01f || units_per_metre>100 || !vr_camera::rigid(left_grip) || !vr_camera::rigid(right_grip)) return {};
        for(int axis=0;axis<3;++axis) {left_grip[12+axis]*=units_per_metre;right_grip[12+axis]*=units_per_metre;}
        return Swap{{vr_camera::multiply(flip_parent(left_grip),vr_camera::inverse(right_grip)),
                     vr_camera::multiply(flip_parent(right_grip),vr_camera::inverse(left_grip))}};
    }
    // relative rests on `hand`'s wrist in the game's pose: the same hold on the other wrist.
    Matrix moved(const Matrix& relative,size_t hand) const { return vr_camera::multiply(flip_local(relative),fit[hand]); }
};

// GUN_R is a child of HANDR and carries the melee weapon (equipment_preset_marine.sso: right_hand,
// locator GUN_R), so the sword follows the right arm. Writing the bone onto the left wrist moves
// the sword and everything that reads it, the game's hit sweep too (VrMeleeSweep.h). locals: after
// the arm solve. Nothing without one HANDL, one HANDR and one GUN_R under it.
struct Socket { size_t index{};Matrix local{}; };
inline std::optional<Socket> sword_to_left(std::span<const Matrix> locals,std::span<const int32_t> parents,std::span<const std::string> names,
    const Matrix& root,const Swap& swap) {
    const auto index=[&](const char* name)->std::optional<size_t> {
        const auto it=std::find(names.begin(),names.end(),name);
        if(it==names.end() || std::count(names.begin(),names.end(),name)!=1) return {};
        return static_cast<size_t>(it-names.begin());
    };
    const auto left=index("HANDL"),right=index("HANDR"),socket=index("GUN_R");
    if(!left || !right || !socket || names.size()!=parents.size() || parents[*socket]!=static_cast<int32_t>(*right)) return {};
    std::vector<Matrix> world;
    if(!vr_arm::hierarchy(locals,parents,root,world)) return {};
    const auto drawn=vr_camera::multiply(swap.moved(locals[*socket],1),world[*left]);
    const auto local=vr_camera::multiply(drawn,vr_camera::inverse(world[*right]));
    if(!vr_camera::rigid(drawn) || !vr_camera::rigid(local)) return {};
    return Socket{*socket,local};
}
} // namespace vr_hand
