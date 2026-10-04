"""Havok binary packfile reader (JC2: Havok 5.5.0-r1, 32-bit; Skyrim SE: 2010.2.0-r1, 64-bit).
Resolves the local / global / virtual fixups into a flat view of the data section: objects by
offset with their class name, and pointers by source offset."""
import struct

MAGIC = (0x57E0E057, 0x10C0C010)


class Section:
    def __init__(self, b, o, base):
        self.tag = b[o:o + 19].split(b"\0")[0].decode()
        (self.start, self.local, self.glob, self.virt, self.exports, self.imports,
         self.end) = struct.unpack_from("<7I", b, o + 20)
        self.data = b[self.start:self.start + self.local]


class Packfile:
    def __init__(self, b):
        self.b = b
        m0, m1, self.user_tag, self.version = struct.unpack_from("<4I", b, 0)
        if (m0, m1) != MAGIC:
            raise ValueError("not a Havok packfile")
        self.ptr_size, self.little, self.reuse_pad, self.empty_base = b[16:20]
        (self.num_sections, self.contents_sec, self.contents_off, self.cname_sec,
         self.cname_off) = struct.unpack_from("<5i", b, 20)
        self.havok = b[40:56].split(b"\0")[0].decode()
        hdr = 0x40
        if self.version >= 11:  # 2010+: flags + max predicate / section offset padding
            pad = struct.unpack_from("<h", b, 0x42)[0] if self.version >= 11 else 0
            hdr = 0x40 + max(pad, 0)
        sec_size = 0x40 if self.version >= 11 else 0x30
        self.sections = [Section(b, hdr + i * sec_size, 0) for i in range(self.num_sections)]
        cn = self.sections[self.cname_sec]
        # class names: (u32 signature, u8 0x09, name\0)
        self.class_at = {}
        self.class_sig = {}
        raw = b[cn.start:cn.start + cn.local]
        o = 0
        while o + 5 < len(raw):
            sig = struct.unpack_from("<I", raw, o)[0]
            if raw[o + 4] != 0x09:
                break
            e = raw.index(b"\0", o + 5)
            name = raw[o + 5:e].decode()
            self.class_at[o + 5] = name
            self.class_sig[name] = sig
            o = e + 1
        self.data_sec = self.contents_sec
        d = self.sections[self.data_sec]
        self.data = d.data
        fix = b[d.start:d.start + d.end]
        self.ptr = {}       # src offset -> dst offset (same section)
        for o in range(d.local, d.glob - 7, 8):
            s, t = struct.unpack_from("<ii", fix, o)
            if s >= 0:
                self.ptr[s] = t
        for o in range(d.glob, d.virt - 11, 12):
            s, sec, t = struct.unpack_from("<iii", fix, o)
            if s >= 0:
                self.ptr[s] = t if sec == self.data_sec else (sec, t)
        self.objects = {}   # offset -> class name
        for o in range(d.virt, d.exports - 11, 12):
            s, sec, t = struct.unpack_from("<iii", fix, o)
            if s >= 0:
                self.objects[s] = self.class_at[t]
        self.order = sorted(self.objects)

    def find(self, cls):
        return [o for o in self.order if self.objects[o] == cls]

    def size_of(self, off):
        i = self.order.index(off)
        return (self.order[i + 1] if i + 1 < len(self.order) else len(self.data)) - off

    # field readers
    def u8(self, o): return self.data[o]
    def i16(self, o): return struct.unpack_from("<h", self.data, o)[0]
    def u16(self, o): return struct.unpack_from("<H", self.data, o)[0]
    def i32(self, o): return struct.unpack_from("<i", self.data, o)[0]
    def u32(self, o): return struct.unpack_from("<I", self.data, o)[0]
    def f32(self, o, n=1):
        v = struct.unpack_from("<%df" % n, self.data, o)
        return v[0] if n == 1 else v

    def p(self, o):
        """Pointer at o -> target offset or None."""
        return self.ptr.get(o)

    def cstr(self, o):
        t = self.p(o)
        if t is None:
            return None
        return self.data[t:self.data.index(b"\0", t)].decode("latin1")

    def array(self, o):
        """(data offset, count) of an array field (pointer then int count)."""
        return self.p(o), self.i32(o + self.ptr_size)


def load(path):
    return Packfile(open(path, "rb").read())


if __name__ == "__main__":
    import sys
    pf = load(sys.argv[1])
    print(pf.havok, "v%d ptr %d sections %s" % (pf.version, pf.ptr_size, [s.tag for s in pf.sections]))
    for o in pf.order:
        n = pf.size_of(o)
        ptrs = sorted((s - o, t) for s, t in pf.ptr.items() if o <= s < o + n and s - o < 0x80)
        print("%06x %-40s size %5d ptrs %s" % (o, pf.objects[o], n, ptrs[:12]))
        print("    " + pf.data[o:o + min(n, 0x60)].hex(" ", 4))
