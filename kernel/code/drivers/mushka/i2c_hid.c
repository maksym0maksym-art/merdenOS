#include "../../../headers/i2c_hid.h"
#include "../../../../standarts_lib/stdio.h"
#include "../../../headers/klog.h"
#undef t_printf
#define t_printf klog_rawf   /* лог іде в журнал ядра: видно в dmesg і потрапляє в savelog */

/* ------------------------------------------------------------------ */
/*  Розбір дескриптора звіту                                           */
/* ------------------------------------------------------------------ */
#define PAGE_GENERIC 0x01
#define PAGE_BUTTON  0x09
#define PAGE_DIGIT   0x0D

#define USE_X        0x30
#define USE_Y        0x31
#define USE_WHEEL    0x38
#define USE_FINGER   0x22
#define USE_TIP      0x42
#define USE_CONF     0x47
#define USE_CID      0x51
#define USE_MODE     0x52
#define USE_COUNT    0x54
#define USE_SURFACE  0x57
#define USE_BTNSW    0x58

static uint16_t in_bits[256];     /* накопичувачі зсувів за Report ID */
static uint16_t feat_bits[256];

static int32_t sext(uint32_t v, int size) {
    if (size >= 4) return (int32_t)v;
    if (size == 0) return 0;
    uint32_t m = 1u << (size * 8 - 1);
    return (v & m) ? (int32_t)(v | (~0u << (size * 8))) : (int32_t)v;
}

int hid_parse(const uint8_t *d, int len, hid_desc_t *o) {
    o->nf = 0;
    o->has_ids = 0;
    for (int i = 0; i < 256; i++) { in_bits[i] = 0; feat_bits[i] = 0; o->feature_bits[i] = 0; }

    uint16_t page = 0;
    int32_t  lmin = 0, lmax_s = 0;
    uint32_t lmax_u = 0;
    int      rsize = 0, rcount = 0, rid = 0;

    uint16_t usages[32]; int nu = 0;
    uint16_t umin = 0, umax = 0; int have_range = 0;

    uint8_t colgroup[16]; int depth = 0;
    int cur_group = 0, ngroup = 0;

    int i = 0;
    while (i < len) {
        uint8_t b = d[i++];
        if (b == 0xFE) {                                   /* довгий елемент: пропускаємо */
            if (i >= len) break;
            int sz = d[i];
            i += 2 + sz;
            continue;
        }
        int size = b & 3; if (size == 3) size = 4;
        int type = (b >> 2) & 3;
        int tag  = b >> 4;
        if (i + size > len) break;

        uint32_t v = 0;
        for (int k = 0; k < size; k++) v |= (uint32_t)d[i + k] << (8 * k);
        i += size;

        if (type == 1) {                                   /* global */
            switch (tag) {
            case 0: page = (uint16_t)v; break;
            case 1: lmin = sext(v, size); break;
            case 2: lmax_s = sext(v, size); lmax_u = v; break;
            case 7: rsize = (int)v; break;
            case 8: rid = (int)(v & 0xFF); o->has_ids = 1; break;
            case 9: rcount = (int)v; break;
            default: break;
            }
        } else if (type == 2) {                            /* local */
            switch (tag) {
            case 0: if (nu < 32) usages[nu++] = (uint16_t)v; break;
            case 1: umin = (uint16_t)v; have_range = 1; break;
            case 2: umax = (uint16_t)v; have_range = 1; break;
            default: break;
            }
        } else if (type == 0) {                            /* main */
            if (tag == 10) {                               /* Collection */
                uint16_t u = nu ? usages[0] : (have_range ? umin : 0);
                if (depth < 16) {
                    colgroup[depth] = (uint8_t)cur_group;
                    depth++;
                }
                if ((v & 0xFF) == 2 && page == PAGE_DIGIT && u == USE_FINGER) cur_group = ++ngroup;
            } else if (tag == 12) {                        /* End collection */
                if (depth > 0) { depth--; cur_group = colgroup[depth]; }
            } else if (tag == 8 || tag == 11) {            /* Input / Feature */
                int is_feat = (tag == 11);
                uint16_t *bits = is_feat ? feat_bits : in_bits;
                int constant = v & 1, variable = (v >> 1) & 1, relative = (v >> 2) & 1;

                /* Report Count — 32-бітне число з дескриптора; битий дескриптор не має зависити парсер */
                if (rsize > 0 && rcount > 0 && rsize <= 32 && rcount <= 256 &&
                    (int)bits[rid] + rsize * rcount <= 0xFFFF) {
                    for (int k = 0; k < rcount; k++) {
                        uint16_t fu = nu ? (k < nu ? usages[k] : usages[nu - 1])
                                         : (have_range ? (uint16_t)(umin + k > umax ? umax : umin + k) : 0);

                        /* Зберігаємо лише потрібне. Сертифікаційний блок Windows PTP (вендорна сторінка, Feature,
                         * 256 байтів) раніше займав усі слоти таблиці, і поля Input Mode, що йдуть після нього,
                         * губились: тачпад лишався в режимі миші, а два пальці були рухом курсора. */
                        int keep = !constant && variable && page < 0xFF00;
                        if (keep && is_feat)
                            keep = (page == PAGE_DIGIT && (fu == USE_MODE || fu == USE_SURFACE || fu == USE_BTNSW));

                        if (keep && o->nf < HID_MAX_FIELDS) {
                            hid_field_t *f = &o->f[o->nf++];
                            f->rid = (uint8_t)rid;
                            f->kind = is_feat ? 2 : 1;
                            f->group = (uint8_t)cur_group;
                            f->relative = (uint8_t)relative;
                            f->page = page;
                            f->usage = fu;
                            f->off = bits[rid];
                            f->size = (uint8_t)rsize;
                            f->lmin = lmin;
                            f->lmax = (lmin >= 0) ? (lmax_u > 0x7FFFFFFFu ? 0x7FFFFFFF : (int32_t)lmax_u) : lmax_s;
                        }
                        bits[rid] = (uint16_t)(bits[rid] + rsize);
                    }
                }
            }
            nu = 0; have_range = 0;                        /* локальні стани скидаються після кожного main */
        }
    }
    for (int k = 0; k < 256; k++) o->feature_bits[k] = feat_bits[k];
    return o->nf;
}

/* ------------------------------------------------------------------ */
/*  Декодування звітів                                                 */
/* ------------------------------------------------------------------ */
static uint32_t get_bits(const uint8_t *d, int len, int off, int size) {
    uint32_t v = 0;
    for (int b = 0; b < size; b++) {
        int bit = off + b, byte = bit >> 3;
        if (byte >= len) return v;
        if (d[byte] & (1u << (bit & 7))) v |= 1u << b;
    }
    return v;
}

static int32_t field_val(const hid_field_t *f, const uint8_t *d, int len) {
    uint32_t v = get_bits(d, len, f->off, f->size);
    if (f->lmin < 0 && f->size < 32 && (v >> (f->size - 1)) & 1) v |= ~0u << f->size;
    return (int32_t)v;
}

typedef struct { int present, tip, conf, id, has_x, x, has_y, y; } slot_t;

/* Кадр, розбитий на кілька звітів (hybrid-режим Precision Touchpad) */
static struct {
    int active;          /* чекаємо на продовження кадру */
    int expected;        /* Contact Count з першого звіту */
    int got;             /* скільки дотиків уже прийшло */
    int n;
    hid_contact_t c[HID_MAX_CONTACTS];
} pend;

static void pend_add(const hid_contact_t *c, int n) {
    for (int i = 0; i < n && pend.n < HID_MAX_CONTACTS; i++) pend.c[pend.n++] = c[i];
}

static void pend_emit(hid_frame_t *fr) {
    fr->n = pend.n;
    for (int i = 0; i < pend.n; i++) fr->c[i] = pend.c[i];
    pend.active = 0; pend.n = 0; pend.got = 0; pend.expected = 0;
}

int hid_decode_input(const hid_desc_t *desc, const uint8_t *rep, int len, hid_frame_t *fr) {
    int rid = 0;
    if (desc->has_ids) {
        if (len < 1) return 0;
        rid = rep[0];
        rep++; len--;
    }

    slot_t s[HID_MAX_CONTACTS + 1];
    for (int i = 0; i <= HID_MAX_CONTACTS; i++) { s[i].present = 0; s[i].tip = 0; s[i].conf = 1; s[i].id = i; s[i].has_x = s[i].has_y = 0; s[i].x = s[i].y = 0; }

    int matched = 0, touch = 0, mouse = 0;
    int button = 0, dx = 0, dy = 0, wheel = 0;
    int cnt = -1;                                        /* Contact Count; -1 — поля нема */

    for (int i = 0; i < desc->nf; i++) {
        const hid_field_t *f = &desc->f[i];
        if (f->kind != 1 || f->rid != rid) continue;
        matched = 1;
        int32_t v = field_val(f, rep, len);

        if (f->group > 0 && f->group <= HID_MAX_CONTACTS) {
            slot_t *sl = &s[f->group];
            sl->present = 1;
            if (f->page == PAGE_DIGIT && f->usage == USE_TIP)  { sl->tip = v != 0; touch = 1; }
            else if (f->page == PAGE_DIGIT && f->usage == USE_CONF) sl->conf = v != 0;
            else if (f->page == PAGE_DIGIT && f->usage == USE_CID)  sl->id = v;
            else if (f->page == PAGE_GENERIC && f->usage == USE_X)  { sl->x = v; sl->has_x = 1; }
            else if (f->page == PAGE_GENERIC && f->usage == USE_Y)  { sl->y = v; sl->has_y = 1; }
        } else if (f->page == PAGE_DIGIT && f->usage == USE_COUNT) {
            cnt = v;
        } else if (f->page == PAGE_BUTTON && f->usage >= 1 && f->usage <= 3) {
            if (v) button |= 1 << (f->usage - 1);
        } else if (f->page == PAGE_GENERIC && f->relative) {
            if (f->usage == USE_X) { dx = v; mouse = 1; }
            else if (f->usage == USE_Y) { dy = v; mouse = 1; }
            else if (f->usage == USE_WHEEL) { wheel = v; mouse = 1; }
        }
    }
    if (!matched) return 0;

    fr->n = 0;
    fr->button = button;
    fr->dx = fr->dy = fr->wheel = 0;

    if (touch) {
        fr->kind = HID_KIND_TOUCHPAD;

        hid_contact_t cur[HID_MAX_CONTACTS];
        int m = 0, tips = 0;
        for (int g = 1; g <= HID_MAX_CONTACTS; g++) {
            if (!s[g].present || !s[g].tip) continue;
            tips++;                                       /* палець є, навіть якщо це долоня (conf = 0) */
            if (!s[g].conf || !s[g].has_x || !s[g].has_y) continue;
            cur[m].id = s[g].id;
            cur[m].x = s[g].x;
            cur[m].y = s[g].y;
            m++;
        }

        if (cnt < 0) {                                    /* поля Contact Count нема: кадр цілий */
            for (int i = 0; i < m; i++) fr->c[i] = cur[i];
            fr->n = m;
            return 1;
        }

        if (cnt > 0) {                                    /* початок нового кадру */
            pend.n = 0;
            pend.active = 0;
            pend.expected = cnt;
            pend.got = tips;
            pend_add(cur, m);
            if (tips > 0 && pend.got < pend.expected) {   /* решта пальців у наступних звітах */
                pend.active = 1;
                return 0;
            }
            pend_emit(fr);
            return 1;
        }

        /* cnt == 0 */
        if (pend.active) {                                /* продовження кадру */
            pend.got += tips;
            pend_add(cur, m);
            if (tips == 0 || pend.got >= pend.expected) { pend_emit(fr); return 1; }
            return 0;
        }
        for (int i = 0; i < m; i++) fr->c[i] = cur[i];    /* усі пальці підняті */
        fr->n = m;
        return 1;
    }
    if (mouse) {
        fr->kind = HID_KIND_MOUSE;
        fr->dx = dx; fr->dy = dy; fr->wheel = wheel;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Протокол HID over I2C                                              */
/* ------------------------------------------------------------------ */
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static const char *hex(uint32_t v) {
    static char b[4][12];
    static int k;
    char *o = b[k++ & 3];
    const char *dg = "0123456789ABCDEF";
    int n = 0;
    o[n++] = '0'; o[n++] = 'x';
    int started = 0;
    for (int sh = 28; sh >= 0; sh -= 4) {
        int dgt = (v >> sh) & 0xF;
        if (dgt || started || sh == 0) { o[n++] = dg[dgt]; started = 1; }
    }
    o[n] = 0;
    return o;
}

static int hid_cmd(i2c_hid_t *h, uint8_t byte0, uint8_t opcode) {
    uint8_t b[4] = { (uint8_t)(h->reg_cmd & 0xFF), (uint8_t)(h->reg_cmd >> 8), byte0, opcode };
    return dw_i2c_xfer(h->c, h->addr, b, 4, 0, 0, 50);
}

static int read_reg(i2c_hid_t *h, uint16_t reg, uint8_t *buf, int n) {
    uint8_t r[2] = { (uint8_t)(reg & 0xFF), (uint8_t)(reg >> 8) };
    return dw_i2c_xfer(h->c, h->addr, r, 2, buf, n, 300);
}

static void set_bits(uint8_t *d, int off, int size, uint32_t v) {
    for (int b = 0; b < size; b++) {
        int bit = off + b;
        if ((v >> b) & 1) d[bit >> 3] |= (uint8_t)(1u << (bit & 7));
        else              d[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
    }
}

/* Надсилає Feature-звіт rid: усі поля Input Mode / Surface Switch / Button Switch в ньому
 * виставляються так, як це робить Linux (hid-multitouch). Повертає 1, якщо пристрій прийняв. */
static int send_mode_report(i2c_hid_t *h, int rid) {
    const hid_desc_t *d = &h->desc;
    int nbytes = (d->feature_bits[rid] + 7) / 8;
    if (nbytes < 1 || nbytes > 32) return 0;

    uint8_t payload[32];
    for (int i = 0; i < nbytes; i++) payload[i] = 0;
    for (int i = 0; i < d->nf; i++) {
        const hid_field_t *f = &d->f[i];
        if (f->kind != 2 || f->rid != rid || f->page != PAGE_DIGIT) continue;
        if (f->usage == USE_MODE)    set_bits(payload, f->off, f->size, 3);   /* Precision Touchpad */
        if (f->usage == USE_SURFACE) set_bits(payload, f->off, f->size, 1);
        if (f->usage == USE_BTNSW)   set_bits(payload, f->off, f->size, 1);
    }

    uint8_t cmd[48];
    int n = 0;
    cmd[n++] = (uint8_t)(h->reg_cmd & 0xFF);
    cmd[n++] = (uint8_t)(h->reg_cmd >> 8);
    cmd[n++] = (uint8_t)((3 << 4) | (rid >= 15 ? 15 : rid));              /* тип Feature + ID */
    cmd[n++] = 0x03;                                                      /* SET_REPORT */
    if (rid >= 15) cmd[n++] = (uint8_t)rid;
    cmd[n++] = (uint8_t)(h->reg_data & 0xFF);
    cmd[n++] = (uint8_t)(h->reg_data >> 8);
    int total = 2 + (rid ? 1 : 0) + nbytes;
    cmd[n++] = (uint8_t)(total & 0xFF);
    cmd[n++] = (uint8_t)(total >> 8);
    if (rid) cmd[n++] = (uint8_t)rid;
    for (int i = 0; i < nbytes; i++) cmd[n++] = payload[i];

    return dw_i2c_xfer(h->c, h->addr, cmd, n, 0, 0, 100) == DW_OK;
}

/* Читає Feature-звіт rid назад (GET_REPORT) і повертає значення Input Mode; -1 — не вдалось. */
static int read_input_mode(i2c_hid_t *h, int rid) {
    const hid_desc_t *d = &h->desc;
    int nbytes = (d->feature_bits[rid] + 7) / 8;
    if (nbytes < 1 || nbytes > 32) return -1;

    uint8_t cmd[8]; int n = 0;
    cmd[n++] = (uint8_t)(h->reg_cmd & 0xFF);
    cmd[n++] = (uint8_t)(h->reg_cmd >> 8);
    cmd[n++] = (uint8_t)((3 << 4) | (rid >= 15 ? 15 : rid));
    cmd[n++] = 0x02;                                                      /* GET_REPORT */
    if (rid >= 15) cmd[n++] = (uint8_t)rid;
    cmd[n++] = (uint8_t)(h->reg_data & 0xFF);
    cmd[n++] = (uint8_t)(h->reg_data >> 8);

    uint8_t rb[2 + 1 + 32];
    int want = 2 + (rid ? 1 : 0) + nbytes;
    if (dw_i2c_xfer(h->c, h->addr, cmd, n, rb, want, 100) != DW_OK) return -1;

    const uint8_t *data = rb + 2;
    int len = want - 2;
    if (rid && len > 0 && data[0] == (uint8_t)rid) { data++; len--; }   /* пристрій повторює Report ID */
    for (int i = 0; i < d->nf; i++) {
        const hid_field_t *f = &d->f[i];
        if (f->kind != 2 || f->rid != rid || f->page != PAGE_DIGIT || f->usage != USE_MODE) continue;
        if ((f->off + f->size + 7) / 8 > len) return -1;
        return (int)get_bits(data, len, f->off, f->size);
    }
    return -1;
}

void i2c_hid_set_mode(i2c_hid_t *h) {
    const hid_desc_t *d = &h->desc;
    int rids[3], nr = 0;
    for (int i = 0; i < d->nf; i++) {
        const hid_field_t *f = &d->f[i];
        if (f->kind != 2 || f->page != PAGE_DIGIT) continue;
        if (f->usage != USE_MODE && f->usage != USE_SURFACE && f->usage != USE_BTNSW) continue;
        int dup = 0;
        for (int k = 0; k < nr; k++) if (rids[k] == f->rid) dup = 1;
        if (!dup && nr < 3) rids[nr++] = f->rid;
    }
    h->mode_sent = nr; h->mode_ok = 0; h->mode_verified = 0;
    int readback = -2;
    for (int k = 0; k < nr; k++) {
        h->mode_ok += send_mode_report(h, rids[k]);
        i2c_delay_ms(5);
        int m = read_input_mode(h, rids[k]);
        if (m != -1) readback = m;
    }
    h->mode_verified = (readback == 3);
    t_printf("[i2c] touchpad mode (PTP): %d of %d reports accepted, readback Input Mode = %d%s\n",
             h->mode_ok, h->mode_sent, readback, h->mode_verified ? " (OK)" : readback == -1 ? " (cannot read)" : "");
}

static void classify(i2c_hid_t *h) {
    const hid_desc_t *d = &h->desc;
    h->kind = HID_KIND_NONE;
    int tip = 0, xi = -1, yi = -1;
    for (int i = 0; i < d->nf; i++) {
        const hid_field_t *f = &d->f[i];
        if (f->kind != 1) continue;
        if (f->group > 0 && f->page == PAGE_DIGIT && f->usage == USE_TIP) tip = 1;
        if (f->group > 0 && f->page == PAGE_GENERIC && f->usage == USE_X && xi < 0) xi = i;
        if (f->group > 0 && f->page == PAGE_GENERIC && f->usage == USE_Y && yi < 0) yi = i;
    }
    if (tip && xi >= 0 && yi >= 0) {
        h->kind = HID_KIND_TOUCHPAD;
        h->xmin = d->f[xi].lmin; h->xmax = d->f[xi].lmax;
        h->ymin = d->f[yi].lmin; h->ymax = d->f[yi].lmax;
        return;
    }
    for (int i = 0; i < d->nf; i++) {
        const hid_field_t *f = &d->f[i];
        if (f->kind == 1 && f->group == 0 && f->page == PAGE_GENERIC && f->usage == USE_X && f->relative) {
            h->kind = HID_KIND_MOUSE;
            return;
        }
    }
}

static uint8_t rdesc_buf[HID_MAX_RDESC];

/* Перевіряє, чи на цьому адресі HID-пристрій; якщо це тачпад/миша — вмикає.
 * 1 — готово, 0 — пристрій є, але не наш, -1 — за адресою ніхто не відповів. */
static int try_device(i2c_hid_t *h, dw_i2c_t *c, uint8_t addr, uint16_t reg) {
    uint8_t hd[30];
    h->c = c;
    h->addr = addr;
    uint8_t r[2] = { (uint8_t)(reg & 0xFF), (uint8_t)(reg >> 8) };
    int xr = dw_i2c_xfer(c, addr, r, 2, hd, 30, 100);
    if (xr == DW_ERR_NOACK) return -1;
    if (xr != DW_OK) return 0;

    if (le16(hd) != 30 || le16(hd + 2) != 0x0100) return 0;               /* не HID-дескриптор */

    uint16_t rdlen = le16(hd + 4);
    h->reg_report = le16(hd + 6);
    h->reg_input  = le16(hd + 8);
    h->max_in     = le16(hd + 10);
    h->reg_cmd    = le16(hd + 16);
    h->reg_data   = le16(hd + 18);
    h->vid = le16(hd + 20);
    h->pid = le16(hd + 22);

    t_printf("[i2c] HID device: ctrl %d, addr %d, VID %s PID %s\n",
             (int)(c - h->ctl), (int)addr, hex(h->vid), hex(h->pid));

    if (rdlen < 8 || rdlen > HID_MAX_RDESC || h->max_in < 4 || h->max_in > HID_MAX_IN) {
        t_printf("[i2c]   unusable descriptor (report=%d, maxin=%d)\n", (int)rdlen, (int)h->max_in);
        return 0;
    }

    hid_cmd(h, 0x00, 0x08);                                              /* SET_POWER: ON */
    i2c_delay_ms(20);
    hid_cmd(h, 0x00, 0x01);                                              /* RESET */
    i2c_delay_ms(120);
    {   /* забрати відповідь на RESET (довжина 0) */
        uint8_t tmp[HID_MAX_IN];
        for (int k = 0; k < 3; k++) dw_i2c_xfer(c, addr, 0, 0, tmp, h->max_in, 50);
    }

    if (read_reg(h, h->reg_report, rdesc_buf, rdlen) != DW_OK) {
        t_printf("[i2c]   could not read the report descriptor\n");
        return 0;
    }
    if (hid_parse(rdesc_buf, rdlen, &h->desc) <= 0) return 0;
    classify(h);
    {
        int slots = 0, has_cnt = 0, nmode = 0;
        for (int i = 0; i < h->desc.nf; i++) {
            const hid_field_t *f = &h->desc.f[i];
            if (f->kind == 1) {
                if (f->group > slots) slots = f->group;
                if (f->page == PAGE_DIGIT && f->usage == USE_COUNT) has_cnt = 1;
            } else if (f->usage == USE_MODE) nmode++;
        }
        t_printf("[i2c]   descriptor %d bytes, %d fields, finger slots per report: %d, contact count field: %s, input-mode fields: %d\n",
                 (int)rdlen, h->desc.nf, slots, has_cnt ? "yes" : "no", nmode);
    }

    if (h->kind == HID_KIND_NONE) {
        t_printf("[i2c]   not a touchpad or mouse, skipping\n");
        return 0;
    }
    if (h->kind == HID_KIND_TOUCHPAD) {
        t_printf("[i2c]   touchpad, X 0..%d, Y 0..%d\n", h->xmax, h->ymax);
        h->retries = 0; h->next_retry = 0; h->n_touch = h->n_mouse = 0;
        i2c_hid_set_mode(h);
    } else {
        t_printf("[i2c]   mouse (relative)\n");
    }
    h->reading = 0;
    h->next_poll = 0;
    h->errors = 0;
    return 1;
}

int i2c_hid_find(i2c_hid_t *h) {
    h->kind = HID_KIND_NONE;
    h->nctl = dw_i2c_discover(h->ctl, DW_MAX_CTRL);
    if (h->nctl == 0) {
        t_printf("[i2c] no Intel LPSS I2C controllers on PCI (BIOS may hide them or use ACPI mode)\n");
        return 0;
    }
    t_printf("[i2c] I2C controllers found: %d\n", h->nctl);

    /* типові адреси тачпадів і сенсорів — спершу вони */
    static const uint8_t hot[] = { 0x15, 0x2C, 0x38, 0x10, 0x20, 0x2A, 0x4A, 0x5D, 0x14, 0x0A, 0x1A, 0x34 };
    static const uint16_t regs[] = { 0x0001, 0x0020, 0x0000 };
    static const uint32_t clocks[] = { 133000, 216000, 100000, 120000 };

    for (int ci = 0; ci < 4; ci++) {
        int acks = 0;
        for (int k = 0; k < h->nctl; k++) {
            dw_i2c_t *c = &h->ctl[k];
            if (dw_i2c_setup(c, clocks[ci]) != DW_OK) continue;

            for (int pass = 0; pass < 2; pass++) {
                for (int a = 0; a < (pass == 0 ? (int)sizeof hot : 0x78 - 0x08); a++) {
                    uint8_t addr = pass == 0 ? hot[a] : (uint8_t)(0x08 + a);
                    if (pass == 1) {
                        int skip = 0;
                        for (unsigned q = 0; q < sizeof hot; q++) if (hot[q] == addr) skip = 1;
                        if (skip) continue;
                    }
                    for (int ri = 0; ri < 3; ri++) {
                        int r = try_device(h, c, addr, regs[ri]);
                        if (r == 1) {
                            t_printf("[i2c] ready: ctrl %d, addr %d\n", k, (int)addr);
                            return 1;
                        }
                        if (r < 0) break;                      /* там нікого нема */
                        acks++;
                    }
                }
            }
        }
        if (!acks) break;      /* жоден адрес не відповів: інший такт нічого не змінить */
    }
    t_printf("[i2c] no HID touchpad found on %d I2C controller(s)\n", h->nctl);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Опитування без блокування                                          */
/* ------------------------------------------------------------------ */
#define POLL_MS 4

int i2c_hid_poll(i2c_hid_t *h, hid_frame_t *fr) {
    if (h->kind == HID_KIND_NONE) return 0;
    uint64_t now = i2c_now();

    if (!h->reading) {
        if (now < h->next_poll) return 0;
        /* звичайне читання без адреси регістра: пристрій віддає вхідний звіт */
        int r = dw_i2c_begin(h->c, h->addr, 0, 0, h->inbuf, h->max_in, 100);
        if (r < 0) { h->errors++; h->next_poll = now + (uint64_t)(h->errors > 50 ? 50 : 10) * i2c_cycles_per_ms(); return 0; }
        h->reading = 1;
        return 0;
    }

    int r = dw_i2c_step(h->c);
    if (r == 0) return 0;
    h->reading = 0;

    if (r < 0) {                                        /* NACK тут — це "даних нема" */
        h->errors++;
        h->next_poll = now + (uint64_t)(h->errors > 50 ? 50 : POLL_MS) * i2c_cycles_per_ms();
        return 0;
    }
    h->errors = 0;

    int len = le16(h->inbuf);
    if (len < 3 || len > h->max_in) {                   /* 0 — звітів нема */
        h->next_poll = now + (uint64_t)POLL_MS * i2c_cycles_per_ms();
        return 0;
    }
    h->next_poll = now;                                 /* може бути ще звіт — читаємо одразу */
    int got = hid_decode_input(&h->desc, h->inbuf + 2, len - 2, fr);
    if (got && h->kind == HID_KIND_TOUCHPAD) {
        if (fr->kind == HID_KIND_MOUSE) {
            /* Тачпад усе ще шле звіти миші: режим Precision Touchpad не ввімкнувся, а в звітах миші нема
             * кількості пальців — звідси "два пальці = рух курсора". Пробуємо ще, не частіше разу на секунду. */
            h->n_mouse++;
            if (h->retries < 10 && now >= h->next_retry) {
                h->retries++;
                h->next_retry = now + 1000 * (uint64_t)i2c_cycles_per_ms();
                i2c_hid_set_mode(h);
            }
            return got;                                 /* курсор хоч рухається, поки режим не ввімкнеться */
        }
        h->n_touch++;
    }
    return got;
}
