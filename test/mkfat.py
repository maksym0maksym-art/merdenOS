#!/usr/bin/env python3
"""Створює образ із FAT32 (для host-тестів): mkfat.py out.img SIZE_MB [--bps 512] [--spc 8] [--scheme none|mbr|gpt] [--label NAME]"""
import struct, sys, argparse, uuid

def make(path, size_mb, bps=512, spc=8, scheme="mbr", label="TESTVOL", nfats=2, mirror=True):
    total_bytes = size_mb * 1024 * 1024
    total_secs = total_bytes // bps
    part_start = 0
    if scheme == "mbr": part_start = 2048
    if scheme == "gpt": part_start = 2048
    part_secs = total_secs - part_start - (64 if scheme == "gpt" else 0)
    reserved = 32
    # fat size
    fat = 1
    while True:
        data = part_secs - reserved - nfats * fat
        clusters = data // spc
        need = ((clusters + 2) * 4 + bps - 1) // bps
        if need <= fat: break
        fat = need
    data_start = reserved + nfats * fat
    clusters = (part_secs - data_start) // spc

    img = bytearray(total_secs * bps)

    # boot sector
    b = bytearray(bps)
    b[0:3] = b"\xEB\x58\x90"
    b[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", b, 11, bps)
    b[13] = spc
    struct.pack_into("<H", b, 14, reserved)
    b[16] = nfats
    struct.pack_into("<H", b, 17, 0)
    struct.pack_into("<H", b, 19, 0)
    b[21] = 0xF8
    struct.pack_into("<H", b, 22, 0)
    struct.pack_into("<H", b, 24, 63)
    struct.pack_into("<H", b, 26, 255)
    struct.pack_into("<I", b, 28, part_start)
    struct.pack_into("<I", b, 32, part_secs)
    struct.pack_into("<I", b, 36, fat)
    struct.pack_into("<H", b, 40, 0 if mirror else 0x80)
    struct.pack_into("<H", b, 42, 0)
    struct.pack_into("<I", b, 44, 2)
    struct.pack_into("<H", b, 48, 1)
    struct.pack_into("<H", b, 50, 6)
    b[64] = 0x80
    b[66] = 0x29
    struct.pack_into("<I", b, 67, 0x1234ABCD)
    b[71:82] = label.encode().ljust(11)
    b[82:90] = b"FAT32   "
    b[510] = 0x55; b[511] = 0xAA

    fsinfo = bytearray(bps)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<I", fsinfo, 484, 0x61417272)
    struct.pack_into("<I", fsinfo, 488, clusters - 1)
    struct.pack_into("<I", fsinfo, 492, 3)
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)

    base = part_start * bps
    def put(sec, data): img[base + sec * bps: base + sec * bps + len(data)] = data
    put(0, b); put(1, fsinfo); put(6, b); put(7, fsinfo)
    fatsec = bytearray(bps)
    struct.pack_into("<III", fatsec, 0, 0x0FFFFFF8, 0x0FFFFFFF, 0x0FFFFFFF)
    for k in range(nfats):
        put(reserved + k * fat, fatsec)
    # volume label entry in root
    root = bytearray(32)
    root[0:11] = label.encode().ljust(11); root[11] = 0x08
    put(data_start, root + bytes(bps - 32))

    if scheme == "mbr":
        mbr = bytearray(512)
        e = bytearray(16)
        e[0] = 0x80; e[4] = 0x0C
        struct.pack_into("<II", e, 8, part_start, part_secs)
        mbr[446:462] = e
        mbr[510] = 0x55; mbr[511] = 0xAA
        img[0:512] = mbr
    elif scheme == "gpt":
        mbr = bytearray(512)
        e = bytearray(16); e[4] = 0xEE
        struct.pack_into("<II", e, 8, 1, 0xFFFFFFFF)
        mbr[446:462] = e; mbr[510] = 0x55; mbr[511] = 0xAA
        img[0:512] = mbr
        ent = bytearray(128)
        ent[0:16] = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7").bytes_le
        ent[16:32] = uuid.uuid4().bytes_le
        struct.pack_into("<QQ", ent, 32, part_start, part_start + part_secs - 1)
        ent[56:56 + 2 * 4] = "Data".encode("utf-16-le")
        entries_lba = 2
        img[entries_lba * bps: entries_lba * bps + 128] = ent
        h = bytearray(92)
        h[0:8] = b"EFI PART"
        struct.pack_into("<IIIIQQQQ", h, 8, 0x10000, 92, 0, 0, 1, total_secs - 1, 34, total_secs - 34)
        h[56:72] = uuid.uuid4().bytes_le
        struct.pack_into("<QIII", h, 72, entries_lba, 128, 128, 0)
        img[bps: bps + 92] = h
    open(path, "wb").write(img)
    return dict(clusters=clusters, fat=fat, part_start=part_start)

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("out"); ap.add_argument("size_mb", type=int)
    ap.add_argument("--bps", type=int, default=512); ap.add_argument("--spc", type=int, default=8)
    ap.add_argument("--scheme", default="mbr"); ap.add_argument("--label", default="TESTVOL")
    ap.add_argument("--nomirror", action="store_true")
    a = ap.parse_args()
    print(make(a.out, a.size_mb, a.bps, a.spc, a.scheme, a.label, mirror=not a.nomirror))
