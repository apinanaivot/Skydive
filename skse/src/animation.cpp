#include "animation.h"

#include <atomic>

namespace animation {

namespace {

std::atomic<std::uint8_t> g_state{static_cast<std::uint8_t>(jc2::MoveState::OnFoot)};
std::atomic<bool> g_landedWhileDriven{false};  // set by the graph event sink

}  // namespace

namespace {

// The player's hkbCharacters (third and first person graphs), for the clip update hook, which
// runs for every actor, possibly on Havok worker threads.
std::atomic<RE::hkbCharacter*> g_playerCharacters[2]{};

void FindPlayerCharacters(RE::PlayerCharacter* player) {
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    RE::hkbCharacter* found[2]{};
    if (player->GetAnimationGraphManager(manager) && manager) {
        for (std::uint32_t i = 0; i < manager->graphs.size() && i < 2; ++i) {
            if (manager->graphs[i]) found[i] = &manager->graphs[i]->characterInstance;
        }
    }
    for (int i = 0; i < 2; ++i) g_playerCharacters[i].store(found[i]);
}

}  // namespace

void UpdateGraph(RE::PlayerCharacter* player, jc2::MoveState, bool driven) {
    if (!player) return;
    FindPlayerCharacters(player);
    // A landing that got through anyway (it can come from paths the event hook doesn't see):
    // straight back into the jump fall state, under body.cpp's pose.
    const bool landed = g_landedWhileDriven.exchange(false);
    if (!driven) return;
    bool inJump = false;
    if (landed || (player->GetGraphVariableBool("bInJumpState", inJump) && !inJump)) {
        if (landed) SKSE::log::info("  landing while driving: back to JumpFall");
        player->NotifyAnimationGraph("JumpFall");
    }
}

namespace {

std::atomic<bool> g_driven{false};

class GraphEventLog final : public RE::BSTEventSink<RE::BSAnimationGraphEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
                                          RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override {
        if (a_event && g_driven.load()) {
            const std::string_view tag = a_event->tag.c_str();
            if (tag == "JumpLandEnd" || tag == "JumpLand" || tag == "JumpLandDirectional") g_landedWhileDriven.store(true);
            if (tag.find("Jump") != std::string_view::npos || tag.find("Land") != std::string_view::npos ||
                tag.find("Fall") != std::string_view::npos || tag.find("FootDown") != std::string_view::npos) {
                SKSE::log::info("  anim event {} ({}) while driving, state {}", tag, a_event->payload.c_str(),
                                jc2::ToString(static_cast<jc2::MoveState>(g_state.load())));
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};
GraphEventLog g_graphLog;

}  // namespace

void WatchGraphEvents(RE::PlayerCharacter* player) {
    if (!player) return;
    player->RemoveAnimationGraphEventSink(&g_graphLog);
    player->AddAnimationGraphEventSink(&g_graphLog);
}

void SetDriven(bool driven, jc2::MoveState state) {
    g_driven.store(driven);
    g_state.store(static_cast<std::uint8_t>(state));
}

namespace {

// hkbClipGenerator::Update (vfunc 5) for the player's jump fall clips while core drives.
// Skyrim's 0_master.hkx puts a trigger on every clip of the jump fall states (MT_JumpFall,
// MT_JumpFall.hkx / Right / Back / Left.hkx): JumpLand(Directional) 0.1 s before the clip ends,
// feeding an unconditional wildcard transition to the landing state. With the vanilla 5.3 s
// clip that is the mid-air landing 5.2 s into every flight. The trigger is raised inside the
// graph, so the event hooks never see it. While core drives, these clips loop with no triggers
// (body.cpp poses Rico over them); vanilla plays them once.
struct ClipUpdate {
    static void thunk(RE::hkbClipGenerator* a_this, const RE::hkbContext& a_context, float a_timestep) {
        if (!IsPlayerFallClip(a_this, a_context)) {
            func(a_this, a_context, a_timestep);
            return;
        }
        if (!g_driven.load()) {
            a_this->mode = RE::hkbClipGenerator::kModeSinglePlay;  // vanilla, for all five
            func(a_this, a_context, a_timestep);
            return;
        }
        a_this->mode = RE::hkbClipGenerator::kModeLooping;
        // Raw swap: the hkRefPtr's reference count stays as it is.
        auto& triggers = reinterpret_cast<RE::hkbClipTriggerArray*&>(a_this->triggers);
        RE::hkbClipTriggerArray* const saved = triggers;
        triggers = nullptr;
        func(a_this, a_context, a_timestep);
        triggers = saved;
    }

    static bool IsPlayerFallClip(RE::hkbClipGenerator* a_this, const RE::hkbContext& a_context) {
        const RE::hkbCharacter* c = a_context.character;
        if (!c || (c != g_playerCharacters[0].load() && c != g_playerCharacters[1].load())) return false;
        const char* name = a_this->name.c_str();
        return name && std::string_view(name).starts_with("MT_JumpFall");
    }

    static inline REL::Relocation<decltype(thunk)> func;
};

}  // namespace

void InstallClipHook() {
    REL::Relocation<std::uintptr_t> vtbl{RE::VTABLE_hkbClipGenerator[0]};
    ClipUpdate::func = vtbl.write_vfunc(0x5, ClipUpdate::thunk);
    SKSE::log::info("jump fall clip hook installed");
}

}  // namespace animation
