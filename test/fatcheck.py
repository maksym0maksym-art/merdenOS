#!/usr/bin/env python3
"""Незалежний перевіряльник FAT32 (fsck-подібний) + дамп дерева.
   fatcheck.py image [--dump DIR]   -> exit 0 якщо файлова система узгоджена; друкує дерево 'path size sha1'"""
import struct, sys, hashlib, os

def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]

class Fat:
    def __init__(self, path):
        self.img = open(path, "rb").read()
        self.errors = []
        self.extract = None
        self.locate()
        self.parse()

    def err(self, m): self.errors.append(m)

    def locate(self):
        im = self.img
        # без таблиці?
        def valid(b):
            return b[510:512] == b"\x55\xAA" and u16(b, 11) in (512, 1024, 2048, 4096) and u16(b, 17) == 0 and u32(b, 36) != 0
        if valid(im[0:512]): self.start = 0; return
        if im[510:512] != b"\x55\xAA": raise SystemExit("no signature")
        for i in range(4):
            e = im[446 + 16 * i: 462 + 16 * i]
            if e[4] in (0x0B, 0x0C):
                for bs in (512, 4096):
                    self.start = u32(e, 8) * bs
                    if valid(im[self.start: self.start + 512]): return
            if e[4] == 0xEE:
                # GPT
                gbs = 512 if im[512:520] == b"EFI PART" else 4096
                h = im[gbs:gbs + 92]
                lba = struct.unpack_from("<Q", h, 72)[0]; n = u32(h, 80); sz = u32(h, 84)
                for k in range(n):
                    ent = im[lba * gbs + k * sz: lba * gbs + (k + 1) * sz]
                    if not any(ent[:16]): continue
                    first = struct.unpack_from("<Q", ent, 32)[0]
                    for bs in (gbs,):
                        self.start = first * bs
                        if valid(im[self.start: self.start + 512]): return
        raise SystemExit("no FAT32 volume")

    def parse(self):
        b = self.img[self.start: self.start + 512]
        self.bps = u16(b, 11); self.spc = b[13]; self.res = u16(b, 14); self.nf = b[16]
        self.total = u32(b, 32); self.fsz = u32(b, 36); self.root = u32(b, 44); self.fsinfo = u16(b, 48)
        self.flags = u16(b, 40)
        self.data_start = self.res + self.nf * self.fsz
        self.nclus = (self.total - self.data_start) // self.spc
        self.cb = self.bps * self.spc
        self.vol = self.img[self.start:]
        self.fats = []
        for k in range(self.nf):
            o = (self.res + k * self.fsz) * self.bps
            self.fats.append(self.vol[o: o + self.fsz * self.bps])
        self.act = self.fats[0] if not (self.flags & 0x80) else self.fats[self.flags & 0xF]

    def fat(self, c): return u32(self.act, c * 4) & 0x0FFFFFFF
    def csec(self, c): return self.data_start + (c - 2) * self.spc
    def cdata(self, c):
        o = self.csec(c) * self.bps
        return self.vol[o: o + self.cb]

    def chain(self, c, who=""):
        out = []; seen = set()
        while 2 <= c <= self.nclus + 1:
            if c in seen: self.err(f"{who}: loop at cluster {c}"); break
            seen.add(c); out.append(c)
            n = self.fat(c)
            if n >= 0x0FFFFFF8: return out
            if n < 2 or n > self.nclus + 1: self.err(f"{who}: bad link {c}->{n:#x}"); return out
            c = n
        if c != 0: self.err(f"{who}: chain ended on invalid cluster {c}")
        return out

    def read_dir(self, cl, who):
        data = b"".join(self.cdata(c) for c in self.chain(cl, who))
        res = []; lfn = None
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if e[0] == 0: 
                if any(any(data[j:j+32]) for j in range(i, len(data), 32)): self.err(f"{who}: non-zero entries after end marker")
                break
            if e[0] == 0xE5: lfn = None; continue
            if e[11] & 0x3F == 0x0F:
                o = e[0]
                if o & 0x40: lfn = {"n": o & 0x3F, "sum": e[13], "parts": {}, "next": (o & 0x3F)}
                elif lfn is None or (o & 0x3F) != lfn["next"] - 1: self.err(f"{who}: LFN sequence error at {i//32}"); lfn = None; continue
                else: lfn["next"] = o & 0x3F
                if lfn is None: continue
                lfn["next"] = o & 0x3F
                chars = e[1:11] + e[14:26] + e[28:32]
                lfn["parts"][o & 0x3F] = chars
                if e[12] != 0 or u16(e, 26) != 0: self.err(f"{who}: LFN type/cluster nonzero")
                continue
            if e[11] & 0x08: lfn = None; continue
            name = None
            sn = bytes(e[0:11])
            if lfn is not None:
                if lfn["next"] != 1: self.err(f"{who}: incomplete LFN before {sn!r}")
                s = 0
                for ch in sn: s = (((s & 1) << 7) + (s >> 1) + ch) & 0xFF
                if s != lfn["sum"]: self.err(f"{who}: LFN checksum mismatch for {sn!r}")
                raw = b"".join(lfn["parts"][k] for k in sorted(lfn["parts"]))
                txt = raw.decode("utf-16-le")
                txt = txt.split("\x00")[0].rstrip("￿")
                name = txt
                padok = True
            else:
                base = sn[:8].rstrip(b" ").decode("latin-1"); ext = sn[8:].rstrip(b" ").decode("latin-1")
                if e[12] & 0x08: base = base.lower()
                if e[12] & 0x10: ext = ext.lower()
                name = base + ("." + ext if ext else "")
            lfn = None
            cl_ = (u16(e, 20) << 16) | u16(e, 26)
            res.append(dict(name=name, sn=sn, attr=e[11], cl=cl_, size=u32(e, 28), idx=i // 32))
        return res

    def walk(self):
        used = {}
        tree = []
        def claim(chain, who):
            for c in chain:
                if c in used: self.err(f"cross-link: cluster {c} used by {used[c]} and {who}")
                used[c] = who
        claim(self.chain(self.root, "/"), "/")
        def rec(cl, path):
            ents = self.read_dir(cl, path)
            names = set()
            for e in ents:
                if e["sn"][:2] in (b". ", b"..") and e["sn"][0] == 0x2E:
                    continue
                full = path.rstrip("/") + "/" + e["name"]
                key = e["name"].lower()
                if key in names: self.err(f"duplicate name {full}")
                names.add(key)
                if e["attr"] & 0x10:
                    if e["cl"] < 2: self.err(f"{full}: dir with cluster {e['cl']}"); continue
                    ch = self.chain(e["cl"], full); claim(ch, full)
                    d = self.cdata(e["cl"])
                    if d[0:1] != b"." or d[32:34] != b"..": self.err(f"{full}: missing . / ..")
                    else:
                        dd = (u16(d, 20) << 16) | u16(d, 26)
                        if dd != e["cl"]: self.err(f"{full}: '.' points to {dd}")
                        pp = (u16(d, 52) << 16) | u16(d, 58)
                        exp = 0 if cl == self.root else cl
                        if pp != exp: self.err(f"{full}: '..' points to {pp}, expected {exp}")
                    if self.extract: os.makedirs(os.path.join(self.extract, *full.strip("/").split("/")), exist_ok=True)
                    tree.append((full + "/", 0, ""))
                    rec(e["cl"], full)
                else:
                    ch = self.chain(e["cl"], full) if e["cl"] else []
                    claim(ch, full)
                    need = (e["size"] + self.cb - 1) // self.cb
                    if len(ch) != need: self.err(f"{full}: size {e['size']} needs {need} clusters, chain has {len(ch)}")
                    data = b"".join(self.cdata(c) for c in ch)[:e["size"]]
                    if self.extract:
                        outp = os.path.join(self.extract, *[x.encode("utf-8", "replace").decode("utf-8") for x in full.strip("/").split("/")])
                        os.makedirs(os.path.dirname(outp), exist_ok=True)
                        open(outp, "wb").write(data)
                    tree.append((full, e["size"], hashlib.sha1(data).hexdigest()))
        rec(self.root, "/")
        # FAT copies
        if not (self.flags & 0x80):
            for k in range(1, self.nf):
                if self.fats[k] != self.fats[0]: self.err(f"FAT copy {k} differs from FAT 0")
        # orphan clusters / free count
        free = 0
        for c in range(2, self.nclus + 2):
            v = self.fat(c)
            if v == 0: free += 1
            elif c not in used: self.err(f"orphan cluster {c} (FAT={v:#x})")
        fi = self.vol[self.fsinfo * self.bps:(self.fsinfo + 1) * self.bps]
        if u32(fi, 0) == 0x41615252:
            fc = u32(fi, 488)
            if fc != 0xFFFFFFFF and fc != free: self.err(f"FSInfo free count {fc} != real {free}")
        self.free = free
        return sorted(tree)

if __name__ == "__main__":
    f = Fat(sys.argv[1])
    if "--extract" in sys.argv: f.extract = sys.argv[sys.argv.index("--extract") + 1]
    tree = f.walk()
    if "--quiet" not in sys.argv:
        for p, s, h in tree: print(f"{p}\t{s}\t{h}")
    for e in f.errors: print("FSCK ERROR:", e, file=sys.stderr)
    print(f"free clusters: {f.free}", file=sys.stderr)
    sys.exit(1 if f.errors else 0)
