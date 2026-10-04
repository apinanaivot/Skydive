# Skydive

Just Cause 2's skydiving, grappling hook and parachute for Skyrim Special Edition. It's an SKSE plugin
that ports the JC2 mechanics 1:1, with Rico's animations, the canopy, hook and sounds.

This repo contains no Just Cause 2 files. The assets are built on the player's PC from their own copy
of Just Cause 2 by the asset installer in installer/.

## Installing

Download the zip from [Releases](../../releases), then follow installer/README.txt:

1. Install the zip like any SKSE mod (MO2, Vortex or by hand).
2. Run Skydive Installer\Skydive-Installer.exe from the installed mod once. It finds Just Cause 2 and
Skyrim (or lets you pick the folders) and builds the assets into the mod's folder.

Requirements: SKSE64, Address Library for SKSE Plugins, Just Cause 2 (any version). Tested on Skyrim 1.7.104.

## Structure

- core/ - the JC2 mechanics in plain C++20 (grapple, reel, parachute, skydive, wall/ceiling), talks to the game only through jc2::IHost
- skse/ - the SKSE plugin (CommonLibSSE-NG). Hosts core on the player, plays the animations, canopy, hook, wire and sounds, F10 options menu
- installer/ - the asset installer (install.py, a small tkinter window) and release packaging (package.ps1)
- tools/anim/, tools/re/jc2arc.py - what the installer runs: JC2 archive reader, Havok clip/skeleton reader, retargeting onto the Skyrim skeleton, NIF writer, sound decoding

## Building from source

Needs Windows, VS 2022 with "Desktop development with C++" (brings CMake and vcpkg) and Python 3.10+.

Plugin: from skse/, using Visual Studio's CMake (...\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe):

    cmake --preset vs2022
    cmake --build --preset release

Output is build/skse/Release/Skydive.dll. First build fetches and compiles CommonLibSSE-NG, takes about 15 min.
SKYRIM_DATA_DIR in skse/CMakePresets.json is where the DLL gets copied after each build (default Steam path, empty it to skip).

Asset installer from source (instead of the exe):

    python -m pip install numpy lz4 miniaudio
    python installer/install.py

Release zip (plugin, ini, frozen installer, readme):

    python -m pip install pyinstaller numpy lz4 miniaudio
    powershell -ExecutionPolicy Bypass -File installer/package.ps1

Output is build/package/Skydive-<version>.zip. The installer exe in it is just install.py frozen by PyInstaller (one-folder build).

## Disclaimer

Just Cause 2 and its assets belong to their respective owners. The game was developed by Avalanche Studios
and published by Square Enix. This is an unofficial fan project, not affiliated with or endorsed by them.
No Just Cause 2 files are included, the installer builds the assets locally from the user's own copy of the game.
