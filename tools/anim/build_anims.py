"""Converts Rico's JC2 animations into the clip files the plugin plays itself (skse/src/body.cpp,
visuals.cpp, grapple.cpp). They are JC2 data: they go straight into Skyrim's Data folder and a
copy into extracted/anims, never into git.

python build_anims.py [--data <Skyrim Data dir>]"""
import argparse, os, shutil, struct
import numpy as np

import paths
from bsa import SKYRIM_DATA
from jc2havok import read_skeleton, read_animation
from retarget import Retargeter
from chute_ik import Canopy, rico_pose, solve_arms

STAGE = os.path.join(paths.EXTRACTED, "anims")
# The OAR mod earlier versions installed; removed on install (the plugin plays everything now).
OLD_OAR_DIR = os.path.join("meshes", "actors", "character", "animations", "OpenAnimationReplacer", "JC2Mech")
# Rico's whole-body clips (rico_body.bin), by the name the plugin looks them up with.
# Skydive: kc001_base.asb ANIM_SKYDIVE blends these four; the weights are 00853f70's (body.cpp).
BODY = os.path.join("meshes", "jc2mech", "rico_body.bin")
BODY_CLIPS = [("skydive", "mhfreefall"),
              ("skydive_left", "freefall_trn_lft"),
              ("skydive_right", "freefall_trn_rgt"),
              ("skydive_dive", "birdsuit_fwd"),
              ("skydive_enter", "falling_to_freefall"),  # ANIM_JUMP_FALLING_TO_SKYDIVE
              ("chute_open", "rico_open_chute"),        # ANIM_PULL_OPEN_PARACHUTE_VERTICAL
              ("chute_reel_open", "rico_reel_open"),    # ANIM_UNFOLD_PARACHUTE_HORIZONTAL
              ("reel_start", "reel_in_start"),          # ANIM_REEL_START
              ("reel", "grpl_reel_flight_part2"),       # ANIM_REEL_FLIGHT
              ("wall_impact", "grpl_reel_impact"),      # ANIM_REEL_IMPACT
              ("wall_idle", "grpl_reeled_timeblend"),   # ANIM_REELED_IN_IDLE
              # JC2 layers the unarmed right arm (no gun: the hand stays on the wire) over the
              # same time-blend frames; merged here, the plugin takes the right arm from it.
              ("wall_idle_unarmed", "grpl_reeled_timeblend+grpl_reeled_timeblend_unarmed"),
              ("wall_jump", "grpl_reel_bounce"),        # ANIM_REELED_IN_JUMP
              ("wall_drop", "grpl_reeled_release"),     # ANIM_REELED_IN_RELEASE_DROP
              ("hang_impact", "reel_impact_horizontal"),  # ANIM_REEL_IMPACT_HORIZONTAL
              ("hang_enter", "reel_impact_to_hang"),    # ANIM_REELED_IN_TO_HANG
              # ANIM_HANG_IDLE (S_HANGSTUNT_IDLE, after REELED_IN_TO_HANG): two time-blend clips,
              # the swing sideways and fore / aft (ANIM_HANGED grpl_hang is a hung-up NPC).
              ("hang", "heli_default_timeblend_hori"),
              ("hang_diag", "heli_default_timeblend_diag"),
              ("hang_drop", "heli_default_exit_to_fall"),  # ANIM_HANG_TO_FALL
              ("land_roll", "land_chute_roll")]         # ANIM_PARACHUTE_LANDING (the roll)
# Chute flight poses for the plugin (rico_chute.bin): grid over core's chute pitch / bank
# normalized to -1..1, interpolated every frame by skse/src/visuals.cpp.
CHUTE_GRID = list(np.linspace(-1.0, 1.0, 9))
RICO_CHUTE = os.path.join("meshes", "jc2mech", "rico_chute.bin")
# Rico's left-arm grapple layer (kc001_leftarm.asb): the _con clips animate the forearm, hand and
# fingers only; JC2 aims the upper arm at the hook itself (the plugin does, skse/src/grapple.cpp).
RICO_ARM = os.path.join("meshes", "jc2mech", "rico_arm.bin")
ARM_CLIPS = [("fire", "grpl_fire_ani_con"),              # LA_ANIM_RAISE_GRAPPLE (on foot)
             ("fire_air", "grpl_freefall_fire_ani_con"),  # LA_ANIM_RAISE_GRAPPLE_SKYDIVE
             ("pull", "grpl_freefall_pull_con")]          # LA_ANIM_GRAPPLE_PULL_PARACHUTE
ARM_BONES = ("NPC L Forearm", "NPC L ForearmTwist", "NPC L Hand", "NPC L Finger")


def read_clip(spec):
    """A clip, or "base+layer": the layer's tracks replace the base's (same frame count)."""
    base, *layers = spec.split("+")
    anim = read_animation(os.path.join(paths.JC2_ANIMS, base + ".ban"))
    for layer in layers:
        top = read_animation(os.path.join(paths.JC2_ANIMS, layer + ".ban"))
        assert top.frames.shape[0] == anim.frames.shape[0], spec
        anim.frames = anim.frames.copy()
        for ti, n in enumerate(top.track_names):
            anim.frames[:, anim.track_names.index(n)] = top.frames[:, ti]
    return anim


def write_clips(rt, path, bones, clips):
    """canopy_anim.bin format (build_models.py, "JC2C" version 2): bone names, then per clip the
    name, frame count, frame duration and frames x bones x (t xyz, q xyzw, scale) Skyrim locals."""
    out = bytearray(b"JC2C") + struct.pack("<II", 2, len(bones))
    for i in bones:
        b = rt.s.names[i].encode()
        out += struct.pack("<B", len(b)) + b
    out += struct.pack("<I", len(clips))
    for name, clip in clips:
        anim = read_clip(clip)
        frames = rt.convert(anim)
        out += struct.pack("<B", len(name)) + name.encode() + struct.pack("<If", len(frames), anim.frame_duration)
        for f in range(len(frames)):
            for i in bones:
                out += struct.pack("<3f4ff", *frames[f, i, 0:3], *frames[f, i, 3:7], 1.0)
        print("%-16s %-24s %3d frames" % (name, clip, len(frames)))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(bytes(out))


def body_bones(rt):
    return [i for i, n in enumerate(rt.s.names) if not n.startswith("x_")]  # no helper nodes


def bake_chute_poses(rt, canopy, path):
    """rico_chute.bin: "JC2R", u32 1, u32 bones, bone names (u8 length + chars), u32 n, n f32 grid
    values, then n (pitch) x n (bank) x bones x (t xyz, q xyzw) f32: Skyrim parent-local
    transforms of JC2's chute flight pose (rico_speed_TB / rico_turn_TB, hands on the handles)."""
    def clip(name):
        return rt.convert(read_animation(os.path.join(paths.JC2_ANIMS, name + ".ban")))
    speed, turn = clip("rico_speed_TB_UP"), clip("rico_turn_TB_UP")
    bones = body_bones(rt)
    out = bytearray(b"JC2R") + struct.pack("<II", 1, len(bones))
    for i in bones:
        b = rt.s.names[i].encode()
        out += struct.pack("<B", len(b)) + b
    out += struct.pack("<I", len(CHUTE_GRID)) + struct.pack("<%df" % len(CHUTE_GRID), *CHUTE_GRID)
    for p in CHUTE_GRID:
        for b in CHUTE_GRID:
            pose = solve_arms(rt.s, rico_pose(speed, turn, p, b), canopy.handles(p, b))
            for i in bones:
                out += struct.pack("<3f4f", *pose[i, 0:3], *pose[i, 3:7])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(bytes(out))
    print("chute poses: %d x %d grid, %d bones -> %s" % (len(CHUTE_GRID), len(CHUTE_GRID), len(bones), path))


def bake_arm_clips(rt, path):
    """rico_arm.bin: left forearm, hand and finger locals of JC2's grapple arm clips."""
    write_clips(rt, path, [i for i, n in enumerate(rt.s.names) if n.startswith(ARM_BONES)], ARM_CLIPS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=SKYRIM_DATA)
    args = ap.parse_args()

    rt = Retargeter(read_skeleton(paths.JC2_SKELETON), read_skeleton(paths.SKYRIM_SKELETON))
    write_clips(rt, os.path.join(STAGE, BODY), body_bones(rt), BODY_CLIPS)
    bake_chute_poses(rt, Canopy(), os.path.join(STAGE, RICO_CHUTE))
    bake_arm_clips(rt, os.path.join(STAGE, RICO_ARM))
    for f in (BODY, RICO_CHUTE, RICO_ARM):
        os.makedirs(os.path.dirname(os.path.join(args.data, f)), exist_ok=True)
        shutil.copyfile(os.path.join(STAGE, f), os.path.join(args.data, f))
    print("installed to", os.path.join(args.data, "meshes", "jc2mech"))
    old = os.path.join(args.data, OLD_OAR_DIR)
    if os.path.isdir(old):
        shutil.rmtree(old)
        print("removed the old OAR mod", old)


if __name__ == "__main__":
    main()
