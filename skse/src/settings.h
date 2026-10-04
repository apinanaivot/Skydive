#pragma once

#include <string>

// The player's options (Skydive.ini, editable in game through the F10 options menu,
// menu.cpp). Read at plugin load; Save writes them back to the ini.
struct Settings {
    // Either mechanic can be turned off; the skydive always stays.
    bool grapple = true;
    bool parachute = true;

    // Immersive stamina (off by default). The grapple costs grappleCost per shot and can't be
    // fired with less stamina than that; a wall or ceiling costs hangCost per second and lets go
    // when stamina runs out. With chuteStamina, steering the chute costs chuteCost per second at full
    // input; with none left it steers at exhaustedSteer of the input. Stamina does not regenerate
    // while it drains.
    bool stamina = false;
    float grappleCost = 20.0f;
    float hangCost = 2.0f;
    bool chuteStamina = false;
    float chuteCost = 10.0f;
    float exhaustedSteer = 0.3f;

    // A skydive into the ground ragdolls the player (off: only when the fall kills).
    bool landingRagdoll = true;

    // The grapple pulls loose objects up to this mass (Havok kg) in; heavier ones are anchors.
    float pullMaxMass = 250.0f;

    // Getting up from a ragdoll this many times faster than vanilla (the player only).
    float getUpSpeed = 4.0f;

    // JC2's sounds, on top of Skyrim's Effects volume.
    float soundVolume = 1.0f;

    std::uint32_t menuKey = 0x44;  // options menu, DirectInput scan code (F10); ini only

    std::string path;  // the ini

    static Settings& Get();
    void Load(const std::string& ini);
    void Save() const;
};
