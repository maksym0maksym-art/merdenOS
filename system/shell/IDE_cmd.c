/* Команди для IDE-дисків: disks, hd read/mbr/fill. Підключається з shell_execute_command
 * так само, як usbfs_command: повертає 1, якщо команда була його. */
#include <stdint.h>
#include "../../kernel/headers/IDE_drive.h"

void shell_print(char *text);

static void put(const char *s) { shell_print((char *)s); }

static void put_u32(uint32_t v)
{
    char b[12]; int n = 0;
    if (!v) b[n++] = '0';
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    char o[12]; int k = 0;
    while (n) o[k++] = b[--n];
    o[k] = 0;
    put(o);
}

static void put_hex(uint32_t v, int digits)
{
    char o[9]; int k = 0;
    for (int i = digits - 1; i >= 0; i--) o[k++] = "0123456789abcdef"[(v >> (i * 4)) & 15];
    o[k] = 0;
    put(o);
}

static int starts(const char *s, const char *p)
{
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

static const char *skip(const char *s) { while (*s == ' ') s++; return s; }

/* число: десяткове або 0x..; 1 — розібрали, *end — за ним */
static int parse_num(const char *s, uint32_t *out, const char **end)
{
    uint32_t v = 0; int any = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (;; s++, any = 1) {
            char c = *s; uint32_t d;
            if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            else break;
            v = v * 16 + d;
        }
    } else {
        for (; *s >= '0' && *s <= '9'; s++, any = 1) v = v * 10 + (uint32_t)(*s - '0');
    }
    if (!any || (*s && *s != ' ')) return 0;
    *out = v; *end = s;
    return 1;
}

static void put_size(uint32_t sectors)
{
    uint32_t mb = sectors / 2048;
    if (mb >= 1024) { put_u32((mb + 512) / 1024); put(" GB"); }
    else            { put_u32(mb);        put(" MB"); }
}

static void err(const char *what, int code)
{
    put("hd: "); put(what); put(": "); put(ide_strerror(code)); put("\n");
}

static void cmd_disks(void)
{
    if (ide_drive_count() == 0) { put("IDE: no drives found\n"); return; }
    for (int i = 0; i < IDE_MAX_DRIVES; i++) {
        struct ide_devices_s *d = &ide_devices[i];
        if (!d->Reserved) continue;
        if (d->Type == IDE_ATA) { put("hd"); put_u32((uint32_t)i); put("  ATA    "); put_size(d->Size); }
        else                    { put("--  ATAPI  (CD/DVD, not supported yet)"); }
        put("  "); put((const char *)d->Model);
        put(d->channel ? "  (secondary " : "  (primary ");
        put(d->Drive ? "slave)\n" : "master)\n");
    }
}

static void cmd_read(int drive, uint32_t lba)
{
    static uint8_t sec[IDE_SECTOR_SIZE];
    int e = ide_read_blocks(drive, lba, 1, sec);
    if (e) { err("read", e); return; }
    put("hd"); put_u32((uint32_t)drive); put(" sector "); put_u32(lba); put("\n");
    for (int row = 0; row < IDE_SECTOR_SIZE; row += 16) {
        put_hex((uint32_t)row, 4); put(": ");
        for (int i = 0; i < 16; i++) { put_hex(sec[row + i], 2); put(" "); }
        char a[19]; a[0] = '|';
        for (int i = 0; i < 16; i++) { uint8_t c = sec[row + i]; a[1 + i] = (c >= 32 && c < 127) ? (char)c : '.'; }
        a[17] = '|'; a[18] = 0;
        put(a); put("\n");
    }
}

static const char *ptype(uint8_t t)
{
    switch (t) {
    case 0x00: return "empty";
    case 0x01: case 0x04: case 0x06: case 0x0E: return "FAT16";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x07: return "NTFS/exFAT";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI system";
    default:   return "other";
    }
}

static void cmd_mbr(int drive)
{
    static uint8_t sec[IDE_SECTOR_SIZE];
    int e = ide_read_blocks(drive, 0, 1, sec);
    if (e) { err("read", e); return; }
    if (sec[510] != 0x55 || sec[511] != 0xAA) {
        put("hd"); put_u32((uint32_t)drive); put(": no MBR signature (sector 0 ends with ");
        put_hex(sec[510], 2); put(" "); put_hex(sec[511], 2); put(") - raw/unpartitioned disk\n");
        return;
    }
    put("MBR signature 55 AA\n");
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = sec + 446 + i * 16;
        uint32_t start = (uint32_t)(p[8] | (p[9] << 8) | (p[10] << 16) | ((uint32_t)p[11] << 24));
        uint32_t cnt   = (uint32_t)(p[12] | (p[13] << 8) | (p[14] << 16) | ((uint32_t)p[15] << 24));
        put("  #"); put_u32((uint32_t)(i + 1));
        put(p[0] == 0x80 ? " boot " : "      ");
        put("type 0x"); put_hex(p[4], 2); put(" "); put(ptype(p[4]));
        if (p[4]) { put("  start "); put_u32(start); put("  sectors "); put_u32(cnt); }
        put("\n");
    }
}

/* заповнює сектор байтом і перечитує для перевірки; LBA 0 не чіпаємо (там таблиця розділів) */
static void cmd_fill(int drive, uint32_t lba, uint32_t val)
{
    static uint8_t sec[IDE_SECTOR_SIZE], back[IDE_SECTOR_SIZE];
    if (lba == 0) { put("hd: refusing to overwrite sector 0 (partition table)\n"); return; }
    for (int i = 0; i < IDE_SECTOR_SIZE; i++) sec[i] = (uint8_t)val;
    int e = ide_write_blocks(drive, lba, 1, sec);
    if (!e) e = ide_flush_drive(drive);
    if (e) { err("write", e); return; }
    e = ide_read_blocks(drive, lba, 1, back);
    if (e) { err("verify read", e); return; }
    for (int i = 0; i < IDE_SECTOR_SIZE; i++)
        if (back[i] != sec[i]) { put("hd: VERIFY FAILED at byte "); put_u32((uint32_t)i); put("\n"); return; }
    put("sector "); put_u32(lba); put(" filled with 0x"); put_hex(val & 0xFF, 2); put(", verified\n");
}

static void usage(void)
{
    put("hd read <n> <lba>         - hex dump of one sector\n");
    put("hd mbr <n>               - partition table\n");
    put("hd fill <n> <lba> <byte> - write a test pattern + verify (not sector 0)\n");
    put("disks                    - list IDE drives (n = hdN)\n");
}

int ide_command(const char *cmd)
{
    if (starts(cmd, "disks") && (cmd[5] == 0 || cmd[5] == ' ')) { cmd_disks(); return 1; }
    if (!(starts(cmd, "hd") && (cmd[2] == 0 || cmd[2] == ' '))) return 0;

    const char *p = skip(cmd + 2);
    uint32_t n, lba, val;

    if (starts(p, "read ") && parse_num(skip(p + 5), &n, &p) && parse_num(skip(p), &lba, &p)) { cmd_read((int)n, lba); return 1; }
    if (starts(p, "mbr ")  && parse_num(skip(p + 4), &n, &p))                                  { cmd_mbr((int)n);       return 1; }
    if (starts(p, "fill ") && parse_num(skip(p + 5), &n, &p) && parse_num(skip(p), &lba, &p)
                           && parse_num(skip(p), &val, &p))                                    { cmd_fill((int)n, lba, val); return 1; }
    usage();
    return 1;
}
