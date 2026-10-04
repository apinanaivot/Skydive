#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>

#include "animation.h"
#include "settings.h"

// JC2's grapple and parachute sounds (Parachute.fsb, converted by tools/anim/build_sounds.py to
// Data/Sound/FX/jc2mech/*.wav) played on core's events, following parachute.fev's events:
// grappling_fire_wire (fire), grappling_impact_* (hook lands), grappling_reel_loop (reel),
// grappling_detatch + grappling_reel_back (rope released), Parachute_Vert (chute opens),
// Parachute_InFlight (flapping + wind loop), Parachute_steer (A/D).
// Each file plays through its own sound descriptor made at runtime (no plugin file): a copy of a
// vanilla one (kTemplate) with the file, AudioCategorySFX (the Effects slider) and
// SOMMono01400Player1st (the output model of the player's own footsteps: reverb send 80%), so the
// game's volume settings, reverb and audio mods apply as to any other Skyrim sound. A bare file
// sound (GetSoundHandleByFile) has neither.
// APPROX: FMOD's event parameters (speed-driven volume and pitch) are approximated by hand.

namespace animation {

namespace {

constexpr float kSteerThreshold = 0.25f;  // chute bank (rad) that counts as a steer

// Skyrim.esm forms (read from the esm): NPCHumanExecutionerPushPlayer (one file, no conditions),
// AudioCategorySFX, SOMMono01400Player1st.
constexpr RE::FormID kTemplate = 0x10E49C;
constexpr RE::FormID kCategory = 0x172A1;
constexpr RE::FormID kOutput = 0xB4058;
constexpr std::uint16_t kStaticAttenuation = 600;  // dB * 100, about what vanilla effects use

// name -> its descriptor (never freed: the audio manager may hold it while a sound plays)
std::unordered_map<std::string, RE::BGSSoundDescriptorForm*> g_descriptors;

RE::BGSSoundDescriptorForm* Descriptor(const std::string& name, bool loop) {
    if (auto it = g_descriptors.find(name); it != g_descriptors.end()) return it->second;
    RE::BGSSoundDescriptorForm* form = nullptr;
    auto* data = RE::TESDataHandler::GetSingleton();
    auto* tmpl = data ? data->LookupForm<RE::BGSSoundDescriptorForm>(kTemplate, "Skyrim.esm") : nullptr;
    auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSSoundDescriptorForm>();
    if (tmpl && tmpl->soundDescriptor && factory &&
        *reinterpret_cast<std::uintptr_t*>(tmpl->soundDescriptor) == RE::VTABLE_BGSStandardSoundDef[0].address()) {
        // A byte copy of the template's definition (vtable included), then our own fields.
        auto* def = static_cast<RE::BGSStandardSoundDef*>(RE::malloc(sizeof(RE::BGSStandardSoundDef)));
        std::memcpy(static_cast<void*>(def), tmpl->soundDescriptor, sizeof(RE::BGSStandardSoundDef));
        std::construct_at(&def->soundFiles);  // not the template's buffer
        RE::BSResource::ID id;
        id.GenerateFromPath(("Sound\\FX\\jc2mech\\" + name + ".wav").c_str());
        def->soundFiles.push_back(id);
        def->conditions = nullptr;
        def->alternateSoundFormID = 0;
        if (auto* category = data->LookupForm<RE::BGSSoundCategory>(kCategory, "Skyrim.esm")) def->category = category;
        if (auto* output = data->LookupForm<RE::BGSSoundOutput>(kOutput, "Skyrim.esm")) def->outputModel = output;
        auto& c = def->soundCharacteristics;
        c.frequencyShift = c.frequencyVariance = c.dbVariance = 0;
        c.priority = 128;
        c.staticAttenuation = kStaticAttenuation;
        def->lengthCharacteristics.looping = loop ? RE::BGSStandardSoundDef::LengthCharacteristics::Looping::kLoop
                                                  : RE::BGSStandardSoundDef::LengthCharacteristics::Looping::kNone;
        form = factory->Create();
        if (form) form->soundDescriptor = def;
    }
    if (!form) SKSE::log::warn("sounds: no descriptor for {} (Skyrim.esm template missing?)", name);
    g_descriptors[name] = form;
    return form;
}

bool Make(RE::BSSoundHandle& h, const std::string& name, bool loop = false) {
    auto* audio = RE::BSAudioManager::GetSingleton();
    RE::BGSSoundDescriptorForm* form = Descriptor(name, loop);
    if (!audio || !form) return false;
    h = {};
    audio->GetSoundHandle(h, form);
    if (h.IsValid()) return true;
    static bool warned = false;
    if (!warned) SKSE::log::warn("sounds: {} did not load (run tools/anim/build_sounds.py)", name);
    warned = true;
    return false;
}

float Volume(float v) { return v * (std::max)(Settings::Get().soundVolume, 0.0f); }

void Follow(RE::BSSoundHandle& h, RE::PlayerCharacter* player) {
    if (RE::NiAVObject* node = player ? player->Get3D(false) : nullptr) h.SetObjectToFollow(node);
}

void OneShot(RE::PlayerCharacter* player, const std::string& name, float volume) {
    RE::BSSoundHandle h;
    if (!Make(h, name)) return;
    h.SetVolume(Volume(volume));
    Follow(h, player);
    h.Play();
}

// "base_1".."base_n" at random.
std::string Pick(const char* base, int n) { return std::format("{}_{}", base, 1 + std::rand() % n); }

// A looping sound (the descriptor loops the file; the files are also ~30 s of the JC2 loop and are
// restarted when they run out, in case the loop flag doesn't take). Timed by the file's duration,
// not IsPlaying(): that reads false while a new sound is still queued, so polling it started a
// fresh copy every frame on top of the old ones.
struct Loop {
    const char* name;
    RE::BSSoundHandle h;
    bool on = false;
    float elapsed = 0.0f, duration = 0.0f;  // s; duration 0 until the audio manager knows it
    bool failed = false;                     // didn't load: not tried again every frame

    void Set(RE::PlayerCharacter* player, bool want, float volume, float dt) {
        if (want && !failed) {
            elapsed += dt;
            if (on && duration <= 0.0f) duration = static_cast<float>(h.GetDuration()) / 1000.0f;
            const float length = duration > 0.0f ? duration : 25.0f;
            if (!on || elapsed >= length - 0.02f) {
                if (on) h.Stop();  // ran out: straight back in
                if (!Make(h, name, true)) {
                    failed = true;
                    on = false;
                    return;
                }
                Follow(h, player);
                h.SetVolume(Volume(volume));
                if (on) h.Play();
                else h.FadeInPlay(150);
                on = true;
                elapsed = 0.0f;
            }
            h.SetVolume(Volume(volume));
        } else if (on) {
            h.FadeOutAndRelease(250);
            on = false;
        }
    }
};

Loop g_reel{"grapple_reel"};
Loop g_flapSlow{"chute_flap_slow"};
Loop g_flapFast{"chute_flap_fast"};
Loop g_chuteWind{"chute_wind"};
Loop g_freefall{"freefall_wind"};

jc2::MoveState g_prevState = jc2::MoveState::OnFoot;
bool g_prevFlying = false, g_prevAttached = false;
float g_prevHookTime = 0.0f;
float g_prevBank = 0.0f;

}  // namespace

void UpdateSounds(RE::PlayerCharacter* player, const jc2::Mechanics& mech, const jc2::Vec3& vel, float dt) {
    if (!player) return;
    const jc2::MoveState s = mech.State();
    const jc2::HookState& hook = mech.Hook();
    const bool attached = (mech.Grapple().attached &&
                           (s == jc2::MoveState::Grappling || s == jc2::MoveState::Parachuting)) ||
                          mech.Pull().phase != jc2::PullState::Phase::None;
    const float speed = jc2::Length(vel);

    if (hook.flying && (!g_prevFlying || hook.time < g_prevHookTime)) {
        OneShot(player, "grapple_fire", 0.8f);
        OneShot(player, Pick("grapple_wire", 3), 0.8f);
    }
    if (attached && !g_prevAttached) {
        OneShot(player, Pick("grapple_impact", 2), 0.9f);
        OneShot(player, Pick("grapple_twang", 2), 0.6f);
    }
    if (!attached && g_prevAttached) {
        OneShot(player, "grapple_detach", 0.8f);
        OneShot(player, Pick("grapple_reelback", 3), 0.7f);
    }
    g_prevFlying = hook.flying;
    g_prevHookTime = hook.time;
    g_prevAttached = attached;

    const bool reeling = s == jc2::MoveState::Grappling && attached && mech.Grapple().startDelay <= 0.0f;
    g_reel.Set(player, reeling, 0.7f, dt);

    const bool chute = s == jc2::MoveState::Parachuting;
    if (chute && g_prevState != jc2::MoveState::Parachuting) OneShot(player, "chute_open", 1.0f);
    // Flapping: slow under ~20 m/s, fast above (JC2 crossfades them by speed).
    const float fast = std::clamp((speed - 15.0f) / 15.0f, 0.0f, 1.0f);
    g_flapSlow.Set(player, chute, 0.6f * (1.0f - fast) + 0.05f, dt);
    g_flapFast.Set(player, chute, 0.6f * fast + 0.05f, dt);
    g_chuteWind.Set(player, chute, std::clamp(speed / 40.0f, 0.15f, 0.7f), dt);
    const float bank = chute ? mech.Chute().bank : 0.0f;
    if (chute && std::fabs(bank) > kSteerThreshold && std::fabs(g_prevBank) <= kSteerThreshold) {
        OneShot(player, Pick("chute_steer", 4), 0.7f);
    }
    g_prevBank = bank;

    const bool freefall = s == jc2::MoveState::Skydive || (s == jc2::MoveState::Falling && speed > 15.0f);
    g_freefall.Set(player, freefall, std::clamp(speed / 50.0f, 0.2f, 0.9f), dt);
    g_prevState = s;
}

void ResetSounds() {
    for (Loop* l : {&g_reel, &g_flapSlow, &g_flapFast, &g_chuteWind, &g_freefall}) {
        if (l->on) l->h.Stop();
        l->on = false;
        l->elapsed = l->duration = 0.0f;
        l->failed = false;
    }
    g_prevState = jc2::MoveState::OnFoot;
    g_prevFlying = g_prevAttached = false;
    g_prevHookTime = g_prevBank = 0.0f;
}

}  // namespace animation
