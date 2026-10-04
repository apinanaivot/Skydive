#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include "animation.h"
#include "jc2mech/mechanics.h"
#include "menu.h"
#include "settings.h"
#include "skyrim_host.h"
#include "spells.h"

// SKSE entry: hosts jc2::Mechanics on the player. Core ticks at a fixed rate from the player's
// update (PlayerCharacter vfunc 0xAD); input arrives through the input event source.

namespace {

jc2::Mechanics g_mech;
jc2::FixedStepper g_stepper;
SkyrimHost g_host;

bool g_clipHook = true;
float g_fallTop = 0.0f;          // core y of the highest point of the current fall
float g_fallDamageMult = 1.0f;   // skydive landings

void LoadConfig() {
    const std::filesystem::path ini = std::filesystem::current_path() / "Data/SKSE/Plugins/Skydive.ini";
    const std::string path = ini.string();
    SkyrimHost::Config& c = g_host.config;
    char key[32] = {};
    GetPrivateProfileStringA("Input", "iGrappleKey", "", key, sizeof(key), path.c_str());
    if (key[0]) c.grappleKey = static_cast<std::uint32_t>(std::strtoul(key, nullptr, 0));  // decimal or 0x hex
    c.notify = GetPrivateProfileIntA("Debug", "bNotify", c.notify, path.c_str()) != 0;
    g_clipHook = GetPrivateProfileIntA("Debug", "bClipHook", 1, path.c_str()) != 0;
    // Skyrim's fall damage kills from lower drops than JC2's, so the skydive (no fall damage)
    // may start earlier than JC2's 0.6 s / 8 m.
    jc2::Tuning& t = g_mech.GetTuning();
    auto getFloat = [&](const char* section, const char* name, float& value) {
        char buf[32] = {};
        GetPrivateProfileStringA(section, name, "", buf, sizeof(buf), path.c_str());
        if (buf[0]) value = std::strtof(buf, nullptr);
    };
    t.skydiveDelay = 0.45f;
    t.skydiveMinHeight = 7.5f;
    getFloat("Skydive", "fDelay", t.skydiveDelay);
    getFloat("Skydive", "fMinHeight", t.skydiveMinHeight);
    getFloat("Skydive", "fLandingDamageMult", g_fallDamageMult);
    Settings::Get().Load(path);
    SKSE::log::info("config {}: grapple key {:#x}, notify {}, clip hook {}, skydive after {:.2f} s with {:.1f} m below", path,
                    c.grappleKey, c.notify, g_clipHook, t.skydiveDelay, t.skydiveMinHeight);
}

void Reset() {
    animation::ResetVisuals();
    animation::ResetGrapple();
    animation::ResetSounds();
    spells::Reset();
    g_mech.Reset();
    g_host.Release();
    g_stepper = {};
}

// Immersive stamina (Settings): before the frame's ticks, may refuse the grapple press, let go
// of a wall, or weaken the chute steering; drains for hanging and steering.
struct StaminaState {
    bool draining = false;
    float last = 0.0f;  // stamina after last frame's drain (regeneration is taken back)
};
StaminaState g_stamina;

void StaminaBeforeTicks(RE::PlayerCharacter* player, float delta) {
    const Settings& cfg = Settings::Get();
    RE::ActorValueOwner* av = player->AsActorValueOwner();
    const jc2::MoveState s = g_mech.State();
    const bool hang = cfg.stamina && (s == jc2::MoveState::WallCling || s == jc2::MoveState::CeilingHang);
    const jc2::InputState in = g_host.GetInput();
    const float steer = (std::max)(std::fabs(in.moveX), std::fabs(in.moveY));
    const bool chute = cfg.stamina && cfg.chuteStamina && s == jc2::MoveState::Parachuting;
    const float drain = (hang ? cfg.hangCost : 0.0f) + (chute ? cfg.chuteCost * steer : 0.0f);

    float now = av->GetActorValue(RE::ActorValue::kStamina);
    if (drain > 0.0f) {
        // No regeneration while hanging or steering: take back what came in since last frame.
        const float regen = g_stamina.draining ? (std::max)(0.0f, now - g_stamina.last) : 0.0f;
        const float take = (std::min)(now, regen + drain * delta);
        if (take > 0.0f) av->DamageActorValue(RE::ActorValue::kStamina, take);
        now -= take;
    }
    g_stamina.draining = drain > 0.0f;
    g_stamina.last = now;
    if (!cfg.stamina) return;
    const bool empty = now <= 0.0f;
    if (in.grapplePressed && now < cfg.grappleCost) {
        g_host.ClearGrapplePress();
        RE::HUDMenu::FlashMeter(RE::ActorValue::kStamina);
    }
    if (empty && hang) g_host.PressDrop();
    if (empty && chute) g_host.ScaleMove(cfg.exhaustedSteer);
}

void LogGrappleAim() {
    const jc2::CameraState cam = g_host.GetCamera();
    jc2::HitInfo hit;
    if (g_host.Raycast(cam.position, cam.forward, g_mech.GetTuning().grappleRange, hit)) {
        SKSE::log::info("grapple: cam ({:.2f} {:.2f} {:.2f}) fwd ({:.3f} {:.3f} {:.3f}) hit at {:.1f} m, normal ({:.2f} {:.2f} {:.2f})",
            cam.position.x, cam.position.y, cam.position.z, cam.forward.x, cam.forward.y, cam.forward.z,
            hit.distance, hit.normal.x, hit.normal.y, hit.normal.z);
    } else {
        SKSE::log::info("grapple: cam fwd ({:.3f} {:.3f} {:.3f}), nothing in range", cam.forward.x, cam.forward.y, cam.forward.z);
    }
}

// States Skyrim's own controls own: core lets go (the controller back to Skyrim) and waits.
bool Blocked(RE::PlayerCharacter* player) {
    const RE::ActorState* st = player->AsActorState();
    return player->IsDead() || player->IsOnMount() || player->IsInKillMove() ||
           st->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal || st->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal;
}

// The host holds the nodes and bodies of moving anchors; once core uses none, they are let go
// (else a loose object or an NPC's bone stays referenced after its cell unloads).
void DropUnusedAnchors() {
    const jc2::MoveState s = g_mech.State();
    const bool cling = s == jc2::MoveState::WallCling || s == jc2::MoveState::CeilingHang ||
                       s == jc2::MoveState::WallJump || s == jc2::MoveState::WallDrop;
    const bool used = (g_mech.Hook().flying && g_mech.Hook().anchorId) || (g_mech.Grapple().attached && g_mech.Grapple().anchorId) ||
                      (cling && g_mech.Cling().anchorId) || g_mech.Pull().phase != jc2::PullState::Phase::None;
    if (!used) g_host.DropAnchors();
}

void OnFrame(RE::PlayerCharacter* player, float delta) {
    if (RE::UI::GetSingleton()->GameIsPaused()) return;
    // Entering one also resets the visuals: the skydive landing hands over to OnFoot and the
    // ragdoll in the same frame, and with no update after it Rico stayed in the skydive pose.
    static bool wasBlocked = false;
    const bool blocked = Blocked(player);
    if (blocked && !wasBlocked) {
        SKSE::log::info("{}: dead, mounted, sitting or knocked down; letting go", jc2::ToString(g_mech.State()));
        Reset();
    }
    wasBlocked = blocked;
    if (blocked) return;
    if (!g_host.BeginFrame(player, delta)) return;

    // Options into core every frame (the menu can change them).
    const Settings& cfg = Settings::Get();
    g_mech.GetTuning().grappleEnabled = cfg.grapple;
    g_mech.GetTuning().chuteEnabled = cfg.parachute;
    StaminaBeforeTicks(player, delta);
    if (g_host.GetInput().grapplePressed && cfg.grapple) LogGrappleAim();
    const bool hookWasFlying = g_mech.Hook().flying;
    const float hookTime = g_mech.Hook().time;

    const float stepDt = 1.0f / g_mech.GetTuning().tickHz;
    const int steps = g_stepper.Advance(delta, stepDt);
    for (int i = 0; i < steps; ++i) {
        const jc2::MoveState before = g_mech.State();
        const jc2::Vec3 velBefore = g_host.GetVel();
        // Highest point of the current fall (falls and skydives only), for the landing damage.
        if ((before != jc2::MoveState::Falling && before != jc2::MoveState::Skydive) || velBefore.y > 0.0f) {
            g_fallTop = g_host.GetPos().y;
        }
        g_mech.Tick(g_host, stepDt);
        if (before == jc2::MoveState::Skydive && g_mech.State() == jc2::MoveState::OnFoot && !g_host.InWater()) {
            g_host.HardLanding(velBefore, g_fallTop - g_host.GetPos().y, g_fallDamageMult, cfg.landingRagdoll);
        }
        g_host.AfterTick(g_mech.State(), stepDt);
        if (g_mech.State() != before) {
            SKSE::log::info("{} -> {}", jc2::ToString(before), jc2::ToString(g_mech.State()));
            if (g_host.config.notify) {
                RE::ConsoleLog::GetSingleton()->Print("Skydive: %s -> %s", jc2::ToString(before), jc2::ToString(g_mech.State()));
            }
        }
    }
    // A new shot (the hook started flying, or a re-fire restarted it) costs stamina.
    const jc2::HookState& hook = g_mech.Hook();
    if (cfg.stamina && hook.flying && (!hookWasFlying || hook.time < hookTime)) {
        player->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina, cfg.grappleCost);
        g_stamina.last = player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
    }
    g_host.EndFrame(g_mech.State());
    DropUnusedAnchors();
    animation::UpdateGraph(player, g_mech.State(), SkyrimHost::Drives(g_mech.State()));
    animation::UpdateVisuals(player, g_mech, delta);
    animation::UpdateBody(g_mech, delta);
    animation::UpdateGrapple(player, g_mech, delta);
    animation::UpdateSounds(player, g_mech, g_host.GetVel(), delta);
    spells::Update(player, g_mech.State(), delta);
    animation::SetDriven(SkyrimHost::Drives(g_mech.State()), g_mech.State());

    // Twice a second while core drives: what core wants vs what the controller did with it.
    static float logTimer = 0.0f;
    logTimer += delta;
    if (SkyrimHost::Drives(g_mech.State()) && logTimer >= 0.5f) {
        logTimer = 0.0f;
        const jc2::Vec3 v = g_host.GetVel(), c = g_host.ControllerVel(), p = g_host.GetPos();
        const jc2::ChuteState& ch = g_mech.Chute();
        SKSE::log::info("  {} pos ({:.1f} {:.1f} {:.1f}) core vel ({:.2f} {:.2f} {:.2f}) controller ({:.2f} {:.2f} {:.2f}) chute p/h/b {:.2f} {:.2f} {:.2f}",
            jc2::ToString(g_mech.State()), p.x, p.y, p.z, v.x, v.y, v.z, c.x, c.y, c.z, ch.pitch, ch.heading, ch.bank);
    }
}

// IAnimationGraphManagerHolder::NotifyAnimationGraph (vfunc 1) on the player: while core moves
// the body, Skyrim keeps sending landing events (every ~1.7 s under the chute), which play the
// landing animation and sound mid-air. Those are dropped.
struct PlayerNotifyGraph {
    static bool thunk(RE::IAnimationGraphManagerHolder* a_this, const RE::BSFixedString& a_event) {
        if (SkyrimHost::Drives(g_mech.State())) {
            const std::string_view e = a_event.c_str();
            if (e == "JumpLand" || e == "JumpLandDirectional" || e == "JumpDown" || e == "JumpLandEnd") return false;
        }
        return func(a_this, a_event);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

// Faster getting up from a ragdoll (Settings::getUpSpeed), the player only: while down, the
// knockdown timer fExplosionKnockStateExplodeDownTime is divided by it (restored once the player
// is up; NPCs knocked down meanwhile share it), and the get-up animation plays that much faster.
// APPROX: which part of the wait each covers is unverified; the log prints the state changes.
float GetUpAnimationScale(RE::PlayerCharacter* player) {
    static RE::KNOCK_STATE_ENUM last = RE::KNOCK_STATE_ENUM::kNormal;
    static float savedDownTime = -1.0f;
    const float speed = (std::max)(Settings::Get().getUpSpeed, 0.1f);
    const RE::KNOCK_STATE_ENUM state = player->AsActorState()->GetKnockState();
    if (state != last) {
        SKSE::log::info("knock state {} -> {}", static_cast<int>(last), static_cast<int>(state));
        last = state;
    }
    RE::Setting* down = RE::GameSettingCollection::GetSingleton()->GetSetting("fExplosionKnockStateExplodeDownTime");
    const bool knocked = state != RE::KNOCK_STATE_ENUM::kNormal;
    if (down && knocked && savedDownTime < 0.0f) {
        savedDownTime = down->data.f;
        down->data.f = savedDownTime / speed;
        SKSE::log::info("fExplosionKnockStateExplodeDownTime {:.2f} -> {:.2f} while down", savedDownTime, down->data.f);
    } else if (down && !knocked && savedDownTime >= 0.0f) {
        down->data.f = savedDownTime;
        savedDownTime = -1.0f;
    }
    return state == RE::KNOCK_STATE_ENUM::kGetUp ? speed : 1.0f;
}

// PlayerCharacter::UpdateAnimation (vfunc 0x7D): Rico's JC2 body, the canopy and the grapple arm
// go on top of the pose the behavior graph produced.
struct PlayerUpdateAnimation {
    static void thunk(RE::PlayerCharacter* a_this, float a_delta) {
        func(a_this, a_delta * GetUpAnimationScale(a_this));
        animation::AfterAnimationUpdate(a_this);
        animation::GrappleAfterAnimation(a_this);
        spells::PoseArm(a_this);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

struct PlayerUpdate {
    static void thunk(RE::PlayerCharacter* a_this, float a_delta) {
        g_host.HoldInAir(a_this);
        func(a_this, a_delta);
        OnFrame(a_this, a_delta);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

// FirstPersonState::Update (TESCameraState vfunc 3): the state sets the camera root from the
// player's look; the first-person chute roll goes on after. (PlayerCamera::Update is not called
// through its vtable, so a hook there never runs.)
struct CameraUpdate {
    static void thunk(RE::FirstPersonState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_next) {
        func(a_this, a_next);
        animation::AfterCameraUpdate(RE::PlayerCamera::GetSingleton());
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

class InputSink final : public RE::BSTEventSink<RE::InputEvent*> {
public:
    RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override {
        if (!a_event) return RE::BSEventNotifyControl::kContinue;
        RE::UI* ui = RE::UI::GetSingleton();
        if (ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME)) return RE::BSEventNotifyControl::kContinue;
        for (RE::InputEvent* e = *a_event; e; e = e->next) {
            if (const RE::ButtonEvent* b = e->AsButtonEvent()) {
                g_host.OnButton(*b);
                spells::OnButton(*b);
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};
InputSink g_inputSink;

// Startup diagnostics: which menus open (a hang before "Main Menu" is a load-time problem).
class MenuLog final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        if (a_event && a_event->opening && a_event->menuName == RE::MainMenu::MENU_NAME) SKSE::log::info("main menu opened");
        return RE::BSEventNotifyControl::kContinue;
    }
};
MenuLog g_menuLog;

void OnMessage(SKSE::MessagingInterface::Message* msg) {
    switch (msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            menu::Init();
            SKSE::log::info("data loaded");
            RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuLog);
            break;
        case SKSE::MessagingInterface::kInputLoaded:
            RE::BSInputDeviceManager::GetSingleton()->AddEventSink(&g_inputSink);
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            Reset();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            Reset();
            animation::WatchGraphEvents(RE::PlayerCharacter::GetSingleton());
            break;
        default: break;
    }
}

}  // namespace

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse, {.trampoline = true, .trampolineSize = 64});
    SKSE::log::info("Skydive {} loading", SKSE::GetPluginVersion().string());
    LoadConfig();

    REL::Relocation<std::uintptr_t> vtbl{RE::VTABLE_PlayerCharacter[0]};
    PlayerUpdate::func = vtbl.write_vfunc(0xAD, PlayerUpdate::thunk);
    PlayerUpdateAnimation::func = vtbl.write_vfunc(0x7D, PlayerUpdateAnimation::thunk);
    REL::Relocation<std::uintptr_t> graphVtbl{RE::VTABLE_PlayerCharacter[3]};  // IAnimationGraphManagerHolder
    PlayerNotifyGraph::func = graphVtbl.write_vfunc(0x1, PlayerNotifyGraph::thunk);
    REL::Relocation<std::uintptr_t> cameraVtbl{RE::VTABLE_FirstPersonState[0]};
    CameraUpdate::func = cameraVtbl.write_vfunc(0x3, CameraUpdate::thunk);
    g_host.InstallHooks();
    menu::Install();
    if (g_clipHook) animation::InstallClipHook();

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
