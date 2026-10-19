#include <stdint.h>
#include "../headers/fat32.h"
#ifdef FS_NAMES_CP1251
#include "../../standarts_lib/charset.h"   /* імена в API — CP1251, на диску — UTF-16 */
#endif

#define FAT_PATH_MAX 512
#define EOC          0x0FFFFFFFu
#define EOC_MIN      0x0FFFFFF8u
#define MAX_DIR_ENTRIES 65536u

static void m_cpy(void *d, const void *s, uint32_t n) {
    uint8_t *a = (uint8_t *)d; const uint8_t *b = (const uint8_t *)s;
    while (n--) *a++ = *b++;
}
static void m_set(void *d, uint8_t c, uint32_t n) {
    uint8_t *a = (uint8_t *)d;
    while (n--) *a++ = c;
}

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (le16(p + 2) << 16); }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static void (*clock_cb)(uint16_t *date, uint16_t *time) = 0;

void fat_set_clock(void (*get)(uint16_t *date, uint16_t *time)) { clock_cb = get; }

static void now_dt(uint16_t *d, uint16_t *t) {
    if (clock_cb) { clock_cb(d, t); return; }
    *d = (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1);
    *t = 0;
}
const char *fat_strerror(int e) {
    switch (e) {
    case FAT_OK:           return "ok";
    case FAT_E_IO:         return "I/O error";
    case FAT_E_NOENT:      return "no such file or directory";
    case FAT_E_EXIST:      return "already exists";
    case FAT_E_NOTDIR:     return "not a directory";
    case FAT_E_ISDIR:      return "is a directory";
    case FAT_E_NOSPC:      return "no space left on device";
    case FAT_E_NOTEMPTY:   return "directory not empty";
    case FAT_E_INVAL:      return "invalid name or argument";
    case FAT_E_FMT:        return "not FAT32 or file system is damaged";
    case FAT_E_RO:         return "read-only";
    case FAT_E_NAMETOOLONG:return "name too long";
    }
    return "unknown error";
}

static uint32_t utf8_next(const char **sp) {
    const uint8_t *s = (const uint8_t *)*sp;
    uint32_t c = s[0];
    if (c == 0) return 0;
    if (c < 0x80) { *sp += 1; return c; }
#ifdef FS_NAMES_CP1251
    /* імена, що йдуть із шелла/GUI, — CP1251: один байт = один символ */
    *sp += 1;
    return cs_to_unicode((unsigned char)c);
#endif
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *sp += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *sp += 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *sp += 4; return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    *sp += 1;
    return 0xFFFD;
}

static uint32_t fold(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c < 0x80) return c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;          /* Latin-1 */
    if (c >= 0x391 && c <= 0x3A9) return c + 32;                      /* Greek */
    if (c >= 0x410 && c <= 0x42F) return c + 32;                      /* Cyrillic А-Я */
    if (c >= 0x400 && c <= 0x40F) return c + 80;                      /* Ѐ-Џ */
    if (c >= 0x490 && c <= 0x4BF && (c & 1) == 0) return c + 1;       /* Ґ тощо */
    return c;
}

static int name_eq(const char *a, const char *b) {
    for (;;) {
        uint32_t x = utf8_next(&a), y = utf8_next(&b);
        if (fold(x) != fold(y)) return 0;
        if (x == 0) return 1;
    }
}

/* повертає кількість UTF-16 одиниць або -1 */
static int utf8_to_utf16(const char *s, uint16_t *out, int cap) {
    int n = 0;
    for (;;) {
        uint32_t c = utf8_next(&s);
        if (c == 0) break;
        if (c == 0xFFFD) return -1;
        if (c >= 0x10000) {
            if (n + 2 > cap) return -1;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xD800 | (c >> 10));
            out[n++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            if (n + 1 > cap) return -1;
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

static int put_utf8(char *out, int pos, int cap, uint32_t c) {
#ifdef FS_NAMES_CP1251
    if (pos + 1 >= cap) return -1;
    out[pos] = (char)cs_from_unicode(c);
    return pos + 1;
#endif
    int need = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (pos + need >= cap) return -1;
    if (need == 1) out[pos] = (char)c;
    else if (need == 2) { out[pos] = (char)(0xC0 | (c >> 6)); out[pos + 1] = (char)(0x80 | (c & 0x3F)); }
    else if (need == 3) { out[pos] = (char)(0xE0 | (c >> 12)); out[pos + 1] = (char)(0x80 | ((c >> 6) & 0x3F)); out[pos + 2] = (char)(0x80 | (c & 0x3F)); }
    else { out[pos] = (char)(0xF0 | (c >> 18)); out[pos + 1] = (char)(0x80 | ((c >> 12) & 0x3F)); out[pos + 2] = (char)(0x80 | ((c >> 6) & 0x3F)); out[pos + 3] = (char)(0x80 | (c & 0x3F)); }
    return pos + need;
}

static void utf16_to_utf8(const uint16_t *u, int n, char *out, int cap) {
    int pos = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c < 0xE000) {
            c = '?';
        }
        int np = put_utf8(out, pos, cap, c);
        if (np < 0) break;
        pos = np;
    }
    out[pos] = 0;
}

/* ================================================================== */
/*  Доступ до пристрою і кеш секторів                                  */
/* ================================================================== */
static int dev_rw(fat_vol_t *v, uint32_t sec, uint32_t n, void *buf, int wr) {
    uint32_t lba = v->part_lba + sec * v->scale;
    uint32_t cnt = n * v->scale;
    if (wr) {
        if (v->read_only) return FAT_E_RO;
        return v->bd->write(v->bd->ctx, lba, cnt, buf) ? FAT_E_IO : 0;
    }
    return v->bd->read(v->bd->ctx, lba, cnt, buf) ? FAT_E_IO : 0;
}

static int cache_write_back(fat_vol_t *v, fat_cache_t *c) {
    int r = dev_rw(v, c->sector, 1, c->data, 1);
    if (r) return r;
    if (v->fat_extra_copies && c->sector >= v->fat_start && c->sector < v->fat_start + v->fat_size) {
        for (uint32_t k = 1; k <= v->fat_extra_copies; k++) {
            r = dev_rw(v, c->sector + k * v->fat_size, 1, c->data, 1);
            if (r) return r;
        }
    }
    c->dirty = 0;
    return 0;
}

static fat_cache_t *cache_get(fat_vol_t *v, uint32_t sec, int load) {
    fat_cache_t *victim = 0;
    for (int i = 0; i < FAT_CACHE_LINES; i++) {
        fat_cache_t *c = &v->cache[i];
        if (c->valid && c->sector == sec) { c->stamp = ++v->stamp; return c; }
    }
    for (int i = 0; i < FAT_CACHE_LINES; i++) {
        fat_cache_t *c = &v->cache[i];
        if (!c->valid) { victim = c; break; }
        if (!victim || c->stamp < victim->stamp) victim = c;
    }
    if (victim->valid && victim->dirty) {
        if (cache_write_back(v, victim)) return 0;
    }
    victim->valid = 0;
    victim->dirty = 0;
    if (load) {
        if (dev_rw(v, sec, 1, victim->data, 0)) return 0;
    } else {
        m_set(victim->data, 0, v->bps);
    }
    victim->valid = 1;
    victim->sector = sec;
    victim->stamp = ++v->stamp;
    return victim;
}

static int cache_flush_range(fat_vol_t *v, uint32_t sec, uint32_t n) {
    for (int i = 0; i < FAT_CACHE_LINES; i++) {
        fat_cache_t *c = &v->cache[i];
        if (c->valid && c->dirty && c->sector >= sec && c->sector - sec < n) {
            int r = cache_write_back(v, c);
            if (r) return r;
        }
    }
    return 0;
}

static void cache_invalidate_range(fat_vol_t *v, uint32_t sec, uint32_t n) {
    for (int i = 0; i < FAT_CACHE_LINES; i++) {
        fat_cache_t *c = &v->cache[i];
        if (c->valid && c->sector >= sec && c->sector - sec < n) { c->valid = 0; c->dirty = 0; }
    }
}

/* пряме багатосекторне введення-виведення, узгоджене з кешем */
static int data_io(fat_vol_t *v, uint32_t sec, uint32_t n, void *buf, int wr) {
    if (wr) {
        cache_invalidate_range(v, sec, n);
        return dev_rw(v, sec, n, buf, 1);
    }
    int r = cache_flush_range(v, sec, n);
    if (r) return r;
    return dev_rw(v, sec, n, buf, 0);
}

int fat_flush(fat_vol_t *v) {
    if (v->read_only) return 0;
    int r = 0;

    if (v->fsinfo_dirty && v->fsinfo_sector) {
        fat_cache_t *c = cache_get(v, v->fsinfo_sector, 1);
        if (c) {
            put32(c->data + 488, v->free_count);
            put32(c->data + 492, v->next_free);
            c->dirty = 1;
        } else r = FAT_E_IO;
        v->fsinfo_dirty = 0;
    }
    for (int i = 0; i < FAT_CACHE_LINES; i++) {
        fat_cache_t *c = &v->cache[i];
        if (c->valid && c->dirty) {
            int rr = cache_write_back(v, c);
            if (rr && !r) r = rr;
        }
    }
    if (v->bd->flush && v->bd->flush(v->bd->ctx) && !r) r = FAT_E_IO;
    return r;
}

/* ================================================================== */
/*  FAT                                                                */
/* ================================================================== */
static uint32_t max_cluster(const fat_vol_t *v) { return v->total_clusters + 1; }
static uint32_t clus_sector(const fat_vol_t *v, uint32_t cl) { return v->data_start + (cl - 2) * v->spc; }
static int cl_valid(const fat_vol_t *v, uint32_t cl) { return cl >= 2 && cl <= max_cluster(v); }

static int fat_get(fat_vol_t *v, uint32_t cl, uint32_t *val) {
    uint32_t off = cl * 4;
    uint32_t sec = off / v->bps;
    if (sec >= v->fat_size) return FAT_E_FMT;
    fat_cache_t *c = cache_get(v, v->fat_start + sec, 1);
    if (!c) return FAT_E_IO;
    *val = le32(c->data + off % v->bps) & 0x0FFFFFFFu;
    return 0;
}

static int fat_set(fat_vol_t *v, uint32_t cl, uint32_t val) {
    if (v->read_only) return FAT_E_RO;
    uint32_t off = cl * 4;
    uint32_t sec = off / v->bps;
    if (sec >= v->fat_size) return FAT_E_FMT;
    fat_cache_t *c = cache_get(v, v->fat_start + sec, 1);
    if (!c) return FAT_E_IO;
    uint8_t *p = c->data + off % v->bps;
    put32(p, (le32(p) & 0xF0000000u) | (val & 0x0FFFFFFFu));
    c->dirty = 1;
    return 0;
}

/* 0 — ок, *next == 0 означає кінець ланцюжка */
static int chain_next(fat_vol_t *v, uint32_t cl, uint32_t *next) {
    uint32_t val;
    int r = fat_get(v, cl, &val);
    if (r) return r;
    if (val >= EOC_MIN) { *next = 0; return 0; }
    if (!cl_valid(v, val)) return FAT_E_FMT;
    *next = val;
    return 0;
}

static int alloc_cluster(fat_vol_t *v, uint32_t hint, uint32_t *out) {
    uint32_t total = v->total_clusters;
    uint32_t start = hint;
    if (!cl_valid(v, start)) start = v->next_free;
    if (!cl_valid(v, start)) start = 2;

    for (uint32_t i = 0; i < total; i++) {
        uint32_t cl = 2 + (start - 2 + i) % total;
        uint32_t val;
        int r = fat_get(v, cl, &val);
        if (r) return r;
        if (val == 0) {
            r = fat_set(v, cl, EOC);
            if (r) return r;
            *out = cl;
            v->next_free = (cl + 1 <= max_cluster(v)) ? cl + 1 : 2;
            if (v->free_count != 0xFFFFFFFFu && v->free_count) v->free_count--;
            v->fsinfo_dirty = 1;
            return 0;
        }
    }
    return FAT_E_NOSPC;
}

static int free_chain(fat_vol_t *v, uint32_t cl) {
    uint32_t guard = 0;
    while (cl_valid(v, cl) && guard++ <= v->total_clusters) {
        uint32_t val;
        int r = fat_get(v, cl, &val);
        if (r) return r;
        r = fat_set(v, cl, 0);
        if (r) return r;
        if (v->free_count != 0xFFFFFFFFu) v->free_count++;
        if (cl < v->next_free || !cl_valid(v, v->next_free)) v->next_free = cl;
        if (val >= EOC_MIN || val == 0) break;
        cl = val;
    }
    v->fsinfo_dirty = 1;
    return 0;
}

static uint8_t zbuf[16384];

static int zero_cluster(fat_vol_t *v, uint32_t cl) {
    uint32_t per = sizeof(zbuf) / v->bps;
    uint32_t sec = clus_sector(v, cl), left = v->spc;
    while (left) {
        uint32_t n = left < per ? left : per;
        int r = data_io(v, sec, n, zbuf, 1);
        if (r) return r;
        sec += n; left -= n;
    }
    return 0;
}

/* ================================================================== */
/*  Ітератор каталогу                                                  */
/* ================================================================== */
typedef struct { fat_vol_t *v; uint32_t first, cl, idx; } dit_t;

static uint32_t epc(const fat_vol_t *v) { return v->cluster_bytes / 32; }

static void dit_init(dit_t *it, fat_vol_t *v, uint32_t first) {
    it->v = v; it->first = first; it->cl = first; it->idx = 0;
}

static void dit_pos(const dit_t *it, uint32_t *sec, uint32_t *off) {
    uint32_t eps = it->v->bps / 32;
    uint32_t in_cl = it->idx % epc(it->v);
    *sec = clus_sector(it->v, it->cl) + in_cl / eps;
    *off = (in_cl % eps) * 32;
}

/* 1 — запис прочитано, 0 — кінець ланцюжка, <0 — помилка */
static int dit_load(dit_t *it, uint8_t out[32]) {
    if (!it->cl) return 0;
    uint32_t sec, off;
    dit_pos(it, &sec, &off);
    fat_cache_t *c = cache_get(it->v, sec, 1);
    if (!c) return FAT_E_IO;
    m_cpy(out, c->data + off, 32);
    return 1;
}

static int dit_store(dit_t *it, const uint8_t in[32]) {
    if (!it->cl) return FAT_E_FMT;
    if (it->v->read_only) return FAT_E_RO;
    uint32_t sec, off;
    dit_pos(it, &sec, &off);
    fat_cache_t *c = cache_get(it->v, sec, 1);
    if (!c) return FAT_E_IO;
    m_cpy(c->data + off, in, 32);
    c->dirty = 1;
    return 0;
}

static int dit_next(dit_t *it) {
    it->idx++;
    if (it->cl && it->idx % epc(it->v) == 0) {
        uint32_t n;
        int r = chain_next(it->v, it->cl, &n);
        if (r) return r;
        it->cl = n;
    }
    return 0;
}

static int dit_seek(dit_t *it, uint32_t idx) {
    dit_init(it, it->v, it->first);
    uint32_t steps = idx / epc(it->v);
    while (steps-- && it->cl) {
        uint32_t n;
        int r = chain_next(it->v, it->cl, &n);
        if (r) return r;
        it->cl = n;
    }
    it->idx = idx;
    return it->cl ? 0 : FAT_E_FMT;
}

/* ================================================================== */
/*  Читання каталогу (з LFN)                                           */
/* ================================================================== */
static uint8_t lfn_checksum(const uint8_t *sn) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) ? 0x80 : 0) + (s >> 1) + sn[i]);
    return s;
}

static const int lfn_offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static char walk_alias[16];            /* коротке ім'я поточного запису ("README~1.TXT") */

static void short_to_str(const uint8_t *raw, char *out, int cap) {
    int pos = 0;
    uint8_t nt = raw[12];
    for (int part = 0; part < 2; part++) {
        int from = part ? 8 : 0, len = part ? 3 : 8;
        int end = len;
        while (end > 0 && raw[from + end - 1] == ' ') end--;
        if (part && end == 0) break;
        if (part) { if (pos + 1 < cap) out[pos++] = '.'; }
        for (int i = 0; i < end; i++) {
            uint32_t c = raw[from + i];
            if (part == 0 && i == 0 && c == 0x05) c = 0xE5;
            if (c >= 'A' && c <= 'Z') {
                if (part == 0 && (nt & 0x08)) c += 32;
                if (part == 1 && (nt & 0x10)) c += 32;
            }
            int np = put_utf8(out, pos, cap, c);
            if (np < 0) { out[pos] = 0; return; }
            pos = np;
        }
    }
    out[pos] = 0;
}

typedef int (*walk_cb)(const fat_dirent_t *e, void *arg);

static uint16_t lfn_buf[20 * 13 + 1];

/* Обхід каталогу dc. Колбек повертає !=0, щоб зупинитись (це значення повертається нагору). */
static int dir_walk(fat_vol_t *v, uint32_t dc, walk_cb cb, void *arg) {
    static fat_dirent_t e;
    dit_t it;
    dit_init(&it, v, dc);

    int lfn_ok = 0, lfn_expect = 0, lfn_len = 0;
    uint8_t lfn_sum = 0;
    uint32_t lfn_first = 0;

    for (;;) {
        uint8_t raw[32];
        int r = dit_load(&it, raw);
        if (r < 0) return r;
        if (r == 0) break;
        if (raw[0] == 0x00) break;

        if (raw[0] == 0xE5) {
            lfn_ok = 0;
        } else if ((raw[11] & 0x3F) == FAT_ATTR_LFN) {
            uint32_t ord = raw[0];
            uint32_t seq = ord & 0x3F;
            if (ord & 0x40) {
                lfn_ok = (seq >= 1 && seq <= 20);
                lfn_expect = (int)seq;
                lfn_sum = raw[13];
                lfn_first = it.idx;
                lfn_len = (int)seq * 13;
                for (int i = 0; i < (int)(sizeof(lfn_buf) / 2); i++) lfn_buf[i] = 0xFFFF;
            } else if (lfn_ok && (int)seq == lfn_expect - 1 && raw[13] == lfn_sum && seq >= 1) {
                lfn_expect--;
            } else {
                lfn_ok = 0;
            }
            if (lfn_ok)
                for (int i = 0; i < 13; i++)
                    lfn_buf[(lfn_expect - 1) * 13 + i] = (uint16_t)le16(raw + lfn_offs[i]);
        } else if (raw[11] & FAT_ATTR_VOLUME) {
            lfn_ok = 0;
        } else {
            int use_lfn = lfn_ok && lfn_expect == 1 && lfn_checksum(raw) == lfn_sum;
            short_to_str(raw, walk_alias, sizeof(walk_alias));

            int skip = (raw[0] == '.' && (raw[1] == ' ' || (raw[1] == '.' && raw[2] == ' ')));
            if (!skip) {
                if (use_lfn) {
                    int n = 0;
                    while (n < lfn_len && lfn_buf[n] != 0x0000 && lfn_buf[n] != 0xFFFF) n++;
                    utf16_to_utf8(lfn_buf, n, e.name, sizeof(e.name));
                    e.lfn_idx = lfn_first;
                } else {
                    short_to_str(raw, e.name, sizeof(e.name));
                    e.lfn_idx = it.idx;
                }
                e.attr = raw[11];
                e.ctime = (uint16_t)le16(raw + 14);
                e.cdate = (uint16_t)le16(raw + 16);
                e.adate = (uint16_t)le16(raw + 18);
                e.wtime = (uint16_t)le16(raw + 22);
                e.wdate = (uint16_t)le16(raw + 24);
                e.first_cluster = (le16(raw + 20) << 16) | le16(raw + 26);
                e.size = le32(raw + 28);
                e.dir_cluster = dc;
                e.ent_idx = it.idx;
                int cr = cb(&e, arg);
                if (cr) return cr;
            }
            lfn_ok = 0;
        }
        r = dit_next(&it);
        if (r) return r;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Пошук імені в каталозі                                             */
/* ------------------------------------------------------------------ */
typedef struct { const char *name; fat_dirent_t *out; } look_t;

static int look_cb(const fat_dirent_t *e, void *arg) {
    look_t *l = (look_t *)arg;
    if (name_eq(e->name, l->name) || name_eq(walk_alias, l->name)) {
        *l->out = *e;
        return 1;
    }
    return 0;
}

/* 0 — знайдено, FAT_E_NOENT, або помилка */
static int dir_lookup(fat_vol_t *v, uint32_t dc, const char *name, fat_dirent_t *out) {
    look_t l;
    l.name = name; l.out = out;
    int r = dir_walk(v, dc, look_cb, &l);
    if (r > 0) return 0;
    if (r < 0) return r;
    return FAT_E_NOENT;
}

/* ================================================================== */
/*  Шляхи                                                              */
/* ================================================================== */
int fat_path_join(const char *cwd, const char *arg, char *out, int outsz) {
    int len = 0;
    out[0] = 0;
    if (outsz < 2) return FAT_E_INVAL;

    const char *src[2];
    int ns = 0;
    if (arg[0] != '/' && arg[0] != '\\') src[ns++] = cwd;
    src[ns++] = arg;

    for (int si = 0; si < ns; si++) {
        const char *p = src[si];
        for (;;) {
            while (*p == '/' || *p == '\\') p++;
            if (!*p) break;
            const char *c = p;
            while (*p && *p != '/' && *p != '\\') p++;
            int clen = (int)(p - c);

            if (clen == 1 && c[0] == '.') continue;
            if (clen == 2 && c[0] == '.' && c[1] == '.') {
                while (len > 0 && out[len - 1] != '/') len--;
                if (len > 0) len--;
                out[len] = 0;
                continue;
            }
            if (clen > FAT_NAME_MAX) return FAT_E_NAMETOOLONG;
            if (len + 1 + clen >= outsz) return FAT_E_NAMETOOLONG;
            out[len++] = '/';
            for (int i = 0; i < clen; i++) out[len++] = c[i];
            out[len] = 0;
        }
    }
    if (len == 0) { out[0] = '/'; out[1] = 0; }
    return 0;
}

static char pbuf[3][FAT_PATH_MAX];

static int canon(const char *path, char *out) {
    return fat_path_join("/", path, out, FAT_PATH_MAX);
}

static void root_entry(fat_vol_t *v, fat_dirent_t *e) {
    m_set(e, 0, sizeof(*e));
    e->attr = FAT_ATTR_DIR;
    e->first_cluster = v->root_cluster;
    e->dir_cluster = 0;
}

static uint32_t dir_start(fat_vol_t *v, const fat_dirent_t *e) {
    return e->first_cluster ? e->first_cluster : v->root_cluster;
}

/* canon-шлях -> батьківський каталог + ім'я листа */
static int resolve_parent(fat_vol_t *v, const char *cp, uint32_t *parent_cl, char *leaf) {
    if (cp[0] != '/' || cp[1] == 0) return FAT_E_INVAL;
    const char *last = cp;
    for (const char *p = cp; *p; p++) if (*p == '/') last = p;

    uint32_t cl = v->root_cluster;
    const char *p = cp + 1;
    while (p < last) {
        char comp[FAT_NAME_MAX + 1];
        int n = 0;
        while (p < last && *p != '/') comp[n++] = *p++;
        comp[n] = 0;
        if (p < last) p++;

        fat_dirent_t e;
        int r = dir_lookup(v, cl, comp, &e);
        if (r) return r;
        if (!(e.attr & FAT_ATTR_DIR)) return FAT_E_NOTDIR;
        cl = dir_start(v, &e);
    }
    int n = 0;
    for (const char *q = last + 1; *q; q++) leaf[n++] = *q;
    leaf[n] = 0;
    *parent_cl = cl;
    return 0;
}

static int resolve(fat_vol_t *v, const char *cp, fat_dirent_t *out) {
    if (cp[0] == '/' && cp[1] == 0) { root_entry(v, out); return 0; }
    uint32_t pc;
    char leaf[FAT_NAME_MAX + 1];
    int r = resolve_parent(v, cp, &pc, leaf);
    if (r) return r;
    return dir_lookup(v, pc, leaf, out);
}

int fat_stat(fat_vol_t *v, const char *path, fat_dirent_t *out) {
    int r = canon(path, pbuf[0]);
    if (r) return r;
    return resolve(v, pbuf[0], out);
}

typedef struct { fat_iter_cb cb; void *arg; } iter_t;
static int iter_cb(const fat_dirent_t *e, void *arg) {
    iter_t *i = (iter_t *)arg;
    return i->cb(e, i->arg);
}

int fat_readdir(fat_vol_t *v, const char *path, fat_iter_cb cb, void *arg) {
    fat_dirent_t d;
    int r = fat_stat(v, path, &d);
    if (r) return r;
    if (!(d.attr & FAT_ATTR_DIR)) return FAT_E_NOTDIR;
    iter_t it; it.cb = cb; it.arg = arg;
    r = dir_walk(v, dir_start(v, &d), iter_cb, &it);
    return r < 0 ? r : 0;
}

/* ================================================================== */
/*  Створення записів (короткі імена + LFN)                            */
/* ================================================================== */
static int is_sn_char(uint32_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '(': case ')':
    case '-': case '@': case '^': case '_': case '`': case '{': case '}': case '~': return 1;
    }
    return 0;
}

typedef struct { uint8_t sn[11]; uint8_t nt; int need_lfn; int lossy; } shortname_t;

static int name_valid(const uint16_t *u, int n) {
    if (n <= 0 || n > 255) return 0;
    if (u[n - 1] == ' ' || u[n - 1] == '.') return 0;
    if (u[0] == ' ') return 0;
    for (int i = 0; i < n; i++) {
        uint16_t c = u[i];
        if (c < 0x20) return 0;
        switch (c) { case '"': case '*': case '/': case ':': case '<': case '>': case '?': case '\\': case '|': return 0; }
    }
    return 1;
}

static void build_short(const uint16_t *u, int n, shortname_t *o) {
    m_set(o->sn, ' ', 11);
    o->nt = 0; o->need_lfn = 0; o->lossy = 0;

    int start = 0;
    while (start < n && u[start] == '.') start++;
    int lossy = start > 0;

    int dot = -1;
    for (int i = start; i < n; i++) if (u[i] == '.') dot = i;
    int base_end = dot < 0 ? n : dot;

    int bl = 0, el = 0;
    int base_lower = 0, base_upper = 0, ext_lower = 0, ext_upper = 0;
    int base_chars = 0, ext_chars = 0;

    for (int i = start; i < n; i++) {
        uint32_t c = u[i];
        int in_ext = (dot >= 0 && i > dot);
        if (i == dot) continue;
        if (!in_ext && c == '.') { lossy = 1; continue; }
        if (c == ' ') { lossy = 1; continue; }

        uint32_t out;
        int lo = 0, up = 0;
        if (c >= 'a' && c <= 'z') { out = c - 32; lo = 1; }
        else if (c >= 'A' && c <= 'Z') { out = c; up = 1; }
        else if (c > 0x7F) { out = '_'; lossy = 1; }
        else if (is_sn_char(c)) { out = c; }
        else { out = '_'; lossy = 1; }

        if (!in_ext) {
            base_chars++;
            if (bl < 8) { o->sn[bl++] = (uint8_t)out; if (lo) base_lower = 1; if (up) base_upper = 1; }
            else lossy = 1;
        } else {
            ext_chars++;
            if (el < 3) { o->sn[8 + el++] = (uint8_t)out; if (lo) ext_lower = 1; if (up) ext_upper = 1; }
            else lossy = 1;
        }
    }
    (void)base_end; (void)base_chars; (void)ext_chars;

    if (bl == 0) { o->sn[0] = '_'; lossy = 1; }
    if (o->sn[0] == 0xE5) o->sn[0] = 0x05;

    if (lossy) {
        o->lossy = 1;
        o->need_lfn = 1;
    } else {
        if ((base_lower && base_upper) || (ext_lower && ext_upper)) o->need_lfn = 1;
        else {
            if (base_lower) o->nt |= 0x08;
            if (ext_lower)  o->nt |= 0x10;
        }
    }
}

static void apply_tail(uint8_t *sn, uint32_t num) {
    char t[8];
    int tl = 0;
    t[tl++] = '~';
    char d[8]; int dn = 0;
    while (num) { d[dn++] = (char)('0' + num % 10); num /= 10; }
    while (dn) t[tl++] = d[--dn];

    int bl = 0;
    while (bl < 8 && sn[bl] != ' ') bl++;
    if (bl > 8 - tl) bl = 8 - tl;
    for (int i = 0; i < tl; i++) sn[bl + i] = (uint8_t)t[i];
    for (int i = bl + tl; i < 8; i++) sn[i] = ' ';
}

static int short_exists(fat_vol_t *v, uint32_t dc, const uint8_t *sn) {
    dit_t it;
    dit_init(&it, v, dc);
    for (;;) {
        uint8_t raw[32];
        int r = dit_load(&it, raw);
        if (r < 0) return r;
        if (r == 0 || raw[0] == 0x00) return 0;
        if (raw[0] != 0xE5 && (raw[11] & 0x3F) != FAT_ATTR_LFN && !(raw[11] & FAT_ATTR_VOLUME)) {
            int same = 1;
            for (int i = 0; i < 11; i++) if (raw[i] != sn[i]) { same = 0; break; }
            if (same) return 1;
        }
        r = dit_next(&it);
        if (r) return r;
    }
}

/* гарантувати, що в каталозі є місце на `entries` записів (довантажує кластери) */
static int dir_ensure_capacity(fat_vol_t *v, uint32_t dc, uint32_t entries) {
    if (entries > MAX_DIR_ENTRIES) return FAT_E_NOSPC;
    uint32_t need = (entries + epc(v) - 1) / epc(v);
    uint32_t have = 1, cl = dc;
    for (;;) {
        uint32_t n;
        int r = chain_next(v, cl, &n);
        if (r) return r;
        if (!n) break;
        cl = n; have++;
    }
    while (have < need) {
        uint32_t nc;
        int r = alloc_cluster(v, cl + 1, &nc);
        if (r) return r;
        r = zero_cluster(v, nc);
        if (r) { free_chain(v, nc); return r; }
        r = fat_set(v, cl, nc);
        if (r) return r;
        cl = nc; have++;
    }
    return 0;
}

static int dir_find_slots(fat_vol_t *v, uint32_t dc, uint32_t count, uint32_t *start_out) {
    dit_t it;
    dit_init(&it, v, dc);
    uint32_t run_start = 0, run_len = 0, start = 0;

    for (;;) {
        uint8_t raw[32];
        uint32_t cur = it.idx;
        int r = dit_load(&it, raw);
        if (r < 0) return r;
        if (r == 0 || raw[0] == 0x00) { start = run_len ? run_start : cur; break; }
        if (raw[0] == 0xE5) {
            if (!run_len) run_start = cur;
            if (++run_len >= count) { *start_out = run_start; return 0; }
        } else {
            run_len = 0;
        }
        r = dit_next(&it);
        if (r) return r;
    }
    int r = dir_ensure_capacity(v, dc, start + count);
    if (r) return r;
    *start_out = start;
    return 0;
}

static void fill_short_entry(uint8_t *raw, const uint8_t *sn, uint8_t nt, uint8_t attr,
                             uint32_t cl, uint32_t size, uint16_t date, uint16_t time) {
    m_set(raw, 0, 32);
    m_cpy(raw, sn, 11);
    raw[11] = attr;
    raw[12] = nt;
    put16(raw + 14, time);
    put16(raw + 16, date);
    put16(raw + 18, date);
    put16(raw + 20, cl >> 16);
    put16(raw + 22, time);
    put16(raw + 24, date);
    put16(raw + 26, cl & 0xFFFF);
    put32(raw + 28, size);
}

static int dir_add_entry(fat_vol_t *v, uint32_t dc, const char *name, uint8_t attr,
                         uint32_t cl, uint32_t size, fat_dirent_t *out) {
    static uint16_t u[260];
    if (v->read_only) return FAT_E_RO;
    int n = utf8_to_utf16(name, u, 255);
    if (n < 0) return FAT_E_INVAL;
    if (!name_valid(u, n)) return FAT_E_INVAL;

    shortname_t sh;
    build_short(u, n, &sh);

    int need_tail = sh.lossy;
    if (!need_tail) {
        int e = short_exists(v, dc, sh.sn);
        if (e < 0) return e;
        need_tail = e;
        if (e) sh.need_lfn = 1;
    }
    if (need_tail) {
        uint8_t base[11];
        m_cpy(base, sh.sn, 11);
        int found = 0;
        for (uint32_t k = 1; k < 1000000; k++) {
            m_cpy(sh.sn, base, 11);
            apply_tail(sh.sn, k);
            int e = short_exists(v, dc, sh.sn);
            if (e < 0) return e;
            if (!e) { found = 1; break; }
        }
        if (!found) return FAT_E_NOSPC;
        sh.nt = 0;
        sh.need_lfn = 1;
    }

    uint32_t n_lfn = sh.need_lfn ? (uint32_t)((n + 12) / 13) : 0;
    uint32_t total = n_lfn + 1;
    uint32_t start;
    int r = dir_find_slots(v, dc, total, &start);
    if (r) return r;

    uint16_t date, time;
    now_dt(&date, &time);

    dit_t it;
    dit_init(&it, v, dc);
    r = dit_seek(&it, start);
    if (r) return r;

    uint8_t sum = lfn_checksum(sh.sn);
    for (uint32_t k = n_lfn; k >= 1; k--) {
        uint8_t raw[32];
        m_set(raw, 0, 32);
        raw[0] = (uint8_t)(k | (k == n_lfn ? 0x40 : 0));
        raw[11] = FAT_ATTR_LFN;
        raw[13] = sum;
        for (int j = 0; j < 13; j++) {
            uint32_t ci = (k - 1) * 13 + (uint32_t)j;
            uint16_t ch = ci < (uint32_t)n ? u[ci] : (ci == (uint32_t)n ? 0x0000 : 0xFFFF);
            put16(raw + lfn_offs[j], ch);
        }
        r = dit_store(&it, raw);
        if (r) return r;
        r = dit_next(&it);
        if (r) return r;
    }

    uint8_t raw[32];
    fill_short_entry(raw, sh.sn, sh.nt, attr, cl, size, date, time);
    uint32_t ent_idx = it.idx;
    r = dit_store(&it, raw);
    if (r) return r;

    if (out) {
        m_set(out, 0, sizeof(*out));
        short_to_str(raw, out->name, sizeof(out->name));
        if (n_lfn) { int i = 0; while (name[i] && i < FAT_NAME_MAX) { out->name[i] = name[i]; i++; } out->name[i] = 0; }
        out->attr = attr;
        out->size = size;
        out->first_cluster = cl;
        out->cdate = out->wdate = out->adate = date;
        out->ctime = out->wtime = time;
        out->dir_cluster = dc;
        out->ent_idx = ent_idx;
        out->lfn_idx = start;
    }
    return 0;
}

static int ent_update(fat_vol_t *v, const fat_dirent_t *e, uint32_t cl, uint32_t size, int touch) {
    dit_t it;
    dit_init(&it, v, e->dir_cluster);
    int r = dit_seek(&it, e->ent_idx);
    if (r) return r;
    uint8_t raw[32];
    r = dit_load(&it, raw);
    if (r < 0) return r;
    if (r == 0) return FAT_E_FMT;
    put16(raw + 20, cl >> 16);
    put16(raw + 26, cl & 0xFFFF);
    put32(raw + 28, size);
    if (touch) {
        uint16_t d, t;
        now_dt(&d, &t);
        put16(raw + 22, t);
        put16(raw + 24, d);
        put16(raw + 18, d);
        if (!(raw[11] & FAT_ATTR_DIR)) raw[11] |= FAT_ATTR_ARCHIVE;
    }
    return dit_store(&it, raw);
}

static int ent_delete(fat_vol_t *v, const fat_dirent_t *e) {
    dit_t it;
    dit_init(&it, v, e->dir_cluster);
    int r = dit_seek(&it, e->lfn_idx);
    if (r) return r;
    for (uint32_t i = e->lfn_idx; i <= e->ent_idx; i++) {
        uint8_t raw[32];
        r = dit_load(&it, raw);
        if (r < 0) return r;
        if (r == 0) return FAT_E_FMT;
        raw[0] = 0xE5;
        r = dit_store(&it, raw);
        if (r) return r;
        r = dit_next(&it);
        if (r) return r;
    }
    return 0;
}

/* ================================================================== */
/*  Файли                                                              */
/* ================================================================== */
static int file_cluster_at(fat_file_t *f, uint32_t idx, uint32_t *cl) {
    fat_vol_t *v = f->v;
    if (!f->first_cluster) { *cl = 0; return 0; }
    uint32_t i = 0, c = f->first_cluster;
    if (f->cur_cl && f->cur_idx <= idx) { i = f->cur_idx; c = f->cur_cl; }
    while (i < idx) {
        uint32_t n;
        int r = chain_next(v, c, &n);
        if (r) return r;
        if (!n) { *cl = 0; return 0; }
        c = n; i++;
    }
    f->cur_idx = i; f->cur_cl = c;
    *cl = c;
    return 0;
}

static int file_extend_to(fat_file_t *f, uint32_t need) {
    fat_vol_t *v = f->v;
    if (!need) return 0;
    uint32_t added_first = 0, pred = 0, old_first = f->first_cluster;
    uint32_t cl, idx, n;
    int r;

    if (!f->first_cluster) {
        r = alloc_cluster(v, 0, &cl);
        if (r) return r;
        f->first_cluster = cl;
        added_first = cl;
        f->cur_cl = cl; f->cur_idx = 0;
    }
    if (f->cur_cl && f->cur_idx < need) { cl = f->cur_cl; idx = f->cur_idx; }
    else { cl = f->first_cluster; idx = 0; }

    while (idx + 1 < need) {
        r = chain_next(v, cl, &n);
        if (r) goto fail;
        if (!n) {
            r = alloc_cluster(v, cl + 1, &n);
            if (r) goto fail;
            r = fat_set(v, cl, n);
            if (r) { free_chain(v, n); goto fail; }
            if (!added_first) { added_first = n; pred = cl; }
        }
        cl = n; idx++;
    }
    return 0;

fail:
    if (added_first) {
        free_chain(v, added_first);
        if (pred) fat_set(v, pred, EOC);
        else f->first_cluster = old_first;
    }
    f->cur_cl = 0; f->cur_idx = 0;
    return r;
}

static int partial_sector(fat_vol_t *v, uint32_t sec, uint32_t off, uint8_t *p, uint32_t n, int wr) {
    fat_cache_t *c = cache_get(v, sec, 1);
    if (!c) return FAT_E_IO;
    if (wr) { m_cpy(c->data + off, p, n); c->dirty = 1; }
    else    { m_cpy(p, c->data + off, n); }
    return 0;
}

/* буфер [len байт] лежить у суміжних секторах від кластера cl зі зсувом in_cl */
static int range_io(fat_vol_t *v, uint32_t cl, uint32_t in_cl, uint8_t *buf, uint32_t len, int wr) {
    uint32_t sec = clus_sector(v, cl) + in_cl / v->bps;
    uint32_t head = in_cl % v->bps;
    int r;

    if (head) {
        uint32_t take = v->bps - head;
        if (take > len) take = len;
        r = partial_sector(v, sec, head, buf, take, wr);
        if (r) return r;
        sec++; buf += take; len -= take;
    }
    uint32_t full = len / v->bps;
    if (full) {
        r = data_io(v, sec, full, buf, wr);
        if (r) return r;
        sec += full; buf += full * v->bps; len -= full * v->bps;
    }
    if (len) {
        r = partial_sector(v, sec, 0, buf, len, wr);
        if (r) return r;
    }
    return 0;
}

#define MAX_RUN_CLUSTERS 64

static int file_rw(fat_file_t *f, uint32_t off, uint8_t *buf, uint32_t n, int wr) {
    fat_vol_t *v = f->v;
    uint32_t done = 0;
    while (n) {
        uint32_t idx = off / v->cluster_bytes;
        uint32_t in_cl = off % v->cluster_bytes;
        uint32_t cl;
        int r = file_cluster_at(f, idx, &cl);
        if (r) return r;
        if (!cl) return FAT_E_FMT;                         /* ланцюжок коротший за розмір файлу */

        uint32_t run_bytes = v->cluster_bytes - in_cl, run = 1, c = cl;
        while (run_bytes < n && run < MAX_RUN_CLUSTERS) {
            uint32_t nx;
            r = chain_next(v, c, &nx);
            if (r) return r;
            if (nx != c + 1) break;
            c = nx; run++; run_bytes += v->cluster_bytes;
        }
        uint32_t take = n < run_bytes ? n : run_bytes;
        r = range_io(v, cl, in_cl, buf, take, wr);
        if (r) return r;

        f->cur_idx = idx + run - 1; f->cur_cl = c;
        off += take; buf += take; n -= take; done += take;
    }
    return (int)done;
}

int fat_fread(fat_file_t *f, uint32_t off, void *buf, uint32_t n) {
    if (off >= f->size) return 0;
    if (n > f->size - off) n = f->size - off;
    if (!n) return 0;
    if (n > 0x7FFFFFFFu) n = 0x7FFFFFFFu;
    return file_rw(f, off, (uint8_t *)buf, n, 0);
}

int fat_fwrite(fat_file_t *f, uint32_t off, const void *buf, uint32_t n) {
    fat_vol_t *v = f->v;
    if (!f->writable || v->read_only) return FAT_E_RO;
    if (!n) return 0;
    if (off > f->size) return FAT_E_INVAL;
    if (n > 0x7FFFFFFFu || off + n < off || off + n > 0xFFFFFFFEu) return FAT_E_NOSPC;

    uint32_t end = off + n;
    uint32_t need = (end + v->cluster_bytes - 1) / v->cluster_bytes;
    int r = file_extend_to(f, need);
    if (r) return r;

    r = file_rw(f, off, (uint8_t *)(uintptr_t)buf, n, 1);
    f->dirty = 1;
    if (r < 0) return r;
    if (end > f->size) f->size = end;
    return (int)n;
}

int fat_ftruncate(fat_file_t *f, uint32_t newsize) {
    fat_vol_t *v = f->v;
    if (!f->writable || v->read_only) return FAT_E_RO;
    if (newsize == f->size) return 0;
    if (newsize > f->size) return FAT_E_INVAL;

    int r;
    if (newsize == 0) {
        uint32_t first = f->first_cluster;
        f->first_cluster = 0;
        f->size = 0;
        f->cur_cl = 0; f->cur_idx = 0;
        f->dirty = 1;
        /* спочатку запис у каталозі (щоб не лишити "висячий" ланцюжок), потім звільняємо */
        r = ent_update(v, &f->ent, 0, 0, 1);
        if (r) return r;
        if (first) { r = free_chain(v, first); if (r) return r; }
        return 0;
    }

    uint32_t keep = (newsize + v->cluster_bytes - 1) / v->cluster_bytes;
    f->cur_cl = 0; f->cur_idx = 0;
    uint32_t cl;
    r = file_cluster_at(f, keep - 1, &cl);
    if (r) return r;
    if (!cl) return FAT_E_FMT;
    uint32_t nx;
    r = chain_next(v, cl, &nx);
    if (r) return r;
    if (nx) {
        r = fat_set(v, cl, EOC);
        if (r) return r;
        r = free_chain(v, nx);
        if (r) return r;
    }
    f->size = newsize;
    f->dirty = 1;
    f->cur_cl = 0; f->cur_idx = 0;
    return 0;
}

int fat_fclose(fat_file_t *f) {
    fat_vol_t *v = f->v;
    int r = 0;
    if (f->dirty) {
        r = ent_update(v, &f->ent, f->first_cluster, f->size, 1);
        f->dirty = 0;
        int fr = fat_flush(v);
        if (!r) r = fr;
    }
    return r;
}

int fat_open(fat_vol_t *v, const char *path, int create, fat_file_t *f) {
    int r = canon(path, pbuf[0]);
    if (r) return r;
    if (pbuf[0][1] == 0) return FAT_E_ISDIR;

    uint32_t pc;
    char leaf[FAT_NAME_MAX + 1];
    r = resolve_parent(v, pbuf[0], &pc, leaf);
    if (r) return r;

    m_set(f, 0, sizeof(*f));
    f->v = v;
    r = dir_lookup(v, pc, leaf, &f->ent);
    if (r == FAT_E_NOENT && create) {
        if (v->read_only) return FAT_E_RO;
        r = dir_add_entry(v, pc, leaf, FAT_ATTR_ARCHIVE, 0, 0, &f->ent);
        if (r) return r;
        r = fat_flush(v);
        if (r) return r;
    } else if (r) {
        return r;
    }
    if (f->ent.attr & FAT_ATTR_DIR) return FAT_E_ISDIR;

    f->size = f->ent.size;
    f->first_cluster = f->ent.first_cluster;
    f->writable = !v->read_only && !(f->ent.attr & FAT_ATTR_RO);
    return 0;
}

/* ================================================================== */
/*  Високорівневі операції                                             */
/* ================================================================== */
int fat_read_file(fat_vol_t *v, const char *path, void *buf, uint32_t cap, uint32_t *out_len) {
    fat_file_t f;
    int r = fat_open(v, path, 0, &f);
    if (r) return r;
    uint32_t n = f.size < cap ? f.size : cap;
    r = fat_fread(&f, 0, buf, n);
    if (r < 0) return r;
    if (out_len) *out_len = (uint32_t)r;
    return f.size > cap ? FAT_E_NOSPC : 0;
}

static int count_chain(fat_vol_t *v, uint32_t cl, uint32_t *count) {
    uint32_t n = 0;
    while (cl && n <= v->total_clusters) {
        uint32_t nx;
        int r = chain_next(v, cl, &nx);
        if (r) return r;
        n++; cl = nx;
    }
    *count = n;
    return 0;
}

static uint8_t fatscan_buf[32768];

static int count_free(fat_vol_t *v) {
    int r = fat_flush(v);
    if (r) return r;

    uint32_t per = sizeof(fatscan_buf) / v->bps;
    uint32_t free_n = 0, entries_per_sec = v->bps / 4;
    uint32_t last = max_cluster(v);

    for (uint32_t s = 0; s < v->fat_size; s += per) {
        uint32_t n = v->fat_size - s < per ? v->fat_size - s : per;
        r = dev_rw(v, v->fat_start + s, n, fatscan_buf, 0);
        if (r) return r;
        uint32_t base = s * entries_per_sec;
        if (base > last) break;
        for (uint32_t e = 0; e < n * entries_per_sec; e++) {
            uint32_t cl = base + e;
            if (cl < 2) continue;
            if (cl > last) break;
            if ((le32(fatscan_buf + e * 4) & 0x0FFFFFFFu) == 0) free_n++;
        }
    }
    v->free_count = free_n;
    v->fsinfo_dirty = 1;
    return 0;
}

int fat_statfs(fat_vol_t *v, uint32_t *total, uint32_t *freec, uint32_t *cbytes) {
    if (v->free_count == 0xFFFFFFFFu || v->free_count > v->total_clusters) {
        int r = count_free(v);
        if (r) return r;
    }
    if (total) *total = v->total_clusters;
    if (freec) *freec = v->free_count;
    if (cbytes) *cbytes = v->cluster_bytes;
    return 0;
}

int fat_write_file(fat_vol_t *v, const char *path, const void *buf, uint32_t n) {
    if (v->read_only) return FAT_E_RO;

    fat_dirent_t probe;
    int existed = (fat_stat(v, path, &probe) == 0);

    fat_file_t f;
    int r = fat_open(v, path, 1, &f);
    if (r) return r;
    if (!f.writable) return FAT_E_RO;

    if (n == 0) {
        r = fat_ftruncate(&f, 0);
        int cr = fat_fclose(&f);
        return r ? r : cr;
    }

    uint32_t cb = v->cluster_bytes;
    uint32_t needed = (n + cb - 1) / cb;
    uint32_t total, freec, cbytes;
    r = fat_statfs(v, &total, &freec, &cbytes);
    if (r) return r;

    uint32_t old_first = f.first_cluster, old_clusters = 0;
    if (old_first) {
        r = count_chain(v, old_first, &old_clusters);
        if (r) return r;
    }

    if (needed <= freec) {
        /* безпечний шлях: нові дані пишемо в НОВИЙ ланцюжок, старий звільняємо тільки після оновлення запису */
        fat_file_t g = f;
        g.first_cluster = 0; g.size = 0; g.cur_cl = 0; g.cur_idx = 0; g.dirty = 0;
        r = fat_fwrite(&g, 0, buf, n);
        if (r < 0) {
            if (g.first_cluster) free_chain(v, g.first_cluster);
            fat_flush(v);
            if (!existed) fat_unlink(v, path);
            return r;
        }
        f.first_cluster = g.first_cluster;
        f.size = n;
        f.cur_cl = 0; f.cur_idx = 0;
        f.dirty = 1;
        r = fat_fclose(&f);
        if (r) return r;
        if (old_first) {
            r = free_chain(v, old_first);
            int fr = fat_flush(v);
            if (!r) r = fr;
        }
        return r;
    }

    if (needed <= freec + old_clusters) {
        /* майже повний том: спершу звільняємо старий вміст */
        r = fat_ftruncate(&f, 0);
        if (r) return r;
        r = fat_fwrite(&f, 0, buf, n);
        int cr = fat_fclose(&f);
        if (r < 0) return r;
        return cr;
    }

    if (!existed) fat_unlink(v, path);
    return FAT_E_NOSPC;
}

int fat_append_file(fat_vol_t *v, const char *path, const void *buf, uint32_t n) {
    fat_file_t f;
    int r = fat_open(v, path, 1, &f);
    if (r) return r;
    if (!f.writable) return FAT_E_RO;
    r = n ? fat_fwrite(&f, f.size, buf, n) : 0;
    int cr = fat_fclose(&f);
    if (r < 0) return r;
    return cr;
}

int fat_create(fat_vol_t *v, const char *path) {
    if (v->read_only) return FAT_E_RO;
    int r = canon(path, pbuf[0]);
    if (r) return r;
    uint32_t pc;
    char leaf[FAT_NAME_MAX + 1];
    r = resolve_parent(v, pbuf[0], &pc, leaf);
    if (r) return r;
    fat_dirent_t e;
    r = dir_lookup(v, pc, leaf, &e);
    if (r == 0) return FAT_E_EXIST;
    if (r != FAT_E_NOENT) return r;
    r = dir_add_entry(v, pc, leaf, FAT_ATTR_ARCHIVE, 0, 0, 0);
    if (r) return r;
    return fat_flush(v);
}

int fat_mkdir(fat_vol_t *v, const char *path) {
    if (v->read_only) return FAT_E_RO;
    int r = canon(path, pbuf[0]);
    if (r) return r;
    uint32_t pc;
    char leaf[FAT_NAME_MAX + 1];
    r = resolve_parent(v, pbuf[0], &pc, leaf);
    if (r) return r;
    fat_dirent_t e;
    r = dir_lookup(v, pc, leaf, &e);
    if (r == 0) return FAT_E_EXIST;
    if (r != FAT_E_NOENT) return r;

    uint32_t cl;
    r = alloc_cluster(v, 0, &cl);
    if (r) return r;
    r = zero_cluster(v, cl);
    if (r) { free_chain(v, cl); return r; }

    uint16_t date, time;
    now_dt(&date, &time);
    uint8_t dot[32], dotdot[32];
    static const uint8_t n1[11] = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    static const uint8_t n2[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    fill_short_entry(dot, n1, FAT_ATTR_DIR, 0, cl, 0, date, time);
    fill_short_entry(dotdot, n2, FAT_ATTR_DIR, 0, pc == v->root_cluster ? 0 : pc, 0, date, time);

    dit_t it;
    dit_init(&it, v, cl);
    r = dit_store(&it, dot);
    if (!r) r = dit_next(&it);
    if (!r) r = dit_store(&it, dotdot);
    if (!r) r = dir_add_entry(v, pc, leaf, FAT_ATTR_DIR, cl, 0, 0);
    if (r) { free_chain(v, cl); fat_flush(v); return r; }
    return fat_flush(v);
}

int fat_unlink(fat_vol_t *v, const char *path) {
    if (v->read_only) return FAT_E_RO;
    fat_dirent_t e;
    int r = fat_stat(v, path, &e);
    if (r) return r;
    if (e.attr & FAT_ATTR_DIR) return FAT_E_ISDIR;
    if (e.attr & FAT_ATTR_RO) return FAT_E_RO;
    r = ent_delete(v, &e);
    if (r) return r;
    if (e.first_cluster) { r = free_chain(v, e.first_cluster); if (r) return r; }
    return fat_flush(v);
}

static int count_cb(const fat_dirent_t *e, void *arg) { (void)e; (*(int *)arg)++; return 1; }

int fat_rmdir(fat_vol_t *v, const char *path) {
    if (v->read_only) return FAT_E_RO;
    fat_dirent_t e;
    int r = fat_stat(v, path, &e);
    if (r) return r;
    if (!e.dir_cluster) return FAT_E_INVAL;                          /* корінь */
    if (!(e.attr & FAT_ATTR_DIR)) return FAT_E_NOTDIR;
    if (e.attr & FAT_ATTR_RO) return FAT_E_RO;

    int cnt = 0;
    r = dir_walk(v, dir_start(v, &e), count_cb, &cnt);
    if (r < 0) return r;
    if (cnt) return FAT_E_NOTEMPTY;

    r = ent_delete(v, &e);
    if (r) return r;
    if (e.first_cluster) { r = free_chain(v, e.first_cluster); if (r) return r; }
    return fat_flush(v);
}

static int is_prefix_path(const char *dir, const char *path) {
    /* path == dir або path починається з dir + '/' (без урахування регістру) */
    for (;;) {
        if (*dir == 0) return *path == 0 || *path == '/';
        uint32_t a = utf8_next(&dir), b = utf8_next(&path);
        if (fold(a) != fold(b)) return 0;
    }
}

int fat_rename(fat_vol_t *v, const char *from, const char *to) {
    if (v->read_only) return FAT_E_RO;
    int r = canon(from, pbuf[0]);
    if (r) return r;
    r = canon(to, pbuf[1]);
    if (r) return r;

    fat_dirent_t src;
    r = resolve(v, pbuf[0], &src);
    if (r) return r;
    if (!src.dir_cluster) return FAT_E_INVAL;                        /* корінь */
    if ((src.attr & FAT_ATTR_DIR) && is_prefix_path(pbuf[0], pbuf[1]) ) {
        /* не можна перемістити каталог сам у себе (або перейменувати в себе без зміни) */
        if (!name_eq(pbuf[0], pbuf[1])) return FAT_E_INVAL;
    }

    uint32_t pc;
    char leaf[FAT_NAME_MAX + 1];
    r = resolve_parent(v, pbuf[1], &pc, leaf);
    if (r) return r;

    fat_dirent_t dst;
    r = dir_lookup(v, pc, leaf, &dst);
    int same = 0;
    if (r == 0) {
        if (dst.dir_cluster == src.dir_cluster && dst.ent_idx == src.ent_idx) same = 1;
        else return FAT_E_EXIST;
    } else if (r != FAT_E_NOENT) {
        return r;
    }

    if (same) {
        /* зміна лише регістру: старий запис прибираємо першим, інакше короткі імена конфліктують */
        r = ent_delete(v, &src);
        if (r) return r;
        r = dir_add_entry(v, pc, leaf, src.attr, src.first_cluster, src.size, 0);
        if (r) return r;
        return fat_flush(v);
    }

    fat_dirent_t added;
    r = dir_add_entry(v, pc, leaf, src.attr, src.first_cluster, src.size, &added);
    if (r) return r;

    /* зберегти оригінальні дати */
    {
        dit_t it;
        dit_init(&it, v, added.dir_cluster);
        if (dit_seek(&it, added.ent_idx) == 0) {
            uint8_t raw[32];
            if (dit_load(&it, raw) == 1) {
                put16(raw + 14, src.ctime); put16(raw + 16, src.cdate); put16(raw + 18, src.adate);
                put16(raw + 22, src.wtime); put16(raw + 24, src.wdate);
                dit_store(&it, raw);
            }
        }
    }

    r = ent_delete(v, &src);
    if (r) return r;

    if ((src.attr & FAT_ATTR_DIR) && src.dir_cluster != pc && src.first_cluster) {
        dit_t it;
        dit_init(&it, v, src.first_cluster);
        if (dit_seek(&it, 1) == 0) {
            uint8_t raw[32];
            if (dit_load(&it, raw) == 1 && raw[0] == '.' && raw[1] == '.') {
                uint32_t np = (pc == v->root_cluster) ? 0 : pc;
                put16(raw + 20, np >> 16);
                put16(raw + 26, np & 0xFFFF);
                dit_store(&it, raw);
            }
        }
    }
    return fat_flush(v);
}

/* ================================================================== */
/*  Монтування: MBR / GPT / "суперфлопі"                               */
/* ================================================================== */
static uint8_t bs_buf[FAT_SECTOR_MAX];

static int bpb_valid(const uint8_t *b) {
    if (b[510] != 0x55 || b[511] != 0xAA) return 0;
    uint32_t bps = le16(b + 11), spc = b[13];
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return 0;
    if (!spc || spc > 128 || (spc & (spc - 1))) return 0;
    if (le16(b + 14) == 0) return 0;
    if (b[16] < 1 || b[16] > 4) return 0;
    if (le16(b + 17) != 0) return 0;                                 /* root entries: тільки FAT12/16 */
    if (le16(b + 19) != 0 || le16(b + 22) != 0) return 0;            /* total16 / fat16 */
    if (le32(b + 32) == 0 || le32(b + 36) == 0) return 0;
    return 1;
}

static int load_bpb(fat_vol_t *v, blockdev_t *bd, uint32_t part_lba) {
    if (bd->read(bd->ctx, part_lba, 1, bs_buf)) return FAT_E_IO;
    if (!bpb_valid(bs_buf)) return FAT_E_FMT;

    uint32_t bps = le16(bs_buf + 11);
    if (bps < bd->block_size || bps % bd->block_size) return FAT_E_FMT;

    v->bd = bd;
    v->part_lba = part_lba;
    v->bps = bps;
    v->scale = bps / bd->block_size;
    v->spc = bs_buf[13];
    v->cluster_bytes = v->spc * bps;
    v->reserved = le16(bs_buf + 14);
    v->nfats = bs_buf[16];
    v->total_sectors = le32(bs_buf + 32);
    v->fat_size = le32(bs_buf + 36);
    uint16_t flags = (uint16_t)le16(bs_buf + 40);
    v->root_cluster = le32(bs_buf + 44);
    v->fsinfo_sector = le16(bs_buf + 48);
    v->serial = le32(bs_buf + 67);
    m_cpy(v->label, bs_buf + 71, 11);
    v->label[11] = 0;
    for (int i = 10; i >= 0 && v->label[i] == ' '; i--) v->label[i] = 0;

    if (le16(bs_buf + 42) != 0) return FAT_E_FMT;                    /* версія FAT32 має бути 0.0 */
    if (v->fat_size > 0x1000000u) return FAT_E_FMT;
    if (v->reserved + v->nfats * v->fat_size >= v->total_sectors) return FAT_E_FMT;

    v->data_start = v->reserved + v->nfats * v->fat_size;
    v->total_clusters = (v->total_sectors - v->data_start) / v->spc;
    uint32_t fat_cap = v->fat_size * (bps / 4);
    if (v->total_clusters + 2 > fat_cap) v->total_clusters = fat_cap > 2 ? fat_cap - 2 : 0;
    if (v->total_clusters < 1 || v->total_clusters > 0x0FFFFFF0u) return FAT_E_FMT;
    if (!cl_valid(v, v->root_cluster)) return FAT_E_FMT;

    if (flags & 0x80) {                                              /* дзеркалення вимкнено: активна одна FAT */
        uint32_t act = flags & 0x0F;
        if (act >= v->nfats) return FAT_E_FMT;
        v->fat_start = v->reserved + act * v->fat_size;
        v->fat_extra_copies = 0;
    } else {
        v->fat_start = v->reserved;
        v->fat_extra_copies = v->nfats - 1;
    }

    v->free_count = 0xFFFFFFFFu;
    v->next_free = 0xFFFFFFFFu;
    if (v->fsinfo_sector && v->fsinfo_sector < v->reserved) {
        fat_cache_t *c = cache_get(v, v->fsinfo_sector, 1);
        if (c && le32(c->data) == 0x41615252u && le32(c->data + 484) == 0x61417272u &&
            le32(c->data + 508) == 0xAA550000u) {
            uint32_t fc = le32(c->data + 488), nf = le32(c->data + 492);
            if (fc <= v->total_clusters) v->free_count = fc;
            if (cl_valid(v, nf)) v->next_free = nf;
        }
        if (v->free_count == 0xFFFFFFFFu && !c) v->fsinfo_sector = 0;
    } else {
        v->fsinfo_sector = 0;
    }
    if (!cl_valid(v, v->next_free)) v->next_free = 2;
    return 0;
}

#define MAX_CAND 16
typedef struct { uint32_t lba; uint32_t sectors; int scheme; } cand_t;

static int gpt_candidates(blockdev_t *bd, cand_t *cand, int *nc) {
    static uint8_t gb[FAT_SECTOR_MAX];
    if (bd->read(bd->ctx, 1, 1, gb)) return FAT_E_IO;
    static const char sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
    for (int i = 0; i < 8; i++) if (gb[i] != (uint8_t)sig[i]) return FAT_E_FMT;

    uint32_t ent_lba = le32(gb + 72);
    uint32_t count = le32(gb + 80), esz = le32(gb + 84);
    if (le32(gb + 76) != 0 || esz < 128 || esz > 1024 || (esz & (esz - 1)) || count == 0 || count > 256)
        return FAT_E_FMT;

    uint32_t per_sec = bd->block_size / esz;
    if (!per_sec) return FAT_E_FMT;
    for (uint32_t i = 0; i < count && *nc < MAX_CAND; i++) {
        if ((i % per_sec) == 0) {
            if (bd->read(bd->ctx, ent_lba + i / per_sec, 1, gb)) return FAT_E_IO;
        }
        const uint8_t *e = gb + (i % per_sec) * esz;
        int empty = 1;
        for (int k = 0; k < 16; k++) if (e[k]) { empty = 0; break; }
        if (empty) continue;
        if (le32(e + 36) != 0 || le32(e + 44) != 0) continue;       /* вище 2^32 блоків — не підтримується */
        uint32_t first = le32(e + 32), last = le32(e + 40);
        if (last < first) continue;
        cand[*nc].lba = first;
        cand[*nc].sectors = last - first + 1;
        cand[*nc].scheme = 2;
        (*nc)++;
    }
    return 0;
}

int fat_mount(fat_vol_t *v, blockdev_t *bd, int part_index, fat_part_info_t *info) {
    m_set(v, 0, sizeof(*v));
    if (!bd || !bd->read || bd->block_size < 512 || bd->block_size > FAT_SECTOR_MAX) return FAT_E_INVAL;
    v->read_only = bd->read_only || !bd->write;

    if (bd->read(bd->ctx, 0, 1, bs_buf)) return FAT_E_IO;

    cand_t cand[MAX_CAND];
    int nc = 0;

    if (bpb_valid(bs_buf)) {
        cand[nc].lba = 0; cand[nc].sectors = bd->block_count; cand[nc].scheme = 0; nc++;
    } else if (bs_buf[510] == 0x55 && bs_buf[511] == 0xAA) {
        uint8_t mbr[64];
        m_cpy(mbr, bs_buf + 446, 64);
        int gpt = 0;
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = mbr + i * 16;
            uint8_t type = e[4];
            uint32_t start = le32(e + 8), size = le32(e + 12);
            if (type == 0xEE) { gpt = 1; continue; }
            if ((type == 0x0B || type == 0x0C || type == 0x1B || type == 0x1C) && start && size && nc < MAX_CAND) {
                cand[nc].lba = start; cand[nc].sectors = size; cand[nc].scheme = 1; nc++;
            }
        }
        if (gpt) {
            int r = gpt_candidates(bd, cand, &nc);
            if (r == FAT_E_IO) return r;
        }
    } else {
        return FAT_E_FMT;
    }

    int seen = 0;
    for (int i = 0; i < nc; i++) {
        if (part_index >= 0 && seen++ != part_index) continue;
        m_set(v, 0, sizeof(*v));
        v->read_only = bd->read_only || !bd->write;
        int r = load_bpb(v, bd, cand[i].lba);
        if (r == 0) {
            if (info) { info->scheme = cand[i].scheme; info->start_lba = cand[i].lba; info->sectors = cand[i].sectors; }
            return 0;
        }
        if (r == FAT_E_IO) return r;
        if (part_index >= 0) return r;
    }
    return FAT_E_FMT;
}
