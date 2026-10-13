#!/usr/bin/env python3
"""Незалежний перевіряльник exFAT: exfatcheck.py image [--quiet] [--extract DIR]
Перевіряє: контрольну суму boot-регіону й резервну копію, FAT, бітову карту, набори записів
(контрольна сума, хеш імені, кількість вторинних), унікальність імен, довжини, перехресні посилання."""
import struct, sys, os, argparse

def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]

class XF:
    def __init__(self, path):
        self.img = open(path, "rb").read(); self.errors = []
        self.locate(); self.parse()
    def err(self, m): self.errors.append(m)
    def locate(self):
        im = self.img
        def valid(b): return b[3:11] == b"EXFAT   " and b[510:512] == b"\x55\xAA"
        if valid(im[0:512]): self.start = 0; return
        if im[510:512] != b"\x55\xAA": raise SystemExit("no signature")
        for bs in (512, 4096):
            for i in range(4):
                e = im[446 + 16 * i: 462 + 16 * i]
                if e[4] == 0xEE:
                    if im[bs:bs + 8] != b"EFI PART": continue
                    h = im[bs:bs + 92]; lba = u64(h, 72); n = u32(h, 80); sz = u32(h, 84)
                    for k in range(n):
                        ent = im[lba * bs + k * sz: lba * bs + (k + 1) * sz]
                        if not any(ent[:16]): continue
                        s = u64(ent, 32) * bs
                        if valid(im[s:s + 512]): self.start = s; return
                elif e[4] not in (0, 5, 0xF) and u32(e, 8):
                    s = u32(e, 8) * bs
                    if valid(im[s:s + 512]): self.start = s; return
        raise SystemExit("no exFAT volume")
    def parse(self):
        v = self.vol = self.img[self.start:]
        b = v[:512]
        self.bps = 1 << b[108]; self.spc = 1 << b[109]; self.cb = self.bps * self.spc
        self.fat_off = u32(b, 80); self.fat_len = u32(b, 84); self.heap = u32(b, 88)
        self.n = u32(b, 92); self.root = u32(b, 96); self.nfats = b[110]
        bps = self.bps
        # контрольна сума boot-регіону
        reg = v[:bps * 11]; c = 0
        for i, x in enumerate(reg):
            if i in (106, 107, 112): continue
            c = (((c << 31) | (c >> 1)) + x) & 0xFFFFFFFF
        for k in range(bps // 4):
            if u32(v, bps * 11 + 4 * k) != c: self.err("boot checksum mismatch"); break
        if v[:bps * 12] != v[bps * 12:bps * 24]: self.err("backup boot region differs")
        self.fat = [u32(v, self.fat_off * bps + 4 * i) for i in range(self.n + 2)]
        if self.nfats == 2:
            o2 = (self.fat_off + self.fat_len) * bps
            f2 = [u32(v, o2 + 4 * i) for i in range(self.n + 2)]
            if f2 != self.fat: self.err("FAT copies differ")
        self.owner = {}
        self.read_root()

    def csec(self, c): return (self.heap + (c - 2) * self.spc) * self.bps
    def stream(self, first, length, nofat):
        """список кластерів потоку"""
        need = (length + self.cb - 1) // self.cb
        if first == 0:
            if length: self.err("zero first cluster with nonzero length")
            return []
        out = []
        if nofat:
            out = [first + i for i in range(need)]
            for c in out:
                if not (2 <= c <= self.n + 1): self.err(f"contiguous run out of range {c}"); return []
        else:
            c = first
            while True:
                if not (2 <= c <= self.n + 1): self.err(f"bad cluster {c} in chain from {first}"); break
                out.append(c)
                if len(out) > self.n: self.err("chain loop"); break
                nx = self.fat[c]
                if nx == 0xFFFFFFFF: break
                if nx == 0: self.err(f"chain from {first} hits free FAT entry at {c}"); break
                c = nx
            if len(out) != need: self.err(f"chain from {first}: {len(out)} clusters, need {need} for {length} bytes")
        return out
    def claim(self, cl, who):
        for c in cl:
            if c in self.owner: self.err(f"cluster {c} used by {who} and {self.owner[c]}")
            self.owner[c] = who
    def data(self, cl, length):
        out = bytearray()
        for c in cl: out += self.vol[self.csec(c): self.csec(c) + self.cb]
        return bytes(out[:length])

    def read_dir_bytes(self, first, length, nofat, who):
        cl = self.stream(first, length, nofat); self.claim(cl, who)
        return self.data(cl, length)

    def read_root(self):
        # довжина кореня — по ланцюжку
        cl = self.stream_chain(self.root)
        self.claim(cl, "root")
        raw = self.data(cl, len(cl) * self.cb)
        self.upcase = None; self.bm = None; self.label = None
        self.tree = {}
        self.scan(raw, "/", True)
        if self.bm is None: self.err("no bitmap entry"); return
        if not hasattr(self, "up_ent"): self.err("no upcase entry")
        self.check_bitmap()

    def stream_chain(self, first):
        out = []; c = first
        while 2 <= c <= self.n + 1 and len(out) <= self.n:
            out.append(c); nx = self.fat[c]
            if nx == 0xFFFFFFFF: break
            if nx == 0: self.err("root chain broken"); break
            c = nx
        return out

    def up(self, ch):
        if self.upcase is None: return ch
        return self.upcase[ch] if ch < 65536 else ch

    def load_upcase(self, first, length):
        raw = self.data(self.stream(first, length, True), length)
        t = list(range(65536)); i = 0; k = 0
        w = struct.unpack("<%dH" % (len(raw) // 2), raw[:len(raw) // 2 * 2])
        while k < len(w):
            if w[k] == 0xFFFF: i += w[k + 1]; k += 2
            else:
                if i < 65536: t[i] = w[k]
                i += 1; k += 1
        self.upcase = t

    def scan(self, raw, path, is_root):
        end = False; i = 0; names = set(); out = {}
        n = len(raw) // 32
        while i < n:
            e = raw[i * 32:(i + 1) * 32]; t = e[0]
            if end:
                if any(e): self.err(f"{path}: data after end marker at entry {i}")
                i += 1; continue
            if t == 0: end = True; i += 1; continue
            if not (t & 0x80): i += 1; continue
            if t == 0x81 and is_root:
                if self.bm is None: self.bm = (u32(e, 20), u64(e, 24))
                i += 1; continue
            if t == 0x82 and is_root:
                self.up_ent = (u32(e, 20), u64(e, 24), u32(e, 4)); i += 1; continue
            if t == 0x83 and is_root: i += 1; continue
            if t != 0x85: self.err(f"{path}: unexpected in-use entry type {t:#x} at {i}"); i += 1; continue
            sc = e[1]
            if i + sc >= n or sc < 2: self.err(f"{path}: bad secondary count at {i}"); i += 1; continue
            s = raw[i * 32:(i + sc + 1) * 32]
            c = 0
            for k, x in enumerate(s):
                if k in (2, 3): continue
                c = (((c << 15) | (c >> 1)) & 0xFFFF); c = (c + x) & 0xFFFF
            if c != u16(s, 2): self.err(f"{path}: set checksum bad at {i}")
            if s[32] != 0xC0: self.err(f"{path}: no stream entry at {i}"); i += sc + 1; continue
            nl = s[35]; nn = (nl + 14) // 15
            if nn != sc - 1: self.err(f"{path}: name entries {sc-1} != {nn} at {i}")
            u = b""
            for k in range(nn):
                o = 64 + 32 * k
                if s[o] != 0xC1: self.err(f"{path}: bad name entry type at {i}")
                u += s[o + 2:o + 32]
            u = u[:nl * 2]
            name = u.decode("utf-16-le", "replace")
            if self.upcase is None and hasattr(self, "up_ent"): self.load_upcase(*self.up_ent[:2])
            key = tuple(self.up(ord(ch)) for ch in name)
            h = 0
            for ch in name:
                uc = self.up(ord(ch))
                for byte in (uc & 0xFF, uc >> 8):
                    h = (((h << 15) | (h >> 1)) & 0xFFFF); h = (h + byte) & 0xFFFF
            if h != u16(s, 36): self.err(f"{path}{name}: name hash mismatch")
            if key in names: self.err(f"{path}: duplicate name {name!r}")
            names.add(key)
            attr = u16(s, 4); flags = s[33]; vdl = u64(s, 40); first = u32(s, 52); dl = u64(s, 56)
            if vdl > dl: self.err(f"{path}{name}: VDL > DL")
            nofat = bool(flags & 2)
            full = path + name
            cl = self.stream(first, dl, nofat); self.claim(cl, full)
            if attr & 0x10:
                if dl % self.cb or dl == 0: self.err(f"{full}: dir length {dl}")
                if vdl != dl: self.err(f"{full}: dir VDL != DL")
                sub = self.data(cl, dl)
                out[name] = self.scan(sub, full + "/", False)
            else:
                out[name] = self.data(cl, vdl)
            i += sc + 1
        if is_root: self.tree = out
        return out

    def check_bitmap(self):
        first, length = self.bm
        if length < (self.n + 7) // 8: self.err("bitmap too short")
        cl = self.stream(first, length, True); self.claim(cl, "bitmap")
        bm = self.data(cl, length)
        if hasattr(self, "up_ent"):
            f, l, _ = self.up_ent
            self.claim(self.stream(f, l, True), "upcase")
        for c in range(2, self.n + 2):
            bit = (bm[(c - 2) // 8] >> ((c - 2) % 8)) & 1
            used = c in self.owner
            if bit and not used: self.err(f"cluster {c} marked used but unowned")
            if not bit and used: self.err(f"cluster {c} owned by {self.owner[c]} but marked free")
        self.used = sum(1 for c in range(2, self.n + 2) if (bm[(c - 2) // 8] >> ((c - 2) % 8)) & 1)

def dump(tree, d):
    os.makedirs(d, exist_ok=True)
    for k, v in tree.items():
        p = os.path.join(d, k)
        if isinstance(v, dict): dump(v, p)
        else: open(p, "wb").write(v)

def count(t):
    return sum(1 + (count(v) if isinstance(v, dict) else 0) for v in t.values())

if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("image"); ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--extract")
    a = ap.parse_args()
    x = XF(a.image)
    if a.extract: dump(x.tree, a.extract)
    if not a.quiet: print(f"exFAT: bps={x.bps} spc={x.spc} clusters={x.n} used={x.used} nodes={count(x.tree)}")
    if x.errors:
        for e in x.errors[:30]: print("ERROR:", e)
        print(f"{len(x.errors)} error(s)"); sys.exit(1)
    if not a.quiet: print("fsck: clean")
