"""Minimal Skyrim SE (20.2.0.7, user 12, BS 100) NIF writer: a root NiNode with BSTriShapes, each
with a BSLightingShaderProperty and a BSShaderTextureSet, optionally skinned to a tree of bone
NiNodes (NiSkinInstance + NiSkinData + NiSkinPartition, SSE layout per niftools nif.xml).
Static block layouts checked against vanilla meshes (clutter\\basket01.nif). Game units, Z up."""
import struct
import numpy as np

FLAGS1 = 0x82400301        # vanilla static: specular, receive/cast shadows, ...
FLAGS1_SKINNED = 0x82400303  # + skinned
FLAGS2 = 0x00008001        # ZBuffer write, ...
DOUBLE_SIDED = 0x10        # in flags2
VF_FULL = 0x41B            # vertex, uv, normal, tangent, full precision
VF_SKINNED = 0x40


class Bone:
    """name, parent index (-1 = under the root), local rotation (3x3) and translation."""
    def __init__(self, name, parent, rotation, translation):
        self.name, self.parent, self.r, self.t = name, parent, np.asarray(rotation, float), np.asarray(translation, float)


class Shape:
    def __init__(self, name, positions, normals, uvs, triangles, textures, double_sided=False,
                 bone_indices=None, bone_weights=None):
        """bone_indices / bone_weights: (n, 4) into the bones passed to write()."""
        self.name, self.p, self.n, self.uv, self.tris = name, positions, normals, uvs, triangles
        self.textures, self.double_sided = textures, double_sided
        self.bi, self.bw = bone_indices, bone_weights


def _tangents(p, n, uv, tris):
    t = np.zeros_like(p)
    e1, e2 = p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]]
    d1, d2 = uv[tris[:, 1]] - uv[tris[:, 0]], uv[tris[:, 2]] - uv[tris[:, 0]]
    r = d1[:, 0] * d2[:, 1] - d2[:, 0] * d1[:, 1]
    r = np.where(np.abs(r) > 1e-12, 1.0 / np.where(r == 0, 1, r), 0.0)
    f = (e1 * d2[:, 1:2] - e2 * d1[:, 1:2]) * r[:, None]
    for k in range(3):
        np.add.at(t, tris[:, k], f)
    t -= n * np.sum(t * n, 1, keepdims=True)
    length = np.linalg.norm(t, axis=1, keepdims=True)
    t = np.where(length > 1e-9, t / np.where(length > 0, length, 1), np.array([1.0, 0, 0]))
    return t, np.cross(n, t)


def _byte(v):
    return np.clip(np.round((v + 1.0) * 127.5), 0, 255).astype(np.uint8)


def _transform(r, t, scale=1.0):
    return struct.pack("<9f3ff", *np.asarray(r, float).ravel(), *t, scale)


def _avobject(name_idx, r=np.eye(3), t=(0, 0, 0)):
    b = struct.pack("<iIi", name_idx, 0, -1)                 # name, extra data, controller
    b += struct.pack("<I", 0x0008000E)                       # flags
    b += struct.pack("<3f", *t) + struct.pack("<9f", *np.asarray(r, float).ravel()) + struct.pack("<f", 1.0)
    b += struct.pack("<i", -1)                               # collision
    return b


def _node(name_idx, children, r=np.eye(3), t=(0, 0, 0)):
    return _avobject(name_idx, r, t) + struct.pack("<I", len(children)) + \
        struct.pack("<%di" % len(children), *children) + struct.pack("<I", 0)


def _vertices(s, skinned):
    n = len(s.p)
    t, bt = _tangents(s.p, s.n, s.uv, s.tris)
    fields = [("p", "<f4", 3), ("btx", "<f4"), ("uv", "<f2", 2), ("n", "u1", 3), ("bty", "u1"),
              ("t", "u1", 3), ("btz", "u1")]
    if skinned:
        fields += [("w", "<f2", 4), ("i", "u1", 4)]
    v = np.zeros(n, dtype=fields)
    v["p"], v["uv"] = s.p, s.uv
    v["n"], v["t"] = _byte(s.n), _byte(bt)        # Skyrim's "tangent" slot holds the bitangent
    v["btx"] = t[:, 0]
    v["bty"], v["btz"] = _byte(t[:, 1]), _byte(t[:, 2])
    if skinned:
        v["w"], v["i"] = s.bw, s.bi
    size = 40 if skinned else 28
    desc = (size // 4) | (4 << 8) | (5 << 16) | (6 << 20) | ((7 << 28) if skinned else 0) | \
        ((VF_FULL | (VF_SKINNED if skinned else 0)) << 44)
    return v.tobytes(), desc, size


def write(path, shapes, root_name="JC2Mech", bones=()):
    strings = [root_name]

    def name(s):
        strings.append(s)
        return len(strings) - 1

    types = ["NiNode", "BSTriShape", "BSLightingShaderProperty", "BSShaderTextureSet",
             "NiSkinInstance", "NiSkinData", "NiSkinPartition"]
    blocks = [None]  # block 0 = root, filled last

    # Bones first (block indices known before the skins reference them). World bind transforms
    # are in root space.
    bone_block, bone_world = [], []
    for b in bones:
        bone_block.append(len(blocks))
        blocks.append(None)
        pr, pt = (bone_world[b.parent] if b.parent >= 0 else (np.eye(3), np.zeros(3)))
        bone_world.append((pr @ b.r, pr @ b.t + pt))
    for i, b in enumerate(bones):
        kids = [bone_block[k] for k, c in enumerate(bones) if c.parent == i]
        blocks[bone_block[i]] = (0, _node(name(b.name), kids, b.r, b.t))
    top_bones = [bone_block[i] for i, b in enumerate(bones) if b.parent < 0]

    shape_blocks = []
    for s in shapes:
        skinned = s.bi is not None
        n, tri = len(s.p), s.tris.astype("<u2")
        if n > 65535 or len(tri) > 65535:
            raise ValueError("BSTriShape limit")
        vdata, desc, vsize = _vertices(s, skinned)
        c = (s.p.min(0) + s.p.max(0)) / 2
        radius = float(np.linalg.norm(s.p - c, axis=1).max())
        me = len(blocks)
        shape_blocks.append(me)
        blocks += [None, None, None]
        sh = struct.pack("<IiIi", 0, -1, 0, -1)
        sh += struct.pack("<II4fi", FLAGS1_SKINNED if skinned else FLAGS1,
                          FLAGS2 | (DOUBLE_SIDED if s.double_sided else 0), 0, 0, 1, 1, me + 2)
        # emissive color, multiple, clamp mode, alpha, refraction, glossiness, specular color,
        # specular strength, lighting effects 1 and 2
        sh += struct.pack("<3ffIfff3ffff", 0, 0, 0, 1.0, 3, 1.0, 0.0, 30.0, 1, 1, 1, 0.3, 0.3, 2.0)
        tex = (list(s.textures) + [""] * 9)[:9]
        ts = struct.pack("<i", 9) + b"".join(struct.pack("<I", len(x)) + x.encode() for x in tex)
        blocks[me + 1], blocks[me + 2] = (2, sh), (3, ts)

        skin_ref = -1
        if skinned:
            skin_ref = len(blocks)
            blocks += [None, None, None]
            nb = len(bones)
            inst = struct.pack("<iii", skin_ref + 1, skin_ref + 2, 0) + struct.pack("<I", nb) + \
                struct.pack("<%di" % nb, *bone_block)
            data = _transform(np.eye(3), (0, 0, 0)) + struct.pack("<IB", nb, 0)
            for k in range(nb):
                wr, wt = bone_world[k]
                inv_r, inv_t = wr.T, -wr.T @ wt               # mesh space -> bone space
                used = np.any((s.bi == k) & (s.bw > 0), axis=1)
                pts = (s.p[used] if used.any() else s.p) @ inv_r.T + inv_t
                center = (pts.min(0) + pts.max(0)) / 2
                rad = float(np.linalg.norm(pts - center, axis=1).max())
                data += _transform(inv_r, inv_t) + struct.pack("<3ff", *center, rad) + struct.pack("<H", 0)
            part = struct.pack("<I", 1) + struct.pack("<IIQ", len(vdata), vsize, desc) + vdata
            part += struct.pack("<5H", n, len(tri), nb, 0, 4) + struct.pack("<%dH" % nb, *range(nb))
            part += struct.pack("<B", 1) + np.arange(n, dtype="<u2").tobytes()
            part += struct.pack("<B", 1) + s.bw.astype("<f4").tobytes()
            part += struct.pack("<B", 1) + tri.tobytes()
            part += struct.pack("<B", 1) + s.bi.astype("u1").tobytes()
            part += struct.pack("<BBQ", 0, 1, desc) + tri.tobytes()
            blocks[skin_ref], blocks[skin_ref + 1], blocks[skin_ref + 2] = (4, inst), (5, data), (6, part)

        b = _avobject(name(s.name))
        b += struct.pack("<3ff", *c, radius)
        b += struct.pack("<iii", skin_ref, me + 1, -1)       # skin, shader, alpha
        if skinned:  # SSE keeps skinned vertex data in the partition
            b += struct.pack("<QHHI", desc, len(tri), n, 0) + struct.pack("<I", 0)
        else:
            payload = vdata + tri.tobytes()
            b += struct.pack("<QHHI", desc, len(tri), n, len(payload)) + payload + struct.pack("<I", 0)
        blocks[me] = (1, b)

    blocks[0] = (0, _node(0, shape_blocks + top_bones))

    out = bytearray(b"Gamebryo File Format, Version 20.2.0.7\n")
    out += struct.pack("<IBIII", 0x14020007, 1, 12, len(blocks), 100)
    for s in ("jc2mech", "", ""):
        out += struct.pack("<B", len(s) + 1) + s.encode() + b"\0"
    out += struct.pack("<H", len(types))
    for t in types:
        out += struct.pack("<I", len(t)) + t.encode()
    out += struct.pack("<%dH" % len(blocks), *[t for t, _ in blocks])
    out += struct.pack("<%dI" % len(blocks), *[len(b) for _, b in blocks])
    out += struct.pack("<II", len(strings), max(len(s) for s in strings))
    for s in strings:
        out += struct.pack("<I", len(s)) + s.encode()
    out += struct.pack("<I", 0)  # groups
    for _, b in blocks:
        out += b
    out += struct.pack("<Ii", 1, 0)  # footer: one root, block 0
    open(path, "wb").write(bytes(out))
