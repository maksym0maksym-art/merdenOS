#include <stdint.h>
#include "gui.h"
#include "explorer.h"
#include "../../kernel/headers/usbfs.h"
#include "../../kernel/headers/ram_files.h"
#include "../../kernel/headers/klog.h"

/* Провідник файлів.
 *   ліворуч — диски (RAM-диск, змонтовані USB-флешки, «Scan USB»);
 *   праворуч — вміст папки: папки зверху, потім файли, за алфавітом;
 *   подвійний клік / Enter — відкрити (папка — увійти, файл — у редактор);
 *   Backspace — вгору; Delete (двічі) — видалити; кнопки New folder / New file / Rename.
 * RAM-диск плаский: у ньому немає папок. */

void gui_window_set_icon(gui_window_t *win, int icon);   /* gui.c */

#define MAXE        256
#define MAX_DRIVES  3
#define PATH_MAX_   256

#define TB_H        36
#define PATH_H      24
#define SB_W        158
#define HDR_H       22
#define ROW_H       22
#define STAT_H      24
#define DBL_MS      600

#define C_BG        0x0D0D0D  
#define C_PANEL     0x151515   
#define C_BAR       0x1C1C1C  
#define C_SEL       0x3A3A3A 
#define C_SEL_DIM   0x262626          
#define C_TEXT      0xE8E8E8
#define C_DIM       0x8C8C8C
#define C_ERR       0xFF7B7B
#define C_OK        0x8FE3A0
#define C_FOLDER    0xF2C94C
#define C_FILE      0xD0D0D0
#define C_BTN       0x2A2A2A
#define C_BTN_OFF   0x1A1A1A

enum { M_NONE = 0, M_NEW_DIR, M_NEW_FILE, M_RENAME };
enum { B_UP = 0, B_NEWDIR, B_NEWFILE, B_RENAME, B_DELETE, B_REFRESH, B_COUNT };

static const char *const btn_label[B_COUNT] = { "Up", "New folder", "New file", "Rename", "Delete", "Refresh" };

typedef struct {
    gui_window_t *win;
    int  drive;                         /* -1 — RAM-диск, інакше номер usbN: */
    char path[PATH_MAX_];               /* "/" або "/dir/sub" */
    int  n, sel, top;
    int  truncated;
    int  mode;
    char nbuf[100]; int nlen;
    int  armed;                         /* 1 + позиція запису, для якого чекаємо підтвердження видалення */
    char msg[112]; int msg_err;
    uint32_t last_ms; int last_row;
    int  cw, ch;
    int  dn; int dnum[MAX_DRIVES]; char dlabel[MAX_DRIVES][24];
    uint32_t free_kb, total_kb; int have_space;
} ex_t;

static ex_t X;
static usbfs_entry_t ents[MAXE];
static uint16_t order[MAXE];

/* ---------- рядки ---------- */
static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void s_copy(char *d, int sz, const char *s) { int i = 0; while (s[i] && i < sz - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int s_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int ci_cmp(const char *a, const char *b) {
    while (*a && *b) {
        char x = lower(*a), y = lower(*b);
        if (x != y) return (unsigned char)x < (unsigned char)y ? -1 : 1;
        a++; b++;
    }
    return (unsigned char)lower(*a) - (unsigned char)lower(*b);
}
static void s_cat(char *d, int sz, const char *s) { int n = s_len(d); s_copy(d + n, sz - n, s); }

static void put_u(char *d, int sz, uint32_t v) {
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    int l = s_len(d);
    while (n && l < sz - 1) d[l++] = t[--n];
    d[l] = 0;
}

static void set_msg(const char *m, int err) { s_copy(X.msg, sizeof X.msg, m); X.msg_err = err; }
static void clear_msg(void) { X.msg[0] = 0; X.msg_err = 0; }
static void fail_msg(const char *what, int rc) {
    char m[112];
    s_copy(m, sizeof m, what);
    s_cat(m, sizeof m, ": ");
    s_cat(m, sizeof m, rc >= 0 ? "error" : usbfs_strerror(rc));
    set_msg(m, 1);
}

static void size_text(uint32_t sz, char *out) {
    out[0] = 0;
    if (sz < 1024) { put_u(out, 16, sz); s_cat(out, 16, " B"); }
    else if (sz < (1u << 20)) { put_u(out, 16, sz >> 10); s_cat(out, 16, " KB"); }
    else if (sz < (1u << 30)) { put_u(out, 16, sz >> 20); s_cat(out, 16, " MB"); }
    else { put_u(out, 16, sz >> 30); s_cat(out, 16, " GB"); }
}

static void kb_text(uint32_t kb, char *out) {
    out[0] = 0;
    if (kb < 1024) { put_u(out, 16, kb); s_cat(out, 16, " KB"); }
    else if (kb < (1u << 20)) { put_u(out, 16, kb >> 10); s_cat(out, 16, " MB"); }
    else { put_u(out, 16, kb >> 20); s_cat(out, 16, " GB"); }
}

/* ---------- шляхи ---------- */
static void join(char *out, int sz, const char *dir, const char *name) {
    s_copy(out, sz, dir);
    if (!s_eq(dir, "/")) s_cat(out, sz, "/");
    s_cat(out, sz, name);
}

static void full_name_for_editor(char *out, int sz, const char *name) {
    if (X.drive < 0) { s_copy(out, sz, name); return; }
    char p[PATH_MAX_ + 4];
    join(p, sizeof p, X.path, name);
    out[0] = 0;
    s_cat(out, sz, "usb");
    put_u(out, sz, (uint32_t)X.drive);
    s_cat(out, sz, ":");
    s_cat(out, sz, p);
}

static void path_up(void) {
    int l = s_len(X.path);
    while (l > 1 && X.path[l - 1] != '/') l--;
    if (l > 1) l--;                       /* прибрати останній '/' */
    X.path[l] = 0;
    if (!X.path[0]) { X.path[0] = '/'; X.path[1] = 0; }
}

/* ---------- дані ---------- */
static int lt(int a, int b) {             /* a має стояти перед b */
    if (ents[a].is_dir != ents[b].is_dir) return ents[a].is_dir > ents[b].is_dir;
    return ci_cmp(ents[a].name, ents[b].name) < 0;
}

static void sort_entries(void) {
    for (int i = 0; i < X.n; i++) order[i] = (uint16_t)i;
    for (int i = 1; i < X.n; i++) {
        uint16_t v = order[i];
        int j = i - 1;
        while (j >= 0 && lt(v, order[j])) { order[j + 1] = order[j]; j--; }
        order[j + 1] = v;
    }
}

static void refresh_drives(void) {
    int n = usbfs_drive_count();
    if (n > MAX_DRIVES) n = MAX_DRIVES;
    X.dn = 0;
    for (int i = 0; i < n; i++) {
        int d = usbfs_drive_at(i, X.dlabel[X.dn], sizeof X.dlabel[0]);
        if (d < 0) continue;
        X.dnum[X.dn++] = d;
    }
}

static int drive_present(int d) {
    for (int i = 0; i < X.dn; i++) if (X.dnum[i] == d) return 1;
    return 0;
}

static void load(void) {
    X.n = 0; X.truncated = 0; X.have_space = 0;
    refresh_drives();
    if (X.drive >= 0 && !drive_present(X.drive)) {       /* флешку відмонтовано */
        X.drive = -1; X.path[0] = '/'; X.path[1] = 0;
        set_msg("Drive is no longer mounted - switched to RAM disk", 1);
    }
    if (X.drive < 0) {
        FS_FILE *f = fs_get_files();
        int cnt = fs_get_file_count();
        for (int i = 0; i < cnt && X.n < MAXE; i++) {
            usbfs_entry_t *e = &ents[X.n++];
            s_copy(e->name, sizeof e->name, f[i].name);
            e->is_dir = 0;
            e->size = (uint32_t)s_len(f[i].data);
        }
    } else {
        int cnt = 0;
        int rc = usbfs_list(X.drive, X.path, ents, MAXE, &cnt);
        X.n = cnt;
        if (rc) { X.n = 0; fail_msg("Cannot read folder", rc); }
        else if (cnt >= MAXE) { X.truncated = 1; set_msg("Folder has more than 256 items - showing the first 256", 1); }
        X.have_space = usbfs_drive_space(X.drive, &X.free_kb, &X.total_kb) == 0;
    }
    sort_entries();
    if (X.sel >= X.n) X.sel = X.n - 1;
    if (X.sel < 0) X.sel = 0;
    X.armed = 0;
}

static int visible_rows(void) {
    int r = (X.ch - TB_H - PATH_H - HDR_H - STAT_H) / ROW_H;
    return r < 1 ? 1 : r;
}

static void ensure_visible(void) {
    int rows = visible_rows();
    if (X.sel < X.top) X.top = X.sel;
    if (X.sel >= X.top + rows) X.top = X.sel - rows + 1;
    int max_top = X.n - rows; if (max_top < 0) max_top = 0;
    if (X.top > max_top) X.top = max_top;
    if (X.top < 0) X.top = 0;
}

static void redraw(void) { if (X.win) gui_window_invalidate_client(X.win); }

static void select_name(const char *name) {
    for (int i = 0; i < X.n; i++) {
        if (s_eq(ents[order[i]].name, name)) { X.sel = i; ensure_visible(); return; }
    }
}

static void go_drive(int drive) {
    X.drive = drive; X.path[0] = '/'; X.path[1] = 0;
    X.sel = X.top = 0; X.mode = M_NONE; clear_msg();
    load();
    redraw();
}

static const usbfs_entry_t *cur_entry(void) {
    if (X.sel < 0 || X.sel >= X.n) return 0;
    return &ents[order[X.sel]];
}

/* ---------- дії ---------- */
static void act_up(void) {
    if (X.path[0] == '/' && X.path[1] == 0) return;
    char from[64]; s_copy(from, sizeof from, "");
    int l = s_len(X.path); int k = l;
    while (k > 0 && X.path[k - 1] != '/') k--;
    s_copy(from, sizeof from, X.path + k);                /* ім'я папки, з якої виходимо — виділимо її */
    path_up();
    X.sel = X.top = 0; clear_msg();
    load();
    select_name(from);
    redraw();
}

static void act_open(void) {
    const usbfs_entry_t *e = cur_entry();
    if (!e) return;
    clear_msg();
    if (e->is_dir) {
        char p[PATH_MAX_];
        join(p, sizeof p, X.path, e->name);
        if (s_len(p) >= PATH_MAX_ - 2) { set_msg("Path too long", 1); redraw(); return; }
        s_copy(X.path, sizeof X.path, p);
        X.sel = X.top = 0;
        load();
    } else {
        char name[PATH_MAX_ + 8];
        full_name_for_editor(name, sizeof name, e->name);
        if (!gui_editor_open(name)) set_msg("Cannot open editor (too many windows?)", 1);
    }
    redraw();
}

static void act_delete(void) {
    const usbfs_entry_t *e = cur_entry();
    if (!e) return;
    if (X.armed != X.sel + 1) {
        char m[112];
        s_copy(m, sizeof m, "Press Delete again to remove '");
        s_cat(m, sizeof m, e->name);
        s_cat(m, sizeof m, "'");
        set_msg(m, 1);
        X.armed = X.sel + 1;
        redraw();
        return;
    }
    char name[256]; s_copy(name, sizeof name, e->name);
    int rc;
    if (X.drive < 0) rc = fs_delete(name) ? -2 : 0;
    else { char p[PATH_MAX_]; join(p, sizeof p, X.path, name); rc = usbfs_remove(X.drive, p); }
    int keep = X.sel;
    X.armed = 0;
    if (rc) {
        if (rc == -7) set_msg("Folder is not empty", 1); else fail_msg("Delete failed", rc);
        redraw();
        return;
    }
    load();
    X.sel = keep; if (X.sel >= X.n) X.sel = X.n - 1; if (X.sel < 0) X.sel = 0;
    ensure_visible();
    set_msg("Deleted", 0);
    redraw();
}

static void begin_name(int mode) {
    const usbfs_entry_t *e = cur_entry();
    if (mode == M_RENAME && !e) return;
    if (X.drive < 0 && mode == M_NEW_DIR) { set_msg("RAM disk has no folders", 1); redraw(); return; }
    if (X.drive < 0 && mode == M_RENAME && e->is_dir) return;
    X.mode = mode; X.armed = 0; clear_msg();
    X.nlen = 0; X.nbuf[0] = 0;
    if (mode == M_RENAME) { s_copy(X.nbuf, sizeof X.nbuf, e->name); X.nlen = s_len(X.nbuf); }
    redraw();
}

static int name_char_ok(int c) {
    if (c < 32 || c > 126) return 0;
    switch (c) { case '/': case '\\': case ':': case '*': case '?': case '"': case '<': case '>': case '|': return 0; }
    return 1;
}

static void commit_name(void) {
    if (X.nlen == 0) { X.mode = M_NONE; redraw(); return; }
    int rc = 0;
    char name[100]; s_copy(name, sizeof name, X.nbuf);
    if (X.drive < 0) {
        if (X.mode == M_NEW_FILE) { int r = fs_new_file(name); rc = r == 0 ? 0 : (r == -1 ? -3 : r == -2 ? -6 : -8); }
        else {
            const usbfs_entry_t *e = cur_entry();
            char old[256]; s_copy(old, sizeof old, e ? e->name : "");
            int r = fs_rename_file(old, name); rc = r == 0 ? 0 : (r == -2 ? -3 : r == -1 ? -2 : -8);
        }
    } else {
        char p[PATH_MAX_]; join(p, sizeof p, X.path, name);
        if (X.mode == M_RENAME) {
            const usbfs_entry_t *e = cur_entry();
            char o[PATH_MAX_]; join(o, sizeof o, X.path, e ? e->name : "");
            rc = usbfs_rename(X.drive, o, p);
        } else rc = usbfs_make(X.drive, p, X.mode == M_NEW_DIR);
    }
    if (rc) {                              /* лишаємося в режимі введення, щоб виправити ім'я */
        fail_msg(rc == -3 ? "Name already exists" : "Failed", rc);
        if (rc == -3) set_msg("Name already exists", 1);
        redraw();
        return;
    }
    X.mode = M_NONE;
    load();
    select_name(name);
    set_msg(X.n >= 0 ? "Done" : "", 0);
    redraw();
}

static void act_refresh(void) { clear_msg(); load(); ensure_visible(); redraw(); }

static void act_scan(void) {
    set_msg("Scanning USB...", 0);
    int n = usbfs_rescan();
    refresh_drives();
    load();
    if (n == 0) set_msg("No USB flash drive with FAT32/exFAT found", 1); else set_msg("USB drives updated", 0);
    redraw();
}

/* ---------- розмітка ---------- */
static int btn_x(int i) {
    int x = 8;
    for (int k = 0; k < i; k++) x += 8 * s_len(btn_label[k]) + 16 + 6;
    return x;
}
static int btn_w(int i) { return 8 * s_len(btn_label[i]) + 16; }

static int btn_enabled(int i) {
    switch (i) {
    case B_UP:      return !(X.path[0] == '/' && X.path[1] == 0);
    case B_NEWDIR:  return X.drive >= 0;
    case B_RENAME:
    case B_DELETE:  return X.n > 0;
    default:        return 1;
    }
}

static int in_box(int px, int py, int x, int y, int w, int h) { return px >= x && py >= y && px < x + w && py < y + h; }

#define SB_ITEM_H   28
#define SB_TOP      (TB_H + PATH_H + 6)

static void draw_trunc(int x, int y, int maxch, const char *s, uint32_t col) {
    char b[300];
    int n = 0, l = s_len(s);
    int cut = l > maxch;
    int lim = cut ? maxch - 2 : l;
    if (lim < 0) lim = 0;
    for (int i = 0; i < lim && n < 296; i++) { unsigned char c = (unsigned char)s[i]; b[n++] = (c >= 32 && c < 127) ? (char)c : '?'; }
    if (cut && maxch >= 2) { b[n++] = '.'; b[n++] = '.'; }
    b[n] = 0;
    gui_draw_text(x, y, b, col);
}

static void draw_btn(int x, int y, int w, int h, const char *label, int on, int active) {
    gui_draw_rrect(x, y, w, h, active ? C_SEL : (on ? C_BTN : C_BTN_OFF));
    gui_draw_text(x + 8, y + (h - 18) / 2, label, on ? C_TEXT : C_DIM);
}

static void draw_folder_icon(int x, int y) {
    gui_draw_rect(x, y + 2, 8, 3, C_FOLDER);
    gui_draw_rect(x, y + 4, 16, 11, C_FOLDER);
    gui_draw_rect(x, y + 6, 16, 1, 0xC9A235);
}
static void draw_file_icon(int x, int y) {
    gui_draw_rect(x + 2, y + 1, 12, 15, C_FILE);
    gui_draw_rect(x + 10, y + 1, 4, 4, C_BG);
    gui_draw_rect(x + 10, y + 5, 4, 1, 0xA8A8A8);
    gui_draw_rect(x + 5, y + 8, 7, 1, 0x7A7A7A);
    gui_draw_rect(x + 5, y + 11, 7, 1, 0x7A7A7A);
}

static void paint_cb(gui_window_t *win, int cx, int cy, int cw, int ch, void *ctx) {
    (void)win; (void)ctx;
    X.cw = cw; X.ch = ch;
    ensure_visible();

    gui_draw_rect(cx, cy, cw, ch, C_BG);

    /* панель кнопок */
    gui_draw_rect(cx, cy, cw, TB_H, C_PANEL);
    for (int i = 0; i < B_COUNT; i++)
        draw_btn(cx + btn_x(i), cy + 5, btn_w(i), TB_H - 10, btn_label[i], btn_enabled(i), 0);

    /* адреса */
    gui_draw_rect(cx, cy + TB_H, cw, PATH_H, C_BAR);
    {
        char p[PATH_MAX_ + 16]; p[0] = 0;
        if (X.drive < 0) s_cat(p, sizeof p, "ram:");
        else { s_cat(p, sizeof p, "usb"); put_u(p, sizeof p, (uint32_t)X.drive); s_cat(p, sizeof p, ":"); }
        s_cat(p, sizeof p, X.path);
        draw_trunc(cx + 10, cy + TB_H + 3, (cw - 20) / 8, p, C_TEXT);
    }

    int sbx = cx, sby = cy + TB_H + PATH_H, sbh = ch - TB_H - PATH_H - STAT_H;
    gui_draw_rect(sbx, sby, SB_W, sbh, C_PANEL);
    gui_draw_text(sbx + 10, sby + 2, "Drives", C_DIM);
    for (int i = 0; i <= X.dn; i++) {
        int iy = cy + SB_TOP + 14 + i * SB_ITEM_H;
        int drv = i == 0 ? -1 : X.dnum[i - 1];
        int on = drv == X.drive;
        if (on) gui_draw_rrect(sbx + 6, iy, SB_W - 12, SB_ITEM_H - 4, C_SEL);
        char t[40]; t[0] = 0;
        if (i == 0) s_copy(t, sizeof t, "RAM disk");
        else { s_cat(t, sizeof t, "usb"); put_u(t, sizeof t, (uint32_t)drv); s_cat(t, sizeof t, ": "); s_cat(t, sizeof t, X.dlabel[i - 1]); }
        draw_trunc(sbx + 14, iy + 3, (SB_W - 24) / 8, t, C_TEXT);
    }
    {
        int iy = cy + SB_TOP + 14 + (X.dn + 1) * SB_ITEM_H + 6;
        gui_draw_rrect(sbx + 6, iy, SB_W - 12, SB_ITEM_H - 4, C_BTN);
        gui_draw_text(sbx + 14, iy + 3, "Scan USB", C_TEXT);
    }

    /* список */
    int lx = cx + SB_W, lw = cw - SB_W, ly = sby;
    gui_draw_rect(lx, ly, lw, HDR_H, C_BAR);
    gui_draw_text(lx + 30, ly + 2, "Name", C_DIM);
    gui_draw_text(lx + lw - 100, ly + 2, "Size", C_DIM);
    int rows = visible_rows();
    int name_cols = (lw - 30 - 110) / 8;
    for (int r = 0; r < rows; r++) {
        int idx = X.top + r;
        int ry = ly + HDR_H + r * ROW_H;
        if (idx >= X.n) break;
        const usbfs_entry_t *e = &ents[order[idx]];
        if (idx == X.sel) gui_draw_rect(lx, ry, lw, ROW_H, X.mode == M_NONE ? C_SEL : C_SEL_DIM);
        if (e->is_dir) draw_folder_icon(lx + 8, ry + 3); else draw_file_icon(lx + 8, ry + 3);
        draw_trunc(lx + 30, ry + 2, name_cols, e->name, C_TEXT);
        if (e->is_dir) gui_draw_text(lx + lw - 100, ry + 2, "<DIR>", C_DIM);
        else { char sz[16]; size_text(e->size, sz); gui_draw_text(lx + lw - 100, ry + 2, sz, C_DIM); }
    }
    if (X.n == 0) gui_draw_text(lx + 30, ly + HDR_H + 8, "(empty)", C_DIM);
    if (X.n > rows) {                                   /* смуга прокрутки */
        int track = rows * ROW_H;
        int bar = track * rows / X.n; if (bar < 12) bar = 12;
        int span = track - bar;
        int maxtop = X.n - rows;
        int by = maxtop > 0 ? span * X.top / maxtop : 0;
        gui_draw_rect(lx + lw - 6, ly + HDR_H, 4, track, C_PANEL);
        gui_draw_rect(lx + lw - 6, ly + HDR_H + by, 4, bar, C_SEL);
    }

    /* рядок стану */
    int sy = cy + ch - STAT_H;
    gui_draw_rect(cx, sy, cw, STAT_H, C_PANEL);
    int maxch = (cw - 20) / 8;
    if (X.mode != M_NONE) {
        char t[160]; t[0] = 0;
        s_cat(t, sizeof t, X.mode == M_NEW_DIR ? "New folder: " : X.mode == M_NEW_FILE ? "New file: " : "Rename to: ");
        s_cat(t, sizeof t, X.nbuf);
        s_cat(t, sizeof t, "_");
        if (X.msg[0]) { s_cat(t, sizeof t, "   "); s_cat(t, sizeof t, X.msg); }
        draw_trunc(cx + 10, sy + 3, maxch, t, X.msg[0] && X.msg_err ? C_ERR : C_TEXT);
    } else if (X.msg[0]) {
        draw_trunc(cx + 10, sy + 3, maxch, X.msg, X.msg_err ? C_ERR : C_OK);
    } else {
        char t[160]; t[0] = 0;
        put_u(t, sizeof t, (uint32_t)X.n); s_cat(t, sizeof t, X.n == 1 ? " item" : " items");
        if (X.have_space) {
            char a[16], b[16];
            kb_text(X.free_kb, a); kb_text(X.total_kb, b);
            s_cat(t, sizeof t, "   |   "); s_cat(t, sizeof t, a); s_cat(t, sizeof t, " free of "); s_cat(t, sizeof t, b);
        }
        const usbfs_entry_t *e = cur_entry();
        if (e) { s_cat(t, sizeof t, "   |   "); s_cat(t, sizeof t, e->name); }
        draw_trunc(cx + 10, sy + 3, maxch, t, C_DIM);
    }
}

/* ---------- події ---------- */
static void on_resize(gui_window_t *w, int cw, int ch, void *ctx) {
    (void)w; (void)ctx;
    X.cw = cw; X.ch = ch;
    ensure_visible();
}

static void move_sel(int to) {
    if (X.n == 0) return;
    if (to < 0) to = 0;
    if (to >= X.n) to = X.n - 1;
    if (to != X.sel) { X.sel = to; X.armed = 0; clear_msg(); }
    ensure_visible();
    redraw();
}

static void on_click(gui_window_t *w, int x, int y, void *ctx) {
    (void)w; (void)ctx;

    if (y < TB_H) {                                           /* кнопки */
        for (int i = 0; i < B_COUNT; i++) {
            if (!in_box(x, y, btn_x(i), 5, btn_w(i), TB_H - 10)) continue;
            if (!btn_enabled(i)) return;
            if (X.mode != M_NONE) { X.mode = M_NONE; clear_msg(); }
            switch (i) {
            case B_UP:      act_up(); break;
            case B_NEWDIR:  begin_name(M_NEW_DIR); break;
            case B_NEWFILE: begin_name(M_NEW_FILE); break;
            case B_RENAME:  begin_name(M_RENAME); break;
            case B_DELETE:  act_delete(); break;
            case B_REFRESH: act_refresh(); break;
            }
            return;
        }
        return;
    }

    int sbtop = SB_TOP + 14;
    if (x < SB_W && y >= TB_H + PATH_H) {                     /* диски */
        for (int i = 0; i <= X.dn; i++) {
            if (in_box(x, y, 6, sbtop + i * SB_ITEM_H, SB_W - 12, SB_ITEM_H - 4)) {
                go_drive(i == 0 ? -1 : X.dnum[i - 1]);
                return;
            }
        }
        if (in_box(x, y, 6, sbtop + (X.dn + 1) * SB_ITEM_H + 6, SB_W - 12, SB_ITEM_H - 4)) { act_scan(); return; }
        return;
    }

    int ly = TB_H + PATH_H + HDR_H;
    if (x >= SB_W && y >= ly) {                               /* список */
        int r = (y - ly) / ROW_H;
        int idx = X.top + r;
        if (r >= visible_rows() || idx >= X.n) return;
        if (X.mode != M_NONE) { X.mode = M_NONE; clear_msg(); }
        uint32_t now = klog_uptime_ms();
        int dbl = (idx == X.last_row) && (now - X.last_ms < DBL_MS) && idx == X.sel;
        X.last_ms = now; X.last_row = idx;
        if (idx != X.sel) { X.sel = idx; X.armed = 0; clear_msg(); }
        if (dbl) { X.last_row = -1; act_open(); return; }
        redraw();
    }
}

static void on_wheel(gui_window_t *w, int delta, void *ctx) {
    (void)w; (void)ctx;
    int rows = visible_rows();
    int nt = X.top - delta * 3;
    int mx = X.n - rows; if (mx < 0) mx = 0;
    if (nt > mx) nt = mx;
    if (nt < 0) nt = 0;
    if (nt != X.top) { X.top = nt; redraw(); }
}

static void on_key(gui_window_t *w, const gui_key_t *k, void *ctx) {
    (void)w; (void)ctx;

    if (X.mode != M_NONE) {                                   /* введення імені */
        if (k->code == GUI_KEY_ESC) { X.mode = M_NONE; clear_msg(); redraw(); return; }
        if (k->code == GUI_KEY_ENTER) { commit_name(); return; }
        if (k->code == GUI_KEY_BACKSPACE) { if (X.nlen > 0) X.nbuf[--X.nlen] = 0; clear_msg(); redraw(); return; }
        if (!k->ctrl && !k->alt && name_char_ok(k->code) && X.nlen < 96) { X.nbuf[X.nlen++] = (char)k->code; X.nbuf[X.nlen] = 0; clear_msg(); redraw(); }
        return;
    }

    int rows = visible_rows();
    switch (k->code) {
    case GUI_KEY_UP:    move_sel(X.sel - 1); break;
    case GUI_KEY_DOWN:  move_sel(X.sel + 1); break;
    case GUI_KEY_PGUP:  move_sel(X.sel - rows); break;
    case GUI_KEY_PGDN:  move_sel(X.sel + rows); break;
    case GUI_KEY_HOME:  move_sel(0); break;
    case GUI_KEY_END:   move_sel(X.n - 1); break;
    case GUI_KEY_ENTER: act_open(); break;
    case GUI_KEY_BACKSPACE: act_up(); break;
    case GUI_KEY_DELETE:    act_delete(); break;
    case GUI_KEY_LEFT:      act_up(); break;
    case GUI_KEY_RIGHT: { const usbfs_entry_t *e = cur_entry(); if (e && e->is_dir) act_open(); break; }
    case 'n': case 'N': if (k->ctrl) begin_name(M_NEW_FILE); break;
    default: break;
    }
}

static void on_close(gui_window_t *w, void *ctx) {
    (void)w; (void)ctx;
    X.win = 0;
}

static const gui_ops_t ex_ops = {
    .paint = paint_cb, .resize = on_resize, .key = on_key, .click = on_click,
    .wheel = on_wheel, .close = on_close, .can_close = 0,
};

gui_window_t *gui_explorer_open(void) {
    if (X.win) { gui_window_focus(X.win); return X.win; }

    X.drive = -1;
    X.path[0] = '/'; X.path[1] = 0;
    X.n = X.sel = X.top = 0;
    X.mode = M_NONE; X.nlen = 0; X.nbuf[0] = 0; X.armed = 0;
    X.last_ms = 0; X.last_row = -1;
    clear_msg();

    gui_window_t *win = gui_window_create(110, 60, 720, 480, "Explorer");
    if (!win) return 0;
    X.win = win;
    gui_window_set_icon(win, GUI_ICON_FOLDER);
    gui_window_set_resizable(win, 1);
    X.cw = 720; X.ch = 480 - GUI_TITLE_H;
    gui_window_set_ops(win, &ex_ops, 0);

    /* якщо флешка вже змонтована при старті — відкриємо її, інакше RAM-диск */
    load();
    if (X.dn > 0) { X.drive = X.dnum[0]; load(); }
    gui_window_focus(win);
    return win;
}
