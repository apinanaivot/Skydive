"""JC2 archive (archives_win32/pcN.tab + .arc) reader. Raw output stays in extracted/ (git-ignored).
tab: u32 alignment, then (u32 name hash, u32 offset, u32 size) per file. Names are hashed with
Bob Jenkins' lookup3 hashlittle(lowercase name, 0); the archive stores no names.
python jc2arc.py stats            magic counts per archive
python jc2arc.py extract <magic>  extract files whose first 4 bytes match (hex), named by hash
python jc2arc.py sarcs <pattern>  list named files inside the SARC sub-archives (zlib or raw)
python jc2arc.py unsarc <regex>   extract matching SARC entries to extracted/<archive hash>/<name>
SARC: u32 4, "SARC", u32 2, u32 header size, then (u32 name len, name, u32 offset, u32 size)."""
import os, re, struct, sys, zlib
from collections import Counter

# The installer (installer/install.py) sets both for the user's install and a temporary folder.
ARC = os.environ.get("JC2MECH_JC2_ARCHIVES") or r"C:\Program Files (x86)\Steam\steamapps\common\Just Cause 2\archives_win32"
OUT = os.environ.get("JC2MECH_EXTRACTED") or os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "extracted"))


def entries(n):
    b = open(os.path.join(ARC, "pc%d.tab" % n), "rb").read()
    return [struct.unpack_from("<III", b, o) for o in range(4, len(b) - 11, 12)]


def rot(x, k):
    return ((x << k) | (x >> (32 - k))) & 0xFFFFFFFF


def hashlittle(data, init=0):
    a = b = c = (0xDEADBEEF + len(data) + init) & 0xFFFFFFFF
    i, n = 0, len(data)
    M = 0xFFFFFFFF
    while n > 12:
        a = (a + struct.unpack_from("<I", data, i)[0]) & M
        b = (b + struct.unpack_from("<I", data, i + 4)[0]) & M
        c = (c + struct.unpack_from("<I", data, i + 8)[0]) & M
        a = (a - c) & M; a ^= rot(c, 4); c = (c + b) & M
        b = (b - a) & M; b ^= rot(a, 6); a = (a + c) & M
        c = (c - b) & M; c ^= rot(b, 8); b = (b + a) & M
        a = (a - c) & M; a ^= rot(c, 16); c = (c + b) & M
        b = (b - a) & M; b ^= rot(a, 19); a = (a + c) & M
        c = (c - b) & M; c ^= rot(b, 4); b = (b + a) & M
        i += 12; n -= 12
    if n == 0:
        return c
    tail = data[i:] + b"\0" * (12 - n)
    a = (a + struct.unpack_from("<I", tail, 0)[0]) & M
    b = (b + struct.unpack_from("<I", tail, 4)[0]) & M
    c = (c + struct.unpack_from("<I", tail, 8)[0]) & M
    c ^= b; c = (c - rot(b, 14)) & M
    a ^= c; a = (a - rot(c, 11)) & M
    b ^= a; b = (b - rot(a, 25)) & M
    c ^= b; c = (c - rot(b, 16)) & M
    a ^= c; a = (a - rot(c, 4)) & M
    b ^= a; b = (b - rot(a, 14)) & M
    c ^= b; c = (c - rot(b, 24)) & M
    return c


def name_hash(name):
    return hashlittle(name.lower().encode())


def files():
    for n in range(5):
        f = open(os.path.join(ARC, "pc%d.arc" % n), "rb")
        for h, off, size in entries(n):
            yield n, h, off, size, f


def data(f, off, size):
    f.seek(off)
    d = f.read(size)
    if d[:2] == b"x":
        try:
            d = zlib.decompress(d)
        except zlib.error:
            pass
    return d


def sarc_entries(d):
    if d[4:8] != b"SARC":
        return []
    end = 16 + struct.unpack_from("<I", d, 12)[0]
    o, out = 16, []
    while o + 4 <= end:
        n = struct.unpack_from("<I", d, o)[0]
        if n == 0 or o + 4 + n + 8 > end:
            break
        name = d[o + 4:o + 4 + n].rstrip(b"\0").decode("latin1")
        off, size = struct.unpack_from("<II", d, o + 4 + n)
        out.append((name, off, size))
        o += 4 + n + 8
    return out


def sarcs():
    """(archive hash, sarc bytes, entries) for every SARC."""
    for n, h, off, size, f in files():
        f.seek(off)
        head = f.read(8)
        if head[:2] != b"x" and head[4:8] != b"SARC":
            continue
        d = data(f, off, size)
        e = sarc_entries(d)
        if e:
            yield h, d, e


def unsarc(pattern):
    """Extracts the SARC entries whose name matches to OUT/<archive hash>/<name> (first copy of
    each name). Returns the count."""
    pat = re.compile(pattern, re.I)
    seen = set()
    for h, d, e in sarcs():
        for name, off, size in e:
            if pat.search(name) and name not in seen:
                seen.add(name)
                os.makedirs(os.path.join(OUT, "%08x" % h), exist_ok=True)
                open(os.path.join(OUT, "%08x" % h, name), "wb").write(d[off:off + size])
    return len(seen)


if __name__ == "__main__":
    if sys.argv[1] == "stats":
        c = Counter()
        for n, h, off, size, f in files():
            f.seek(off); c[f.read(4)] += 1
        for m, k in c.most_common(40):
            print(m.hex(), m, k, flush=True)
    elif sys.argv[1] == "extract":
        want = bytes.fromhex(sys.argv[2])
        os.makedirs(OUT, exist_ok=True)
        k = 0
        for n, h, off, size, f in files():
            f.seek(off)
            d = f.read(size)
            if d[:len(want)] == want:
                open(os.path.join(OUT, "%08x.bin" % h), "wb").write(d); k += 1
        print("extracted", k, flush=True)
    elif sys.argv[1] == "sarcs":
        pat = re.compile(sys.argv[2] if len(sys.argv) > 2 else ".", re.I)
        for h, d, e in sarcs():
            for name, off, size in e:
                if pat.search(name):
                    print("%08x %-50s %8d" % (h, name, size), flush=True)
    elif sys.argv[1] == "unsarc":
        print("extracted", unsarc(sys.argv[2]), flush=True)
