"""JC2 (Havok 5.5.0-r1) skeletons (.bsk) and spline-compressed animations (.ban) -> numpy.
Spline decoding follows HavokLib (hka_spline_decompressor.cpp). Quaternions are (x, y, z, w).

python jc2havok.py <file.bsk|file.ban>   prints a summary"""
import math, struct, sys
import numpy as np
from hkpack import load


class Skeleton:
    def __init__(self, names, parents, ref):
        self.names, self.parents, self.ref = names, np.array(parents), ref  # ref: (n, 10) t(3) q(4) s(3)
        self.index = {n: i for i, n in enumerate(names)}


def read_skeleton(path):
    """First hkaSkeleton of a JC2 .bsk (5.5) or a Skyrim skeleton.hkx (2010, 64-bit)."""
    pf = load(path)
    o = pf.find("hkaSkeleton")[0]
    if pf.ptr_size == 4:
        par, npar = pf.array(o + 4)
        bones, nb = pf.array(o + 12)
        ref, nref = pf.array(o + 20)
        names = [pf.cstr(pf.p(bones + 4 * i)) for i in range(nb)]
    else:
        par, npar = pf.array(o + 24)
        bones, nb = pf.array(o + 40)
        ref, nref = pf.array(o + 56)
        names = [pf.cstr(bones + 16 * i) for i in range(nb)]
    parents = [pf.i16(par + 2 * i) for i in range(npar)]
    r = np.zeros((nref, 10), np.float32)
    for i in range(nref):
        b = ref + 48 * i
        r[i, 0:3] = pf.f32(b, 3)
        r[i, 3:7] = pf.f32(b + 16, 4)
        r[i, 7:10] = pf.f32(b + 32, 3)
    return Skeleton(names, parents, r)


# --- spline block decoding -------------------------------------------------------------------

def _q40(b, o):
    v = int.from_bytes(b[o:o + 5], "little")
    f = 0.000345436
    c = [(((v >> s) & 0xFFF) - 2047) * f for s in (0, 12, 24)]
    w = math.sqrt(max(0.0, 1.0 - sum(x * x for x in c)))
    if (v >> 38) & 1:
        w = -w
    return _place(c, w, (v >> 36) & 3), o + 5


def _q48(b, o):
    x, y, z = struct.unpack_from("<hhh", b, o)
    shift = ((y >> 14) & 2) | ((x >> 15) & 1)
    f = 0.000043161
    c = [((v & 0x7FFF) - 16383) * f for v in (x, y, z)]
    w = math.sqrt(max(0.0, 1.0 - sum(v * v for v in c)))
    if z < 0:
        w = -w
    return _place(c, w, shift), o + 6


def _place(c, w, shift):
    if shift == 0: return (w, c[0], c[1], c[2])
    if shift == 1: return (c[0], w, c[1], c[2])
    if shift == 2: return (c[0], c[1], w, c[2])
    return (c[0], c[1], c[2], w)


def _quat(qt, b, o):
    if qt == 3: return _q40(b, o)
    if qt == 4: return _q48(b, o)
    if qt == 7: return struct.unpack_from("<4f", b, o), o + 16
    raise NotImplementedError("rotation quantization %d" % qt)


def _pad(o, a=4):
    return (o + a - 1) & ~(a - 1)


def _span(degree, t, n, knots):
    if t >= knots[n]:
        return n - 1
    lo, hi = degree, n
    mid = (lo + hi) // 2
    while t < knots[mid] or t >= knots[mid + 1]:
        if t < knots[mid]:
            hi = mid
        else:
            lo = mid
        mid = (lo + hi) // 2
    return mid


def _eval(degree, t, knots, pts):
    """pts: (n, k) control points -> (k,)"""
    n = len(pts)
    if n == 1:
        return pts[0]
    s = _span(degree, t, n, knots)
    N = [1.0] + [0.0] * degree
    for i in range(1, degree + 1):
        for j in range(i - 1, -1, -1):
            A = (t - knots[s - j]) / (knots[s + i - j] - knots[s - j])
            tmp = N[j] * A
            N[j + 1] += N[j] - tmp
            N[j] = tmp
    return sum(pts[s - i] * N[i] for i in range(degree + 1))


class _Vec:
    """Position / scale sub-track: per axis static, spline or default."""
    def __init__(self, b, o, qt, types, default):
        static = [(types >> i) & 1 for i in range(3)]
        spline = [bool((types >> (4 + i)) & 1) and not static[i] for i in range(3)]
        self.value = [default] * 3
        self.spline = None
        if any(spline):
            n = struct.unpack_from("<H", b, o)[0]
            self.degree = b[o + 2]
            o += 3
            self.knots = list(b[o:o + n + self.degree + 2])
            o = _pad(o + n + self.degree + 2)
            ext = [None] * 3
            for i in range(3):
                if spline[i]:
                    ext[i] = struct.unpack_from("<2f", b, o)
                    o += 8
                elif static[i]:
                    self.value[i] = struct.unpack_from("<f", b, o)[0]
                    o += 4
            pts = np.tile(np.array(self.value, np.float64), (n + 1, 1))
            for p in range(n + 1):
                for i in range(3):
                    if spline[i]:
                        if qt == 0:
                            q = b[o] / 255.0
                            o += 1
                        else:
                            q = struct.unpack_from("<H", b, o)[0] / 65535.0
                            o += 2
                        pts[p, i] = ext[i][0] + (ext[i][1] - ext[i][0]) * q
            self.spline = pts
            o = _pad(o)
        else:
            for i in range(3):
                if static[i]:
                    self.value[i] = struct.unpack_from("<f", b, o)[0]
                    o += 4
        self.end = o

    def at(self, t):
        return _eval(self.degree, t, self.knots, self.spline) if self.spline is not None else np.array(self.value)


class _Rot:
    def __init__(self, b, o, qt, types):
        self.spline = None
        self.value = (0.0, 0.0, 0.0, 1.0)
        if types & 0xF0:
            n = struct.unpack_from("<H", b, o)[0]
            self.degree = b[o + 2]
            o += 3
            self.knots = list(b[o:o + n + self.degree + 2])
            o += n + self.degree + 2
            if qt in (4, 6):
                o = _pad(o, 2)
            elif qt in (2, 7):
                o = _pad(o)
            pts = []
            for _ in range(n + 1):
                q, o = _quat(qt, b, o)
                pts.append(q)
            self.spline = np.array(pts)
        elif types & 0x0F:
            self.value, o = _quat(qt, b, o)
        self.end = _pad(o)

    def at(self, t):
        if self.spline is None:
            return np.array(self.value)
        q = _eval(self.degree, t, self.knots, self.spline)
        return q / np.linalg.norm(q)


class Animation:
    """frames: (num_frames, num_tracks, 10) local t(3) q(4) s(3); track_names; duration."""


# Field offsets: Havok 5.5 32-bit (JC2) and 2010.2 64-bit (Skyrim SE).
LAYOUT = {
    4: dict(cls="hkaSplineSkeletalAnimation", duration=12, ntracks=16, nfloat=20, ann=28, ann_stride=4,
            frames=36, block_offsets=64, data=112),
    8: dict(cls="hkaSplineCompressedAnimation", duration=20, ntracks=24, nfloat=28, ann=40, ann_stride=24,
            frames=56, block_offsets=88, data=152),
}


def _read_interleaved(pf, o):
    """Havok 5.5 hkaInterleavedSkeletalAnimation: transforms (ptr, count) at 36, frames x tracks."""
    a = Animation()
    a.duration = pf.f32(o + 12)
    ntracks = pf.i32(o + 16)
    ann, nann = pf.array(o + 28)
    a.track_names = [(pf.cstr(pf.p(ann + 4 * i)) or "").replace("_AnimationTrack", "") for i in range(nann)]
    tr, n = pf.array(o + 36)
    frames = n // ntracks
    a.frames = np.zeros((frames, ntracks, 10), np.float32)
    for f in range(frames):
        for t in range(ntracks):
            b = tr + 48 * (f * ntracks + t)
            a.frames[f, t, 0:3] = pf.f32(b, 3)
            a.frames[f, t, 3:7] = pf.f32(b + 16, 4)
            a.frames[f, t, 7:10] = pf.f32(b + 32, 3)
    a.frame_duration = a.duration / max(frames - 1, 1)
    return a


def read_animation(path):
    pf = load(path)
    L = LAYOUT[pf.ptr_size]
    if pf.ptr_size == 4 and pf.find("hkaInterleavedSkeletalAnimation"):
        return _read_interleaved(pf, pf.find("hkaInterleavedSkeletalAnimation")[0])
    o = pf.find(L["cls"])[0]
    a = Animation()
    a.duration = pf.f32(o + L["duration"])
    ntracks, nfloat = pf.i32(o + L["ntracks"]), pf.i32(o + L["nfloat"])
    ann, nann = pf.array(o + L["ann"])
    if pf.ptr_size == 4:  # array of pointers to tracks
        names = [pf.cstr(pf.p(ann + 4 * i)) for i in range(nann)]
    else:  # inline hkaAnnotationTrack structs, name first
        names = [pf.cstr(ann + L["ann_stride"] * i) for i in range(nann)]
    a.track_names = [(n or "").replace("_AnimationTrack", "") for n in names]
    f = L["frames"]
    num_frames, num_blocks, max_fpb = pf.i32(o + f), pf.i32(o + f + 4), pf.i32(o + f + 8)
    a.frame_duration = pf.f32(o + f + 24)
    boff, _ = pf.array(o + L["block_offsets"])
    data, ndata = pf.array(o + L["data"])
    b = pf.data[data:data + ndata]
    blocks = []
    for k in range(num_blocks):
        start = pf.u32(boff + 4 * k)
        masks = [b[start + 4 * i:start + 4 * i + 4] for i in range(ntracks)]
        p = _pad(start + 4 * ntracks + nfloat)
        tracks = []
        for qt, pt, rt, st in masks:
            pos = _Vec(b, p, qt & 3, pt, 0.0)
            rot = _Rot(b, pos.end, ((qt >> 2) & 0xF) + 2, rt)
            scl = _Vec(b, rot.end, (qt >> 6) & 3, st, 1.0)
            p = scl.end
            tracks.append((pos, rot, scl))
        blocks.append(tracks)
    a.frames = np.zeros((num_frames, ntracks, 10), np.float32)
    for f in range(num_frames):
        k = min(f // (max_fpb - 1), num_blocks - 1)
        t = float(f - k * (max_fpb - 1))
        for i, (pos, rot, scl) in enumerate(blocks[k]):
            a.frames[f, i, 0:3] = pos.at(t)
            a.frames[f, i, 3:7] = rot.at(t)
            a.frames[f, i, 7:10] = scl.at(t)
    return a


if __name__ == "__main__":
    p = sys.argv[1]
    if p.endswith(".bsk") or "skeleton" in p.lower():
        s = read_skeleton(p)
        for i, n in enumerate(s.names):
            print(i, n, s.parents[i], np.round(s.ref[i], 3))
    else:
        a = read_animation(p)
        print("%.3f s, %d frames, %d tracks" % (a.duration, len(a.frames), a.frames.shape[1]))
        for i, n in enumerate(a.track_names[:10]):
            print(n, np.round(a.frames[0, i], 3), np.round(a.frames[-1, i], 3))
