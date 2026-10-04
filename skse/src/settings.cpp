#include "settings.h"

#include <format>

namespace {

bool ReadBool(const std::string& path, const char* section, const char* key, bool fallback) {
    return GetPrivateProfileIntA(section, key, fallback ? 1 : 0, path.c_str()) != 0;
}

float ReadFloat(const std::string& path, const char* section, const char* key, float fallback) {
    char buf[32] = {};
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path.c_str());
    return buf[0] ? std::strtof(buf, nullptr) : fallback;
}

void Write(const std::string& path, const char* section, const char* key, const std::string& value) {
    WritePrivateProfileStringA(section, key, value.c_str(), path.c_str());
}

}  // namespace

Settings& Settings::Get() {
    static Settings s;
    return s;
}

void Settings::Load(const std::string& ini) {
    path = ini;
    char key[32] = {};
    GetPrivateProfileStringA("Input", "iMenuKey", "", key, sizeof(key), path.c_str());
    if (key[0]) menuKey = static_cast<std::uint32_t>(std::strtoul(key, nullptr, 0));
    grapple = ReadBool(path, "Features", "bGrapple", grapple);
    parachute = ReadBool(path, "Features", "bParachute", parachute);
    stamina = ReadBool(path, "Stamina", "bEnabled", stamina);
    grappleCost = ReadFloat(path, "Stamina", "fGrappleCost", grappleCost);
    hangCost = ReadFloat(path, "Stamina", "fHangCostPerSecond", hangCost);
    chuteStamina = ReadBool(path, "Stamina", "bParachute", chuteStamina);
    chuteCost = ReadFloat(path, "Stamina", "fParachuteCostPerSecond", chuteCost);
    exhaustedSteer = ReadFloat(path, "Stamina", "fExhaustedSteering", exhaustedSteer);
    landingRagdoll = ReadBool(path, "Skydive", "bLandingRagdoll", landingRagdoll);
    pullMaxMass = ReadFloat(path, "Grapple", "fPullMaxMass", pullMaxMass);
    getUpSpeed = ReadFloat(path, "Ragdoll", "fGetUpSpeed", getUpSpeed);
    soundVolume = ReadFloat(path, "Sound", "fVolume", soundVolume);
}

void Settings::Save() const {
    if (path.empty()) return;
    auto b = [](bool v) { return std::string(v ? "1" : "0"); };
    auto f = [](float v) { return std::format("{:.2f}", v); };
    Write(path, "Features", "bGrapple", b(grapple));
    Write(path, "Features", "bParachute", b(parachute));
    Write(path, "Stamina", "bEnabled", b(stamina));
    Write(path, "Stamina", "fGrappleCost", f(grappleCost));
    Write(path, "Stamina", "fHangCostPerSecond", f(hangCost));
    Write(path, "Stamina", "bParachute", b(chuteStamina));
    Write(path, "Stamina", "fParachuteCostPerSecond", f(chuteCost));
    Write(path, "Stamina", "fExhaustedSteering", f(exhaustedSteer));
    Write(path, "Skydive", "bLandingRagdoll", b(landingRagdoll));
    Write(path, "Grapple", "fPullMaxMass", f(pullMaxMass));
    Write(path, "Ragdoll", "fGetUpSpeed", f(getUpSpeed));
    Write(path, "Sound", "fVolume", f(soundVolume));
}
