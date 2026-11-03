// Experimental extension of re4_tweaks, not an official upstream feature.
#include "dllmain.h"
#include "Patches.h"
#include "input.hpp"
#include "ConsoleWnd.h"
#include "ModernControls.h"
#include "MovementCore.h"
#include "CameraContinuity.h"
#include "NativeDiagnostics.h"
#include <array>
#include <cstdlib>
#include <cstring>

extern bool bImGuiUIFocus;

namespace {
using Routine = void(__cdecl*)(cPlayer*);
std::array<Routine, 7> originals{};
std::uint64_t* keyOn{};
std::uint64_t* keyTrigger{};
bool installed{};
bool enabled{};
bool diagnosticEnabled{};
float aimSpeed = 35.0f;
re4mc::FreeCameraFrame cameraFrame;
std::array<unsigned, 5> keyCodes{'W', 'S', 'A', 'D', VK_SHIFT};
// Gamepad: walk while aiming with an analog stick (the rest stays native).
bool controllerEnabled = true;
float controllerDeadZone = 0.25f;
bool controllerInvertY{};
bool controllerRightStick{};
bool controllerAimRightStick = true;
bool controllerAimDirectionBits = true;

// The game aims with the left stick on a gamepad. While the mod moves the
// character with that stick, the native aim routine is fed the RIGHT stick
// instead, then every value is restored. Scales are learned from the game's
// own left-stick values, so no unknown range is assumed.
struct StickScale {
    float ratio{};
    bool known{};
    void learn(float axis, int value) {
        if (std::fabs(axis) < 0.5f || value == 0) return;
        ratio = float(value) / axis;
        known = std::isfinite(ratio);
    }
    int map(float axis) const { return known ? int(std::lround(axis * ratio)) : 0; }
};
StickScale joyX, joyY, pcX, pcY;

class ScopedPadAim {
    JOY* joy_;
    std::uint64_t* on_;
    std::uint64_t* trigger_;
    int saved_[4]{};
    float savedF_[2]{};
    std::uint64_t savedOn_{}, savedTrigger_{};
public:
    ScopedPadAim(JOY* joy, std::uint64_t* on, std::uint64_t* trigger)
        : joy_(joy), on_(on), trigger_(trigger) {
        if (!fAnalogLX || !fAnalogLY || !fAnalogRX || !fAnalogRY) return;
        const float rx = *fAnalogRX, ry = *fAnalogRY;
        savedF_[0] = *fAnalogLX; savedF_[1] = *fAnalogLY;
        if (joy_) {
            saved_[0] = joy_->leftStick_X_0; saved_[1] = joy_->leftStick_Y_4;
            saved_[2] = joy_->pc_leftStick_X_118; saved_[3] = joy_->pc_leftStick_Y_11C;
            joyX.learn(savedF_[0], saved_[0]); joyY.learn(savedF_[1], saved_[1]);
            pcX.learn(savedF_[0], saved_[2]); pcY.learn(savedF_[1], saved_[3]);
            joy_->leftStick_X_0 = joyX.map(rx); joy_->leftStick_Y_4 = joyY.map(ry);
            joy_->pc_leftStick_X_118 = pcX.map(rx); joy_->pc_leftStick_Y_11C = pcY.map(ry);
        }
        *fAnalogLX = rx; *fAnalogLY = ry;
        if (on_ && trigger_) {
            constexpr std::uint64_t dirs = 0xF; // FORWARD, BACK, RIGHT, LEFT
            savedOn_ = *on_; savedTrigger_ = *trigger_;
            *on_ &= ~dirs; *trigger_ &= ~dirs;
            if (controllerAimDirectionBits) {
                const float fy = controllerInvertY ? -ry : ry;
                if (fy > 0.5f) *on_ |= std::uint64_t(KEY_BTN::KEY_FORWARD);
                if (fy < -0.5f) *on_ |= std::uint64_t(KEY_BTN::KEY_BACK);
                if (rx > 0.5f) *on_ |= std::uint64_t(KEY_BTN::KEY_RIGHT);
                if (rx < -0.5f) *on_ |= std::uint64_t(KEY_BTN::KEY_LEFT);
            }
        }
        static int logged{};
        if (logged++ < 3)
            spd::log()->info("ModernControls -> pad aim swap: fLX={} fLY={} joy=({},{}) pc=({},{}) fRX={} fRY={}",
                savedF_[0], savedF_[1], saved_[0], saved_[1], saved_[2], saved_[3], rx, ry);
        active_ = true;
    }
    ~ScopedPadAim() {
        if (!active_) return;
        *fAnalogLX = savedF_[0]; *fAnalogLY = savedF_[1];
        if (joy_) {
            joy_->leftStick_X_0 = saved_[0]; joy_->leftStick_Y_4 = saved_[1];
            joy_->pc_leftStick_X_118 = saved_[2]; joy_->pc_leftStick_Y_11C = saved_[3];
        }
        if (on_ && trigger_) {
            constexpr std::uint64_t dirs = 0xF;
            *on_ = (*on_ & ~dirs) | (savedOn_ & dirs);
            *trigger_ = (*trigger_ & ~dirs) | (savedTrigger_ & dirs);
        }
    }
    ScopedPadAim(const ScopedPadAim&) = delete;
    ScopedPadAim& operator=(const ScopedPadAim&) = delete;
private:
    bool active_{};
};

bool executableAddress(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                          PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Validate the *whole* dispatch-table candidate before changing a single byte.
// Walk and Run are independently resolved, using existing upstream signatures.
Routine* findTable(Routine walk, Routine run) {
    const auto base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto sections = IMAGE_FIRST_SECTION(nt);
    Routine* found = nullptr;
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        const auto& section = sections[s];
        if (!(section.Characteristics & IMAGE_SCN_MEM_READ) ||
             (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const auto start = base + section.VirtualAddress;
        const auto length = section.Misc.VirtualSize;
        for (std::size_t i = 0; i + 7 * sizeof(Routine) <= length; i += sizeof(Routine)) {
            auto candidate = reinterpret_cast<Routine*>(start + i);
            if (candidate[1] != walk || candidate[3] != run) continue;
            bool valid = true;
            for (int j = 0; j < 7; ++j) {
                if (!executableAddress(reinterpret_cast<const void*>(candidate[j]))) valid = false;
                for (int k = 0; k < j; ++k)
                    if (candidate[k] == candidate[j]) valid = false;
            }
            if (!valid) continue;
            if (found) return nullptr; // Ambiguity must disable this extension.
            found = candidate;
        }
    }
    return found;
}

re4mc::Context context(cPlayer* player) {
    re4mc::Context c;
    c.enabled = enabled && installed;
    c.focused = hWindow && GetForegroundWindow() == hWindow;
    c.keyboardMouse = isKeyboardMouse();
    c.controller = controllerEnabled && !c.keyboardMouse && isController();
    c.blocked =bCfgMenuOpen || bConsoleOpen || bImGuiUIFocus || OptionOpenFlag();
    auto g = GlobalPtr();
    if (!g || !player) { c.mainLoop = false; return c; }
    if (player != PlayerPtr()) { c.playerObject = false; return c; }
    c.mainLoop = g->Rno0_20 == 3;
    c.blocked |= (g->flags_STOP_0_170[0] & 0x92000000U) != 0; // KEY, PL, CTRL
    c.blocked |= SubScreenWk && SubScreenWk->open_flag_2C != SS_OPEN_NULL;
    c.playerObject = player == PlayerPtr();
    c.character = static_cast<re4mc::Character>(g->pl_type_4FC8);
    c.specialAction = FlagIsSet(g->flags_STATUS_0_501C, unsigned(Flags_STATUS::STA_KLAUSER_TRANSFORM)) ||
                      FlagIsSet(g->flags_STATUS_0_501C, unsigned(Flags_STATUS::STA_PL_EM_ACTION)) ||
                      FlagIsSet(g->flags_STATUS_0_501C, unsigned(Flags_STATUS::STA_EVENT));
    c.alive = g->playerHpCur_4FB4 > 0;
    c.routine0 = player->r_no_0_FC;
    c.routine1 = player->r_no_1_FD;
    c.routine2 = player->r_no_2_FE;
    c.weapon = g->weapon_no_4FC0;
    c.collisionEnabled = (player->atari_2B4.m_flag_1A & (SAT_SCA_ENABLE | SAT_OBA_ENABLE))
                      == (SAT_SCA_ENABLE | SAT_OBA_ENABLE);
    c.scoped = SubScreenWk && (SubScreenWk->scope_flag_1B0 || SubScreenWk->binocular_flag_1B1);
    c.aiming = keyOn && (*keyOn & std::uint64_t(KEY_BTN::KEY_KAMAE));
    // A held knife retains the original routine, including its transitions.
    c.blocked |= keyOn && (*keyOn & std::uint64_t(KEY_BTN::KEY_KAMAE_KNIFE));
    return c;
}

re4mc::Keys readKeys(re4mc::Context& c) {
    if (!pInput) { c.blocked = true; return {}; }
    const auto lock = pInput->lock();
    c.blocked |= pInput->is_blocking_keyboard_input() || pInput->is_blocking_mouse_input();
    return {pInput->is_key_down(keyCodes[0]), pInput->is_key_down(keyCodes[1]),
            pInput->is_key_down(keyCodes[2]), pInput->is_key_down(keyCodes[3]),
            pInput->is_key_down(keyCodes[4])};
}

// Keyboard: WASD. Gamepad: the analog stick chosen in the INI (left by default).
re4mc::Direction readDirection(const re4mc::Context& c, const re4mc::Keys& keys) {
    if (c.keyboardMouse) return re4mc::direction(keys);
    if (!c.controller) return {};
    const float* x = controllerRightStick ? fAnalogRX : fAnalogLX;
    const float* y = controllerRightStick ? fAnalogRY : fAnalogLY;
    if (!x || !y) return {};
    return re4mc::analogDirection(*x, controllerInvertY ? -*y : *y, controllerDeadZone);
}

void setRoutine(cPlayer* player, unsigned routine) {
    if (player->r_no_1_FD == routine) return;
    player->r_no_1_FD = std::uint8_t(routine);
    player->r_no_2_FE = 0;
    player->r_no_3_FF = 0;
}

bool shoulderCameraAvailable() {
    const auto g = GlobalPtr();
    return g && CamCtrl && (CamCtrl->be_flag_28 & 1) && CamCtrl->r0_34 == 0xA &&
           !CamCtrl->m_pExtraCamera_250 && !(CamCtrl->m_state_flag_30 & 4) &&
           !(g->flags_STOP_0_170[0] & 0x40000000U); // SPF_CAMERA
}

std::uint32_t cameraSceneKey() {
    const auto g = GlobalPtr();
    // Character changes can reuse both player and camera addresses.
    return std::uint32_t(g->curRoomId_4FAC) | (std::uint32_t(g->pl_type_4FC8) << 16);
}

bool nativeHandoffAllowed(const re4mc::Context& c) {
    return c.enabled && c.keyboardMouse && c.mainLoop &&
           c.alive && c.playerObject && c.collisionEnabled && !c.scoped && !c.specialAction &&
           re4mc::supportedCharacter(c.character) &&
           c.routine0 == 0 && c.routine1 <= 6 && shoulderCameraAvailable();
}

void releaseCamera(cPlayer* player, const re4mc::Context& c) {
    if (cameraFrame.belongsTo(reinterpret_cast<std::uintptr_t>(player),
                              reinterpret_cast<std::uintptr_t>(CamCtrl)) &&
        nativeHandoffAllowed(c) && cameraFrame.sameRoom(cameraSceneKey()) &&
        fMousePosX && fCameraPosX && std::isfinite(*fMousePosX) &&
        std::isfinite(player->ang_A0.y)) {
        // Free look uses a virtual player reference; native aiming uses Leon.
        // Face the saved view BEFORE the native raise-weapon routine zeros orbit.
        const bool aiming = c.character != re4mc::Character::Ashley &&
                            !c.blocked && c.focused && (c.aiming || c.routine1 == 6);
        const auto handoff = cameraFrame.handoff(player->ang_A0.y, *fMousePosX, aiming);
        if (aiming) {
            player->ang_A0.y = handoff.playerYaw;
            if (AnalogRX_8) *AnalogRX_8 = 0;
        }
        *fMousePosX = handoff.mouseX;
        *fCameraPosX = handoff.cameraYaw;
    }
    cameraFrame.reset();
}

template<unsigned entry> void __cdecl dispatch(cPlayer* player) {
    re4mc::diagnostics::mark(1, entry); // Entering context/input collection.
    auto c = context(player);
    const auto keys = readKeys(c); // Release the input lock before calling native code.
    const auto moveDirection = readDirection(c, keys);
    const auto decision = re4mc::decide(c, moveDirection, keys.run);
    // Gamepad aim: the left stick moves, so the game aims with the right stick.
    const bool padAim = c.controller && controllerAimRightStick && !controllerRightStick &&
                        re4mc::aimState(c);
    static std::array<unsigned, 5> observations{};
    const auto action = unsigned(decision.action);
    const bool report = observations[action]++ < 2;
    const auto invoke = [&](unsigned next) {
        if (report) re4mc::diagnostics::trace("before_native", entry, action, player,
            reinterpret_cast<const void*>(originals[next]));
        re4mc::diagnostics::mark(2, entry);
        if (padAim && next == entry) {
            ScopedPadAim swap(JoyPtr(), keyOn, keyTrigger);
            originals[next](player);
        } else {
            originals[next](player);
        }
        re4mc::diagnostics::mark(3, entry);
        if (report) re4mc::diagnostics::trace("after_native", entry, action, player,
            reinterpret_cast<const void*>(originals[next]));
    };
    if (decision.action == re4mc::Action::Native) {
        releaseCamera(player, c);
        invoke(entry); return;
    }
    if (decision.action == re4mc::Action::AimMove) {
        releaseCamera(player, c);
        // Keep native aim, fire cadence, ammunition, reload and hit detection.
        invoke(entry);
        // A shot/interaction can exit the weapon state. Never drag another action.
        const auto after = context(player);
        if (re4mc::decide(after, moveDirection, keys.run).action != re4mc::Action::AimMove) return;
        const auto& camera = GlobalPtr()->Camera_74;
        const auto step = re4mc::aimDisplacement(decision.direction,
            {-camera.Look_D0.x, -camera.Look_D0.z},
            {camera.Right_DC.x, camera.Right_DC.z}, aimSpeed, GlobalPtr()->deltaTime_70);
        // Called from the movement dispatch, BEFORE cPlayer's native collision
        // and floor resolution. Never update from EndScene or disable collision.
        player->pos_94.x += step.x;
        player->pos_94.z += step.z;
        return;
    }
    if (!fMousePosX || !fCameraPosX || !std::isfinite(*fMousePosX) ||
        !shoulderCameraAvailable() || c.scoped) {
        releaseCamera(player, c);
        invoke(entry); return;
    }
    const auto g = GlobalPtr();
    if (!cameraFrame.current(reinterpret_cast<std::uintptr_t>(player),
                             reinterpret_cast<std::uintptr_t>(CamCtrl), cameraSceneKey(),
                             g->frameCounter_530C)) releaseCamera(player, c);
    if (!cameraFrame.begin(reinterpret_cast<std::uintptr_t>(player),
                           reinterpret_cast<std::uintptr_t>(CamCtrl), cameraSceneKey(),
                           g->frameCounter_530C, player->ang_A0.y)) {
        invoke(entry); return;
    }
    re4mc::diagnostics::mark(4, entry); // Input override and orbit update.
    re4mc::ScopedInput input(*keyOn, *keyTrigger, decision.action);
    unsigned next = 0;
    if (decision.action != re4mc::Action::Idle) {
        player->ang_A0.y = cameraFrame.movementYaw(decision.direction, *fMousePosX);
        next = decision.action == re4mc::Action::Run ? 3 : 1;
    }
    setRoutine(player, next);
    invoke(next); // Original walking/running animation and root motion.
    const auto afterContext = context(player);
    const auto after = re4mc::decide(afterContext, keys).action;
    if ((after == re4mc::Action::Walk || after == re4mc::Action::Run || after == re4mc::Action::Idle) &&
        shoulderCameraAvailable()) return;
    releaseCamera(player, afterContext);
}

void readConfig() {
    const auto path = rootPath + L"re4-modern-controls.ini";
    enabled = GetPrivateProfileIntW(L"CONTROLS", L"Enabled", 1, path.c_str()) != 0;
    diagnosticEnabled = GetPrivateProfileIntW(L"CONTROLS", L"Diagnostics", 0, path.c_str()) != 0;
    wchar_t number[80]{};
    GetPrivateProfileStringW(L"CONTROLS", L"AimMoveUnitsPerTick", L"35", number, 80, path.c_str());
    wchar_t* end{};
    const auto speed = std::wcstof(number, &end);
    if (end != number && *end == 0 && std::isfinite(speed)) aimSpeed = std::clamp(speed, 0.0f, 80.0f);
    const wchar_t* names[]{L"Forward", L"Back", L"Left", L"Right", L"Run"};
    for (unsigned i = 0; i < keyCodes.size(); ++i) {
        const auto key = GetPrivateProfileIntW(L"KEYS", names[i], keyCodes[i], path.c_str());
        if (key > 0 && key < 256) keyCodes[i] = key;
    }
    controllerEnabled = GetPrivateProfileIntW(L"CONTROLLER", L"Enabled", 1, path.c_str()) != 0;
    controllerInvertY = GetPrivateProfileIntW(L"CONTROLLER", L"InvertY", 0, path.c_str()) != 0;
    controllerRightStick = GetPrivateProfileIntW(L"CONTROLLER", L"MoveStick", 0, path.c_str()) == 1;
    controllerAimRightStick = GetPrivateProfileIntW(L"CONTROLLER", L"AimRightStick", 1, path.c_str()) != 0;
    controllerAimDirectionBits = GetPrivateProfileIntW(L"CONTROLLER", L"AimDirectionBits", 1, path.c_str()) != 0;
    GetPrivateProfileStringW(L"CONTROLLER", L"DeadZone", L"0.25", number, 80, path.c_str());
    const auto dead = std::wcstof(number, &end);
    if (end != number && *end == 0 && std::isfinite(dead)) controllerDeadZone = std::clamp(dead, 0.0f, 0.9f);
}
} // namespace

bool ModernControls_OwnsLocomotion() {
    auto player = PlayerPtr();
    if (!player) return false;
    auto c = context(player);
    const auto keys = readKeys(c);
    const auto action = re4mc::decide(c, keys).action;
    return shoulderCameraAvailable() && !c.scoped &&
           (action == re4mc::Action::Walk || action == re4mc::Action::Run || action == re4mc::Action::Idle);
}

void ModernControls_UpdateCamera(CameraQuasiFPS* camera) {
    // Called by the existing camera-type hook, immediately before the engine
    // computes its shoulder basis, offsets, wall collision and interpolation.
    auto player = PlayerPtr();
    const auto g = GlobalPtr();
    if (!player || !g || !CamCtrl || camera != &CamCtrl->m_QuasiFPS_278) return;
    auto c = context(player);
    const auto keys = readKeys(c);
    const auto action = re4mc::decide(c, keys).action;
    if (!shoulderCameraAvailable() || c.scoped ||
        (action != re4mc::Action::Walk && action != re4mc::Action::Run && action != re4mc::Action::Idle) ||
        !cameraFrame.current(reinterpret_cast<std::uintptr_t>(player),
                             reinterpret_cast<std::uintptr_t>(CamCtrl), cameraSceneKey(),
                             g->frameCounter_530C)) {
        releaseCamera(player, c);
        return;
    }
    if (!re4mc::queueCameraBasis(*camera, cameraFrame.basisYaw())) {
        // A script queued its own direction. Do not rewrite its orbit or history.
        cameraFrame.reset();
    }
}

bool ModernControls_UseAshleyShoulderCamera() {
    const auto g = GlobalPtr();
    return enabled && installed && isKeyboardMouse() && g &&
           g->pl_type_4FC8 == PlayerCharacter::Ashley;
}

void re4t::init::ModernControls() {
    static_assert(sizeof(void*) == 4, "RE4 Steam UHD is a 32-bit game");
    static_assert(unsigned(PlayerCharacter::Leon) == unsigned(re4mc::Character::Leon) &&
                  unsigned(PlayerCharacter::Ashley) == unsigned(re4mc::Character::Ashley) &&
                  unsigned(PlayerCharacter::Ada) == unsigned(re4mc::Character::Ada) &&
                  unsigned(PlayerCharacter::HUNK) == unsigned(re4mc::Character::Hunk) &&
                  unsigned(PlayerCharacter::Krauser) == unsigned(re4mc::Character::Krauser) &&
                  unsigned(PlayerCharacter::Wesker) == unsigned(re4mc::Character::Wesker),
                  "Character policy must match the PC SDK");
    readConfig();
    if (diagnosticEnabled) {
        const auto diagnosticPath = rootPath + L"re4-modern-controls-diagnostico.log";
        re4mc::diagnostics::initialize(diagnosticPath.c_str(), g_module_handle);
    }
    if (!enabled) return;
    const auto version = GameVersion();
    if (version != "1.1.0" && version != "1.0.6") {
        spd::log()->warn("ModernControls -> Unsupported regional/debug build {}; disabled", version);
        enabled = false; return;
    }
    // Resolve BEFORE the upstream inline hooks modify either function prologue.
    auto walk = hook::pattern("55 8B EC 83 EC ? 53 56 8B 75 ? 33 DB 38 9E");
    auto run = hook::pattern("55 8B EC 83 EC ? 53 56 8B 75 ? 0F B6 86 ? ? ? ? 33 DB");
    auto on = hook::pattern("A1 ? ? ? ? 33 C9 83 E0 ? 83 3D");
    auto trigger = hook::pattern("A1 ? ? ? ? 25 ? ? ? ? 0B C1 74 ? 88 0D");
    if (walk.size() != 1 || run.size() != 1 || on.size() != 1 || trigger.size() != 1) {
        spd::log()->error("ModernControls -> Signature mismatch; no dispatch hooks installed");
        enabled = false; return;
    }
    auto table = findTable(reinterpret_cast<Routine>(walk.get(0).get<void>(0)),
                           reinterpret_cast<Routine>(run.get(0).get<void>(0)));
    if (!table) {
        spd::log()->error("ModernControls -> Missing or ambiguous movement table; disabled");
        enabled = false; return;
    }
    keyOn = *on.get(0).get<std::uint64_t*>(1);
    keyTrigger = *trigger.get(0).get<std::uint64_t*>(1);
    std::memcpy(originals.data(), table, sizeof(originals));
    const std::array<Routine, 7> replacements{
        dispatch<0>, dispatch<1>, dispatch<2>, dispatch<3>, dispatch<4>, dispatch<5>, dispatch<6>};
    DWORD protection{};
    if (!VirtualProtect(table, sizeof(replacements), PAGE_READWRITE, &protection)) {
        spd::log()->error("ModernControls -> Could not write movement table; disabled");
        enabled = false; return;
    }
    std::memcpy(table, replacements.data(), sizeof(replacements));
    DWORD ignored{};
    VirtualProtect(table, sizeof(replacements), protection, &ignored);
    installed = true;
    re4t::cfg->bCameraImprovements = true;
    re4t::cfg->bUseMouseTurning = false;
    re4t::cfg->bResetCameraWhenRunning = false;
    re4t::cfg->bResetCameraAfterUsingWeapons = false;
    re4t::cfg->bResetCameraAfterUsingKnife = false;
    re4t::cfg->bUseSprintToggle = false;
    spd::log()->info("ModernControls -> EXPERIMENTAL 0.1.4 + CONTROLE, KB/M enabled; gamepad aim-walk={} (stick={}, deadzone={}, invertY={}, aimRight={}, aimBits={}); Leon/Ashley/Ada/HUNK/Krauser/Wesker; independent camera reference; diagnostics={}; native Walk/Run prologues preserved; game validation pending",
        controllerEnabled, controllerRightStick ? "right" : "left", controllerDeadZone, controllerInvertY, controllerAimRightStick, controllerAimDirectionBits, diagnosticEnabled);
}
