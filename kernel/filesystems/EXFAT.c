#include <stdint.h>
#include "../headers/EXFAT.h"
#ifdef FS_NAMES_CP1251
#include "../../standarts_lib/charset.h"   /* імена в API — CP1251, на диску — UTF-16 */
#endif

#define XF_EOC        0xFFFFFFFFu
#define XF_EOC_MIN    0xFFFFFFF8u
#define XF_BADCL      0xFFFFFFF7u
#define XF_MAX_DIR_ENTRIES 2097152u
#define XF_PATH_MAX   512
#define XF_NAME_MAX   255

static void m_cpy(void *d, const void *s, uint32_t n) {
    uint8_t *a = (uint8_t *)d; const uint8_t *b = (const uint8_t *)s;
    while (n--) *a++ = *b++;
}
static void m_set(void *d, uint8_t c, uint32_t n) { uint8_t *a = (uint8_t *)d; while (n--) *a++ = c; }

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (le16(p + 2) << 16); }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint32_t lo, uint32_t hi) { put32(p, lo); put32(p + 4, hi); }

static void (*clock_cb)(uint16_t *date, uint16_t *time) = 0;
void xf_set_clock(void (*get)(uint16_t *date, uint16_t *time)) { clock_cb = get; }

static uint32_t now_ts(void) {
    uint16_t d = (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1), t = 0;
    if (clock_cb) clock_cb(&d, &t);
    return ((uint32_t)d << 16) | t;
}

/* ---- UTF-8 <-> UTF-16 ---- */
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
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { *sp += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *sp += 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *sp += 4; return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    *sp += 1;
    return 0xFFFD;
}

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

/* ---- Up-case table ---- */
static uint16_t upcase[65536];

static void upcase_default(void) {
    for (uint32_t i = 0; i < 65536; i++) upcase[i] = (uint16_t)i;
    for (uint32_t c = 'a'; c <= 'z'; c++) upcase[c] = (uint16_t)(c - 32);
    for (uint32_t c = 0xE0; c <= 0xFE; c++) if (c != 0xF7) upcase[c] = (uint16_t)(c - 32);
    upcase[0xFF] = 0x178;
    for (uint32_t c = 0x3B1; c <= 0x3C9; c++) if (c != 0x3C2) upcase[c] = (uint16_t)(c - 32);
    upcase[0x3C2] = 0x3A3;
    for (uint32_t c = 0x430; c <= 0x44F; c++) upcase[c] = (uint16_t)(c - 32);
    for (uint32_t c = 0x450; c <= 0x45F; c++) upcase[c] = (uint16_t)(c - 80);
    for (uint32_t c = 0x490; c <= 0x4BF; c += 2) upcase[c + 1] = (uint16_t)c;
}

static uint32_t up(uint32_t c) { return c < 65536 ? upcase[c] : c; }

static int name16_eq(const uint16_t *a, int na, const uint16_t *b, int nb) {
    if (na != nb) return 0;
    for (int i = 0; i < na; i++) if (up(a[i]) != up(b[i])) return 0;
    return 1;
}

static uint32_t name_hash(const uint16_t *n, int len) {
    uint32_t h = 0;
    for (int i = 0; i < len; i++) {
        uint32_t c = up(n[i]);
        h = ((h << 15) | (h >> 1)) & 0xFFFFu; h = (h + (c & 0xFF)) & 0xFFFFu;
        h = ((h << 15) | (h >> 1)) & 0xFFFFu; h = (h + (c >> 8)) & 0xFFFFu;
    }
    return h;
}

static uint32_t set_checksum(const uint8_t *set, uint32_t nent) {
    uint32_t chk = 0, bytes = nent * 32;
    for (uint32_t i = 0; i < bytes; i++) {
        if (i == 2 || i == 3) continue;
        chk = (((chk << 15) | (chk >> 1)) & 0xFFFFu);
        chk = (chk + set[i]) & 0xFFFFu;
    }
    return chk;
}

static int name_valid(const uint16_t *n, int len) {
    if (len < 1 || len > XF_NAME_MAX) return 0;
    if (len == 1 && n[0] == '.') return 0;
    if (len == 2 && n[0] == '.' && n[1] == '.') return 0;
    for (int i = 0; i < len; i++) {
        uint16_t c = n[i];
        if (c < 0x20) return 0;
        if (c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|') return 0;
    }
    return 1;
}

/* ================================================================== */
/*  Пристрій і кеш секторів                                            */
/* ================================================================== */
static int dev_rw(xf_vol_t *v, uint32_t sec, uint32_t n, void *buf, int wr) {
    uint32_t lba = v->part_lba + sec * v->scale;
    uint32_t cnt = n * v->scale;
    if (wr) {
        if (v->read_only) return FAT_E_RO;
        return v->bd->write(v->bd->ctx, lba, cnt, buf) ? FAT_E_IO : 0;
    }
    return v->bd->read(v->bd->ctx, lba, cnt, buf) ? FAT_E_IO : 0;
}

static int cache_write_back(xf_vol_t *v, xf_cache_t *c) {
    int r = dev_rw(v, c->sector, 1, c->data, 1);
    if (r) return r;
    if (v->fat_extra_copies && c->sector >= v->fat_start && c->sector < v->fat_start + v->fat_size) {
        r = dev_rw(v, c->sector + v->fat_size, 1, c->data, 1);
        if (r) return r;
    }
    c->dirty = 0;
    return 0;
}

static xf_cache_t *cache_get(xf_vol_t *v, uint32_t sec, int load) {
    xf_cache_t *victim = 0;
    for (int i = 0; i < XF_CACHE_LINES; i++) {
        xf_cache_t *c = &v->cache[i];
        if (c->valid && c->sector == sec) { c->stamp = ++v->stamp; return c; }
    }
    for (int i = 0; i < XF_CACHE_LINES; i++) {
        xf_cache_t *c = &v->cache[i];
        if (!c->valid) { victim = c; break; }
        if (!victim || c->stamp < victim->stamp) victim = c;
    }
    if (victim->valid && victim->dirty) {
        if (cache_write_back(v, victim)) return 0;
    }
    victim->valid = 0; victim->dirty = 0;
    if (load) { if (dev_rw(v, sec, 1, victim->data, 0)) return 0; }
    else m_set(victim->data, 0, v->bps);
    victim->valid = 1; victim->sector = sec; victim->stamp = ++v->stamp;
    return victim;
}

static int cache_flush_range(xf_vol_t *v, uint32_t sec, uint32_t n) {
    for (int i = 0; i < XF_CACHE_LINES; i++) {
        xf_cache_t *c = &v->cache[i];
        if (c->valid && c->dirty && c->sector >= sec && c->sector - sec < n) {
            int r = cache_write_back(v, c);
            if (r) return r;
        }
    }
    return 0;
}

static void cache_invalidate_range(xf_vol_t *v, uint32_t sec, uint32_t n) {
    for (int i = 0; i < XF_CACHE_LINES; i++) {
        xf_cache_t *c = &v->cache[i];
        if (c->valid && c->sector >= sec && c->sector - sec < n) { c->valid = 0; c->dirty = 0; }
    }
}

static int data_io(xf_vol_t *v, uint32_t sec, uint32_t n, void *buf, int wr) {
    if (wr) { cache_invalidate_range(v, sec, n); return dev_rw(v, sec, n, buf, 1); }
    int r = cache_flush_range(v, sec, n);
    if (r) return r;
    return dev_rw(v, sec, n, buf, 0);
}

int xf_flush(xf_vol_t *v) {
    if (v->read_only) return 0;
    int r = 0;
    for (int i = 0; i < XF_CACHE_LINES; i++) {
        xf_cache_t *c = &v->cache[i];
        if (c->valid && c->dirty) {
            int rr = cache_write_back(v, c);
            if (rr && !r) r = rr;
        }
    }
    if (v->bd->flush && v->bd->flush(v->bd->ctx) && !r) r = FAT_E_IO;
    return r;
}

/* ================================================================== */
/*  FAT, бітова карта, кластери                                        */
/* ================================================================== */
static uint32_t max_cluster(const xf_vol_t *v) { return v->total_clusters + 1; }
static int cl_valid(const xf_vol_t *v, uint32_t cl) { return cl >= 2 && cl <= max_cluster(v); }
static uint32_t clus_sector(const xf_vol_t *v, uint32_t cl) { return v->heap_start + (cl - 2) * v->spc; }
static uint32_t ncl(const xf_vol_t *v, uint32_t len) { return len / v->cluster_bytes + ((len % v->cluster_bytes) ? 1u : 0u); }

static uint32_t memo_first, memo_idx, memo_cl;      /* курсор по ланцюжку */
static void memo_reset(void) { memo_first = 0; }

static int fat_get(xf_vol_t *v, uint32_t cl, uint32_t *val) {
    uint32_t off = cl * 4, sec = off / v->bps;
    if (sec >= v->fat_size) return FAT_E_FMT;
    xf_cache_t *c = cache_get(v, v->fat_start + sec, 1);
    if (!c) return FAT_E_IO;
    *val = le32(c->data + off % v->bps);
    return 0;
}

static int fat_set(xf_vol_t *v, uint32_t cl, uint32_t val) {
    if (v->read_only) return FAT_E_RO;
    uint32_t off = cl * 4, sec = off / v->bps;
    if (sec >= v->fat_size) return FAT_E_FMT;
    xf_cache_t *c = cache_get(v, v->fat_start + sec, 1);
    if (!c) return FAT_E_IO;
    put32(c->data + off % v->bps, val);
    c->dirty = 1;
    return 0;
}

static int chain_next(xf_vol_t *v, uint32_t cl, uint32_t *next) {
    uint32_t val;
    int r = fat_get(v, cl, &val);
    if (r) return r;
    if (val >= XF_EOC_MIN && val != XF_BADCL) { *next = 0; return 0; }
    if (!cl_valid(v, val)) return FAT_E_FMT;
    *next = val;
    return 0;
}

/* кластер з номером idx у потоці; 0 — потік коротший */
static int ref_cluster_at(xf_vol_t *v, const xf_ref_t *ref, uint32_t idx, uint32_t *cl) {
    if (!ref->first) { *cl = 0; return 0; }
    if (ref->nofat) {
        if (idx >= ncl(v, ref->len)) { *cl = 0; return 0; }
        *cl = ref->first + idx;
        return cl_valid(v, *cl) ? 0 : FAT_E_FMT;
    }
    uint32_t c = ref->first, i = 0;
    if (memo_first == ref->first && memo_idx <= idx) { c = memo_cl; i = memo_idx; }
    while (i < idx) {
        uint32_t n;
        int r = chain_next(v, c, &n);
        if (r) return r;
        if (!n) { *cl = 0; return 0; }
        c = n; i++;
    }
    memo_first = ref->first; memo_idx = i; memo_cl = c;
    *cl = c;
    return 0;
}

/* ---- бітова карта ---- */
static int bm_sector(xf_vol_t *v, uint32_t byte_off, xf_cache_t **c, uint32_t *in_sec) {
    if (byte_off >= v->bm_len) return FAT_E_FMT;
    xf_ref_t bm = { v->bm_first, v->bm_len, v->bm_nofat };
    uint32_t cl;
    int r = ref_cluster_at(v, &bm, byte_off / v->cluster_bytes, &cl);
    if (r) return r;
    if (!cl) return FAT_E_FMT;
    uint32_t in_cl = byte_off % v->cluster_bytes;
    *c = cache_get(v, clus_sector(v, cl) + in_cl / v->bps, 1);
    if (!*c) return FAT_E_IO;
    *in_sec = in_cl % v->bps;
    return 0;
}

static int bm_set(xf_vol_t *v, uint32_t cl, int val) {
    if (v->read_only) return FAT_E_RO;
    uint32_t i = cl - 2;
    xf_cache_t *c; uint32_t o;
    int r = bm_sector(v, i >> 3, &c, &o);
    if (r) return r;
    if (val) c->data[o] |= (uint8_t)(1u << (i & 7)); else c->data[o] &= (uint8_t)~(1u << (i & 7));
    c->dirty = 1;
    return 0;
}

static int alloc_cluster(xf_vol_t *v, uint32_t hint, uint32_t *out) {
    uint32_t total = v->total_clusters;
    uint32_t start = hint;
    if (!cl_valid(v, start)) start = v->next_free;
    if (!cl_valid(v, start)) start = 2;
    uint32_t i = start - 2, scanned = 0;
    while (scanned < total) {
        if (i >= total) i = 0;
        xf_cache_t *c; uint32_t o;
        int r = bm_sector(v, i >> 3, &c, &o);
        if (r) return r;
        /* пройти байти цього сектора */
        uint32_t room = v->bps - o;
        const uint8_t *p = c->data + o;
        uint32_t k = 0;
        while (k < room && scanned < total) {
            if (i >= total) { i = 0; break; }
            uint8_t b = p[k];
            if ((i & 7) == 0 && b == 0xFF && scanned + 8 <= total && i + 8 <= total) { i += 8; scanned += 8; k++; continue; }
            if (!((b >> (i & 7)) & 1)) {
                uint32_t cl = i + 2;
                r = bm_set(v, cl, 1);
                if (r) return r;
                r = fat_set(v, cl, XF_EOC);
                if (r) { bm_set(v, cl, 0); return r; }
                *out = cl;
                v->next_free = (cl + 1 <= max_cluster(v)) ? cl + 1 : 2;
                if (v->free_count) v->free_count--;
                memo_reset();
                return 0;
            }
            i++; scanned++;
            if ((i & 7) == 0) k++;
        }
    }
    return FAT_E_NOSPC;
}

static int free_cluster(xf_vol_t *v, uint32_t cl, int clear_fat) {
    int r = bm_set(v, cl, 0);
    if (r) return r;
    if (clear_fat) { r = fat_set(v, cl, 0); if (r) return r; }
    v->free_count++;
    if (cl < v->next_free || !cl_valid(v, v->next_free)) v->next_free = cl;
    return 0;
}

static int free_chain(xf_vol_t *v, uint32_t cl) {
    uint32_t guard = 0;
    memo_reset();
    while (cl_valid(v, cl) && guard++ <= v->total_clusters) {
        uint32_t val;
        int r = fat_get(v, cl, &val);
        if (r) return r;
        r = free_cluster(v, cl, 1);
        if (r) return r;
        if (val >= XF_EOC_MIN || val == 0) break;
        cl = val;
    }
    return 0;
}

static int free_run(xf_vol_t *v, uint32_t first, uint32_t count) {
    memo_reset();
    for (uint32_t i = 0; i < count; i++) {
        if (!cl_valid(v, first + i)) return FAT_E_FMT;
        int r = free_cluster(v, first + i, 1);
        if (r) return r;
    }
    return 0;
}

/* суміжний потік -> ланцюжок у FAT */
static int ref_make_chain(xf_vol_t *v, xf_ref_t *ref) {
    if (!ref->nofat) return 0;
    uint32_t n = ncl(v, ref->len);
    for (uint32_t i = 0; i < n; i++) {
        int r = fat_set(v, ref->first + i, i + 1 < n ? ref->first + i + 1 : XF_EOC);
        if (r) return r;
    }
    ref->nofat = 0;
    memo_reset();
    return 0;
}

static uint8_t zbuf[16384];

static int zero_cluster(xf_vol_t *v, uint32_t cl) {
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
/*  Читання/запис потоку (файли, каталоги, таблиці)                    */
/* ================================================================== */
static int partial_sector(xf_vol_t *v, uint32_t sec, uint32_t off, uint8_t *p, uint32_t n, int wr) {
    xf_cache_t *c = cache_get(v, sec, 1);
    if (!c) return FAT_E_IO;
    if (wr) { m_cpy(c->data + off, p, n); c->dirty = 1; }
    else m_cpy(p, c->data + off, n);
    return 0;
}

static int range_io(xf_vol_t *v, uint32_t cl, uint32_t in_cl, uint8_t *buf, uint32_t len, int wr) {
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

static int ref_rw(xf_vol_t *v, const xf_ref_t *ref, uint32_t off, uint8_t *buf, uint32_t n, int wr) {
    uint32_t done = 0;
    while (n) {
        uint32_t idx = off / v->cluster_bytes, in_cl = off % v->cluster_bytes, cl;
        int r = ref_cluster_at(v, ref, idx, &cl);
        if (r) return r;
        if (!cl) return FAT_E_FMT;

        uint32_t run_bytes = v->cluster_bytes - in_cl, run = 1, c = cl;
        while (run_bytes < n && run < MAX_RUN_CLUSTERS) {
            uint32_t nx;
            if (ref->nofat) nx = (idx + run < ncl(v, ref->len)) ? c + 1 : 0;
            else { r = chain_next(v, c, &nx); if (r) return r; }
            if (nx != c + 1) break;
            c = nx; run++; run_bytes += v->cluster_bytes;
        }
        uint32_t take = n < run_bytes ? n : run_bytes;
        r = range_io(v, cl, in_cl, buf, take, wr);
        if (r) return r;
        memo_first = ref->nofat ? 0 : ref->first; memo_idx = idx + run - 1; memo_cl = c;
        off += take; buf += take; n -= take; done += take;
    }
    return (int)done;
}

/* ================================================================== */
/*  Каталоги: записи по 32 байти                                       */
/* ================================================================== */
#define SET_MAX_ENT 19

typedef struct {
    xf_dir_t     dir;                 /* каталог, у якому лежить набір */
    uint32_t     idx, sc;             /* індекс 0x85-запису, кількість вторинних */
    uint8_t      flags;               /* General Secondary Flags потоку */
    uint32_t     first, dl, vdl;
    fat_dirent_t pub;
} xf_found_t;

static void root_dir(xf_vol_t *v, xf_dir_t *d) {
    m_set(d, 0, sizeof(*d));
    d->ref.first = v->root_cluster; d->ref.len = v->root_len; d->ref.nofat = 0;
    d->is_root = 1;
}

/* 0 — є; 1 — за межами каталогу */
static int dir_read_ent(xf_vol_t *v, const xf_ref_t *ref, uint32_t idx, uint8_t *out) {
    if (idx >= XF_MAX_DIR_ENTRIES) return 1;
    uint32_t off = idx * 32;
    if (off >= ref->len) return 1;
    uint32_t cl;
    int r = ref_cluster_at(v, ref, off / v->cluster_bytes, &cl);
    if (r) return r;
    if (!cl) return FAT_E_FMT;
    uint32_t in_cl = off % v->cluster_bytes;
    xf_cache_t *c = cache_get(v, clus_sector(v, cl) + in_cl / v->bps, 1);
    if (!c) return FAT_E_IO;
    m_cpy(out, c->data + in_cl % v->bps, 32);
    return 0;
}

static int dir_write_ent(xf_vol_t *v, const xf_ref_t *ref, uint32_t idx, const uint8_t *in) {
    if (v->read_only) return FAT_E_RO;
    uint32_t off = idx * 32;
    if (idx >= XF_MAX_DIR_ENTRIES || off >= ref->len) return FAT_E_FMT;
    uint32_t cl;
    int r = ref_cluster_at(v, ref, off / v->cluster_bytes, &cl);
    if (r) return r;
    if (!cl) return FAT_E_FMT;
    uint32_t in_cl = off % v->cluster_bytes;
    xf_cache_t *c = cache_get(v, clus_sector(v, cl) + in_cl / v->bps, 1);
    if (!c) return FAT_E_IO;
    m_cpy(c->data + in_cl % v->bps, in, 32);
    c->dirty = 1;
    return 0;
}

static int set_load(xf_vol_t *v, const xf_ref_t *ref, uint32_t idx, uint8_t *buf, uint32_t *nent) {
    int r = dir_read_ent(v, ref, idx, buf);
    if (r < 0) return r;
    if (r == 1 || buf[0] != 0x85) return FAT_E_FMT;
    uint32_t sc = buf[1];
    if (sc < 2 || sc > SET_MAX_ENT - 1) return FAT_E_FMT;
    for (uint32_t i = 1; i <= sc; i++) {
        r = dir_read_ent(v, ref, idx + i, buf + 32 * i);
        if (r < 0) return r;
        if (r == 1) return FAT_E_FMT;
    }
    *nent = sc + 1;
    return 0;
}

static int set_store(xf_vol_t *v, const xf_ref_t *ref, uint32_t idx, uint8_t *buf, uint32_t nent) {
    put16(buf + 2, set_checksum(buf, nent));
    for (uint32_t i = 0; i < nent; i++) {
        int r = dir_write_ent(v, ref, idx + i, buf + 32 * i);
        if (r) return r;
    }
    return 0;
}

static int parse_set(const uint8_t *s, uint32_t nent, uint16_t *name, int *nl) {
    if (s[32] != 0xC0) return 0;
    if (le16(s + 2) != set_checksum(s, nent)) return 0;
    uint32_t n = s[35];
    if (n < 1) return 0;
    uint32_t need = (n + 14) / 15;
    if (need > nent - 2) return 0;
    uint32_t k = 0;
    for (uint32_t i = 0; i < need; i++) {
        const uint8_t *e = s + 64 + 32 * i;
        if (e[0] != 0xC1) return 0;
        for (uint32_t j = 0; j < 15 && k < n; j++) name[k++] = (uint16_t)le16(e + 2 + 2 * j);
    }
    *nl = (int)n;
    return 1;
}

static void fill_found(xf_found_t *f, const xf_dir_t *dir, uint32_t idx, const uint8_t *s, uint32_t nent,
                       const uint16_t *name, int nl) {
    m_set(f, 0, sizeof(*f));
    f->dir = *dir;
    f->idx = idx; f->sc = nent - 1;
    f->flags = s[33];
    f->first = le32(s + 52);
    f->dl = le32(s + 56);
    if (le32(s + 60)) f->dl = 0xFFFFFFFFu;
    f->vdl = le32(s + 40);
    if (le32(s + 44)) f->vdl = 0xFFFFFFFFu;
    uint8_t attr = (uint8_t)(le16(s + 4) & 0x37);
    if (!(attr & FAT_ATTR_DIR) && f->vdl > f->dl) f->vdl = f->dl;
    f->pub.attr = attr;
    f->pub.size = (attr & FAT_ATTR_DIR) ? f->dl : f->vdl;
    f->pub.first_cluster = f->first;
    f->pub.ctime = (uint16_t)le16(s + 8);  f->pub.cdate = (uint16_t)le16(s + 10);
    f->pub.wtime = (uint16_t)le16(s + 12); f->pub.wdate = (uint16_t)le16(s + 14);
    f->pub.adate = (uint16_t)le16(s + 18);
    f->pub.dir_cluster = dir->ref.first;
    f->pub.ent_idx = idx; f->pub.lfn_idx = idx;
    utf16_to_utf8(name, nl, f->pub.name, sizeof(f->pub.name));
}

/* прохід по каталогу; cb != 0 — виклик для кожного запису, інакше пошук за ім'ям */
static int dir_walk(xf_vol_t *v, const xf_dir_t *dir, const uint16_t *want, int wl,
                    fat_iter_cb cb, void *arg, xf_found_t *out) {
    uint8_t s[SET_MAX_ENT * 32], e[32];
    uint16_t nm[256];
    uint32_t hash = want ? name_hash(want, wl) : 0;
    uint32_t idx = 0;
    static xf_found_t tmp;
    for (;;) {
        int r = dir_read_ent(v, &dir->ref, idx, e);
        if (r < 0) return r;
        if (r == 1 || e[0] == 0) return want ? FAT_E_NOENT : 0;
        if (e[0] != 0x85) { idx++; continue; }
        uint32_t nent;
        int nl;
        if (set_load(v, &dir->ref, idx, s, &nent) || !parse_set(s, nent, nm, &nl)) { idx++; continue; }
        if (want) {
            if (le16(s + 36) == hash && name16_eq(nm, nl, want, wl)) {
                fill_found(out, dir, idx, s, nent, nm, nl);
                return 0;
            }
        } else {
            fill_found(&tmp, dir, idx, s, nent, nm, nl);
            if (cb(&tmp.pub, arg)) return 0;
        }
        idx += nent;
    }
}

static int dir_find(xf_vol_t *v, const xf_dir_t *dir, const uint16_t *name, int nl, xf_found_t *out) {
    return dir_walk(v, dir, name, nl, 0, 0, out);
}

static void dir_from_found(const xf_found_t *f, xf_dir_t *d) {
    m_set(d, 0, sizeof(*d));
    d->ref.first = f->first; d->ref.len = f->dl; d->ref.nofat = (f->flags & 2) ? 1 : 0;
    d->pref = f->dir.ref; d->pidx = f->idx; d->proot = f->dir.is_root;
}

static int dir_is_empty(xf_vol_t *v, const xf_ref_t *ref) {
    uint8_t e[32];
    for (uint32_t idx = 0;; idx++) {
        int r = dir_read_ent(v, ref, idx, e);
        if (r < 0) return r;
        if (r == 1 || e[0] == 0) return 1;
        if (e[0] & 0x80) return 0;
    }
}

/* оновити довжину/кластер каталогу в батьківському наборі записів */
static int dir_update_own(xf_vol_t *v, const xf_dir_t *d) {
    uint8_t s[SET_MAX_ENT * 32];
    uint32_t nent;
    int r = set_load(v, &d->pref, d->pidx, s, &nent);
    if (r) return r;
    s[33] = (uint8_t)(0x01 | (d->ref.nofat ? 2 : 0));
    put64(s + 40, d->ref.len, 0);
    put32(s + 52, d->ref.first);
    put64(s + 56, d->ref.len, 0);
    return set_store(v, &d->pref, d->pidx, s, nent);
}

static int dir_extend(xf_vol_t *v, xf_dir_t *dir) {
    if (dir->ref.len / 32 >= XF_MAX_DIR_ENTRIES) return FAT_E_NOSPC;
    if (!dir->ref.first || !dir->ref.len) return FAT_E_FMT;
    int r;
    if (dir->ref.nofat) { r = ref_make_chain(v, &dir->ref); if (r) return r; }
    uint32_t last;
    r = ref_cluster_at(v, &dir->ref, ncl(v, dir->ref.len) - 1, &last);
    if (r) return r;
    if (!last) return FAT_E_FMT;
    uint32_t nc;
    r = alloc_cluster(v, last + 1, &nc);
    if (r) return r;
    r = zero_cluster(v, nc);
    if (!r) r = fat_set(v, last, nc);
    if (r) { free_cluster(v, nc, 1); return r; }
    dir->ref.len += v->cluster_bytes;
    if (dir->is_root) v->root_len = dir->ref.len;
    else r = dir_update_own(v, dir);
    return r;
}

/* знайти nent вільних записів підряд (розширюючи каталог) і записати туди набір */
static int dir_add_set(xf_vol_t *v, xf_dir_t *dir, uint8_t *set, uint32_t nent) {
    uint8_t e[32];
    uint32_t run = 0, start = 0, idx = 0;
    for (;;) {
        int r = dir_read_ent(v, &dir->ref, idx, e);
        if (r < 0) return r;
        if (r == 1) {
            r = dir_extend(v, dir);
            if (r) return r;
            continue;
        }
        if (!(e[0] & 0x80)) {
            if (!run) start = idx;
            if (++run >= nent) break;
        } else {
            run = 0;
        }
        idx++;
    }
    return set_store(v, &dir->ref, start, set, nent);
}

static int delete_set(xf_vol_t *v, const xf_dir_t *dir, uint32_t idx, uint32_t sc) {
    uint8_t e[32];
    for (uint32_t i = 0; i <= sc; i++) {
        int r = dir_read_ent(v, &dir->ref, idx + i, e);
        if (r < 0) return r;
        if (r == 1) return FAT_E_FMT;
        e[0] &= 0x7F;
        r = dir_write_ent(v, &dir->ref, idx + i, e);
        if (r) return r;
    }
    return 0;
}

static uint32_t build_set(uint8_t *s, const uint16_t *name, int nl, uint8_t attr, uint32_t first,
                          uint32_t dl, uint32_t vdl, uint8_t flags, uint32_t ts) {
    uint32_t nn = (uint32_t)(nl + 14) / 15, nent = 2 + nn;
    m_set(s, 0, nent * 32);
    s[0] = 0x85; s[1] = (uint8_t)(nent - 1);
    put16(s + 4, attr);
    put32(s + 8, ts); put32(s + 12, ts); put32(s + 16, ts);
    s[32] = 0xC0; s[33] = flags; s[35] = (uint8_t)nl;
    put16(s + 36, name_hash(name, nl));
    put64(s + 40, vdl, 0);
    put32(s + 52, first);
    put64(s + 56, dl, 0);
    for (uint32_t i = 0; i < nn; i++) {
        uint8_t *e = s + 64 + 32 * i;
        e[0] = 0xC1;
        for (uint32_t k = 0; k < 15; k++) {
            uint32_t j = i * 15 + k;
            if (j < (uint32_t)nl) put16(e + 2 + 2 * k, name[j]);
        }
    }
    return nent;
}

/* ================================================================== */
/*  Шляхи                                                              */
/* ================================================================== */
static char pbuf[3][XF_PATH_MAX];

static int canon(const char *path, char *out) { return fat_path_join("/", path, out, XF_PATH_MAX); }

static int resolve_parent(xf_vol_t *v, const char *cp, xf_dir_t *dir, uint16_t *leaf, int *ln,
                          uint32_t avoid, int *hit) {
    if (cp[0] != '/' || cp[1] == 0) return FAT_E_INVAL;
    if (hit) *hit = 0;
    const char *last = cp;
    for (const char *p = cp; *p; p++) if (*p == '/') last = p;

    root_dir(v, dir);
    const char *p = cp + 1;
    while (p < last) {
        char comp[FAT_NAME_MAX + 1];
        int n = 0;
        while (p < last && *p != '/') comp[n++] = *p++;
        comp[n] = 0;
        if (p < last) p++;
        uint16_t u[256];
        int un = utf8_to_utf16(comp, u, 256);
        if (un <= 0) return FAT_E_NOENT;
        static xf_found_t f;
        int r = dir_find(v, dir, u, un, &f);
        if (r) return r;
        if (!(f.pub.attr & FAT_ATTR_DIR)) return FAT_E_NOTDIR;
        if (!cl_valid(v, f.first)) return FAT_E_FMT;
        xf_dir_t nd;
        dir_from_found(&f, &nd);
        *dir = nd;
        if (hit && avoid && nd.ref.first == avoid) *hit = 1;
    }
    int n = utf8_to_utf16(last + 1, leaf, 256);
    if (n < 0) return FAT_E_INVAL;
    if (n == 0) return FAT_E_INVAL;
    *ln = n;
    return 0;
}

static void fill_root(xf_vol_t *v, xf_found_t *f) {
    m_set(f, 0, sizeof(*f));
    f->pub.attr = FAT_ATTR_DIR;
    f->pub.first_cluster = v->root_cluster;
    f->first = v->root_cluster; f->dl = v->root_len; f->vdl = v->root_len;
    root_dir(v, &f->dir);
}

/* canon-шлях -> запис; для кореня dir_cluster == 0 */
static int resolve(xf_vol_t *v, const char *cp, xf_found_t *out) {
    if (cp[0] == '/' && cp[1] == 0) { fill_root(v, out); return 0; }
    xf_dir_t dir;
    uint16_t leaf[256];
    int ln;
    int r = resolve_parent(v, cp, &dir, leaf, &ln, 0, 0);
    if (r) return r;
    return dir_find(v, &dir, leaf, ln, out);
}

int xf_stat(xf_vol_t *v, const char *path, fat_dirent_t *out) {
    int r = canon(path, pbuf[0]);
    if (r) return r;
    static xf_found_t f;
    r = resolve(v, pbuf[0], &f);
    if (r) return r;
    *out = f.pub;
    return 0;
}

int xf_readdir(xf_vol_t *v, const char *path, fat_iter_cb cb, void *arg) {
    int r = canon(path, pbuf[0]);
    if (r) return r;
    static xf_found_t f;
    r = resolve(v, pbuf[0], &f);
    if (r) return r;
    if (!(f.pub.attr & FAT_ATTR_DIR)) return FAT_E_NOTDIR;
    xf_dir_t d;
    if (pbuf[0][1] == 0) root_dir(v, &d); else dir_from_found(&f, &d);
    return dir_walk(v, &d, 0, 0, cb, arg, 0);
}

/* ================================================================== */
/*  Монтування                                                         */
/* ================================================================== */
static uint8_t bs_buf[FAT_SECTOR_MAX];
static uint8_t scan_buf[4096];

static int vbr_valid(const uint8_t *b) {
    static const char sig[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
    for (int i = 0; i < 8; i++) if (b[3 + i] != (uint8_t)sig[i]) return 0;
    for (int i = 11; i < 64; i++) if (b[i]) return 0;
    return b[510] == 0x55 && b[511] == 0xAA;
}

static int chain_len(xf_vol_t *v, uint32_t first, uint32_t *count) {
    uint32_t n = 0, cl = first;
    while (cl && n <= v->total_clusters) {
        uint32_t nx;
        int r = chain_next(v, cl, &nx);
        if (r) return r;
        n++; cl = nx;
    }
    if (n > v->total_clusters) return FAT_E_FMT;
    *count = n;
    return 0;
}

static void load_upcase(xf_vol_t *v, uint32_t first, uint32_t len) {
    upcase_default();
    len &= ~1u;
    if (!first || len < 2 || len > 131072u || !cl_valid(v, first)) return;
    if (first + ncl(v, len) - 1 > max_cluster(v)) return;
    xf_ref_t ref = { first, len, 1 };
    for (uint32_t i = 0; i < 65536; i++) upcase[i] = (uint16_t)i;
    uint32_t idx = 0, off = 0;
    int skip = 0;
    while (off < len) {
        uint32_t take = len - off < sizeof(scan_buf) ? len - off : (uint32_t)sizeof(scan_buf);
        if (ref_rw(v, &ref, off, scan_buf, take, 0) < 0) { upcase_default(); return; }
        for (uint32_t k = 0; k < take; k += 2) {
            uint32_t w = le16(scan_buf + k);
            if (skip) { idx += w; skip = 0; }
            else if (w == 0xFFFF) skip = 1;
            else { if (idx < 65536) upcase[idx] = (uint16_t)w; idx++; }
        }
        off += take;
    }
}

static int count_free(xf_vol_t *v) {
    xf_ref_t bm = { v->bm_first, v->bm_len, 1 };
    uint32_t bytes = (v->total_clusters + 7) / 8, off = 0, used = 0;
    while (off < bytes) {
        uint32_t take = bytes - off < sizeof(scan_buf) ? bytes - off : (uint32_t)sizeof(scan_buf);
        int r = ref_rw(v, &bm, off, scan_buf, take, 0);
        if (r < 0) return r;
        for (uint32_t k = 0; k < take; k++) {
            uint32_t b = scan_buf[k];
            if (off + k == bytes - 1 && (v->total_clusters & 7)) b &= (1u << (v->total_clusters & 7)) - 1;
            while (b) { used += b & 1; b >>= 1; }
        }
        off += take;
    }
    v->free_count = v->total_clusters - used;
    return 0;
}

static int load_vbr(xf_vol_t *v, blockdev_t *bd, uint32_t part_lba) {
    if (bd->read(bd->ctx, part_lba, 1, bs_buf)) return FAT_E_IO;
    if (!vbr_valid(bs_buf)) return FAT_E_FMT;
    uint32_t bshift = bs_buf[108], sshift = bs_buf[109];
    if (bshift < 9 || bshift > 12 || bshift + sshift > 25) return FAT_E_FMT;
    if (bs_buf[105] != 1) return FAT_E_FMT;
    uint32_t bps = 1u << bshift;
    if (bps < bd->block_size || bps % bd->block_size) return FAT_E_FMT;

    v->bd = bd;
    v->part_lba = part_lba;
    v->bps = bps;
    v->scale = bps / bd->block_size;
    v->spc = 1u << sshift;
    v->cluster_bytes = v->spc * bps;
    v->fat_start = le32(bs_buf + 80);
    v->fat_size = le32(bs_buf + 84);
    v->heap_start = le32(bs_buf + 88);
    v->total_clusters = le32(bs_buf + 92);
    v->root_cluster = le32(bs_buf + 96);
    v->serial = le32(bs_buf + 100);
    v->nfats = bs_buf[110];
    if (le32(bs_buf + 76)) return FAT_E_FMT;                 /* том > 2^32 секторів */
    v->total_sectors = le32(bs_buf + 72);

    if (v->nfats < 1 || v->nfats > 2) return FAT_E_FMT;
    if (v->fat_size == 0 || v->fat_size > 0x1000000u || v->fat_start < 1) return FAT_E_FMT;
    if (v->total_clusters < 1 || v->total_clusters > 0xFFFFFFF5u) return FAT_E_FMT;
    if (v->heap_start < v->fat_start + v->nfats * v->fat_size) return FAT_E_FMT;
    if (v->total_clusters > (0xFFFFFFFFu - v->heap_start) / v->spc) return FAT_E_FMT;
    if (v->heap_start + v->total_clusters * v->spc > v->total_sectors) return FAT_E_FMT;
    uint32_t per = bps / 4;
    if ((v->total_clusters + 2 + per - 1) / per > v->fat_size) return FAT_E_FMT;
    if (!cl_valid(v, v->root_cluster)) return FAT_E_FMT;
    v->fat_extra_copies = v->nfats - 1;
    v->next_free = 2;
    return 0;
}

static int finish_mount(xf_vol_t *v) {
    uint32_t n;
    int r = chain_len(v, v->root_cluster, &n);
    if (r) return r;
    v->root_len = n * v->cluster_bytes;

    uint32_t up_first = 0, up_len = 0;
    int have_bm = 0;
    uint8_t e[32];
    for (uint32_t idx = 0;; idx++) {
        r = dir_read_ent(v, &(xf_ref_t){ v->root_cluster, v->root_len, 0 }, idx, e);
        if (r < 0) return r;
        if (r == 1 || e[0] == 0) break;
        if (e[0] == 0x81 && !have_bm && !(e[1] & 1)) {
            v->bm_first = le32(e + 20); v->bm_len = le32(e + 24); have_bm = 1;
        } else if (e[0] == 0x82) {
            up_first = le32(e + 20); up_len = le32(e + 24);
        } else if (e[0] == 0x83) {
            uint32_t cc = e[1] > 11 ? 11 : e[1];
            uint16_t u[11];
            for (uint32_t i = 0; i < cc; i++) u[i] = (uint16_t)le16(e + 2 + 2 * i);
            utf16_to_utf8(u, (int)cc, v->label, sizeof(v->label));
        }
    }
    if (!have_bm) return FAT_E_FMT;
    v->bm_nofat = 1;
    if (v->bm_len < (v->total_clusters + 7) / 8) return FAT_E_FMT;
    if (!cl_valid(v, v->bm_first) || v->bm_first + ncl(v, v->bm_len) - 1 > max_cluster(v)) return FAT_E_FMT;

    load_upcase(v, up_first, up_len);
    return count_free(v);
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
    if (le32(gb + 76) != 0 || esz < 128 || esz > 1024 || (esz & (esz - 1)) || count == 0 || count > 256) return FAT_E_FMT;
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
        if (le32(e + 36) != 0 || le32(e + 44) != 0) continue;
        uint32_t first = le32(e + 32), last = le32(e + 40);
        if (last < first) continue;
        cand[*nc].lba = first; cand[*nc].sectors = last - first + 1; cand[*nc].scheme = 2;
        (*nc)++;
    }
    return 0;
}

int xf_mount(xf_vol_t *v, blockdev_t *bd, int part_index, fat_part_info_t *info) {
    m_set(v, 0, sizeof(*v));
    if (!bd || !bd->read || bd->block_size < 512 || bd->block_size > FAT_SECTOR_MAX) return FAT_E_INVAL;
    int ro = bd->read_only || !bd->write;

    if (bd->read(bd->ctx, 0, 1, bs_buf)) return FAT_E_IO;
    cand_t cand[MAX_CAND];
    int nc = 0;
    if (vbr_valid(bs_buf)) {
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
            if (type == 0 || type == 0x05 || type == 0x0F || type == 0x85) continue;
            if (start && size && nc < MAX_CAND) { cand[nc].lba = start; cand[nc].sectors = size; cand[nc].scheme = 1; nc++; }
        }
        if (gpt) {
            int r = gpt_candidates(bd, cand, &nc);
            if (r == FAT_E_IO) return r;
        }
    } else {
        return FAT_E_FMT;
    }

    int seen = 0, last_err = FAT_E_FMT;
    for (int i = 0; i < nc; i++) {
        /* виглядає як exFAT? */
        if (bd->read(bd->ctx, cand[i].lba, 1, bs_buf)) return FAT_E_IO;
        if (!vbr_valid(bs_buf)) continue;
        if (part_index >= 0 && seen++ != part_index) continue;
        m_set(v, 0, sizeof(*v));
        v->read_only = ro;
        int r = load_vbr(v, bd, cand[i].lba);
        if (!r) r = finish_mount(v);
        if (r == 0) {
            if (info) { info->scheme = cand[i].scheme; info->start_lba = cand[i].lba; info->sectors = cand[i].sectors; }
            return 0;
        }
        if (r == FAT_E_IO) return r;
        last_err = r;
        if (part_index >= 0) return r;
    }
    m_set(v, 0, sizeof(*v));
    return last_err;
}

int xf_statfs(xf_vol_t *v, uint32_t *total, uint32_t *freec, uint32_t *cbytes) {
    if (v->free_count > v->total_clusters) {
        int r = count_free(v);
        if (r) return r;
    }
    if (total) *total = v->total_clusters;
    if (freec) *freec = v->free_count;
    if (cbytes) *cbytes = v->cluster_bytes;
    return 0;
}

/* ================================================================== */
/*  Файли                                                              */
/* ================================================================== */
static xf_ref_t fref(const xf_file_t *f) {
    xf_ref_t r; r.first = f->first_cluster; r.len = f->dl; r.nofat = f->nofat;
    return r;
}

static int free_stream(xf_vol_t *v, uint32_t first, uint32_t dl, int nofat) {
    if (!first) return 0;
    return nofat ? free_run(v, first, ncl(v, dl)) : free_chain(v, first);
}

/* залишити keep кластерів, решту звільнити */
static int file_trim(xf_file_t *f, uint32_t keep) {
    xf_vol_t *v = f->v;
    if (!f->first_cluster) return 0;
    memo_reset();
    if (f->nofat) {
        uint32_t total = ncl(v, f->dl);
        if (keep >= total) return 0;
        int r = free_run(v, f->first_cluster + keep, total - keep);
        if (keep == 0) { f->first_cluster = 0; f->nofat = 0; }
        return r;
    }
    if (keep == 0) {
        int r = free_chain(v, f->first_cluster);
        f->first_cluster = 0;
        return r;
    }
    xf_ref_t ref = fref(f);
    uint32_t cl, nx;
    int r = ref_cluster_at(v, &ref, keep - 1, &cl);
    if (r) return r;
    if (!cl) return FAT_E_FMT;
    r = chain_next(v, cl, &nx);
    if (r) return r;
    if (nx) {
        r = fat_set(v, cl, XF_EOC);
        if (r) return r;
        r = free_chain(v, nx);
    }
    memo_reset();
    return r;
}

/* VDL < DL (попередньо виділений файл): привести довжину в порядок перед зміною */
static int file_normalize(xf_file_t *f) {
    if (f->dl == f->size) return 0;
    int r = file_trim(f, f->size ? ncl(f->v, f->size) : 0);
    if (r) return r;
    f->dl = f->size;
    f->dirty = 1;
    return 0;
}

static int file_alloc_to(xf_file_t *f, uint32_t need) {
    xf_vol_t *v = f->v;
    uint32_t have = f->first_cluster ? ncl(v, f->dl) : 0;
    if (need <= have) return 0;
    int r;
    if (f->nofat && f->first_cluster) {
        xf_ref_t rr = fref(f);
        r = ref_make_chain(v, &rr);
        if (r) return r;
        f->nofat = 0;
    }
    uint32_t last = 0;
    if (have) {
        xf_ref_t rr = fref(f);
        r = ref_cluster_at(v, &rr, have - 1, &last);
        if (r) return r;
        if (!last) return FAT_E_FMT;
    }
    for (uint32_t i = have; i < need; i++) {
        uint32_t nc;
        r = alloc_cluster(v, last ? last + 1 : 0, &nc);
        if (!r) {
            if (last) r = fat_set(v, last, nc);
            else f->first_cluster = nc;
            if (r) free_cluster(v, nc, 1);
        }
        if (r) {
            uint32_t keep = have;
            xf_file_t tmp = *f;
            tmp.dl = have * v->cluster_bytes;                 /* тільки для nofat, тут вже ланцюжок */
            if (!have) { if (tmp.first_cluster) { free_chain(v, tmp.first_cluster); f->first_cluster = 0; } }
            else file_trim(&tmp, keep);
            return r;
        }
        last = nc;
    }
    return 0;
}

int xf_fread(xf_file_t *f, uint32_t off, void *buf, uint32_t n) {
    if (off >= f->size || !n) return 0;
    if (n > f->size - off) n = f->size - off;
    xf_ref_t ref = fref(f);
    int r = ref_rw(f->v, &ref, off, (uint8_t *)buf, n, 0);
    return r < 0 ? r : (int)n;
}

int xf_fwrite(xf_file_t *f, uint32_t off, const void *buf, uint32_t n) {
    xf_vol_t *v = f->v;
    if (!f->writable || v->read_only) return FAT_E_RO;
    if (off > f->size) return FAT_E_INVAL;
    if (!n) return 0;
    if (n > 0xFFFFFFFEu - off) return FAT_E_NOSPC;
    int r = file_normalize(f);
    if (r) return r;
    uint32_t end = off + n;
    uint32_t old_cl = f->first_cluster ? ncl(v, f->dl) : 0;
    r = file_alloc_to(f, ncl(v, end));
    if (r) return r;
    xf_ref_t ref = fref(f);
    r = ref_rw(v, &ref, off, (uint8_t *)buf, n, 1);
    if (r < 0) {
        if (ncl(v, end) > old_cl) {
            uint32_t sdl = f->dl;
            f->dl = old_cl * v->cluster_bytes;
            file_trim(f, old_cl);
            f->dl = sdl;
        }
        return r;
    }
    if (end > f->size) { f->size = end; f->dl = end; }
    f->dirty = 1;
    return (int)n;
}

int xf_ftruncate(xf_file_t *f, uint32_t newsize) {
    xf_vol_t *v = f->v;
    if (!f->writable || v->read_only) return FAT_E_RO;
    if (newsize > f->size) return FAT_E_INVAL;
    if (newsize == f->size && f->dl == f->size) return 0;
    int r = file_normalize(f);
    if (r) return r;
    if (newsize == f->size) return 0;
    r = file_trim(f, newsize ? ncl(v, newsize) : 0);
    if (r) return r;
    f->size = newsize; f->dl = newsize;
    if (!newsize) { f->first_cluster = 0; f->nofat = 0; }
    f->dirty = 1;
    return 0;
}

int xf_fclose(xf_file_t *f) {
    xf_vol_t *v = f->v;
    int r = 0;
    if (f->dirty) {
        uint8_t s[SET_MAX_ENT * 32];
        uint32_t nent;
        r = set_load(v, &f->parent.ref, f->set_idx, s, &nent);
        if (!r) {
            s[33] = (uint8_t)(0x01 | ((f->nofat && f->first_cluster) ? 2 : 0));
            put64(s + 40, f->size, 0);
            put32(s + 52, f->first_cluster);
            put64(s + 56, f->dl, 0);
            put32(s + 12, now_ts());
            put16(s + 4, le16(s + 4) | FAT_ATTR_ARCHIVE);
            r = set_store(v, &f->parent.ref, f->set_idx, s, nent);
        }
        f->dirty = 0;
        int fr = xf_flush(v);
        if (!r) r = fr;
    }
    return r;
}

static int add_new(xf_vol_t *v, xf_dir_t *dir, const uint16_t *leaf, int ln, uint8_t attr,
                   uint32_t first, uint32_t dl, uint8_t flags) {
    uint8_t s[SET_MAX_ENT * 32];
    uint32_t nent = build_set(s, leaf, ln, attr, first, dl, dl, flags, now_ts());
    return dir_add_set(v, dir, s, nent);
}

int xf_open(xf_vol_t *v, const char *path, int create, xf_file_t *f) {
    int r = canon(path, pbuf[0]);
    if (r) return r;
    if (pbuf[0][1] == 0) return FAT_E_ISDIR;
    xf_dir_t dir;
    uint16_t leaf[256];
    int ln;
    r = resolve_parent(v, pbuf[0], &dir, leaf, &ln, 0, 0);
    if (r) return r;

    static xf_found_t fd;
    m_set(f, 0, sizeof(*f));
    f->v = v;
    r = dir_find(v, &dir, leaf, ln, &fd);
    if (r == FAT_E_NOENT && create) {
        if (v->read_only) return FAT_E_RO;
        if (!name_valid(leaf, ln)) return FAT_E_INVAL;
        r = add_new(v, &dir, leaf, ln, FAT_ATTR_ARCHIVE, 0, 0, 0x01);
        if (!r) r = xf_flush(v);
        if (r) return r;
        r = dir_find(v, &dir, leaf, ln, &fd);
    }
    if (r) return r;
    if (fd.pub.attr & FAT_ATTR_DIR) return FAT_E_ISDIR;

    f->ent = fd.pub;
    f->parent = fd.dir;
    f->set_idx = fd.idx; f->set_sc = fd.sc;
    f->flags = fd.flags;
    f->first_cluster = fd.first;
    f->dl = fd.dl; f->size = fd.vdl;
    f->nofat = (fd.flags & 2) ? 1 : 0;
    if (!f->first_cluster) { f->size = 0; f->dl = 0; f->nofat = 0; }
    else if (!cl_valid(v, f->first_cluster)) return FAT_E_FMT;
    f->writable = !v->read_only && !(fd.pub.attr & FAT_ATTR_RO);
    return 0;
}

/* ================================================================== */
/*  Високорівневі операції                                             */
/* ================================================================== */
int xf_read_file(xf_vol_t *v, const char *path, void *buf, uint32_t cap, uint32_t *out_len) {
    xf_file_t f;
    int r = xf_open(v, path, 0, &f);
    if (r) return r;
    uint32_t n = f.size < cap ? f.size : cap;
    r = xf_fread(&f, 0, buf, n);
    if (r < 0) return r;
    if (out_len) *out_len = (uint32_t)r;
    return f.size > cap ? FAT_E_NOSPC : 0;
}

int xf_write_file(xf_vol_t *v, const char *path, const void *buf, uint32_t n) {
    if (v->read_only) return FAT_E_RO;
    fat_dirent_t probe;
    int existed = (xf_stat(v, path, &probe) == 0);

    xf_file_t f;
    int r = xf_open(v, path, 1, &f);
    if (r) return r;
    if (!f.writable) return FAT_E_RO;

    if (n == 0) {
        r = xf_ftruncate(&f, 0);
        int cr = xf_fclose(&f);
        return r ? r : cr;
    }

    uint32_t needed = ncl(v, n);
    uint32_t total, freec, cbytes;
    r = xf_statfs(v, &total, &freec, &cbytes);
    if (r) return r;
    uint32_t old_first = f.first_cluster, old_dl = f.dl, old_nofat = f.nofat;
    uint32_t old_clusters = old_first ? ncl(v, old_dl) : 0;

    if (needed <= freec) {
        /* нові дані — у НОВИЙ ланцюжок; старий звільняємо після оновлення запису */
        xf_file_t g = f;
        g.first_cluster = 0; g.size = 0; g.dl = 0; g.nofat = 0; g.dirty = 0;
        r = xf_fwrite(&g, 0, buf, n);
        if (r < 0) {
            if (g.first_cluster) free_chain(v, g.first_cluster);
            xf_flush(v);
            if (!existed) xf_unlink(v, path);
            return r;
        }
        f.first_cluster = g.first_cluster; f.nofat = 0;
        f.size = n; f.dl = n; f.dirty = 1;
        r = xf_fclose(&f);
        if (r) return r;
        if (old_first) {
            r = free_stream(v, old_first, old_dl, (int)old_nofat);
            int fr = xf_flush(v);
            if (!r) r = fr;
        }
        return r;
    }
    if (needed <= freec + old_clusters) {
        r = xf_ftruncate(&f, 0);
        if (r) return r;
        r = xf_fwrite(&f, 0, buf, n);
        int cr = xf_fclose(&f);
        if (r < 0) return r;
        return cr;
    }
    if (!existed) xf_unlink(v, path);
    return FAT_E_NOSPC;
}

int xf_append_file(xf_vol_t *v, const char *path, const void *buf, uint32_t n) {
    xf_file_t f;
    int r = xf_open(v, path, 1, &f);
    if (r) return r;
    if (!f.writable) return FAT_E_RO;
    r = n ? xf_fwrite(&f, f.size, buf, n) : 0;
    int cr = xf_fclose(&f);
    if (r < 0) return r;
    return cr;
}

static int prep_new(xf_vol_t *v, const char *path, xf_dir_t *dir, uint16_t *leaf, int *ln) {
    if (v->read_only) return FAT_E_RO;
    int r = canon(path, pbuf[0]);
    if (r) return r;
    r = resolve_parent(v, pbuf[0], dir, leaf, ln, 0, 0);
    if (r) return r;
    static xf_found_t e;
    r = dir_find(v, dir, leaf, *ln, &e);
    if (r == 0) return FAT_E_EXIST;
    if (r != FAT_E_NOENT) return r;
    if (!name_valid(leaf, *ln)) return FAT_E_INVAL;
    return 0;
}

int xf_create(xf_vol_t *v, const char *path) {
    xf_dir_t dir; uint16_t leaf[256]; int ln;
    int r = prep_new(v, path, &dir, leaf, &ln);
    if (r) return r;
    r = add_new(v, &dir, leaf, ln, FAT_ATTR_ARCHIVE, 0, 0, 0x01);
    if (r) return r;
    return xf_flush(v);
}

int xf_mkdir(xf_vol_t *v, const char *path) {
    xf_dir_t dir; uint16_t leaf[256]; int ln;
    int r = prep_new(v, path, &dir, leaf, &ln);
    if (r) return r;
    uint32_t cl;
    r = alloc_cluster(v, 0, &cl);
    if (r) return r;
    r = zero_cluster(v, cl);
    if (!r) r = add_new(v, &dir, leaf, ln, FAT_ATTR_DIR, cl, v->cluster_bytes, 0x01);
    if (r) { free_chain(v, cl); xf_flush(v); return r; }
    return xf_flush(v);
}

int xf_unlink(xf_vol_t *v, const char *path) {
    if (v->read_only) return FAT_E_RO;
    int r = canon(path, pbuf[0]);
    if (r) return r;
    static xf_found_t e;
    r = resolve(v, pbuf[0], &e);
    if (r) return r;
    if (pbuf[0][1] == 0) return FAT_E_INVAL;
    if (e.pub.attr & FAT_ATTR_DIR) return FAT_E_ISDIR;
    if (e.pub.attr & FAT_ATTR_RO) return FAT_E_RO;
    r = delete_set(v, &e.dir, e.idx, e.sc);
    if (r) return r;
    r = free_stream(v, e.first, e.dl, (e.flags & 2) ? 1 : 0);
    if (r) return r;
    return xf_flush(v);
}

int xf_rmdir(xf_vol_t *v, const char *path) {
    if (v->read_only) return FAT_E_RO;
    int r = canon(path, pbuf[0]);
    if (r) return r;
    static xf_found_t e;
    r = resolve(v, pbuf[0], &e);
    if (r) return r;
    if (pbuf[0][1] == 0) return FAT_E_INVAL;
    if (!(e.pub.attr & FAT_ATTR_DIR)) return FAT_E_NOTDIR;
    if (e.pub.attr & FAT_ATTR_RO) return FAT_E_RO;
    xf_dir_t d;
    dir_from_found(&e, &d);
    r = dir_is_empty(v, &d.ref);
    if (r < 0) return r;
    if (!r) return FAT_E_NOTEMPTY;
    r = delete_set(v, &e.dir, e.idx, e.sc);
    if (r) return r;
    r = free_stream(v, e.first, e.dl, (e.flags & 2) ? 1 : 0);
    if (r) return r;
    return xf_flush(v);
}

/* path == dir або path починається з dir + '/' (без урахування регістру) */
static int is_prefix_path(const char *dir, const char *path) {
    for (;;) {
        if (*dir == 0) return *path == 0 || *path == '/';
        uint32_t a = utf8_next(&dir), b = utf8_next(&path);
        if (up(a) != up(b)) return 0;
    }
}

static int same_path(const char *a, const char *b) { return is_prefix_path(a, b) && is_prefix_path(b, a); }

int xf_rename(xf_vol_t *v, const char *from, const char *to) {
    if (v->read_only) return FAT_E_RO;
    int r = canon(from, pbuf[0]);
    if (r) return r;
    r = canon(to, pbuf[1]);
    if (r) return r;

    static xf_found_t src, dst;
    r = resolve(v, pbuf[0], &src);
    if (r) return r;
    if (pbuf[0][1] == 0) return FAT_E_INVAL;

    xf_dir_t dd;
    uint16_t leaf[256];
    int ln, hit = 0;
    int is_dir = (src.pub.attr & FAT_ATTR_DIR) != 0;
    if (is_dir && is_prefix_path(pbuf[0], pbuf[1]) && !same_path(pbuf[0], pbuf[1])) return FAT_E_INVAL;
    r = resolve_parent(v, pbuf[1], &dd, leaf, &ln, is_dir ? src.first : 0, &hit);
    if (r) return r;
    if (hit) return FAT_E_INVAL;                          /* каталог не можна перемістити в самого себе */
    if (!name_valid(leaf, ln)) return FAT_E_INVAL;

    r = dir_find(v, &dd, leaf, ln, &dst);
    if (r == 0) {
        if (!(dst.dir.ref.first == src.dir.ref.first && dst.idx == src.idx)) return FAT_E_EXIST;
    } else if (r != FAT_E_NOENT) {
        return r;
    }

    uint8_t old[SET_MAX_ENT * 32], nw[SET_MAX_ENT * 32];
    uint32_t onent;
    r = set_load(v, &src.dir.ref, src.idx, old, &onent);
    if (r) return r;
    uint32_t nnent = build_set(nw, leaf, ln, 0, 0, 0, 0, 0, 0);
    m_cpy(nw + 4, old + 4, 28);                           /* атрибути і мітки часу */
    nw[33] = old[33];
    m_cpy(nw + 40, old + 40, 24);                         /* VDL, перший кластер, довжина */
    r = dir_add_set(v, &dd, nw, nnent);
    if (r) return r;
    if (src.dir.ref.first == dd.ref.first) src.dir.ref = dd.ref;   /* каталог міг вирости */
    r = delete_set(v, &src.dir, src.idx, src.sc);
    if (r) return r;
    return xf_flush(v);
}
