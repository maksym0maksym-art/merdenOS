/*
 * USB Mass Storage: Bulk-Only Transport (BOT) + SCSI (INQUIRY, TEST UNIT READY,
 * READ CAPACITY(10), READ(10), WRITE(10), SYNCHRONIZE CACHE, REQUEST SENSE).
 *
 * Послідовність однієї команди (USB MSC BOT 1.0):
 *     CBW (31 байт, bulk OUT) -> [дані, bulk IN/OUT] -> CSW (13 байт, bulk IN)
 * Якщо endpoint відповів STALL — знімаємо halt і читаємо CSW. При фатальних помилках —
 * "reset recovery": Bulk-Only Mass Storage Reset + CLEAR_HALT на обох endpoint'ах.
 */

#include <stdint.h>
#include "../../../headers/usb_msc.h"
#include "../../../headers/usb_dbg.h"

#define CBW_SIG 0x43425355u        /* "USBC" */
#define CSW_SIG 0x53425355u        /* "USBS" */

#ifdef USB_HOSTTEST                 /* у host-тесті таймаути короткі, щоб ін'єкція збоїв не гальмувала прогін */
#define BOT_CMD_TIMEOUT   400
#define BOT_DATA_TIMEOUT  400
#else
#define BOT_CMD_TIMEOUT   5000
#define BOT_DATA_TIMEOUT  15000
#endif

static usb_msc_t msc_tab[MSC_MAX];

/* DMA-буфери для службових команд (вирівняно, щоб не перетинати межу 64 КБ) */
static uint8_t cbw_buf[32]    __attribute__((aligned(64)));
static uint8_t csw_buf[16]    __attribute__((aligned(64)));
static uint8_t small_buf[64]  __attribute__((aligned(64)));
static uint8_t cfg_buf[1024]  __attribute__((aligned(64)));

static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get_le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t get_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

const char *usb_msc_err_str(int e) {
    switch (e) {
    case MSC_OK:       return "ok";
    case MSC_E_IO:     return "USB transfer error";
    case MSC_E_SCSI:   return "device reported an error";
    case MSC_E_ARGS:   return "bad arguments";
    case MSC_E_NOMED:  return "no medium in the drive";
    case MSC_E_WP:     return "drive is write-protected";
    }
    return "unknown error";
}

/* ------------------------------------------------------------------ */
/*  Bulk-Only Transport                                                */
/* ------------------------------------------------------------------ */
static void bot_reset_recovery(usb_msc_t *m) {
    USB_DBG("[msc] reset recovery\n");
    xhci_control(m->dev, 0x21, 0xFF, 0, m->iface, 0, 0);       /* Bulk-Only Mass Storage Reset */
    xhci_clear_halt(m->dev, 1);
    xhci_clear_halt(m->dev, 0);
}

static int bot_cmd(usb_msc_t *m, const uint8_t *cdb, uint8_t cdb_len,
                   void *data, uint32_t dlen, int data_in, uint32_t timeout_ms, uint32_t *got) {
    uint32_t act = 0, xfer = 0;
    int r;

    for (int i = 0; i < 32; i++) cbw_buf[i] = 0;
    put_le32(cbw_buf + 0, CBW_SIG);
    uint32_t tag = ++m->tag;
    put_le32(cbw_buf + 4, tag);
    put_le32(cbw_buf + 8, dlen);
    cbw_buf[12] = (dlen && data_in) ? 0x80 : 0x00;
    cbw_buf[13] = 0;                                   /* LUN 0 */
    cbw_buf[14] = cdb_len;
    for (int i = 0; i < cdb_len && i < 16; i++) cbw_buf[15 + i] = cdb[i];

    r = xhci_bulk(m->dev, 0, cbw_buf, 31, &act, BOT_CMD_TIMEOUT);
    if (r) {
        USB_DBG("[msc] CBW failed (%d)\n", r);
        bot_reset_recovery(m);
        return MSC_E_IO;
    }

    if (dlen) {
        r = xhci_bulk(m->dev, data_in, data, dlen, &xfer, timeout_ms);
        if (r == XHCI_E_STALL) {
            USB_DBG("[msc] data stage STALL after %u bytes\n", xfer);
            xhci_clear_halt(m->dev, data_in);
        } else if (r) {
            USB_DBG("[msc] data stage failed (%d)\n", r);
            bot_reset_recovery(m);
            return MSC_E_IO;
        }
    }
    if (got) *got = xfer;

    for (int attempt = 0; attempt < 2; attempt++) {
        r = xhci_bulk(m->dev, 1, csw_buf, 13, &act, BOT_CMD_TIMEOUT);
        if (r == XHCI_E_STALL) { xhci_clear_halt(m->dev, 1); continue; }
        break;
    }
    if (r) {
        USB_DBG("[msc] CSW failed (%d)\n", r);
        bot_reset_recovery(m);
        return MSC_E_IO;
    }

    if (act != 13 || get_le32(csw_buf) != CSW_SIG || get_le32(csw_buf + 4) != tag) {
        usb_printf("[msc] invalid CSW (len %u sig %08x tag %u/%u)\n", act, get_le32(csw_buf), get_le32(csw_buf + 4), tag);
        bot_reset_recovery(m);
        return MSC_E_IO;
    }
    uint8_t st = csw_buf[12];
    if (st == 2) {
        USB_DBG("[msc] phase error\n");
        bot_reset_recovery(m);
        return MSC_E_IO;
    }
    if (st == 1) return MSC_E_SCSI;
    return MSC_OK;
}

/* ------------------------------------------------------------------ */
/*  SCSI                                                               */
/* ------------------------------------------------------------------ */
static int request_sense(usb_msc_t *m) {
    uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    for (int i = 0; i < 18; i++) small_buf[i] = 0;
    int r = bot_cmd(m, cdb, 6, small_buf, 18, 1, BOT_CMD_TIMEOUT, 0);
    if (r != MSC_OK) return MSC_E_IO;
    m->sense_key = small_buf[2] & 0x0F;
    m->asc  = small_buf[12];
    m->ascq = small_buf[13];
    return MSC_OK;
}

/* Виконати SCSI-команду; CHECK CONDITION -> REQUEST SENSE -> осмислений код. */
static int scsi_cmd(usb_msc_t *m, const uint8_t *cdb, uint8_t cdb_len,
                    void *data, uint32_t dlen, int data_in, uint32_t timeout_ms) {
    int r = bot_cmd(m, cdb, cdb_len, data, dlen, data_in, timeout_ms, 0);
    if (r != MSC_E_SCSI) return r;

    if (request_sense(m) != MSC_OK) return MSC_E_IO;
    USB_DBG("[msc] cmd %02x -> sense %x/%02x/%02x\n", cdb[0], m->sense_key, m->asc, m->ascq);

    if (m->sense_key == 0x02 && m->asc == 0x3A) return MSC_E_NOMED;        /* NOT READY, medium not present */
    if (m->sense_key == 0x07) return MSC_E_WP;                             /* DATA PROTECT */
    return MSC_E_SCSI;
}

static void copy_trim(char *dst, const uint8_t *src, int n) {
    int e = n;
    while (e > 0 && (src[e - 1] == ' ' || src[e - 1] == 0)) e--;
    for (int i = 0; i < e; i++) dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
    dst[e] = 0;
}

static int msc_init_unit(usb_msc_t *m) {
    int r;

    /* INQUIRY */
    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    for (int i = 0; i < 36; i++) small_buf[i] = 0;
    r = scsi_cmd(m, inq, 6, small_buf, 36, 1, BOT_CMD_TIMEOUT);
    if (r == MSC_OK) {
        copy_trim(m->vendor,  small_buf + 8,  8);
        copy_trim(m->product, small_buf + 16, 16);
        copy_trim(m->rev,     small_buf + 32, 4);
    } else {
        m->vendor[0] = m->product[0] = m->rev[0] = 0;
        USB_DBG("[msc] INQUIRY failed (%d)\n", r);
    }

    /* TEST UNIT READY: флешки після підключення часто "прокидаються" 1-3 секунди */
    uint8_t tur[6] = { 0, 0, 0, 0, 0, 0 };
    int nomed = 0, ready = 0;
    for (int i = 0; i < 40; i++) {
        r = scsi_cmd(m, tur, 6, 0, 0, 0, BOT_CMD_TIMEOUT);
        if (r == MSC_OK) { ready = 1; break; }
        if (r == MSC_E_NOMED && ++nomed > 3) return MSC_E_NOMED;
        usb_delay_ms(250);
    }
    if (!ready) { usb_printf("[msc] drive never became ready\n"); return r < 0 ? r : MSC_E_IO; }

    /* READ CAPACITY(10) */
    uint8_t rc[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    r = MSC_E_IO;
    for (int i = 0; i < 5; i++) {
        for (int k = 0; k < 8; k++) small_buf[k] = 0;
        r = scsi_cmd(m, rc, 10, small_buf, 8, 1, BOT_CMD_TIMEOUT);
        if (r == MSC_OK) break;
        usb_delay_ms(200);
    }
    if (r != MSC_OK) return r;

    uint32_t last = get_be32(small_buf);
    m->block_size = get_be32(small_buf + 4);
    if (m->block_size < 512 || m->block_size > 4096 || (m->block_size & (m->block_size - 1))) {
        usb_printf("[msc] unsupported block size %u\n", m->block_size);
        return MSC_E_ARGS;
    }
    if (last == 0xFFFFFFFFu) {
        m->block_count = 0xFFFFFFFFu;
        usb_printf("[msc] drive is larger than 2 TB, only the first 2 TB are usable\n");
    } else {
        m->block_count = last + 1;
    }

    /* MODE SENSE(6): біт WP у device-specific parameter. Деякі флешки не вміють — ігноруємо помилку. */
    uint8_t ms[6] = { 0x1A, 0x08 /* DBD */, 0x3F, 0, 4, 0 };
    for (int k = 0; k < 4; k++) small_buf[k] = 0;
    m->write_protected = 0;
    if (scsi_cmd(m, ms, 6, small_buf, 4, 1, BOT_CMD_TIMEOUT) == MSC_OK)
        m->write_protected = (small_buf[2] & 0x80) != 0;

    return MSC_OK;
}

/* ------------------------------------------------------------------ */
/*  Читання / запис                                                    */
/* ------------------------------------------------------------------ */
int usb_msc_read(void *ctx, uint32_t lba, uint32_t count, void *buf) {
    usb_msc_t *m = (usb_msc_t *)ctx;
    if (!m || !m->used || !m->block_size) return MSC_E_ARGS;
    if (lba >= m->block_count || count > m->block_count - lba) return MSC_E_ARGS;

    uint8_t *p = (uint8_t *)buf;
    uint32_t max_blocks = 65536u / m->block_size;

    while (count) {
        uint32_t n = count < max_blocks ? count : max_blocks;
        uint8_t cdb[10] = { 0x28, 0, 0, 0, 0, 0, 0, (uint8_t)(n >> 8), (uint8_t)n, 0 };
        put_be32(cdb + 2, lba);

        int r = MSC_E_IO;
        for (int attempt = 0; attempt < 3; attempt++) {
            r = scsi_cmd(m, cdb, 10, p, n * m->block_size, 1, BOT_DATA_TIMEOUT);
            if (r == MSC_OK) break;
            if (r == MSC_E_NOMED) break;
            usb_printf("[msc] read LBA %u x%u failed (%s), retry %d\n", lba, n, usb_msc_err_str(r), attempt + 1);
        }
        if (r != MSC_OK) return r;

        p   += n * m->block_size;
        lba += n;
        count -= n;
    }
    return MSC_OK;
}

int usb_msc_write(void *ctx, uint32_t lba, uint32_t count, const void *buf) {
    usb_msc_t *m = (usb_msc_t *)ctx;
    if (!m || !m->used || !m->block_size) return MSC_E_ARGS;
    if (m->write_protected) return MSC_E_WP;
    if (lba >= m->block_count || count > m->block_count - lba) return MSC_E_ARGS;

    const uint8_t *p = (const uint8_t *)buf;
    uint32_t max_blocks = 65536u / m->block_size;

    while (count) {
        uint32_t n = count < max_blocks ? count : max_blocks;
        uint8_t cdb[10] = { 0x2A, 0, 0, 0, 0, 0, 0, (uint8_t)(n >> 8), (uint8_t)n, 0 };
        put_be32(cdb + 2, lba);

        int r = MSC_E_IO;
        for (int attempt = 0; attempt < 3; attempt++) {
            r = scsi_cmd(m, cdb, 10, (void *)p, n * m->block_size, 0, BOT_DATA_TIMEOUT);
            if (r == MSC_OK || r == MSC_E_WP || r == MSC_E_NOMED) break;
            usb_printf("[msc] write LBA %u x%u failed (%s), retry %d\n", lba, n, usb_msc_err_str(r), attempt + 1);
        }
        if (r != MSC_OK) return r;

        p   += n * m->block_size;
        lba += n;
        count -= n;
    }
    return MSC_OK;
}

int usb_msc_sync(void *ctx) {
    usb_msc_t *m = (usb_msc_t *)ctx;
    if (!m || !m->used) return MSC_E_ARGS;
    uint8_t cdb[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };         /* SYNCHRONIZE CACHE(10) */
    int r = scsi_cmd(m, cdb, 10, 0, 0, 0, 30000);
    /* Багато флешок не знають цієї команди (ILLEGAL REQUEST) — це не помилка запису. */
    if (r == MSC_E_SCSI && m->sense_key == 0x05) return MSC_OK;
    return r;
}

int usb_msc_eject(usb_msc_t *m) {
    if (!m || !m->used) return MSC_E_ARGS;
    usb_msc_sync(m);
    uint8_t cdb[6] = { 0x1B, 0, 0, 0, 0x02, 0 };                    /* START STOP UNIT: LoEj=1, Start=0 */
    return scsi_cmd(m, cdb, 6, 0, 0, 0, 10000);
}

/* ------------------------------------------------------------------ */
/*  Реєстр пристроїв                                                   */
/* ------------------------------------------------------------------ */
int usb_msc_count(void) {
    int n = 0;
    for (int i = 0; i < MSC_MAX; i++) if (msc_tab[i].used) n++;
    return n;
}

usb_msc_t *usb_msc_get(int idx) {
    int n = 0;
    for (int i = 0; i < MSC_MAX; i++) {
        if (!msc_tab[i].used) continue;
        if (n++ == idx) return &msc_tab[i];
    }
    return 0;
}

void usb_msc_forget_all(void) {
    for (int i = 0; i < MSC_MAX; i++) {
        uint8_t *b = (uint8_t *)&msc_tab[i];
        for (uint32_t k = 0; k < sizeof(msc_tab[i]); k++) b[k] = 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Probe: розбір конфігурації, пошук інтерфейсу 08/06/50                */
/* ------------------------------------------------------------------ */
int usb_msc_probe(xhci_dev_t *dev, void *arg) {
    (void)arg;
    if (dev->dclass == 9) return 0;                                 /* хаб */

    int r = xhci_control(dev, 0x80, 6, 0x0200, 0, cfg_buf, 9);
    if (r < 9) { USB_DBG("[msc] cannot read config header (%d)\n", r); return 0; }

    uint32_t total = (uint32_t)cfg_buf[2] | ((uint32_t)cfg_buf[3] << 8);
    if (total < 9) return 0;
    if (total > sizeof(cfg_buf)) total = sizeof(cfg_buf);

    r = xhci_control(dev, 0x80, 6, 0x0200, 0, cfg_buf, (uint16_t)total);
    if (r < 9) { USB_DBG("[msc] cannot read configuration (%d)\n", r); return 0; }
    if ((uint32_t)r < total) total = (uint32_t)r;

    uint8_t cfg_value = cfg_buf[5];
    int in_ep = 0, out_ep = 0;
    uint16_t in_mps = 0, out_mps = 0;
    uint8_t in_burst = 0, out_burst = 0, iface = 0;
    int in_msc = 0, found = 0, last_is_in = -1;

    for (uint32_t off = cfg_buf[0]; off + 2 <= total; ) {
        uint32_t len = cfg_buf[off];
        uint32_t type = cfg_buf[off + 1];
        if (len < 2 || off + len > total) break;

        if (type == 4 && len >= 9) {                               /* interface */
            if (in_msc && in_ep && out_ep) { found = 1; break; }
            in_msc = (cfg_buf[off + 3] == 0 && cfg_buf[off + 5] == 0x08 &&
                      cfg_buf[off + 6] == 0x06 && cfg_buf[off + 7] == 0x50);
            if (in_msc) { iface = cfg_buf[off + 2]; in_ep = out_ep = 0; }
            last_is_in = -1;
        } else if (type == 5 && len >= 7 && in_msc) {              /* endpoint */
            uint8_t addr = cfg_buf[off + 2];
            uint8_t attr = cfg_buf[off + 3];
            uint16_t mps = (uint16_t)((cfg_buf[off + 4] | (cfg_buf[off + 5] << 8)) & 0x7FF);
            if ((attr & 3) == 2) {                                  /* bulk */
                if (addr & 0x80) { in_ep = addr;  in_mps = mps;  in_burst = 0;  last_is_in = 1; }
                else             { out_ep = addr; out_mps = mps; out_burst = 0; last_is_in = 0; }
            } else {
                last_is_in = -1;
            }
        } else if (type == 0x30 && len >= 6 && in_msc) {           /* SuperSpeed Endpoint Companion */
            if (last_is_in == 1) in_burst = cfg_buf[off + 2];
            else if (last_is_in == 0) out_burst = cfg_buf[off + 2];
        }
        off += len;
    }
    if (in_msc && in_ep && out_ep) found = 1;
    if (!found) return 0;

    usb_printf("[msc] mass storage interface found (bulk IN %02x/%u, OUT %02x/%u)\n", in_ep, in_mps, out_ep, out_mps);

    usb_msc_t *m = 0;
    for (int i = 0; i < MSC_MAX; i++) if (!msc_tab[i].used) { m = &msc_tab[i]; break; }
    if (!m) { usb_printf("[msc] too many mass storage devices\n"); return 0; }

    uint8_t *mb = (uint8_t *)m;
    for (uint32_t k = 0; k < sizeof(*m); k++) mb[k] = 0;
    m->dev = dev;
    m->iface = iface;
    m->used = 1;

    r = xhci_configure_bulk(dev, cfg_value, (uint8_t)in_ep, in_mps, in_burst, (uint8_t)out_ep, out_mps, out_burst);
    if (r) {
        usb_printf("[msc] cannot configure endpoints (%d)\n", r);
        m->used = 0;
        return 0;
    }

    r = msc_init_unit(m);
    if (r != MSC_OK) {
        usb_printf("[msc] device init failed: %s\n", usb_msc_err_str(r));
        m->used = 0;
        return 0;
    }

    usb_printf("[msc] \"%s %s\" rev %s: %u blocks x %u B (%u MB)%s\n", m->vendor, m->product, m->rev,
               m->block_count, m->block_size,
               (uint32_t)(((uint64_t)m->block_count * m->block_size) >> 20),
               m->write_protected ? " [write-protected]" : "");
    return 1;
}
