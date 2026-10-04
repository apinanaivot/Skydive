"""JC2 render block models (.rbm). Reads the two block types the grapple and parachute use:
General (0xA7583B2B) and SkinnedGeneral (0x5E657F20). Layouts worked out from the files
(re/notes.md, "Models"); JC2 units (meters, Y up).

File: u32 5, "RBMDL", u32 1, u32 13, u32 0, bbox 6f, u32 block count, then per block u32 type
hash + data, each block ending in 0x89ABCDEF.
General: u8 3, material constants (position scale first; length varies, so the reader looks for
  the first texture string), 8 strings (u32 length + chars: dif, nrm, mpm,
  ...), u32 3, u32 n, n * 28-byte vertices (uv0 i16x2, uv1 i16x2, 3 packed floats (normal frame),
  position i16x3 / 32767 + pad), u32 index count, u16 triangle list.
SkinnedGeneral: u8 3, u32 flags, 6 floats, 8 strings, u32 3, u32 n, n * (position 3f, 4 u8
  weights, 4 u8 batch bone indices), u32 n, n * (normal, tangent, binormal as u8x4, uv 2f),
  u32 batch count, per batch (u32 index count, u32 first index, u32 bone count, u16 bones),
  u32 index count, u16 triangle list."""
import re, struct
import numpy as np

GENERAL = 0xA7583B2B
SKINNED_GENERAL = 0x5E657F20
END = 0x89ABCDEF
TEXTURE_STRING = re.compile(rb"[\x01-\x7f]\x00\x00\x00[\w\-]+\.dds")


class Mesh:
    """positions (n, 3), uvs (n, 2), triangles (m, 3), textures [str], bones/weights for skinned."""
    def __init__(self):
        self.textures, self.bones, self.weights, self.batch_bones = [], None, None, None


def _strings(d, o, n=8):
    out = []
    for _ in range(n):
        k = struct.unpack_from("<I", d, o)[0]
        out.append(d[o + 4:o + 4 + k].decode("latin1"))
        o += 4 + k
    return out, o


def _general(d, o):
    m = Mesh()
    consts = struct.unpack_from("<3f", d, o + 1)
    first = TEXTURE_STRING.search(d, o + 1)
    m.textures, o = _strings(d, first.start())
    _, n = struct.unpack_from("<II", d, o)
    o += 8
    raw = np.frombuffer(d, np.uint8, 28 * n, o).reshape(n, 28)
    o += 28 * n
    uv = raw[:, 0:4].copy().view("<i2").astype(np.float64) / 32767.0
    pos = raw[:, 20:26].copy().view("<i2").astype(np.float64) / 32767.0
    m.positions = pos * np.array(consts[0:3])
    m.uvs = uv
    k = struct.unpack_from("<I", d, o)[0]
    m.triangles = np.frombuffer(d, "<u2", k, o + 4).reshape(-1, 3).astype(np.int64)
    return m, o + 4 + 2 * k + 4


def _skinned(d, o):
    m = Mesh()
    m.textures, o = _strings(d, o + 1 + 4 + 24)
    _, n = struct.unpack_from("<II", d, o)
    o += 8
    raw = np.frombuffer(d, np.uint8, 20 * n, o).reshape(n, 20)
    o += 20 * n
    m.positions = raw[:, 0:12].copy().view("<f4").astype(np.float64)
    m.weights = raw[:, 12:16] / 255.0
    m.bones = raw[:, 16:20].astype(np.int64)
    n2 = struct.unpack_from("<I", d, o)[0]
    raw2 = np.frombuffer(d, np.uint8, 20 * n2, o + 4).reshape(n2, 20)
    o += 4 + 20 * n2
    m.normals_packed = raw2[:, 0:4] / 127.5 - 1.0
    m.uvs = raw2[:, 12:20].copy().view("<f4").astype(np.float64)
    nb = struct.unpack_from("<I", d, o)[0]
    o += 4
    m.batch_bones = []
    for _ in range(nb):
        cnt, first, nbones = struct.unpack_from("<III", d, o)
        m.batch_bones.append(list(struct.unpack_from("<%dH" % nbones, d, o + 12)))
        o += 12 + 2 * nbones
    k = struct.unpack_from("<I", d, o)[0]
    m.triangles = np.frombuffer(d, "<u2", k, o + 4).reshape(-1, 3).astype(np.int64)
    return m, o + 4 + 2 * k + 4


def read(path):
    """All meshes of an .rbm file."""
    d = open(path, "rb").read()
    if d[4:9] != b"RBMDL":
        raise ValueError("not an RBM model")
    count = struct.unpack_from("<I", d, 0x2D)[0]
    o, meshes = 0x31, []
    for _ in range(count):
        kind = struct.unpack_from("<I", d, o)[0]
        if kind == GENERAL:
            m, o = _general(d, o + 4)
        elif kind == SKINNED_GENERAL:
            m, o = _skinned(d, o + 4)
        else:
            raise NotImplementedError("render block %08x" % kind)
        if struct.unpack_from("<I", d, o - 4)[0] != END:
            raise ValueError("block did not end with the marker; layout guess is off")
        meshes.append(m)
    return meshes


def vertex_normals(pos, tris):
    """Area-weighted smooth normals from the triangle winding."""
    n = np.zeros_like(pos)
    f = np.cross(pos[tris[:, 1]] - pos[tris[:, 0]], pos[tris[:, 2]] - pos[tris[:, 0]])
    for k in range(3):
        np.add.at(n, tris[:, k], f)
    length = np.linalg.norm(n, axis=1, keepdims=True)
    return n / np.where(length > 0, length, 1.0)


if __name__ == "__main__":
    import sys
    for m in read(sys.argv[1]):
        print(len(m.positions), "verts", len(m.triangles), "tris", m.textures[:3],
              "bbox", np.round(m.positions.min(0), 3), np.round(m.positions.max(0), 3),
              "uv", np.round(m.uvs.min(0), 2), np.round(m.uvs.max(0), 2))
        if m.batch_bones:
            g = vertex_normals(m.positions, m.triangles)
            print("  batch bones", m.batch_bones,
                  "winding check (mean dot geometric . packed):", round(float(np.mean(np.sum(g * m.normals_packed[:, :3], 1))), 3))
