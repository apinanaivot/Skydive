Skydive - Just Cause 2's grapple and parachute for Skyrim Special Edition
==========================================================================

This mod contains no Just Cause 2 files. Its animations, parachute, grappling hook and sounds are
built on your PC from your own copy of Just Cause 2 (Steam, GOG or any other version).


REQUIREMENTS
------------
- Skyrim Special Edition / Anniversary Edition (tested on 1.7.104)
- SKSE64
- Address Library for SKSE Plugins
- Just Cause 2, installed


INSTALLING (two steps)
----------------------
Step 1: install the mod like any other mod (Mod Organizer 2, Vortex, or by hand).

Step 2: run the asset installer ONCE. It is in the mod, in the folder "Skydive Installer":

    Skydive Installer\Skydive-Installer.exe

  Where to find it:

  Mod Organizer 2
    In MO2's left pane, right-click the Skydive mod > "Open in Explorer".
    Open the "Skydive Installer" folder and double-click Skydive-Installer.exe.
    When it says Done, press F5 in MO2.

  Vortex
    On the Mods page, right-click Skydive > "Open in File Manager".
    Open the "Skydive Installer" folder and double-click Skydive-Installer.exe.
    When it says Done, click "Deploy Mods" in Vortex.

  By hand
    In your Skyrim folder, open Data\Skydive Installer and double-click
    Skydive-Installer.exe.

A small window opens. It looks for Just Cause 2 and Skyrim by itself; each line says OK when the
folder is right. If a line is red, click Browse... and pick the folder:
  - Just Cause 2 folder: the one with "archives_win32" in it
  - Skyrim Special Edition folder: the one with SkyrimSE.exe in it
  - Install the files into: already filled in with the mod's own folder. Leave it.
Click Install. It takes a minute or two and says Done when finished.

Without step 2 the mod has no animations, parachute, hook or sounds.


USING IT
--------
- Grapple: G (change iGrappleKey in SKSE\Plugins\Skydive.ini)
- In the air: Jump opens the parachute; fall from high up to skydive
- Move keys steer the parachute and the skydive
- On a wall: Jump pushes off, Sneak lets go, Grapple fires again
- On a ceiling: Sneak or Jump drops
- F10: options menu (stamina costs, turn the grapple or parachute off, volume, ...)


UNINSTALLING
------------
Remove the mod. The built files are inside the mod's folder, so they go with it (by hand: delete
Data\SKSE\Plugins\Skydive.dll, Skydive.ini, and the folders Data\Skydive Installer,
Data\meshes\jc2mech, Data\textures\jc2mech and Data\sound\fx\jc2mech).
