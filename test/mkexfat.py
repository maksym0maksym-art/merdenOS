#!/usr/bin/env python3
"""Створює образ exFAT у стилі Windows (для host-тестів).
mkexfat.py out.img SIZE_MB [--bps 512] [--spc 8] [--scheme none|mbr|gpt] [--label NAME] [--prefill] [--nolabel]
--prefill: додає суміжні (NoFatChain) файли й каталог, як це роблять Windows/Linux."""
import struct, sys, argparse, uuid

def upcase_table():
    t = []
    for c in range(65536):
        if 0xD800 <= c < 0xE000:
            t.append(c); continue
        u = chr(c).upper()
        t.append(ord(u) if len(u) == 1 and ord(u) < 65536 else c)
    out = bytearray(); i = 0
    while i < 65536:
        if t[i] == i:
            j = i
            while j < 65536 and t[j] == j: j += 1
            if j - i > 2:
                out += struct.pack("<HH", 0xFFFF, j - i); i = j; continue
        out += struct.pack("<H", t[i]); i += 1
    return bytes(out)

def table_checksum(data):
    c = 0
    for b in data: c = (((c << 31) | (c >> 1)) + b) & 0xFFFFFFFF
    return c

def boot_checksum(sectors, bps):
    c = 0
    for i, b in enumerate(sectors):
        if i in (106, 107, 112): continue
        c = (((c << 31) | (c >> 1)) + b) & 0xFFFFFFFF
    return c

def name_hash(name):
    h = 0
    for ch in name:
        u = chr(ord(ch)).upper(); u = ord(u) if len(u) == 1 else ord(ch)
        for byte in (u & 0xFF, u >> 8):
            h = (((h << 15) | (h >> 1)) & 0xFFFF); h = (h + byte) & 0xFFFF
    return h

def set_checksum(entries):
    c = 0
    for i, b in enumerate(entries):
        if i in (2, 3): continue
        c = (((c << 15) | (c >> 1)) & 0xFFFF); c = (c + b) & 0xFFFF
    return c

def file_set(name, attr, first, size, nofat):
    u = name.encode("utf-16-le"); n = len(u) // 2
    nn = (n + 14) // 15
    e = bytearray(32 * (2 + nn))
    e[0] = 0x85; e[1] = 1 + nn; struct.pack_into("<H", e, 4, attr)
    ts = ((2026 - 1980) << 25) | (10 << 21) | (5 << 16) | (12 << 11)
    struct.pack_into("<III", e, 8, ts, ts, ts)
    e[32] = 0xC0; e[33] = 3 if nofat else 1; e[35] = n
    struct.pack_into("<H", e, 36, name_hash(name))
    struct.pack_into("<Q", e, 40, size); struct.pack_into("<I", e, 52, first); struct.pack_into("<Q", e, 56, size)
    for i in range(nn):
        o = 64 + 32 * i; e[o] = 0xC1
        chunk = u[i * 30:(i + 1) * 30]
        e[o + 2:o + 2 + len(chunk)] = chunk
    struct.pack_into("<H", e, 2, set_checksum(e))
    return bytes(e)

def make(path, size_mb, bps=512, spc=8, scheme="mbr", label="EXTEST", prefill=False, nolabel=False):
    total_secs = size_mb * 1024 * 1024 // bps
    part_start = 2048 if scheme in ("mbr", "gpt") else 0
    part_secs = total_secs - part_start - (64 if scheme == "gpt" else 0)
    cb = bps * spc
    fat_off = 24
    heap_off = fat_off + 1
    while True:
        clusters = (part_secs - heap_off) // spc
        fat_len = ((clusters + 2) * 4 + bps - 1) // bps
        need = fat_off + fat_len
        need = (need + spc - 1) // spc * spc
        if need <= heap_off: break
        heap_off = need
    clusters = (part_secs - heap_off) // spc
    up = upcase_table()
    nbm = ((clusters + 7) // 8 + cb - 1) // cb
    nup = (len(up) + cb - 1) // cb
    bm_first = 2; up_first = bm_first + nbm; root = up_first + nup
    nxt = root + 1

    img = bytearray(total_secs * bps)
    base = part_start * bps
    def put(sec, data): img[base + sec * bps: base + sec * bps + len(data)] = data
    def clus_sec(c): return heap_off + (c - 2) * spc

    fat = bytearray(((clusters + 2) * 4 + bps - 1) // bps * bps)
    struct.pack_into("<II", fat, 0, 0xFFFFFFF8, 0xFFFFFFFF)
    def chain(first, n):
        for i in range(n):
            struct.pack_into("<I", fat, (first + i) * 4, first + i + 1 if i + 1 < n else 0xFFFFFFFF)
    chain(bm_first, nbm); chain(up_first, nup); chain(root, 1)
    used = set(range(2, nxt))

    root_ents = bytearray()
    if not nolabel:
        lab = bytearray(32); lab[0] = 0x83; lab[1] = len(label); lab[2:2 + 2 * len(label)] = label.encode("utf-16-le")
        root_ents += lab
    root_ents += bytes([0x81, 0]) + bytes(18) + struct.pack("<IQ", bm_first, (clusters + 7) // 8)
    ent = bytearray(32); ent[0] = 0x82
    struct.pack_into("<I", ent, 4, table_checksum(up)); struct.pack_into("<I", ent, 20, up_first); struct.pack_into("<Q", ent, 24, len(up))
    root_ents += ent

    def pat(n, k): return bytes((i * k + 3) & 255 for i in range(n))
    if prefill:
        size = 3 * cb + 123
        n = (size + cb - 1) // cb
        put(clus_sec(nxt), pat(size, 7)); used |= set(range(nxt, nxt + n))
        root_ents += file_set("PRE.BIN", 0x20, nxt, size, True); nxt += n
        # каталог NoFatChain з одним файлом
        dcl = nxt; nxt += 1; used.add(dcl)
        isz = 2 * cb + 5; n = (isz + cb - 1) // cb
        put(clus_sec(nxt), pat(isz, 5)); used |= set(range(nxt, nxt + n))
        put(clus_sec(dcl), file_set("inner.bin", 0x20, nxt, isz, True)); nxt += n
        root_ents += file_set("NFDIR", 0x10, dcl, cb, True)
    put(clus_sec(root), bytes(root_ents))
    put(clus_sec(bm_first), b"")
    bm = bytearray(nbm * cb)
    for c in used: bm[(c - 2) // 8] |= 1 << ((c - 2) % 8)
    put(clus_sec(bm_first), bytes(bm)); put(clus_sec(up_first), up)
    put(fat_off, bytes(fat))

    b = bytearray(bps)
    b[0:3] = b"\xEB\x76\x90"; b[3:11] = b"EXFAT   "
    struct.pack_into("<Q", b, 64, part_start); struct.pack_into("<Q", b, 72, part_secs)
    struct.pack_into("<IIIII", b, 80, fat_off, fat_len, heap_off, clusters, root)
    struct.pack_into("<I", b, 100, 0xC0FFEE42)
    struct.pack_into("<H", b, 104, 0x0100)
    b[108] = bps.bit_length() - 1; b[109] = spc.bit_length() - 1; b[110] = 1; b[111] = 0x80
    b[510] = 0x55; b[511] = 0xAA
    ext = bytearray(bps); struct.pack_into("<I", ext, bps - 4, 0xAA550000)
    region = bytearray(b)
    for _ in range(8): region += ext
    region += bytes(bps * 2)
    cs = boot_checksum(region, bps)
    chk = struct.pack("<I", cs) * (bps // 4)
    for k in range(2):
        put(k * 12, bytes(region) + chk)
    # MBR/GPT
    if scheme == "mbr":
        mbr = bytearray(512); e = bytearray(16); e[0] = 0x80; e[4] = 0x07
        struct.pack_into("<II", e, 8, part_start, part_secs); mbr[446:462] = e
        mbr[510] = 0x55; mbr[511] = 0xAA; img[0:512] = mbr
    elif scheme == "gpt":
        mbr = bytearray(512); e = bytearray(16); e[4] = 0xEE
        struct.pack_into("<II", e, 8, 1, 0xFFFFFFFF); mbr[446:462] = e; mbr[510] = 0x55; mbr[511] = 0xAA
        img[0:512] = mbr
        ent = bytearray(128)
        ent[0:16] = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7").bytes_le
        ent[16:32] = uuid.uuid4().bytes_le
        struct.pack_into("<QQ", ent, 32, part_start, part_start + part_secs - 1)
        img[2 * bps:2 * bps + 128] = ent
        h = bytearray(92); h[0:8] = b"EFI PART"
        struct.pack_into("<IIIIQQQQ", h, 8, 0x10000, 92, 0, 0, 1, total_secs - 1, 34, total_secs - 34)
        h[56:72] = uuid.uuid4().bytes_le
        struct.pack_into("<QIII", h, 72, 2, 128, 128, 0)
        img[bps:bps + 92] = h
    open(path, "wb").write(img)
    return dict(clusters=clusters, fat_len=fat_len, heap_off=heap_off, part_start=part_start)

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("out"); ap.add_argument("size_mb", type=int)
    ap.add_argument("--bps", type=int, default=512); ap.add_argument("--spc", type=int, default=8)
    ap.add_argument("--scheme", default="mbr"); ap.add_argument("--label", default="EXTEST")
    ap.add_argument("--prefill", action="store_true"); ap.add_argument("--nolabel", action="store_true")
    a = ap.parse_args()
    print(make(a.out, a.size_mb, a.bps, a.spc, a.scheme, a.label, a.prefill, a.nolabel))
