"""Skyrim SE BSA (v105) reader.
python bsa.py <archive> [regex]          list files
python bsa.py <archive> <regex> <outdir>  extract matching files (LZ4-compressed ones need the lz4 module)"""
import os, re, struct, sys

SKYRIM_DATA = r"C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\Data"


class Bsa:
    def __init__(self, path):
        self.path = path
        b = self.b = open(path, "rb").read()
        magic, ver, off, self.flags, nfold, nfile, lfold, lfile, fflags = struct.unpack_from("<4sIIIIIIII", b, 0)
        if magic != b"BSA\0" or ver != 105:
            raise ValueError("not a v105 BSA")
        folders = [struct.unpack_from("<QIIQ", b, off + 24 * i) for i in range(nfold)]
        o = off + 24 * nfold
        recs = []
        for _, count, _, _ in folders:
            n = b[o]
            name = b[o + 1:o + n].decode("latin1")
            o += 1 + n
            for _ in range(count):
                h, size, foff = struct.unpack_from("<QII", b, o)
                recs.append((name, size, foff))
                o += 16
        names = b[o:o + lfile].split(b"\0")
        self.files = {}
        for (folder, size, foff), fn in zip(recs, names):
            self.files[(folder + "\\" + fn.decode("latin1")).lower()] = (size, foff)

    def read(self, name):
        size, off = self.files[name.lower()]
        compressed = bool(self.flags & 4) ^ bool(size & 0x40000000)
        size &= 0x3FFFFFFF
        d = self.b[off:off + size]
        if self.flags & 0x100:  # embedded file names
            d = d[1 + d[0]:]
        if compressed:
            import lz4.frame
            d = lz4.frame.decompress(d[4:])
        return d


if __name__ == "__main__":
    a = Bsa(sys.argv[1] if os.path.isabs(sys.argv[1]) else os.path.join(SKYRIM_DATA, sys.argv[1]))
    pat = re.compile(sys.argv[2] if len(sys.argv) > 2 else ".", re.I)
    hits = [n for n in sorted(a.files) if pat.search(n)]
    if len(sys.argv) > 3:
        for n in hits:
            p = os.path.join(sys.argv[3], n.replace("\\", os.sep))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "wb").write(a.read(n))
        print("extracted", len(hits))
    else:
        for n in hits:
            print(n, a.files[n][0] & 0x3FFFFFFF)
