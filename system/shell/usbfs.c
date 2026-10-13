#include <stdint.h>
#include "../../kernel/headers/usbfs.h"
#include "../../kernel/headers/xhci.h"
#include "../../kernel/headers/usb_msc.h"
#include "../../kernel/headers/xhci_hid.h"
#include "../../kernel/headers/blockdev.h"
#include "../../kernel/headers/fat32.h"
#include "../../kernel/headers/EXFAT.h"
#include "../../kernel/headers/usb_dbg.h"
#include "../../kernel/headers/rtc.h"

void shell_print(char *text);

/* Змонтовані томи ("диски" usb0:, usb1:, ...). Кожен том — ~33 КБ кешу, тому тільки статично (стек ядра — 16 КБ).
 * Поточний диск (cur) визначає, на що діють звичайні ls/cd/cat/...; cur < 0 — рідна файлова система ОС. */
#define VOL_SLOTS 3
#define FS_FAT    1
#define FS_EXFAT  2

typedef struct {
    int         used;
    int         drive;                   /* номер флешки в usb_msc (usbN:) */
    int         fs;                      /* FS_FAT / FS_EXFAT */
    char        cwd[256];
    blockdev_t  bd;
    union { fat_vol_t f; xf_vol_t x; } v;
} vslot_t;

static vslot_t     slots[VOL_SLOTS];
static int         cur = -1;
static int         scanned;
static int         need_rescan;             /* після eject останньої флешки: наступне звернення сканує заново */
static uint8_t     iobuf[4096];

/* ---- дрібні рядкові допоміжні (libc нема) ---- */
static int  u_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int  u_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int  u_starts(const char *s, const char *p) { while (*p) { if (*s++ != *p++) return 0; } return 1; }
static const char *skip_sp(const char *s) { while (*s == ' ') s++; return s; }

int  klog_booting(void);
void klog_raw(const char *text);
/* під час завантаження вивід іде в журнал ядра (з часом, потрапляє в dmesg) */
static void out(const char *s) { if (klog_booting()) klog_raw(s); else shell_print((char *)s); }

/* друк числа */
static void out_u(uint32_t v) {
    char b[12]; int i = 11; b[i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    out(b + i);
}
static void out_size(uint32_t v) {              /* "12.3 MB"-подібний вивід без 64-бітного ділення */
    if (v < 1024) { out_u(v); out(" B"); return; }
    if (v < 1024 * 1024) { out_u(v >> 10); out(" KB"); return; }
    if (v < 1024u * 1024u * 1024u) { out_u(v >> 20); out(" MB"); return; }
    out_u(v >> 30); out(" GB");
}
static void out_blocks_size(uint32_t blocks, uint32_t bsz) {   /* blocks*bsz може не влізти в 32 біти */
    uint32_t shift = 0; while ((1u << shift) < bsz) shift++;
    uint32_t kb = shift >= 10 ? blocks << (shift - 10) : blocks >> (10 - shift);
    if (kb < 1024) { out_u(kb); out(" KB"); }
    else if (kb < 1024 * 1024) { out_u(kb >> 10); out(" MB"); }
    else { out_u(kb >> 20); out(" GB"); }
}

const char *usbfs_strerror(int rc) {
    if (rc <= -100) return usb_msc_err_str(rc + 100);
    return fat_strerror(rc);
}

static void fail(const char *what, int rc) {
    out(what); out(": "); out(fat_strerror(rc)); out("\n");
}

/* ---- годинник для часових міток FAT ---- */
static void clock_get(uint16_t *date, uint16_t *time) {
    RTC_TIME t = rtc_get_time();
    int y = 2000 + t.year; if (y < 1980) y = 1980; if (y > 2107) y = 2107;
    int mo = t.month < 1 || t.month > 12 ? 1 : t.month;
    int d = t.day < 1 || t.day > 31 ? 1 : t.day;
    *date = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
    *time = (uint16_t)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
}

/* ---- уніфікований доступ до FAT32 / exFAT ---- */
static int  v_ro(vslot_t *s)    { return s->fs == FS_EXFAT ? s->v.x.read_only : s->v.f.read_only; }
static const char *v_label(vslot_t *s) { return s->fs == FS_EXFAT ? s->v.x.label : s->v.f.label; }
static const char *v_fsname(vslot_t *s) { return s->fs == FS_EXFAT ? "exFAT" : "FAT32"; }

static int v_flush(vslot_t *s) { return s->fs == FS_EXFAT ? xf_flush(&s->v.x) : fat_flush(&s->v.f); }
static int v_stat(vslot_t *s, const char *p, fat_dirent_t *e) { return s->fs == FS_EXFAT ? xf_stat(&s->v.x, p, e) : fat_stat(&s->v.f, p, e); }
static int v_readdir(vslot_t *s, const char *p, fat_iter_cb cb, void *a) { return s->fs == FS_EXFAT ? xf_readdir(&s->v.x, p, cb, a) : fat_readdir(&s->v.f, p, cb, a); }
static int v_read_file(vslot_t *s, const char *p, void *b, uint32_t cap, uint32_t *len) { return s->fs == FS_EXFAT ? xf_read_file(&s->v.x, p, b, cap, len) : fat_read_file(&s->v.f, p, b, cap, len); }
static int v_write_file(vslot_t *s, const char *p, const void *b, uint32_t n) { return s->fs == FS_EXFAT ? xf_write_file(&s->v.x, p, b, n) : fat_write_file(&s->v.f, p, b, n); }
static int v_append_file(vslot_t *s, const char *p, const void *b, uint32_t n) { return s->fs == FS_EXFAT ? xf_append_file(&s->v.x, p, b, n) : fat_append_file(&s->v.f, p, b, n); }
static int v_create(vslot_t *s, const char *p) { return s->fs == FS_EXFAT ? xf_create(&s->v.x, p) : fat_create(&s->v.f, p); }
static int v_mkdir(vslot_t *s, const char *p) { return s->fs == FS_EXFAT ? xf_mkdir(&s->v.x, p) : fat_mkdir(&s->v.f, p); }
static int v_unlink(vslot_t *s, const char *p) { return s->fs == FS_EXFAT ? xf_unlink(&s->v.x, p) : fat_unlink(&s->v.f, p); }
static int v_rmdir(vslot_t *s, const char *p) { return s->fs == FS_EXFAT ? xf_rmdir(&s->v.x, p) : fat_rmdir(&s->v.f, p); }
static int v_rename(vslot_t *s, const char *a, const char *b) { return s->fs == FS_EXFAT ? xf_rename(&s->v.x, a, b) : fat_rename(&s->v.f, a, b); }
static int v_statfs(vslot_t *s, uint32_t *t, uint32_t *f, uint32_t *c) { return s->fs == FS_EXFAT ? xf_statfs(&s->v.x, t, f, c) : fat_statfs(&s->v.f, t, f, c); }

typedef struct { vslot_t *s; union { fat_file_t f; xf_file_t x; } u; } vf_t;
static int vf_open(vslot_t *s, const char *p, int create, vf_t *h) {
    h->s = s;
    return s->fs == FS_EXFAT ? xf_open(&s->v.x, p, create, &h->u.x) : fat_open(&s->v.f, p, create, &h->u.f);
}
static int vf_read(vf_t *h, uint32_t off, void *b, uint32_t n) { return h->s->fs == FS_EXFAT ? xf_fread(&h->u.x, off, b, n) : fat_fread(&h->u.f, off, b, n); }
static int vf_write(vf_t *h, uint32_t off, const void *b, uint32_t n) { return h->s->fs == FS_EXFAT ? xf_fwrite(&h->u.x, off, b, n) : fat_fwrite(&h->u.f, off, b, n); }
static int vf_trunc(vf_t *h, uint32_t n) { return h->s->fs == FS_EXFAT ? xf_ftruncate(&h->u.x, n) : fat_ftruncate(&h->u.f, n); }
static int vf_close(vf_t *h) { return h->s->fs == FS_EXFAT ? xf_fclose(&h->u.x) : fat_fclose(&h->u.f); }

/* ---- підключення ---- */
/* Скан забирає і флешки, і USB-миші (повторний скан скидає контролер, миші треба підняти знову) */
static int probe_all(xhci_dev_t *d, void *arg) {
    if (usb_msc_probe(d, arg)) return 1;
    return xhci_hid_probe(d, arg);
}

static void out_hex(uint32_t v, int digits);

/* скан уже був (наш або при старті системи в mouse_init) — повторний скидає контролер і псує змонтовані диски */
static int have_scan(void) { return !need_rescan && (scanned || usb_msc_count() > 0 || xhci_seen_count() > 0); }

static void out_drive(int drive) { out("usb"); out_u((uint32_t)drive); out(":"); }

static vslot_t *slot_of_drive(int drive) {
    for (int i = 0; i < VOL_SLOTS; i++) if (slots[i].used && slots[i].drive == drive) return &slots[i];
    return 0;
}
static int slot_index(vslot_t *s) { return (int)(s - slots); }

static void unmount_all(void) {
    for (int i = 0; i < VOL_SLOTS; i++) {
        if (slots[i].used) v_flush(&slots[i]);
        slots[i].used = 0;
    }
    cur = -1;
}

static int do_scan(void) {
    usb_time_init();
    unmount_all();
    usb_msc_forget_all();
    xhci_hid_forget_all();
    out("Scanning xHCI ports...\n");
    int n = xhci_scan(probe_all, 0);
    scanned = 1; need_rescan = 0;
    if (n < 0) { out("xHCI: controller not found or failed to start (try `usb debug on`, then `usb scan`)\n"); return -1; }
    if (usb_msc_count() == 0) { out("No USB flash drive found. Plug it in and run `usb scan` again.\n"); return -1; }
    return 0;
}

/* Що лежить на флешці, якщо це не FAT32/exFAT: таблиця розділів і назва ФС із завантажувального сектора */
static void fs_name_at(usb_msc_t *m, uint32_t lba, char *name /* >= 9 */, char *fat /* >= 9 */) {
    name[0] = fat[0] = 0;
    if (m->block_size > sizeof iobuf) return;
    if (usb_msc_read(m, lba, 1, iobuf) != 0) { name[0] = '?'; name[1] = 0; return; }
    for (int i = 0; i < 8; i++) { uint8_t c = iobuf[3 + i]; name[i] = (c >= 32 && c < 127) ? (char)c : '.'; }
    name[8] = 0;
    for (int i = 0; i < 8; i++) { uint8_t c = iobuf[0x52 + i]; fat[i] = (c >= 32 && c < 127) ? (char)c : '.'; }
    fat[8] = 0;
}

static void diagnose(usb_msc_t *m) {
    char name[9], fat[9];
    if (m->block_size > sizeof iobuf) { out("  (block size too large to inspect)\n"); return; }
    if (usb_msc_read(m, 0, 1, iobuf) != 0) { out("  cannot read sector 0\n"); return; }
    uint8_t mbr[64];
    for (int i = 0; i < 64; i++) mbr[i] = iobuf[446 + i];
    int sig = iobuf[510] == 0x55 && iobuf[511] == 0xAA;
    out("  sector 0: boot signature "); out(sig ? "ok" : "MISSING"); out("\n");
    int any = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t type = mbr[i * 16 + 4];
        uint32_t start = (uint32_t)mbr[i * 16 + 8] | ((uint32_t)mbr[i * 16 + 9] << 8) | ((uint32_t)mbr[i * 16 + 10] << 16) | ((uint32_t)mbr[i * 16 + 11] << 24);
        uint32_t cnt = (uint32_t)mbr[i * 16 + 12] | ((uint32_t)mbr[i * 16 + 13] << 8) | ((uint32_t)mbr[i * 16 + 14] << 16) | ((uint32_t)mbr[i * 16 + 15] << 24);
        if (!type) continue;
        any = 1;
        out("  partition "); out_u((uint32_t)i + 1); out(": type 0x"); out_hex(type, 2);
        out(", start "); out_u(start); out(", "); out_blocks_size(cnt, m->block_size);
        if (type == 0xEE) { out("  (GPT)\n"); continue; }
        fs_name_at(m, start, name, fat);
        out("  fs: '"); out(name); out("' / '"); out(fat); out("'\n");
    }
    if (!any) {
        fs_name_at(m, 0, name, fat);
        out("  no partitions; sector 0 fs: '"); out(name); out("' / '"); out(fat); out("'\n");
    }
    out("  Supported: FAT32 and exFAT. NTFS (and others) are not supported - reformat the drive as exFAT or FAT32.\n");
}

/* змонтувати флешку drive у вільний слот; 0 — помилка */
static vslot_t *mount_drive(int drive, int verbose) {
    vslot_t *ex = slot_of_drive(drive);
    if (ex) return ex;
    usb_msc_t *m = usb_msc_get(drive);
    if (!m) { out("No such drive: "); out_drive(drive); out(" (see `usb info`)\n"); return 0; }
    vslot_t *s = 0;
    for (int i = 0; i < VOL_SLOTS; i++) if (!slots[i].used) { s = &slots[i]; break; }
    if (!s) { out("Too many mounted drives (max "); out_u(VOL_SLOTS); out("). Use `usb eject N`.\n"); return 0; }

    s->bd.ctx = m; s->bd.block_size = m->block_size; s->bd.block_count = m->block_count;
    s->bd.read = usb_msc_read; s->bd.write = usb_msc_write; s->bd.flush = usb_msc_sync;
    s->bd.read_only = m->write_protected;
    fat_set_clock(clock_get);
    xf_set_clock(clock_get);
    fat_part_info_t info;
    s->fs = FS_FAT;
    int rc = fat_mount(&s->v.f, &s->bd, -1, &info);
    if (rc == FAT_E_FMT) { s->fs = FS_EXFAT; rc = xf_mount(&s->v.x, &s->bd, -1, &info); }
    if (rc) {
        if (verbose) {
            out_drive(drive); out(" ("); out(m->vendor); out(" "); out(m->product); out("): ");
            out(rc == FAT_E_FMT ? "no FAT32/exFAT file system" : fat_strerror(rc)); out("\n");
            if (rc == FAT_E_FMT) diagnose(m);
        }
        return 0;
    }
    s->used = 1; s->drive = drive;
    s->cwd[0] = '/'; s->cwd[1] = 0;
    out("Mounted "); out_drive(drive); out(" "); out(m->vendor); out(" "); out(m->product);
    out("  "); out(v_fsname(s));
    if (v_label(s)[0]) { out("  label '"); out(v_label(s)); out("'"); }
    if (v_ro(s)) out("  [READ ONLY]");
    out("\n");
    return s;
}

/* змонтувати всі флешки з підтримуваною ФС; поточним стає перша */
static int mount_all(void) {
    if (!have_scan() && do_scan()) return -1;
    if (usb_msc_count() == 0) { if (do_scan()) return -1; }
    int n = usb_msc_count(), ok = 0;
    for (int i = 0; i < n; i++) {
        vslot_t *s = mount_drive(i, 1);
        if (s) { ok++; if (cur < 0) cur = slot_index(s); }
    }
    if (!ok) { out("No FAT32/exFAT drive found among "); out_u((uint32_t)n); out(" USB drive(s).\n"); return -1; }
    out("Current drive: "); out_drive(slots[cur].drive); out("  (switch: usb0:, usb1:...; back to the system disk: ram:)\n");
    return 0;
}

/* пересвідчитися, що є поточний диск; при потребі сканувати/монтувати */
static int need_mount(void) {
    if (cur >= 0) return 0;
    return mount_all();
}

/* розбір префікса диска "usbN:" на початку шляху; повертає слот (0 — немає префікса); *err = 1 — диск не змонтовано */
static vslot_t *drive_prefix(const char **pp, int *err) {
    const char *p = *pp;
    *err = 0;
    if (!(p[0] == 'u' && p[1] == 's' && p[2] == 'b')) return 0;
    int i = 3, d = -1;
    while (p[i] >= '0' && p[i] <= '9') { d = (d < 0 ? 0 : d * 10) + (p[i] - '0'); i++; }
    if (p[i] != ':') return 0;
    *pp = p + i + 1;
    if (d < 0) return cur >= 0 ? &slots[cur] : (*err = 1, (vslot_t *)0);
    vslot_t *s = slot_of_drive(d);
    if (!s) { *err = 1; out_drive(d); out(" is not mounted (run `usb mount "); out_u((uint32_t)d); out("`)\n"); }
    return s;
}

/* аргумент -> (диск, абсолютний шлях). 0 — помилка (повідомлення вже надруковано) */
static int resolve_on(const char *arg, vslot_t **sl, char *out_path, int sz) {
    const char *a = arg;
    int err;
    vslot_t *s = drive_prefix(&a, &err);
    if (err) { if (!s && cur < 0) out("no current drive\n"); return -1; }
    if (!s) s = &slots[cur];
    int rc = fat_path_join(s->cwd, a, out_path, sz);
    if (rc) { fail("path", rc); return -1; }
    *sl = s;
    return 0;
}

/* розбір аргументів: наступне слово (до пробілу) копіюється в w, повертається вказівник за ним */
static const char *next_word(const char *s, char *w, int sz) {
    s = skip_sp(s);
    int n = 0;
    while (*s && *s != ' ') { if (n < sz - 1) w[n++] = *s; s++; }
    w[n] = 0;
    return s;
}

/* ---- ls ---- */
typedef struct { int files, dirs; uint32_t bytes; } ls_ctx_t;
static int ls_cb(const fat_dirent_t *e, void *arg) {
    ls_ctx_t *c = arg;
    if (e->attr & FAT_ATTR_VOLUME) return 0;
    if (e->attr & FAT_ATTR_DIR) {
        out("  <DIR>      "); out(e->name); out("\n"); c->dirs++;
    } else {
        char num[12]; int i = 11; num[i] = 0; uint32_t v = e->size;
        if (!v) num[--i] = '0';
        while (v) { num[--i] = (char)('0' + v % 10); v /= 10; }
        out("  ");
        for (int pad = 11 - i; pad < 10; pad++) out(" ");
        out(num + i); out("  "); out(e->name); out("\n");
        c->files++; c->bytes += e->size;
    }
    return 0;
}

static void cmd_ls(const char *arg) {
    char p[256], w[256];
    vslot_t *s;
    next_word(arg, w, sizeof w);
    if (resolve_on(w, &s, p, sizeof p)) return;
    ls_ctx_t c = {0, 0, 0};
    out_drive(s->drive); out(p); out(":\n");
    int rc = v_readdir(s, p, ls_cb, &c);
    if (rc) { fail("ls", rc); return; }
    out_u((uint32_t)c.files); out(" file(s), "); out_u((uint32_t)c.dirs); out(" dir(s), ");
    out_size(c.bytes); out("\n");
}

static void cmd_cat(const char *arg) {
    char p[256], w[256];
    vslot_t *s;
    next_word(arg, w, sizeof w);
    if (!w[0]) { out("usage: cat <file>\n"); return; }
    if (resolve_on(w, &s, p, sizeof p)) return;
    vf_t f;
    int rc = vf_open(s, p, 0, &f);
    if (rc) { fail("cat", rc); return; }
    uint32_t off = 0, total = 0;
    char line[257];
    while (total < 65536) {
        int n = vf_read(&f, off, iobuf, 256);
        if (n < 0) { fail("cat", n); break; }
        if (n == 0) break;
        for (int i = 0; i < n; i++) {
            uint8_t c = iobuf[i];
            line[i] = (c == '\n' || (c >= 32 && c < 127)) ? (char)c : (c == '\t' ? ' ' : (c == '\r' ? 0 : '?'));
            if (line[i] == 0) line[i] = ' ';
        }
        line[n] = 0;
        out(line);
        off += (uint32_t)n; total += (uint32_t)n;
    }
    out("\n");
    if (total >= 65536) out("... (truncated at 64 KB)\n");
    vf_close(&f);
}


/* ---- Merden executable loader: MEXE v1 ---- */

#define MEXE_HEADER_SIZE 24u
#define MEXE_MAX_FILE    (64u * 1024u)

static uint8_t mexe_image[MEXE_MAX_FILE]
    __attribute__((aligned(16)));

static uint16_t mexe_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t mexe_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void cmd_run(const char *arg)
{
    char name[256];
    char path[256];
    vslot_t *s;

    next_word(arg, name, sizeof name);

    if (!name[0]) {
        out("usage: run <program.mexe>\n");
        return;
    }

    if (resolve_on(name, &s, path, sizeof path))
        return;

    fat_dirent_t st;
    int rc = v_stat(s, path, &st);

    if (rc) {
        fail("run", rc);
        return;
    }

    if (st.attr & FAT_ATTR_DIR) {
        out("run: path is a directory\n");
        return;
    }

    if (st.size < MEXE_HEADER_SIZE ||
        st.size > sizeof mexe_image) {
        out("run: invalid file size\n");
        return;
    }

    vf_t file;
    rc = vf_open(s, path, 0, &file);

    if (rc) {
        fail("run: open", rc);
        return;
    }

    uint32_t total = 0;

    while (total < st.size) {
        int n = vf_read(
            &file,
            total,
            mexe_image + total,
            st.size - total
        );

        if (n <= 0) {
            vf_close(&file);
            out("run: failed to read executable\n");
            return;
        }

        total += (uint32_t)n;
    }

    vf_close(&file);

    /* Header: MEXE, version, header size, payload size,
       entry offset, flags, reserved. */

    const uint8_t *h = mexe_image;

    if (h[0] != 'M' || h[1] != 'E' ||
        h[2] != 'X' || h[3] != 'E') {
        out("run: not a MEXE executable\n");
        return;
    }

    uint16_t version     = mexe_u16(h + 4);
    uint16_t header_size = mexe_u16(h + 6);
    uint32_t load_size   = mexe_u32(h + 8);
    uint32_t entry_offset = mexe_u32(h + 12);

    if (version != 1 || header_size != MEXE_HEADER_SIZE) {
        out("run: unsupported MEXE version/header\n");
        return;
    }

    if (load_size == 0 ||
        load_size != total - header_size ||
        entry_offset >= load_size) {
        out("run: invalid executable layout\n");
        return;
    }

    out("MEXE v1: ");
    out(name);
    out("\n");
    out("Code size: ");
    out_u(load_size);
    out(" bytes\n");

    /*
     * Temporary execution model:
     * entry point is a C function: int program(void).
     *
     * IMPORTANT: this runs in kernel mode, without isolation.
     * Use only trusted test programs in QEMU.
     */

    uint8_t *code = mexe_image + header_size;
    typedef int (*mexe_entry_t)(void);

    mexe_entry_t entry =
        (mexe_entry_t)(void *)(code + entry_offset);

    out("Starting program...\n");

    int result = entry();

    out("Program returned: ");
    out_u((uint32_t)result);
    out("\n");
}


/* write/append: текст до кінця рядка; "\n" у тексті = перенос рядка */
static uint32_t unescape(const char *s, uint8_t *dst, uint32_t cap) {
    uint32_t n = 0;
    while (*s && n < cap) {
        if (s[0] == '\\' && s[1] == 'n') { dst[n++] = '\n'; s += 2; }
        else if (s[0] == '\\' && s[1] == '\\') { dst[n++] = '\\'; s += 2; }
        else dst[n++] = (uint8_t)*s++;
    }
    return n;
}

static void cmd_write(const char *arg, int append) {
    char w[256], p[256];
    vslot_t *s;
    const char *rest = next_word(arg, w, sizeof w);
    if (!w[0]) { out(append ? "usage: append <file> <text>\n" : "usage: write <file> <text>\n"); return; }
    if (resolve_on(w, &s, p, sizeof p)) return;
    rest = skip_sp(rest);
    uint32_t n = unescape(rest, iobuf, sizeof iobuf - 1);
    iobuf[n++] = '\n';
    int rc = append ? v_append_file(s, p, iobuf, n) : v_write_file(s, p, iobuf, n);
    if (rc) { fail(append ? "append" : "write", rc); return; }
    out("ok\n");
}

static void cmd_cp(const char *arg) {
    char a[256], b[256], pa[256], pb[256];
    vslot_t *sa, *sb;
    const char *r = next_word(arg, a, sizeof a);
    next_word(r, b, sizeof b);
    if (!a[0] || !b[0]) { out("usage: cp <from> <to>   (drives allowed: cp usb0:/a.txt usb1:/b.txt)\n"); return; }
    if (resolve_on(a, &sa, pa, sizeof pa) || resolve_on(b, &sb, pb, sizeof pb)) return;
    fat_dirent_t st;
    int rc = v_stat(sb, pb, &st);
    if (rc == 0 && (st.attr & FAT_ATTR_DIR)) {                 /* копіювання в каталог: додати ім'я файлу */
        fat_dirent_t sd;
        if (v_stat(sa, pa, &sd) == 0) {
            int l = u_len(pb);
            if (l + 1 + u_len(sd.name) < (int)sizeof pb) { if (pb[l - 1] != '/') pb[l++] = '/'; for (int i = 0; sd.name[i]; i++) pb[l++] = sd.name[i]; pb[l] = 0; }
        }
    }
    vf_t src, dst;
    rc = vf_open(sa, pa, 0, &src);
    if (rc) { fail("cp", rc); return; }
    if (sa == sb && u_eq(pa, pb)) { out("cp: source and destination are the same\n"); vf_close(&src); return; }
    rc = v_create(sb, pb);
    int existed = (rc == FAT_E_EXIST);
    if (rc && !existed) { fail("cp", rc); vf_close(&src); return; }
    rc = vf_open(sb, pb, 0, &dst);
    if (rc) { fail("cp", rc); vf_close(&src); return; }
    if (existed) { rc = vf_trunc(&dst, 0); if (rc) { fail("cp", rc); vf_close(&src); vf_close(&dst); return; } }
    uint32_t off = 0;
    for (;;) {
        int n = vf_read(&src, off, iobuf, sizeof iobuf);
        if (n < 0) { rc = n; break; }
        if (n == 0) { rc = 0; break; }
        int w = vf_write(&dst, off, iobuf, (uint32_t)n);
        if (w < 0) { rc = w; break; }
        off += (uint32_t)n;
    }
    vf_close(&src);
    int rc2 = vf_close(&dst);
    if (!rc) rc = rc2;
    if (rc) { fail("cp", rc); return; }
    out("ok ("); out_size(off); out(")\n");
}

static void cmd_info(void) {
    int n = usb_msc_count();
    xhci_print_info();
    if (n == 0) { out("No USB mass storage devices (run `usb scan`).\n"); return; }
    for (int i = 0; i < n; i++) {
        usb_msc_t *m = usb_msc_get(i);
        vslot_t *s = slot_of_drive(i);
        out(s && slot_index(s) == cur ? "* " : "  ");
        out_drive(i); out(" "); out(m->vendor); out(" "); out(m->product); out(" rev "); out(m->rev);
        out("  "); out_blocks_size(m->block_count, m->block_size);
        out("  block "); out_u(m->block_size); out(m->write_protected ? "  write-protected" : "");
        if (s) {
            uint32_t tot, fr, cb;
            out("\n    "); out(v_fsname(s));
            if (v_label(s)[0]) { out(" '"); out(v_label(s)); out("'"); }
            if (v_statfs(s, &tot, &fr, &cb) == 0) {
                out(", cluster "); out_size(cb); out(", free ");
                out_blocks_size(fr, cb); out(" of "); out_blocks_size(tot, cb);
            }
            out(", dir "); out(s->cwd);
        } else {
            out("  (not mounted: `usb mount "); out_u((uint32_t)i); out("`)");
        }
        out("\n");
    }
    out("(* = current drive)\n");
}

static const char *class_name(const xhci_seen_t *e) {
    switch (e->dclass) {
    case 0x01: return "Audio";
    case 0x02: return "Communications";
    case 0x03: return e->dproto == 1 ? "Keyboard" : e->dproto == 2 ? "Mouse" : "HID device";
    case 0x05: return "Physical";
    case 0x06: return "Camera/Image";
    case 0x07: return "Printer";
    case 0x08: return "Flash drive / storage";
    case 0x09: return "Hub";
    case 0x0A: return "CDC data";
    case 0x0B: return "Smart card";
    case 0x0E: return "Webcam";
    case 0x0F: return "Personal healthcare";
    case 0xE0: return e->dsub == 1 && e->dproto == 1 ? "Bluetooth adapter" : "Wireless controller";
    case 0xFF: return "Vendor specific";
    default:   return "Unknown";
    }
}

static void out_hex(uint32_t v, int digits) {
    static const char H[] = "0123456789abcdef";
    char b[9]; int i = 0;
    for (int k = digits - 1; k >= 0; k--) b[i++] = H[(v >> (k * 4)) & 0xF];
    b[i] = 0; out(b);
}

static void cmd_list(void) {
    if (!have_scan() && do_scan() && xhci_seen_count() == 0) return;
    int n = xhci_seen_count();
    if (n == 0) { out("No USB devices found on xHCI ports.\n(Devices behind a hub or on a legacy UHCI/EHCI controller are not shown. Run `usb scan` to rescan.)\n"); return; }
    out("USB devices (xHCI):\n");
    for (int i = 0; i < n; i++) {
        const xhci_seen_t *e = xhci_seen_get(i);
        out("  port "); out_u((uint32_t)e->port); out("  ");
        if (e->failed) { out("device detected but could not be read\n"); continue; }
        out_hex(e->vid, 4); out(":"); out_hex(e->pid, 4); out("  ");
        out(class_name(e));
        if (e->manufacturer[0] || e->product[0]) {
            out("  \""); out(e->manufacturer); if (e->manufacturer[0] && e->product[0]) out(" "); out(e->product); out("\"");
        }
        out("  ["); out(xhci_speed_str(e->speed)); out("]");
        if (e->claimed) out("  <- storage drive");
        if (e->dclass == 9) out("  (hubs not supported yet)");
        out("\n");
    }
}

static void cmd_help(void) {
    out("USB flash drives (xHCI; FAT32 and exFAT):\n");
    out("> usb scan                 -  find flash drives, mount them all, switch to the first\n");
    out("> usb0:  usb1: ...         -  switch to that drive (then plain ls/cd/cat/... work on it)\n");
    out("> ram:                     -  switch back to the system disk\n");
    out("> usb mount <N>            -  mount drive N and switch to it\n");
    out("> usb list                 -  all connected USB devices\n");
    out("> usb info                 -  controller / drives / free space (* = current)\n");
    out("> ls [dir], cd, pwd, cat, write, append, create, mkdir, rm, rmdir, mv, cp, edit\n");
    out("                           -  on the current USB drive (also: usb ls, usb cat, ...)\n");
    out("> cp usb0:/a.txt usb1:/    -  copy between drives (any path accepts a usbN: prefix)\n");
    out("> write <file> <text>      -  create/replace file (\\n = new line)\n");
    out("> usb eject [N]            -  flush and safely unmount (current drive by default)\n");
    out("> usb debug on|off         -  verbose USB log (send it to the author if it fails)\n");
}

/* ---- інтеграція з shell / терміналом / редактором ---- */
int usbfs_mounted(void) { return cur >= 0; }

/* При старті системи: флешки вже взяті першим скан-проходом (mouse_init) — просто монтуємо їх,
 * першою стає поточний диск (`ram:` повертає на системний). Без флешки нічого не робить і не друкує. */
void usbfs_autostart(void) {
    if (usb_msc_count() == 0) return;
    scanned = 1;
    mount_all();
}
int usbfs_active(void) { return cur >= 0; }

const char *usbfs_prompt(void) {
    static char b[48];
    if (cur < 0) return "";
    int i = 0;
    b[i++] = 'u'; b[i++] = 's'; b[i++] = 'b';
    uint32_t d = (uint32_t)slots[cur].drive;
    char num[8]; int k = 0;
    if (!d) num[k++] = '0';
    while (d && k < 7) { num[k++] = (char)('0' + d % 10); d /= 10; }
    while (k) b[i++] = num[--k];
    b[i++] = ':';
    const char *c = slots[cur].cwd;
    int cl = u_len(c);
    if (cl > 28) {
        b[i++] = '.'; b[i++] = '.'; b[i++] = '.';
        c += cl - 25;
        while (((uint8_t)*c & 0xC0) == 0x80) c++;          /* не різати багатобайтовий символ */
    }
    while (*c && i < (int)sizeof b - 3) b[i++] = *c++;
    b[i++] = ' ';
    b[i] = 0;
    return b;
}

/* edit <файл> при активному USB-диску -> "usb:файл" (відносно поточного каталогу диска) */
static int has_drive_prefix(const char *a) {
    if (!u_starts(a, "usb")) return 0;
    int i = 3;
    while (a[i] >= '0' && a[i] <= '9') i++;
    return a[i] == ':';
}

const char *usbfs_edit_path(const char *arg, char *buf, int sz) {
    if (cur < 0) return arg;
    if (u_starts(arg, "ram:")) return arg + 4;
    if (has_drive_prefix(arg)) return arg;
    int n = 0;
    const char *pre = "usb:";
    while (*pre && n < sz - 1) buf[n++] = *pre++;
    while (*arg && n < sz - 1) buf[n++] = *arg++;
    buf[n] = 0;
    return buf;
}

/* слот для редактора: "usb:" / "usbN:" або поточний; без поточного — перший змонтований / змонтувати */
static vslot_t *edit_slot(const char **path, int *rc_err) {
    const char *p = *path;
    int err;
    vslot_t *s = drive_prefix(&p, &err);
    if (err) { *rc_err = FAT_E_NOENT; return 0; }
    if (!s) s = cur >= 0 ? &slots[cur] : 0;
    if (!s) {
        for (int i = 0; i < VOL_SLOTS; i++) if (slots[i].used) { s = &slots[i]; break; }
    }
    if (!s) {
        if (!have_scan() && do_scan()) { *rc_err = FAT_E_IO; return 0; }
        int n = usb_msc_count();
        for (int i = 0; i < n && !s; i++) s = mount_drive(i, 0);
        if (!s) { *rc_err = FAT_E_FMT; return 0; }
    }
    *path = p;
    return s;
}

int usbfs_read(const char *path, char *buf, uint32_t cap, uint32_t *len) {
    int err = FAT_E_IO;
    vslot_t *s = edit_slot(&path, &err);
    if (!s) return err;
    char p[256];
    int rc = fat_path_join(s->cwd, path, p, sizeof p);
    if (rc) return rc;
    return v_read_file(s, p, buf, cap, len);
}

int usbfs_write(const char *path, const char *buf, uint32_t n) {
    int err = FAT_E_IO;
    vslot_t *s = edit_slot(&path, &err);
    if (!s) return err;
    char p[256];
    int rc = fat_path_join(s->cwd, path, p, sizeof p);
    if (rc) return rc;
    return v_write_file(s, p, buf, n);
}

/* ---- API для GUI-провідника: без виводу в консоль ---- */
int usbfs_drive_count(void) {
    int n = 0;
    for (int i = 0; i < VOL_SLOTS; i++) if (slots[i].used) n++;
    return n;
}

int usbfs_drive_at(int idx, char *label, int sz) {
    int k = 0;
    for (int i = 0; i < VOL_SLOTS; i++) {
        if (!slots[i].used) continue;
        if (k++ != idx) continue;
        if (label && sz > 0) {
            const char *l = v_label(&slots[i]);
            if (!l[0]) l = v_fsname(&slots[i]);
            int n = 0;
            while (l[n] && n < sz - 1) { label[n] = l[n]; n++; }
            label[n] = 0;
        }
        return slots[i].drive;
    }
    return -1;
}

int usbfs_drive_space(int drive, uint32_t *free_kb, uint32_t *total_kb) {
    vslot_t *s = slot_of_drive(drive);
    if (!s) return FAT_E_NOENT;
    uint32_t tot, fr, cb;
    int rc = v_statfs(s, &tot, &fr, &cb);
    if (rc) return rc;
    if (free_kb)  *free_kb  = (uint32_t)(((uint64_t)fr  * cb) >> 10);
    if (total_kb) *total_kb = (uint32_t)(((uint64_t)tot * cb) >> 10);
    return 0;
}

typedef struct { usbfs_entry_t *o; int max, n; } lst_t;
static int lst_cb(const fat_dirent_t *e, void *arg) {
    lst_t *c = arg;
    if (e->attr & FAT_ATTR_VOLUME) return 0;
    if (u_eq(e->name, ".") || u_eq(e->name, "..")) return 0;
    if (c->n >= c->max) return 1;                                /* місця нема — зупинити обхід */
    usbfs_entry_t *d = &c->o[c->n++];
    int i = 0;
    while (e->name[i] && i < (int)sizeof d->name - 1) { d->name[i] = e->name[i]; i++; }
    d->name[i] = 0;
    d->is_dir = (e->attr & FAT_ATTR_DIR) ? 1 : 0;
    d->size = e->size;
    return 0;
}

int usbfs_list(int drive, const char *path, usbfs_entry_t *out_e, int max, int *count) {
    vslot_t *s = slot_of_drive(drive);
    if (count) *count = 0;
    if (!s) return FAT_E_NOENT;
    lst_t c = { out_e, max, 0 };
    int rc = v_readdir(s, path, lst_cb, &c);
    if (count) *count = c.n;
    return rc;
}

int usbfs_remove(int drive, const char *path) {
    vslot_t *s = slot_of_drive(drive);
    if (!s) return FAT_E_NOENT;
    fat_dirent_t st;
    int rc = v_stat(s, path, &st);
    if (rc) return rc;
    rc = (st.attr & FAT_ATTR_DIR) ? v_rmdir(s, path) : v_unlink(s, path);
    int f = v_flush(s);
    return rc ? rc : f;
}

int usbfs_make(int drive, const char *path, int is_dir) {
    vslot_t *s = slot_of_drive(drive);
    if (!s) return FAT_E_NOENT;
    int rc = is_dir ? v_mkdir(s, path) : v_create(s, path);
    int f = v_flush(s);
    return rc ? rc : f;
}

int usbfs_rename(int drive, const char *from, const char *to) {
    vslot_t *s = slot_of_drive(drive);
    if (!s) return FAT_E_NOENT;
    int rc = v_rename(s, from, to);
    int f = v_flush(s);
    return rc ? rc : f;
}

int usbfs_rescan(void) {
    if (do_scan() == 0) mount_all();
    return usbfs_drive_count();
}

/* перемикання диска: "usb0:", "usb:" або "ram:" */
static int switch_drive(const char *cmd) {
    if (u_eq(cmd, "ram:") || u_eq(cmd, "root:") || u_eq(cmd, "sys:")) {
        if (cur >= 0) { v_flush(&slots[cur]); cur = -1; }
        out("Current drive: ram: (system disk)\n");
        return 1;
    }
    if (!u_starts(cmd, "usb")) return 0;
    int i = 3, d = -1;
    while (cmd[i] >= '0' && cmd[i] <= '9') { d = (d < 0 ? 0 : d * 10) + (cmd[i] - '0'); i++; }
    if (cmd[i] != ':' || cmd[i + 1] != 0) return 0;
    if (d < 0) {                                              /* "usb:" — перший/поточний */
        if (cur >= 0) { out("Current drive: "); out_drive(slots[cur].drive); out("\n"); return 1; }
        mount_all();
        return 1;
    }
    if (!have_scan() && do_scan()) return 1;
    vslot_t *s = mount_drive(d, 1);
    if (!s) return 1;
    cur = slot_index(s);
    out("Current drive: "); out_drive(d); out("\n");
    return 1;
}

static void run_fs_command(const char *sub, const char *arg) {
    char p[256], w[256], w2[256];
    vslot_t *s;
    int rc;
    if (u_eq(sub, "ls") || u_eq(sub, "dir") || u_eq(sub, "list")) cmd_ls(arg);
    else if (u_eq(sub, "pwd")) { out_drive(slots[cur].drive); out(slots[cur].cwd); out("\n"); }
    else if (u_eq(sub, "cd")) {
        next_word(arg, w, sizeof w);
        if (!w[0]) { slots[cur].cwd[0] = '/'; slots[cur].cwd[1] = 0; return; }
        if (resolve_on(w, &s, p, sizeof p)) return;
        fat_dirent_t st;
        if (u_eq(p, "/")) { s->cwd[0] = '/'; s->cwd[1] = 0; cur = slot_index(s); return; }
        rc = v_stat(s, p, &st);
        if (rc) fail("cd", rc);
        else if (!(st.attr & FAT_ATTR_DIR)) out("cd: not a directory\n");
        else { int i = 0; while (p[i] && i < (int)sizeof s->cwd - 1) { s->cwd[i] = p[i]; i++; } s->cwd[i] = 0; cur = slot_index(s); }
    }
    else if (u_eq(sub, "cat") || u_eq(sub, "type")) cmd_cat(arg);
    else if (u_eq(sub, "run")) cmd_run(arg);
    else if (u_eq(sub, "write")) cmd_write(arg, 0);
    else if (u_eq(sub, "append")) cmd_write(arg, 1);
    else if (u_eq(sub, "cp")) cmd_cp(arg);
    else if (u_eq(sub, "create") || u_eq(sub, "touch") || u_eq(sub, "mkdir") || u_eq(sub, "rm") || u_eq(sub, "del") || u_eq(sub, "rmdir")) {
        next_word(arg, w, sizeof w);
        if (!w[0]) { out("usage: "); out(sub); out(" <name>\n"); return; }
        if (resolve_on(w, &s, p, sizeof p)) return;
        if (u_eq(sub, "create") || u_eq(sub, "touch")) rc = v_create(s, p);
        else if (u_eq(sub, "mkdir")) rc = v_mkdir(s, p);
        else if (u_eq(sub, "rmdir")) rc = v_rmdir(s, p);
        else rc = v_unlink(s, p);
        if (rc && !(rc == FAT_E_EXIST && u_eq(sub, "touch"))) fail(sub, rc); else out("ok\n");
    }
    else if (u_eq(sub, "mv") || u_eq(sub, "ren")) {
        vslot_t *s2;
        const char *r = next_word(arg, w, sizeof w);
        next_word(r, w2, sizeof w2);
        if (!w[0] || !w2[0]) { out("usage: mv <from> <to>\n"); return; }
        char p2[256];
        if (resolve_on(w, &s, p, sizeof p) || resolve_on(w2, &s2, p2, sizeof p2)) return;
        if (s != s2) { out("mv: cannot move between drives, use cp + rm\n"); return; }
        rc = v_rename(s, p, p2);
        if (rc) fail("mv", rc); else out("ok\n");
    }
    else if (u_eq(sub, "edit")) {
        next_word(arg, w, sizeof w);
        if (!w[0]) { out("usage: edit <file>\n"); return; }
        out("Run: edit usb:"); out(w); out("   (in the GUI terminal or shell)\n");
    }
}

static int is_fs_cmd(const char *w) {
    static const char *const names[] = { "ls", "dir", "list", "pwd", "cd", "cat", "type", "write", "append", "cp",
                                         "create", "touch", "mkdir", "rm", "del", "rmdir", "mv", "ren", "run", 0 };
    for (int i = 0; names[i]; i++) if (u_eq(w, names[i])) return 1;
    return 0;
}

int usbfs_command(const char *cmd) {
    cmd = skip_sp(cmd);
    if (switch_drive(cmd)) return 1;

    /* звичайні команди, коли активний USB-диск */
    if (cur >= 0 && !(u_eq(cmd, "usb") || u_starts(cmd, "usb "))) {
        char w[16];
        const char *arg = next_word(cmd, w, sizeof w);
        if (!is_fs_cmd(w)) return 0;
        run_fs_command(w, skip_sp(arg));
        return 1;
    }

    if (!(u_eq(cmd, "usb") || u_starts(cmd, "usb "))) return 0;
    const char *a = skip_sp(cmd + 3);
    char sub[16];
    const char *arg = next_word(a, sub, sizeof sub);
    arg = skip_sp(arg);

    if (!sub[0] || u_eq(sub, "help")) { cmd_help(); return 1; }
    if (u_eq(sub, "debug")) {
        if (u_eq(arg, "on")) { usb_debug = 1; out("usb debug on\n"); }
        else if (u_eq(arg, "off")) { usb_debug = 0; out("usb debug off\n"); }
        else out("usage: usb debug on|off\n");
        return 1;
    }
    if (u_eq(sub, "scan")) {
        if (do_scan() == 0) mount_all();
        return 1;
    }
    if (u_eq(sub, "mount") || u_eq(sub, "use")) {              /* usb mount [номер флешки] */
        if (arg[0] >= '0' && arg[0] <= '9') {
            int d = 0;
            for (int i = 0; arg[i] >= '0' && arg[i] <= '9'; i++) d = d * 10 + (arg[i] - '0');
            if (!have_scan() && do_scan()) return 1;
            vslot_t *s = mount_drive(d, 1);
            if (s) { cur = slot_index(s); out("Current drive: "); out_drive(d); out("\n"); }
        } else {
            if (!have_scan() && do_scan()) return 1;
            mount_all();
        }
        return 1;
    }
    if (u_eq(sub, "info")) { cmd_info(); return 1; }
    if (u_eq(sub, "list") || u_eq(sub, "lsusb")) { cmd_list(); return 1; }
    if (u_eq(sub, "eject")) {
        vslot_t *s = 0;
        if (arg[0] >= '0' && arg[0] <= '9') s = slot_of_drive(arg[0] - '0');
        else if (cur >= 0) s = &slots[cur];
        if (!s) { out("not mounted\n"); return 1; }
        int rc = v_flush(s);
        if (rc) { fail("flush", rc); return 1; }
        usb_msc_t *m = usb_msc_get(s->drive);
        usb_msc_sync(m);
        usb_msc_eject(m);
        out_drive(s->drive); out(" ejected. You can unplug it now.\n");
        if (slot_index(s) == cur) { cur = -1; out("Current drive: ram: (system disk)\n"); }
        s->used = 0;
        int any = 0;
        for (int i = 0; i < VOL_SLOTS; i++) if (slots[i].used) any = 1;
        if (!any) { scanned = 0; need_rescan = 1; }
        return 1;
    }

    if (need_mount()) return 1;
    if (is_fs_cmd(sub)) run_fs_command(sub, arg);
    else if (u_eq(sub, "edit")) run_fs_command(sub, arg);
    else out("unknown usb command, try `usb help`\n");
    return 1;
}
