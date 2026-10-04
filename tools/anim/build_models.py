"""Converts the JC2 parachute canopy and grappling hook to Skyrim NIFs and installs them with
their textures (JC2 data: written to Skyrim's Data folder and extracted/models, never to git).

Canopy: gae05_lod1-gae05.rbm (SkinnedGeneral) skinned to its rig Parachute.bsk (bone nodes
"JC2Chute <bone>"); its clips are baked to meshes/jc2mech/canopy_anim.bin for the plugin, which
poses the bones every frame (skse/src/animation.cpp). Hook: wea04_lod1-grapplinghook (static,
tip along +Y). Rope: rope.nif, a thin cylinder skinned to "JC2Rope Start" / "JC2Rope End" (bind
ROPE_LENGTH apart along +Y) so the plugin stretches it from Rico's hand to the hook; its plain
textures are generated here.
Axes: Skyrim = (x, -z, y) * 70 of JC2's, so the canopy faces the actor's forward.

canopy_anim.bin: "JC2C", u32 1, u32 bones, bone names (u8 length + chars), u32 clips, per clip
name (u8 length + chars), u32 frames, f32 frame duration, frames x bones x (t xyz, q xyzw, scale) f32
in Skyrim axes and units (local to the parent bone). Version 2 (version 1 had no scale). The scale
is uniform: the geometric mean of JC2's per-axis scale (exact for base, HarnessExt and the handles,
which grow the canopy in the opening clips).

python build_models.py [--data <Skyrim Data dir>]"""
import argparse, os, shutil, struct
import numpy as np

import paths
from bsa import SKYRIM_DATA
from rbm import read, vertex_normals
from jc2havok import read_skeleton, read_animation
from nif import Bone, Shape, write
from retarget import C, UNITS_PER_M, q2m

SRC = os.path.join(paths.EXTRACTED, "fc60dcc1")
RIG = os.path.join(paths.EXTRACTED, "5b8827ed")
MODELS = {
    # output nif: (rbm, double sided, rig)
    "canopy.nif": ("gae05_lod1-gae05.rbm", True, "Parachute.bsk"),
    "hook.nif": ("wea04_lod1-grapplinghook.rbm", False, None),
}
BONE_PREFIX = "JC2Chute "
# Canopy clips for the plugin: (name in the bin, JC2 clip). add_idle is additive (local deltas
# from identity: the cloth flutter Parachute.asb layers over the flight pose).
CANOPY_CLIPS = [("open", "chute_open_chute"), ("reel_open", "chute_reel_open"),
                ("speed", "chute_speed_TB_UP"), ("turn", "chute_turn_TB_UP"), ("add_idle", "chute_add_idle")]
IDENTITY = np.array([0, 0, 0, 0, 0, 0, 1, 1, 1, 1], float)


def to_sky_q(q):
    """JC2 quaternion -> Skyrim axes (C is a proper rotation, so only the vector part turns)."""
    v = C @ np.asarray(q[:3], float)
    return np.array([v[0], v[1], v[2], q[3]])


def rig_bones(skel):
    return [Bone(BONE_PREFIX + n, int(skel.parents[i]), C @ q2m(skel.ref[i, 3:7]) @ C.T,
                 C @ skel.ref[i, 0:3] * UNITS_PER_M) for i, n in enumerate(skel.names)]


def bake_clips(skel, path):
    out = bytearray(b"JC2C") + struct.pack("<II", 2, len(skel.names))
    for n in skel.names:
        b = (BONE_PREFIX + n).encode()
        out += struct.pack("<B", len(b)) + b
    out += struct.pack("<I", len(CANOPY_CLIPS))
    for name, clip in CANOPY_CLIPS:
        a = read_animation(os.path.join(RIG, clip + ".ban"))
        track = {t: i for i, t in enumerate(a.track_names)}
        out += struct.pack("<B", len(name)) + name.encode() + struct.pack("<If", len(a.frames), a.frame_duration)
        for f in range(len(a.frames)):
            for i, n in enumerate(skel.names):
                rest = IDENTITY if name.startswith("add_") else skel.ref[i]
                src = a.frames[f, track[n]] if n in track else rest
                scale = float(np.cbrt(np.prod(np.abs(src[7:10]))))
                out += struct.pack("<3f4ff", *(C @ src[0:3] * UNITS_PER_M), *to_sky_q(src[3:7]), scale)
        print("  canopy clip %-10s %-20s %3d frames" % (name, clip, len(a.frames)))
    open(path, "wb").write(bytes(out))


ROPE_LENGTH = 100.0  # bind length in units; the plugin places the end bone at the hook
ROPE_RADIUS = 0.5    # JC2's wire is about a centimetre thick
ROPE_SIDES = 6


def rope_shape():
    p, n, uv, bi, bw = [], [], [], [], []
    for end, y in ((0, 0.0), (1, ROPE_LENGTH)):
        for k in range(ROPE_SIDES + 1):  # the seam vertex is doubled for the UVs
            a = 2 * np.pi * k / ROPE_SIDES
            p.append([np.cos(a) * ROPE_RADIUS, y, np.sin(a) * ROPE_RADIUS])
            n.append([np.cos(a), 0.0, np.sin(a)])
            uv.append([k / ROPE_SIDES, end * 10.0])
            bi.append([end, 0, 0, 0])
            bw.append([1.0, 0.0, 0.0, 0.0])
    ring = ROPE_SIDES + 1
    tris = []
    for k in range(ROPE_SIDES):
        a, b, c, d = k, k + 1, ring + k, ring + k + 1
        tris += [(a, c, b), (b, c, d)]
    bones = [Bone("JC2Rope Start", -1, np.eye(3), (0.0, 0.0, 0.0)),
             Bone("JC2Rope End", -1, np.eye(3), (0.0, ROPE_LENGTH, 0.0))]
    shape = Shape("rope", np.array(p), np.array(n), np.array(uv), np.array(tris, np.uint16),
                  [tex_path("rope_d.dds"), tex_path("rope_n.dds")], True, np.array(bi), np.array(bw))
    return shape, bones


def write_dds(path, bgra, size=4):
    """Uncompressed 32-bit DDS filled with one colour (b, g, r, a)."""
    hdr = struct.pack("<4s7I44x", b"DDS ", 124, 0x100F, size, size, size * 4, 0, 0)
    hdr += struct.pack("<8I", 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    hdr += struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    open(path, "wb").write(hdr + bytes(bgra) * (size * size))


def tex_path(name):
    return "textures\\jc2mech\\" + name if name else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=SKYRIM_DATA)
    args = ap.parse_args()
    stage = os.path.join(paths.EXTRACTED, "models")
    os.makedirs(os.path.join(stage, "meshes", "jc2mech"), exist_ok=True)
    os.makedirs(os.path.join(stage, "textures", "jc2mech"), exist_ok=True)

    for out, (src, double_sided, rig) in MODELS.items():
        shapes, bones = [], ()
        skel = read_skeleton(os.path.join(RIG, rig)) if rig else None
        if skel:
            bones = rig_bones(skel)
            bake_clips(skel, os.path.join(stage, "meshes", "jc2mech", os.path.splitext(out)[0] + "_anim.bin"))
        for k, m in enumerate(read(os.path.join(SRC, src))):
            p = (C @ m.positions.T).T * UNITS_PER_M
            n = vertex_normals(p, m.triangles)
            dif, nrm = m.textures[0], m.textures[1]
            for t in (dif, nrm):
                shutil.copyfile(os.path.join(SRC, t), os.path.join(stage, "textures", "jc2mech", t))
            bi = bw = None
            if skel is not None and m.bones is not None:
                bi = np.array(m.batch_bones[0])[m.bones]       # batch-local -> rig bone
                bw = m.weights / np.maximum(m.weights.sum(1, keepdims=True), 1e-9)
                bi = np.where(bw > 0, bi, 0)
            shapes.append(Shape("%s:%d" % (os.path.splitext(out)[0], k), p, n, m.uvs, m.triangles,
                                [tex_path(dif), tex_path(nrm)], double_sided, bi, bw))
            print("%-10s %5d verts %5d tris  bbox %s .. %s" % (out, len(p), len(m.triangles),
                  np.round(p.min(0), 1), np.round(p.max(0), 1)))
        write(os.path.join(stage, "meshes", "jc2mech", out), shapes, bones=bones)

    shape, bones = rope_shape()
    write(os.path.join(stage, "meshes", "jc2mech", "rope.nif"), [shape], bones=bones)
    write_dds(os.path.join(stage, "textures", "jc2mech", "rope_d.dds"), (40, 42, 45, 255))
    write_dds(os.path.join(stage, "textures", "jc2mech", "rope_n.dds"), (255, 128, 128, 255))
    print("rope.nif   %d verts, bind length %.0f" % (len(shape.p), ROPE_LENGTH))

    for sub in ("meshes", "textures"):
        dst = os.path.join(args.data, sub, "jc2mech")
        shutil.copytree(os.path.join(stage, sub, "jc2mech"), dst, dirs_exist_ok=True)  # keeps build_anims' rico_*.bin
    print("installed to", args.data)


if __name__ == "__main__":
    main()
