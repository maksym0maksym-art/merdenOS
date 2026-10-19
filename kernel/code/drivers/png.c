//*PNG-декодер: inflate (RFC 1951/1950) + PNG-фільтри + усі колір-типи, потоково (рядок за рядком)
#include <stdint.h>
#include <stddef.h>

#ifdef PNG_HOST_TEST
#include "png_host.h"                 /* kmalloc/kfree -> malloc/free для тесту на хості */
#else
#include "../../headers/png.h"
#include "../../headers/memory.h"
#endif

#define WIN_SIZE   32768u
#define WIN_MASK   (WIN_SIZE - 1u)

//*стан декодера
typedef struct {
    const uint8_t *data;
    uint32_t       size;

    /* живлення потоку: IDAT-шматки читаються один за одним, межі шматків для inflate невидимі */
    const uint8_t *ip;
    uint32_t       ileft;
    uint32_t       next_chunk;

    /* заголовок */
    uint32_t w, h;
    uint8_t  depth, ctype;
    uint32_t bits_pp, rowbytes, bpp;

    /* палітра (вже з tRNS) і «прозорий колір» для gray/RGB */
    uint32_t pal[256];
    int      has_key;
    uint16_t key[3];

    /* читання бітів */
    uint32_t bitbuf;
    int      bitcnt;

    int      err;                     /* перший код помилки, 0 — немає */
    int      done;                    /* усі рядки віддано */

    /* вікно inflate: заодно буфер для складання рядків */
    uint32_t wpos, flushed;
    uint8_t  win[WIN_SIZE];

    /* таблиці Хаффмана: літерали/довжини та відстані */
    int16_t  lcount[16], lsym[288];
    int16_t  dcount[16], dsym[30];

    /* рядки */
    uint8_t  *cur, *prev;
    uint32_t *out;
    uint32_t  rowfill, y;

    png_row_fn cb;
    void      *user;
} dec_t;

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

const char *png_strerror(int err)
{
    switch (err) {
    case PNG_OK:             return "ok";
    case PNG_E_FORMAT:       return "not a valid PNG file";
    case PNG_E_UNSUPPORTED:  return "unsupported PNG (interlaced?)";
    case PNG_E_NOMEM:        return "out of memory";
    case PNG_E_CORRUPT:      return "PNG data is corrupt or truncated";
    case PNG_E_ABORT:        return "aborted";
    case PNG_E_TOOBIG:       return "image is too big";
    default:                 return "unknown PNG error";
    }
}

//*IHDR
int png_info(const uint8_t *data, uint32_t size, png_info_t *out)
{
    if (!data || size < 33)
        return PNG_E_FORMAT;
    for (int i = 0; i < 8; i++)
        if (data[i] != png_sig[i])
            return PNG_E_FORMAT;

    const uint8_t *c = data + 8;
    if (be32(c) != 13 || c[4] != 'I' || c[5] != 'H' || c[6] != 'D' || c[7] != 'R')
        return PNG_E_FORMAT;

    uint32_t w = be32(c + 8), h = be32(c + 12);
    uint8_t depth = c[16], ctype = c[17], comp = c[18], filt = c[19], inter = c[20];

    if (w == 0 || h == 0)
        return PNG_E_FORMAT;
    if (w > PNG_MAX_DIM || h > PNG_MAX_DIM)
        return PNG_E_TOOBIG;
    if (comp != 0 || filt != 0 || inter > 1)
        return PNG_E_FORMAT;

    int ok = 0;
    switch (ctype) {
    case 0: ok = (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16); break;
    case 3: ok = (depth == 1 || depth == 2 || depth == 4 || depth == 8); break;
    case 2: case 4: case 6: ok = (depth == 8 || depth == 16); break;
    }
    if (!ok)
        return PNG_E_FORMAT;

    if (out) {
        out->width = w; out->height = h;
        out->bit_depth = depth; out->color_type = ctype; out->interlaced = inter;
    }
    return PNG_OK;
}

void png_get_size(const uint8_t *data, uint32_t size, uint32_t *w, uint32_t *h)
{
    png_info_t i;
    if (png_info(data, size, &i) != PNG_OK) {
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    if (w) *w = i.width;
    if (h) *h = i.height;
}

//*вхідний потік: наступний байт IDAT-даних або -1
static int next_byte(dec_t *d)
{
    while (d->ileft == 0) {
        uint32_t pos = d->next_chunk;
        if (pos > d->size || d->size - pos < 12)
            return -1;

        const uint8_t *c = d->data + pos;
        uint32_t len = be32(c);
        if (c[4] != 'I' || c[5] != 'D' || c[6] != 'A' || c[7] != 'T')
            return -1;

        uint64_t nxt = (uint64_t)pos + 12u + len;
        d->next_chunk = nxt > d->size ? d->size : (uint32_t)nxt;

        uint32_t avail = d->size - pos - 8;
        d->ip    = c + 8;
        d->ileft = len > avail ? avail : len;      /* обрізаний останній шматок: беремо, що є */
    }
    d->ileft--;
    return *d->ip++;
}

static uint32_t getbits(dec_t *d, int need)
{
    uint32_t v = d->bitbuf;
    while (d->bitcnt < need) {
        int b = next_byte(d);
        if (b < 0) {
            if (!d->err) d->err = PNG_E_CORRUPT;
            return 0;
        }
        v |= (uint32_t)b << d->bitcnt;
        d->bitcnt += 8;
    }
    d->bitbuf = v >> need;
    d->bitcnt -= need;
    return v & ((1u << need) - 1u);
}

//*Хаффман (канонічне декодування по одному біту, як у zlib/puff)
static int hdecode(dec_t *d, const int16_t *count, const int16_t *sym)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)getbits(d, 1);
        if (d->err)
            return -1;
        int cnt = count[len];
        if (code - cnt < first)
            return sym[index + (code - first)];
        index += cnt;
        first += cnt;
        first <<= 1;
        code  <<= 1;
    }
    return -1;                                     /* код не з цієї таблиці */
}

static int construct(int16_t *count, int16_t *sym, const int16_t *length, int n)
{
    for (int len = 0; len <= 15; len++)
        count[len] = 0;
    for (int i = 0; i < n; i++)
        count[length[i]]++;
    if (count[0] == n)
        return 0;

    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= count[len];
        if (left < 0)
            return left;                           /* переповнений набір кодів */
    }

    int16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++)
        offs[len + 1] = (int16_t)(offs[len] + count[len]);
    for (int i = 0; i < n; i++)
        if (length[i])
            sym[offs[length[i]]++] = (int16_t)i;
    return left;                                   /* >0 — неповний набір */
}

//*рядки: фільтри + перетворення в 0xAARRGGBB
static inline int iabs(int v) { return v < 0 ? -v : v; }

static void convert_row(dec_t *d, const uint8_t *x)
{
    uint32_t w = d->w;
    uint32_t *o = d->out;

    switch (d->ctype) {
    case 0:                                        /* gray */
        if (d->depth == 16) {
            for (uint32_t i = 0; i < w; i++) {
                uint32_t v = x[i * 2], s16 = ((uint32_t)x[i * 2] << 8) | x[i * 2 + 1];
                uint32_t a = (d->has_key && s16 == d->key[0]) ? 0u : 255u;
                o[i] = (a << 24) | (v << 16) | (v << 8) | v;
            }
        } else if (d->depth == 8) {
            for (uint32_t i = 0; i < w; i++) {
                uint32_t v = x[i];
                uint32_t a = (d->has_key && v == d->key[0]) ? 0u : 255u;
                o[i] = (a << 24) | (v << 16) | (v << 8) | v;
            }
        } else {
            uint32_t depth = d->depth, mask = (1u << depth) - 1u;
            uint32_t mul = 255u / mask;
            for (uint32_t i = 0; i < w; i++) {
                uint32_t bit = i * depth;
                uint32_t s = (x[bit >> 3] >> (8 - depth - (bit & 7))) & mask;
                uint32_t v = s * mul;
                uint32_t a = (d->has_key && s == d->key[0]) ? 0u : 255u;
                o[i] = (a << 24) | (v << 16) | (v << 8) | v;
            }
        }
        break;

    case 2:                                        /* RGB */
        if (d->depth == 16) {
            for (uint32_t i = 0; i < w; i++) {
                const uint8_t *p = x + i * 6;
                uint32_t r = p[0], g = p[2], b = p[4];
                uint32_t a = 255u;
                if (d->has_key &&
                    (((uint32_t)p[0] << 8) | p[1]) == d->key[0] &&
                    (((uint32_t)p[2] << 8) | p[3]) == d->key[1] &&
                    (((uint32_t)p[4] << 8) | p[5]) == d->key[2])
                    a = 0;
                o[i] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        } else {
            for (uint32_t i = 0; i < w; i++) {
                const uint8_t *p = x + i * 3;
                uint32_t r = p[0], g = p[1], b = p[2];
                uint32_t a = (d->has_key && r == d->key[0] && g == d->key[1] && b == d->key[2]) ? 0u : 255u;
                o[i] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        break;

    case 3: {                                      /* palette */
        uint32_t depth = d->depth, mask = (1u << depth) - 1u;
        for (uint32_t i = 0; i < w; i++) {
            uint32_t bit = i * depth;
            uint32_t idx = (x[bit >> 3] >> (8 - depth - (bit & 7))) & mask;
            o[i] = d->pal[idx];
        }
        break;
    }

    case 4:                                        /* gray + alpha */
        if (d->depth == 16) {
            for (uint32_t i = 0; i < w; i++) {
                uint32_t v = x[i * 4], a = x[i * 4 + 2];
                o[i] = (a << 24) | (v << 16) | (v << 8) | v;
            }
        } else {
            for (uint32_t i = 0; i < w; i++) {
                uint32_t v = x[i * 2], a = x[i * 2 + 1];
                o[i] = (a << 24) | (v << 16) | (v << 8) | v;
            }
        }
        break;

    default:                                       /* 6: RGBA */
        if (d->depth == 16) {
            for (uint32_t i = 0; i < w; i++) {
                const uint8_t *p = x + i * 8;
                o[i] = ((uint32_t)p[6] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[2] << 8) | p[4];
            }
        } else {
            for (uint32_t i = 0; i < w; i++) {
                const uint8_t *p = x + i * 4;
                o[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            }
        }
        break;
    }
}

static void process_row(dec_t *d)
{
    uint8_t  f  = d->cur[0];
    uint8_t *x  = d->cur + 1;
    const uint8_t *up = d->prev;
    uint32_t n = d->rowbytes, bp = d->bpp;

    switch (f) {
    case 0:
        break;
    case 1:
        for (uint32_t i = bp; i < n; i++)
            x[i] = (uint8_t)(x[i] + x[i - bp]);
        break;
    case 2:
        for (uint32_t i = 0; i < n; i++)
            x[i] = (uint8_t)(x[i] + up[i]);
        break;
    case 3:
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = i >= bp ? x[i - bp] : 0u;
            x[i] = (uint8_t)(x[i] + ((a + up[i]) >> 1));
        }
        break;
    case 4:
        for (uint32_t i = 0; i < n; i++) {
            int a = i >= bp ? x[i - bp] : 0;
            int b = up[i];
            int c = i >= bp ? up[i - bp] : 0;
            int p = a + b - c;
            int pa = iabs(p - a), pb = iabs(p - b), pc = iabs(p - c);
            int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            x[i] = (uint8_t)(x[i] + pr);
        }
        break;
    default:
        d->err = PNG_E_CORRUPT;
        return;
    }

    convert_row(d, x);

    for (uint32_t i = 0; i < n; i++)
        d->prev[i] = x[i];

    if (d->cb(d->y, d->out, d->w, d->user))
        d->err = PNG_E_ABORT;
}

//*розпаковані байти → складання рядків
static void consume(dec_t *d, const uint8_t *p, uint32_t n)
{
    while (n && !d->done && !d->err) {
        uint32_t need = d->rowbytes + 1u - d->rowfill;
        uint32_t take = n < need ? n : need;

        for (uint32_t i = 0; i < take; i++)
            d->cur[d->rowfill + i] = p[i];
        d->rowfill += take;
        p += take;
        n -= take;

        if (d->rowfill == d->rowbytes + 1u) {
            process_row(d);
            d->rowfill = 0;
            if (++d->y == d->h)
                d->done = 1;
        }
    }
}

static void flush(dec_t *d)
{
    uint32_t n = d->wpos - d->flushed;
    if (n) {
        consume(d, d->win + (d->flushed & WIN_MASK), n);
        d->flushed = d->wpos;
    }
}

static inline void emit(dec_t *d, uint8_t b)
{
    d->win[d->wpos & WIN_MASK] = b;
    d->wpos++;
    if ((d->wpos & WIN_MASK) == 0)
        flush(d);
}

//*inflate
static const uint16_t lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  lext[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  dext[30]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void corrupt(dec_t *d)
{
    if (!d->err)
        d->err = PNG_E_CORRUPT;
}

static void codes(dec_t *d)
{
    for (;;) {
        if (d->err || d->done)
            return;

        int sym = hdecode(d, d->lcount, d->lsym);
        if (sym < 0) { corrupt(d); return; }

        if (sym < 256) {
            emit(d, (uint8_t)sym);
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) { corrupt(d); return; }
            uint32_t len = lbase[sym] + getbits(d, lext[sym]);

            int ds = hdecode(d, d->dcount, d->dsym);
            if (ds < 0 || ds >= 30) { corrupt(d); return; }
            uint32_t dist = dbase[ds] + getbits(d, dext[ds]);
            if (d->err) return;
            if (dist > d->wpos) { corrupt(d); return; }   /* посилання за початок даних */

            while (len--)
                emit(d, d->win[(d->wpos - dist) & WIN_MASK]);
        }
    }
}

static void build_fixed(dec_t *d)
{
    int16_t lengths[288];
    int i;
    for (i = 0;   i < 144; i++) lengths[i] = 8;
    for (;        i < 256; i++) lengths[i] = 9;
    for (;        i < 280; i++) lengths[i] = 7;
    for (;        i < 288; i++) lengths[i] = 8;
    construct(d->lcount, d->lsym, lengths, 288);
    for (i = 0; i < 30; i++) lengths[i] = 5;
    construct(d->dcount, d->dsym, lengths, 30);
}

static void build_dynamic(dec_t *d)
{
    static const uint8_t order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    int16_t lengths[320];
    int16_t cc[16], cs[19];

    for (int k = 0; k < 320; k++)
        lengths[k] = 0;

    int nlen  = (int)getbits(d, 5) + 257;
    int ndist = (int)getbits(d, 5) + 1;
    int ncode = (int)getbits(d, 4) + 4;
    if (d->err) return;
    if (nlen > 286 || ndist > 30) { corrupt(d); return; }

    int i;
    for (i = 0; i < ncode; i++) lengths[order[i]] = (int16_t)getbits(d, 3);
    for (; i < 19; i++)         lengths[order[i]] = 0;
    if (d->err) return;

    if (construct(cc, cs, lengths, 19) != 0) { corrupt(d); return; }

    int index = 0;
    while (index < nlen + ndist) {
        int sym = hdecode(d, cc, cs);
        if (sym < 0) { corrupt(d); return; }
        if (sym < 16) {
            lengths[index++] = (int16_t)sym;
        } else {
            int16_t len = 0;
            int rep;
            if (sym == 16) {
                if (index == 0) { corrupt(d); return; }
                len = lengths[index - 1];
                rep = 3 + (int)getbits(d, 2);
            } else if (sym == 17) {
                rep = 3 + (int)getbits(d, 3);
            } else {
                rep = 11 + (int)getbits(d, 7);
            }
            if (d->err || index + rep > nlen + ndist) { corrupt(d); return; }
            while (rep--)
                lengths[index++] = len;
        }
    }

    if (lengths[256] == 0) { corrupt(d); return; }     /* без коду кінця блоку */

    int err = construct(d->lcount, d->lsym, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - d->lcount[0] != 1)) { corrupt(d); return; }
    err = construct(d->dcount, d->dsym, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - d->dcount[0] != 1)) { corrupt(d); return; }
}

static void inflate_stream(dec_t *d)
{
    int last;
    do {
        last = (int)getbits(d, 1);
        int type = (int)getbits(d, 2);
        if (d->err) return;

        if (type == 0) {                               /* stored */
            d->bitbuf = 0;
            d->bitcnt = 0;
            int a = next_byte(d), b = next_byte(d), c = next_byte(d), e = next_byte(d);
            if (a < 0 || b < 0 || c < 0 || e < 0) { corrupt(d); return; }
            uint32_t len  = (uint32_t)a | ((uint32_t)b << 8);
            uint32_t nlen = (uint32_t)c | ((uint32_t)e << 8);
            if (len != (~nlen & 0xFFFFu)) { corrupt(d); return; }
            while (len-- && !d->done && !d->err) {
                int v = next_byte(d);
                if (v < 0) { corrupt(d); return; }
                emit(d, (uint8_t)v);
            }
        } else if (type == 1) {
            build_fixed(d);
            codes(d);
        } else if (type == 2) {
            build_dynamic(d);
            if (d->err) return;
            codes(d);
        } else {
            corrupt(d);
            return;
        }
    } while (!last && !d->err && !d->done);
}

//*головна функція
int png_decode_rows(const uint8_t *data, uint32_t size, png_row_fn cb, void *user)
{
    png_info_t inf;
    int rc = png_info(data, size, &inf);
    if (rc != PNG_OK)
        return rc;
    if (inf.interlaced)
        return PNG_E_UNSUPPORTED;
    if (!cb)
        return PNG_E_ABORT;

    dec_t *d = (dec_t *)kmalloc(sizeof(dec_t));
    if (!d)
        return PNG_E_NOMEM;

    static const uint8_t channels[7] = { 1, 0, 3, 1, 2, 0, 4 };
    d->data = data;      d->size = size;
    d->ip = NULL;        d->ileft = 0;       d->next_chunk = 0;
    d->w = inf.width;    d->h = inf.height;
    d->depth = inf.bit_depth;  d->ctype = inf.color_type;
    d->bits_pp  = (uint32_t)channels[d->ctype] * d->depth;
    d->rowbytes = (d->w * d->bits_pp + 7u) / 8u;
    d->bpp      = d->bits_pp >= 8 ? d->bits_pp / 8u : 1u;
    d->has_key = 0;
    d->key[0] = d->key[1] = d->key[2] = 0;
    d->bitbuf = 0;       d->bitcnt = 0;
    d->err = 0;          d->done = 0;
    d->wpos = 0;         d->flushed = 0;
    d->rowfill = 0;      d->y = 0;
    d->cb = cb;          d->user = user;
    for (int i = 0; i < 256; i++)
        d->pal[i] = 0xFF000000u;

    uint8_t *rows = (uint8_t *)kmalloc(d->w * 4u + d->rowbytes + 1u + d->rowbytes);
    if (!rows) {
        kfree(d);
        return PNG_E_NOMEM;
    }
    d->out  = (uint32_t *)rows;
    d->cur  = rows + d->w * 4u;
    d->prev = d->cur + d->rowbytes + 1u;
    for (uint32_t i = 0; i < d->rowbytes; i++)
        d->prev[i] = 0;

    //*чанки до першого IDAT: PLTE, tRNS
    uint32_t pos = 33;                              /* сигнатура 8 + IHDR 25 */
    int found = 0;
    while (pos <= size && size - pos >= 12) {
        const uint8_t *c = data + pos;
        uint32_t len = be32(c);
        uint32_t avail = size - pos - 12;           /* скільки реально є під дані цього чанка */

        if (c[4] == 'I' && c[5] == 'D' && c[6] == 'A' && c[7] == 'T') {
            d->next_chunk = pos;
            found = 1;
            break;
        }
        if (c[4] == 'I' && c[5] == 'E' && c[6] == 'N' && c[7] == 'D')
            break;
        if (len > avail)
            break;                                  /* обрізаний службовий чанк */

        const uint8_t *p = c + 8;
        if (c[4] == 'P' && c[5] == 'L' && c[6] == 'T' && c[7] == 'E' && d->ctype == 3) {
            uint32_t n = len / 3u;
            if (n > 256) n = 256;
            for (uint32_t i = 0; i < n; i++)
                d->pal[i] = 0xFF000000u | ((uint32_t)p[i * 3] << 16) | ((uint32_t)p[i * 3 + 1] << 8) | p[i * 3 + 2];
        } else if (c[4] == 't' && c[5] == 'R' && c[6] == 'N' && c[7] == 'S') {
            if (d->ctype == 3) {
                uint32_t n = len > 256 ? 256u : len;
                for (uint32_t i = 0; i < n; i++)
                    d->pal[i] = ((uint32_t)p[i] << 24) | (d->pal[i] & 0x00FFFFFFu);
            } else if (d->ctype == 0 && len >= 2) {
                d->has_key = 1;
                d->key[0] = (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
            } else if (d->ctype == 2 && len >= 6) {
                d->has_key = 1;
                for (int k = 0; k < 3; k++)
                    d->key[k] = (uint16_t)(((uint32_t)p[k * 2] << 8) | p[k * 2 + 1]);
            }
        }
        pos += 12u + len;
    }

    if (!found) {
        rc = PNG_E_CORRUPT;
    } else {
        //*zlib-заголовок: CMF/FLG
        int cmf = next_byte(d), flg = next_byte(d);
        if (cmf < 0 || flg < 0 || (cmf & 0x0F) != 8 || (cmf >> 4) > 7 ||
            (((cmf << 8) | flg) % 31) != 0 || (flg & 0x20)) {
            rc = PNG_E_CORRUPT;
        } else {
            inflate_stream(d);
            if (!d->err)
                flush(d);
            if (d->err)
                rc = d->err;
            else
                rc = d->done ? PNG_OK : PNG_E_CORRUPT;     /* потік скінчився раніше за рядки */
        }
    }

    kfree(rows);
    kfree(d);
    return rc;
}

//*завантаження в буфер
typedef struct {
    uint32_t *pixels;
} load_ctx_t;

static int load_row(uint32_t y, const uint32_t *row, uint32_t w, void *user)
{
    load_ctx_t *c = (load_ctx_t *)user;
    uint32_t *dst = c->pixels + (uint32_t)y * w;
    for (uint32_t i = 0; i < w; i++)
        dst[i] = row[i];
    return 0;
}

int png_load_into(const uint8_t *data, uint32_t size,
                  uint32_t *pixels, uint32_t cap_pixels, uint32_t *w, uint32_t *h)
{
    png_info_t inf;
    int rc = png_info(data, size, &inf);
    if (rc != PNG_OK)
        return rc;
    if (!pixels || (uint64_t)inf.width * inf.height > cap_pixels)
        return PNG_E_NOMEM;

    load_ctx_t ctx = { pixels };
    rc = png_decode_rows(data, size, load_row, &ctx);
    if (rc == PNG_OK) {
        if (w) *w = inf.width;
        if (h) *h = inf.height;
    }
    return rc;
}

uint32_t *png_load(const uint8_t *data, uint32_t size, uint32_t *w, uint32_t *h, int *err)
{
    png_info_t inf;
    int rc = png_info(data, size, &inf);
    if (rc != PNG_OK) {
        if (err) *err = rc;
        return NULL;
    }
    if (inf.interlaced) {
        if (err) *err = PNG_E_UNSUPPORTED;
        return NULL;
    }

    uint32_t *px = (uint32_t *)kmalloc((size_t)inf.width * inf.height * 4u);
    if (!px) {
        if (err) *err = PNG_E_NOMEM;
        return NULL;
    }
    rc = png_load_into(data, size, px, inf.width * inf.height, w, h);
    if (rc != PNG_OK) {
        kfree(px);
        if (err) *err = rc;
        return NULL;
    }
    if (err) *err = PNG_OK;
    return px;
}

void png_free(uint32_t *pixels)
{
    if (pixels)
        kfree(pixels);
}
