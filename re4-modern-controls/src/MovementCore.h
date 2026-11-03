// Original keyboard/mouse controller. No game code or assets are included.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace re4mc {
constexpr float pi = 3.14159265358979323846f;
// calcDepressionRatio: ANGLE_LEFT/RIGHT_LIMIT * 127 / C_RANGE.
// CameraImprovements uses C_RANGE=42.3, not an exact factor of three.
constexpr float cameraOrbitScale = (pi / 3.0f) * (127.0f / 42.3f);
struct Keys { bool forward{}, back{}, left{}, right{}, run{}; };
struct Direction { float right{}, forward{}; bool moving{}; };
struct XZ { float x{}, z{}; };
enum class Character : std::uint8_t { Leon, Ashley, Ada, Hunk, Krauser, Wesker, Composite };
inline bool supportedCharacter(Character character) {
    switch (character) {
    case Character::Leon: case Character::Ashley: case Character::Ada:
    case Character::Hunk: case Character::Krauser: case Character::Wesker: return true;
    default: return false;
    }
}
inline bool thrownWeapon(std::uint8_t weapon) {
    switch (weapon) {
    case 0x13: case 0x16: case 0x17: case 0x19: case 0x1E:
    case 0x1F: case 0x20: case 0x29: case 0x2A: return true;
    default: return false;
    }
}
struct Context {
    bool enabled{true}, focused{true}, keyboardMouse{true}, blocked{};
    bool mainLoop{true}, alive{true}, playerObject{true};
    bool collisionEnabled{true}, scoped{}, aiming{};
    bool specialAction{};
    // Gamepad: only walking while aiming is handled; the rest stays native.
    bool controller{};
    Character character{Character::Leon};
    std::uint8_t routine0{}, routine1{}, routine2{}, weapon{2};
};
enum class Action { Native, Idle, Walk, Run, AimMove };
struct Decision { Action action{Action::Native}; Direction direction{}; };

inline float wrap(float value) {
    if (!std::isfinite(value)) return 0;
    return std::remainder(value, 2 * pi);
}
inline Direction direction(Keys keys) {
    float x = float(keys.right) - float(keys.left);
    float z = float(keys.forward) - float(keys.back);
    const float length = std::hypot(x, z);
    if (length == 0) return {};
    return {x / length, z / length, true};
}
// Analog stick (-1..1 per axis) with a radial dead zone. Partial tilt gives a
// proportionally slower step; full tilt is never faster than a keyboard key.
inline Direction analogDirection(float x, float y, float deadZone) {
    if (!std::isfinite(x) || !std::isfinite(y)) return {};
    deadZone = std::clamp(std::isfinite(deadZone) ? deadZone : 0.25f, 0.0f, 0.9f);
    const float length = std::hypot(x, y);
    if (length <= deadZone || length == 0) return {};
    const float scale = std::min((length - deadZone) / (1.0f - deadZone), 1.0f);
    return {x / length * scale, y / length * scale, true};
}
inline Decision decide(const Context& c, Direction d, bool run) {
    if (!c.enabled || !c.focused || (!c.keyboardMouse && !c.controller) || c.blocked ||
        !c.mainLoop || !c.alive || !c.playerObject || !c.collisionEnabled ||
        !supportedCharacter(c.character) || c.specialAction || c.scoped ||
        c.routine0 != 0 || c.routine1 > 6) return {};
    if (c.routine1 == 6) {
        // Ashley has no weapon routine. Draw/holster/reload, scripted actions
        // and item throws keep their native positioning and animation timing.
        const bool aimState = c.routine2 == 1 || (c.routine2 == 2 && !thrownWeapon(c.weapon));
        if (c.character != Character::Ashley && c.aiming && aimState && d.moving)
            return {Action::AimMove, d};
        return {};
    }
    // Gamepad outside the aim: the game's own movement and camera.
    if (!c.keyboardMouse) return {};
    // Let the original game enter the aiming state and handle its animations.
    if (c.aiming) return {};
    if (!d.moving) return {Action::Idle, d};
    return {run ? Action::Run : Action::Walk, d};
}
inline Decision decide(const Context& c, Keys keys) {
    return decide(c, direction(keys), keys.run);
}
// RE4's mouse orbit is relative to the player's yaw; positive yaw turns left.
inline float headingOffset(Direction d) {
    return d.moving ? -std::atan2(d.right, d.forward) : 0;
}
// Project and orthonormalize the camera on the ground plane. Pitch must not
// make movement slower, introduce vertical motion, or accelerate diagonals.
inline XZ aimDisplacement(Direction d, XZ forward, XZ right,
                          float unitsPerTick, float deltaTicks) {
    if (!d.moving || !std::isfinite(unitsPerTick) || !std::isfinite(deltaTicks) ||
        !std::isfinite(forward.x) || !std::isfinite(forward.z) ||
        !std::isfinite(right.x) || !std::isfinite(right.z)) return {};
    const float n = std::hypot(forward.x, forward.z);
    if (n < 0.0001f) return {};
    forward.x /= n; forward.z /= n;
    // Remove projection onto forward, preserving the engine's handedness.
    const float dot = right.x * forward.x + right.z * forward.z;
    right.x -= dot * forward.x; right.z -= dot * forward.z;
    const float rn = std::hypot(right.x, right.z);
    if (rn < 0.0001f) return {};
    right.x /= rn; right.z /= rn;
    const float step = std::clamp(unitsPerTick, 0.0f, 80.0f) *
                       std::clamp(deltaTicks, 0.0f, 1.0f);
    return {(forward.x * d.forward + right.x * d.right) * step,
            (forward.z * d.forward + right.z * d.right) * step};
}

// Replace only locomotion input during a native walking routine. Native aim,
// fire, reload, interaction and menu bits remain owned by the game.
constexpr std::uint64_t directionMask = 0xFULL;
constexpr std::uint64_t runMask = 0x40ULL;
constexpr std::uint64_t quickturnMask = 0x100ULL;
inline std::uint64_t locomotionInput(std::uint64_t native, Action action) {
    auto result = native & ~(directionMask | runMask | quickturnMask);
    if (action == Action::Walk || action == Action::Run) result |= 1;
    if (action == Action::Run) result |= runMask;
    return result;
}
class ScopedInput {
    std::uint64_t& on_;
    std::uint64_t& trigger_;
    const std::uint64_t savedOn_, savedTrigger_;
public:
    ScopedInput(std::uint64_t& on, std::uint64_t& trigger, Action action)
        : on_(on), trigger_(trigger), savedOn_(on), savedTrigger_(trigger) {
        on_ = locomotionInput(on_, action);
        trigger_ &= ~(directionMask | quickturnMask);
    }
    ~ScopedInput() {
        constexpr auto owned = directionMask | runMask | quickturnMask;
        constexpr auto ownedTrigger = directionMask | quickturnMask;
        // Keep any native consumption of fire/interaction/menu input intact.
        on_ = (on_ & ~owned) | (savedOn_ & owned);
        trigger_ = (trigger_ & ~ownedTrigger) | (savedTrigger_ & ownedTrigger);
    }
    ScopedInput(const ScopedInput&) = delete;
    ScopedInput& operator=(const ScopedInput&) = delete;
};
} // namespace re4mc
