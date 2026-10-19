
#include <stdint.h>
#include "gui.h"
#include "../../kernel/headers/ram_files.h"
#include "../../kernel/headers/usbfs.h"
#include "syntax_c.h"
#include "../../standarts_lib/charset.h"

#define ED_MAX    32767
#define RAM_MAX   1023
#define ED_COUNT  3
#define GUTTER    5

#define C_BG       0x1B1E24
#define C_FG       0xE6E6E6
#define C_GUT_BG   0x14161B
#define C_GUT_FG   0x6B7380
#define C_CURLINE  0x242933
#define C_CURSOR   0xF0C674
#define C_STATUS   0x2D6CDF
#define C_STATUS_T 0xFFFFFF
#define C_MSG      0xFFE08A

typedef struct {
    int used;
    gui_window_t *win;
    char name[128];
    char title[144];
    int is_usb;

    char buf[ED_MAX + 1];
    int len;
    int cur;
    int want;
    int top;
    int left;
    int rows, cols;

    int modified;
    int discard_armed;
    char msg[72];
} editor_t;

static editor_t eds[ED_COUNT];

/* Буфер класів синтаксису для одного рядка */
static uint8_t ed_hl_classes[ED_MAX + 1];

/* ---------------------------------------------------------------- */
/* Допоміжні функції                                                 */
/* ---------------------------------------------------------------- */

static int e_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int e_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void e_copy(char *dst, const char *src, int max) {
    int i = 0;
    for (; src[i] && i < max - 1; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static char *put_int(char *p, int v) {
    char t[12];
    int n = 0;

    if (v == 0) t[n++] = '0';

    while (v > 0) {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    }

    while (n > 0)
        *p++ = t[--n];

    return p;
}

static void set_msg(editor_t *e, const char *m) {
    e_copy(e->msg, m, sizeof e->msg);
}

/* ---------------------------------------------------------------- */
/* Буфер                                                             */
/* ---------------------------------------------------------------- */

static int line_start(const editor_t *e, int i) {
    while (i > 0 && e->buf[i - 1] != '\n') i--;
    return i;
}

static int line_end(const editor_t *e, int i) {
    while (i < e->len && e->buf[i] != '\n') i++;
    return i;
}

static int line_count(const editor_t *e) {
    int n = 1;
    for (int i = 0; i < e->len; i++)
        if (e->buf[i] == '\n') n++;
    return n;
}

static int start_of_line(const editor_t *e, int n) {
    int idx = 0;

    while (n > 0) {
        int end = line_end(e, idx);
        if (end >= e->len) break;
        idx = end + 1;
        n--;
    }

    return idx;
}

static void cursor_pos(const editor_t *e, int *line, int *col) {
    int l = 0;

    for (int i = 0; i < e->cur; i++)
        if (e->buf[i] == '\n') l++;

    *line = l;
    *col = e->cur - line_start(e, e->cur);
}

static int text_rows(const editor_t *e) {
    int r = e->rows - 1;
    return r < 1 ? 1 : r;
}

static int text_cols(const editor_t *e) {
    int c = e->cols - GUTTER;
    return c < 1 ? 1 : c;
}

static void ensure_visible(editor_t *e) {
    int line, col;
    cursor_pos(e, &line, &col);

    int tr = text_rows(e);
    int tc = text_cols(e);

    if (line < e->top) e->top = line;
    if (line >= e->top + tr) e->top = line - tr + 1;

    if (col < e->left) e->left = col;
    if (col >= e->left + tc) e->left = col - tc + 1;

    if (e->top < 0) e->top = 0;
    if (e->left < 0) e->left = 0;
}

static void touch(editor_t *e) {
    e->want = e->cur - line_start(e, e->cur);
}

static void update_title(editor_t *e) {
    char *p = e->title;
    const char *pre = "Edit: ";

    while (*pre) *p++ = *pre++;

    for (const char *n = e->name;
         *n && p < e->title + sizeof e->title - 4; n++)
        *p++ = *n;

    if (e->modified) {
        *p++ = ' ';
        *p++ = '*';
    }

    *p = 0;
    gui_window_invalidate_title(e->win);
}

static void mark_modified(editor_t *e) {
    e->discard_armed = 0;

    if (!e->modified) {
        e->modified = 1;
        update_title(e);
    }
}

static int insert_text(editor_t *e, const char *s, int n) {
    if (n < 0 || e->len + n > ED_MAX) {
        set_msg(e, "File is full (RAM disk limit: 1023 bytes)");
        return 0;
    }

    for (int i = e->len; i >= e->cur; i--)
        e->buf[i + n] = e->buf[i];

    for (int i = 0; i < n; i++)
        e->buf[e->cur + i] = s[i];

    e->len += n;
    e->cur += n;

    mark_modified(e);
    return 1;
}

static void delete_range(editor_t *e, int a, int b) {
    if (a >= b) return;

    int n = b - a;

    for (int i = b; i <= e->len; i++)
        e->buf[i - n] = e->buf[i];

    e->len -= n;
    e->cur = a;

    mark_modified(e);
}

/* ---------------------------------------------------------------- */
/* Курсор                                                            */
/* ---------------------------------------------------------------- */

static void move_vert(editor_t *e, int dir) {
    int cs = line_start(e, e->cur);

    if (dir < 0) {
        if (cs == 0) {
            e->cur = 0;
            return;
        }

        int ps = line_start(e, cs - 1);
        int pe = cs - 1;
        int col = e->want < pe - ps ? e->want : pe - ps;

        e->cur = ps + col;
    } else {
        int ce = line_end(e, e->cur);

        if (ce >= e->len) {
            e->cur = e->len;
            return;
        }

        int ns = ce + 1;
        int ne = line_end(e, ns);
        int col = e->want < ne - ns ? e->want : ne - ns;

        e->cur = ns + col;
    }
}

static int is_word(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_';
}

static void word_left(editor_t *e) {
    int i = e->cur;

    while (i > 0 && !is_word(e->buf[i - 1])) i--;
    while (i > 0 && is_word(e->buf[i - 1])) i--;

    e->cur = i;
}

static void word_right(editor_t *e) {
    int i = e->cur;

    while (i < e->len && !is_word(e->buf[i])) i++;
    while (i < e->len && is_word(e->buf[i])) i++;

    e->cur = i;
}

/* ---------------------------------------------------------------- */
/* Завантаження файлу                                                */
/* ---------------------------------------------------------------- */

static void load_file(editor_t *e) {
    e->len = 0;
    e->buf[0] = 0;

    {
        int k = 3;

        while (e->name[k] >= '0' && e->name[k] <= '9') k++;

        e->is_usb = e->name[0] == 'u' &&
                    e->name[1] == 's' &&
                    e->name[2] == 'b' &&
                    e->name[k] == ':';

        if (!e->is_usb && e->name[0] == 'h' && e->name[1] == 'd') {      /* IDE-диск: hd0:/файл */
            k = 2;
            while (e->name[k] >= '0' && e->name[k] <= '9') k++;
            e->is_usb = e->name[k] == ':';
        }
    }

    if (e->is_usb) {
        static char raw[ED_MAX + 1];
        uint32_t got = 0;

        int rc = usbfs_read(e->name, raw, ED_MAX, &got);

        if (rc == -2) {
            set_msg(e, "New file");
            return;
        }

        if (rc == -6) {
            set_msg(e, "File too big for editor (32 KB); showing the beginning, saving disabled");
            e->is_usb = 2;
            got = ED_MAX;
        } else if (rc) {
            set_msg(e, "Error: cannot read from USB (run `usb scan`)");
            e->is_usb = 3;
            return;
        }

        int n = 0;

        for (uint32_t i = 0; i < got && n < ED_MAX; i++) {
            char c = raw[i];

            if (c == '\r') continue;

            if (c == '\t') {
                do {
                    if (n < ED_MAX) e->buf[n++] = ' ';
                } while (n % 4 != 0 && n < ED_MAX);
                continue;
            }

            if ((unsigned char)c < 32 && c != '\n') c = '?';   /* байти >= 0x80 — CP1251 (кирилиця), лишаємо */

            e->buf[n++] = c;
        }

        e->buf[n] = 0;
        e->len = n;
        return;
    }

    FS_FILE *f = fs_find(e->name);

    if (!f) {
        set_msg(e, "New file");
        return;
    }

    int n = 0;

    for (int i = 0; f->data[i] && n < ED_MAX; i++) {
        char c = f->data[i];

        if (c == '\r') continue;

        if (c == '\t') {
            do {
                if (n < ED_MAX) e->buf[n++] = ' ';
            } while (n % 4 != 0 && n < ED_MAX);
            continue;
        }

        e->buf[n++] = c;
    }

    e->buf[n] = 0;
    e->len = n;
}

/* ---------------------------------------------------------------- */
/* Збереження                                                        */
/* ---------------------------------------------------------------- */

static void save_file(editor_t *e) {
    if (e->is_usb) {
        if (e->is_usb != 1) {
            set_msg(e, "Saving disabled (file was not fully loaded)");
            return;
        }

        int rc = usbfs_write(e->name, e->buf, (uint32_t)e->len);

        if (rc) {
            char m[72], *p = m;
            const char *s = "USB save failed: ";

            while (*s) *p++ = *s++;

            s = usbfs_strerror(rc);
            while (*s && p < m + 70) *p++ = *s++;

            *p = 0;
            set_msg(e, m);
            return;
        }

        set_msg(e, "Saved to USB");
        e->discard_armed = 0;

        if (e->modified) {
            e->modified = 0;
            update_title(e);
        }

        return;
    }

    if (e->len > RAM_MAX) {
        set_msg(e, "Error: RAM-disk files are limited to 1023 bytes");
        return;
    }

    FS_FILE *(*get_or_create)(const char *) =
        (FS_FILE *(*)(const char *))fs_get_or_create;

    FS_FILE *f = get_or_create(e->name);

    if (!f) {
        set_msg(e, "Error: could not save (storage full)");
        return;
    }

    for (int i = 0; i < e->len; i++)
        f->data[i] = e->buf[i];

    f->data[e->len] = 0;
    f->size = (unsigned int)e->len;

    char m[72], *p = m;
    const char *s = "Saved, ";

    while (*s) *p++ = *s++;

    p = put_int(p, e->len);

    s = " bytes";
    while (*s) *p++ = *s++;

    *p = 0;
    set_msg(e, m);

    e->discard_armed = 0;

    if (e->modified) {
        e->modified = 0;
        update_title(e);
    }
}

/* ---------------------------------------------------------------- */
/* Малювання                                                         */
/* ---------------------------------------------------------------- */

static void ed_paint(gui_window_t *w, int x, int y,
                     int cw, int ch, void *ctx) {
    (void)w;

    editor_t *e = (editor_t *)ctx;

    e->cols = cw / 8;
    e->rows = ch / 18;

    int tr = text_rows(e);
    int tc = text_cols(e);

    int cx0, cy0, cwid, chei;
    gui_get_clip(&cx0, &cy0, &cwid, &chei);
    (void)cwid;

    int text_h = tr * 18;

    gui_draw_rect(x, y, cw, ch, C_BG);
    gui_draw_rect(x, y, GUTTER * 8 - 4, text_h, C_GUT_BG);

    int curline, curcol;
    cursor_pos(e, &curline, &curcol);

    /*
     * Підсвічуємо лише C/C++ файли.
     * Розширення визначається функцією з syntax_c.c.
     */
    int syntax_enabled = hl_is_c_file(e->name);

    hl_state_t hl;
    hl_init(&hl);

    /*
     * Відновлюємо стан блокового коментаря перед
     * першим видимим рядком.
     */
    int scan_idx = 0;

    for (int line = 0;
         line < e->top && scan_idx < e->len;
         line++) {
        int scan_end = line_end(e, scan_idx);

        if (syntax_enabled) {
            hl_scan(e->buf + scan_idx,
                    scan_end - scan_idx, &hl);
        }

        if (scan_end >= e->len) {
            scan_idx = e->len;
            break;
        }

        scan_idx = scan_end + 1;
    }

    int idx = start_of_line(e, e->top);
    int exists = 1;

    for (int r = 0; r < tr; r++) {
        int ry = y + r * 18;
        int lineno = e->top + r;

        int visible_row =
            !(ry + 18 <= cy0 || ry >= cy0 + chei);

        int start = idx;
        int end = idx;

        if (exists) {
            end = line_end(e, start);

            /*
             * Важливо: аналізуємо рядки послідовно,
             * включно з рядками поза clip-областю.
             * Так зберігається стан /* ... * / коментарів.
             */
            if (syntax_enabled) {
                hl_classify(
                    e->buf + start,
                    end - start,
                    &hl,
                    ed_hl_classes
                );
            }

            if (visible_row) {
                if (lineno == curline) {
                    gui_draw_rect(
                        x + GUTTER * 8 - 4,
                        ry,
                        cw - (GUTTER * 8 - 4),
                        18,
                        C_CURLINE
                    );
                }

                char num[12];
                char *p = put_int(num, lineno + 1);
                *p = 0;

                int nlen = (int)(p - num);
                int nx = x + (GUTTER - 1) * 8 - nlen * 8;

                gui_draw_text(
                    nx, ry, num,
                    lineno == curline ? C_FG : C_GUT_FG
                );

                for (int c = 0; c < tc; c++) {
                    int col = e->left + c;
                    int ci = start + col;

                    if (ci >= end) break;

                    char chr = e->buf[ci];

                    if (!cs_printable((unsigned char)chr))
                        chr = '?';

                    if (chr != ' ') {
                        uint32_t color = C_FG;

                        if (syntax_enabled)
                            color = hl_color(ed_hl_classes[col]);

                        gui_draw_char(
                            x + (GUTTER + c) * 8,
                            ry,
                            chr,
                            color
                        );
                    }
                }

                if (lineno == curline) {
                    int cc = curcol - e->left;

                    if (cc >= 0 && cc < tc) {
                        int cxp = x + (GUTTER + cc) * 8;
                        gui_draw_rect(cxp, ry, 2, 18, C_CURSOR);
                    }
                }
            }

            if (end >= e->len) {
                exists = 0;
            } else {
                idx = end + 1;
            }
        } else if (visible_row) {
            gui_draw_char(x + 8, ry, '~', C_GUT_FG);
        }
    }

    /* Рядок стану */
    int sy = y + ch - 18;

    gui_draw_rect(x, sy, cw, 18, C_STATUS);

    char left[100], *p = left;
    *p++ = ' ';

    for (const char *n = e->name;
         *n && p < left + 60; n++)
        *p++ = *n;

    if (e->modified) {
        *p++ = ' ';
        *p++ = '[';
        *p++ = '+';
        *p++ = ']';
    }

    *p = 0;
    gui_draw_text(x, sy, left, C_STATUS_T);

    char right[40];
    p = right;

    *p++ = 'L';
    *p++ = 'n';
    *p++ = ' ';
    p = put_int(p, curline + 1);

    *p++ = ',';
    *p++ = ' ';
    *p++ = 'C';
    *p++ = 'o';
    *p++ = 'l';
    *p++ = ' ';
    p = put_int(p, curcol + 1);

    *p++ = ' ';
    *p = 0;

    int rw = e_strlen(right) * 8;

    gui_draw_text(
        x + cw - rw - 18, sy, right, C_STATUS_T
    );

    const char *mid =
        e->msg[0] ? e->msg : "Ctrl+S save   Ctrl+Q close";

    int mx = x + (e_strlen(left) + 2) * 8;

    if (mx + e_strlen(mid) * 8 <
        x + cw - rw - 18 - 8) {
        gui_draw_text(
            mx, sy, mid,
            e->msg[0] ? C_MSG : 0xC8D8F8
        );
    }
}

/* ---------------------------------------------------------------- */
/* Клавіатура та миша                                                */
/* ---------------------------------------------------------------- */

static void redraw(editor_t *e) {
    ensure_visible(e);
    gui_window_invalidate_client(e->win);
}

static void ed_key(gui_window_t *w, const gui_key_t *k, void *ctx) {
    (void)w;

    editor_t *e = (editor_t *)ctx;
    int code = k->code;

    int had_msg = e->msg[0] != 0;
    int was_armed = e->discard_armed;

    e->msg[0] = 0;

    if (k->ctrl) {
        if (code == 's' || code == 'S') {
            save_file(e);
            redraw(e);
            return;
        }

        if (code == 'q' || code == 'Q') {
            if (e->modified && !was_armed) {
                e->discard_armed = 1;
                set_msg(e, "Unsaved changes! Ctrl+S save, Ctrl+Q again to discard");
                redraw(e);
            } else {
                gui_window_close(e->win);
            }
            return;
        }

        if (code == GUI_KEY_HOME) {
            e->cur = 0;
            touch(e);
            redraw(e);
            return;
        }

        if (code == GUI_KEY_END) {
            e->cur = e->len;
            touch(e);
            redraw(e);
            return;
        }

        if (code == GUI_KEY_LEFT) {
            word_left(e);
            touch(e);
            redraw(e);
            return;
        }

        if (code == GUI_KEY_RIGHT) {
            word_right(e);
            touch(e);
            redraw(e);
            return;
        }

        if (had_msg) redraw(e);
        return;
    }

    e->discard_armed = 0;

    switch (code) {
    case GUI_KEY_LEFT:
        if (e->cur > 0) e->cur--;
        touch(e);
        break;

    case GUI_KEY_RIGHT:
        if (e->cur < e->len) e->cur++;
        touch(e);
        break;

    case GUI_KEY_UP:
        move_vert(e, -1);
        break;

    case GUI_KEY_DOWN:
        move_vert(e, +1);
        break;

    case GUI_KEY_HOME:
        e->cur = line_start(e, e->cur);
        touch(e);
        break;

    case GUI_KEY_END:
        e->cur = line_end(e, e->cur);
        touch(e);
        break;

    case GUI_KEY_PGUP:
        for (int i = 0; i < text_rows(e) - 1; i++)
            move_vert(e, -1);
        break;

    case GUI_KEY_PGDN:
        for (int i = 0; i < text_rows(e) - 1; i++)
            move_vert(e, +1);
        break;

    case GUI_KEY_DELETE:
        if (e->cur < e->len)
            delete_range(e, e->cur, e->cur + 1);
        touch(e);
        break;

    case GUI_KEY_BACKSPACE:
        if (e->cur > 0)
            delete_range(e, e->cur - 1, e->cur);
        touch(e);
        break;

    case GUI_KEY_ENTER: {
        int ls = line_start(e, e->cur);
        int ind = 0;

        while (ls + ind < e->cur &&
               e->buf[ls + ind] == ' ' && ind < 32)
            ind++;

        char tmp[40];
        tmp[0] = '\n';

        for (int i = 0; i < ind; i++)
            tmp[1 + i] = ' ';

        insert_text(e, tmp, 1 + ind);
        touch(e);
        break;
    }

    case GUI_KEY_TAB: {
        int col = e->cur - line_start(e, e->cur);
        int n = 4 - (col % 4);

        insert_text(e, "    ", n);
        touch(e);
        break;
    }

    case GUI_KEY_ESC:
    case GUI_KEY_F4:
        break;

    default:
        if (code < 256 && cs_printable(code)) {
            char c = (char)code;
            insert_text(e, &c, 1);
            touch(e);
        }
        break;
    }

    redraw(e);
}

static void ed_click(gui_window_t *w, int x, int y, void *ctx) {
    (void)w;

    editor_t *e = (editor_t *)ctx;

    if (y < 0 || y >= text_rows(e) * 18) return;

    int line = e->top + y / 18;
    int col = e->left + (x - GUTTER * 8) / 8;

    if (x < GUTTER * 8) col = e->left;
    if (col < 0) col = 0;

    int total = line_count(e);
    if (line >= total) line = total - 1;

    int ls = start_of_line(e, line);
    int le = line_end(e, ls);
    int maxc = le - ls;

    e->cur = ls + (col < maxc ? col : maxc);

    touch(e);
    e->msg[0] = 0;
    redraw(e);
}

static void ed_wheel(gui_window_t *w, int delta, void *ctx) {
    (void)w;

    editor_t *e = (editor_t *)ctx;
    int nt = e->top - delta * 3;
    int max = line_count(e) - 1;

    if (nt > max) nt = max;
    if (nt < 0) nt = 0;

    if (nt != e->top) {
        e->top = nt;
        gui_window_invalidate_client(e->win);
    }
}

static void ed_resize(gui_window_t *w, int cw, int ch, void *ctx) {
    (void)w;

    editor_t *e = (editor_t *)ctx;

    e->cols = cw / 8;
    e->rows = ch / 18;

    ensure_visible(e);
}

static int ed_can_close(gui_window_t *w, void *ctx) {
    (void)w;

    editor_t *e = (editor_t *)ctx;

    if (!e->modified) return 1;

    set_msg(e, "Unsaved changes! Ctrl+S save, Ctrl+Q twice to discard");
    gui_window_invalidate_client(e->win);

    return 0;
}

static void ed_close(gui_window_t *w, void *ctx) {
    (void)w;

    editor_t *e = (editor_t *)ctx;

    e->used = 0;
    e->win = 0;
}

static const gui_ops_t ed_ops = {
    .paint     = ed_paint,
    .resize    = ed_resize,
    .key       = ed_key,
    .click     = ed_click,
    .wheel     = ed_wheel,
    .close     = ed_close,
    .can_close = ed_can_close,
};

/* ---------------------------------------------------------------- */
/* Відкриття редактора                                               */
/* ---------------------------------------------------------------- */

gui_window_t *gui_editor_open(const char *filename) {
    if (!filename || !filename[0]) return 0;

    char name[128];
    e_copy(name, filename, sizeof name);

    for (int i = 0; i < ED_COUNT; i++) {
        if (eds[i].used && e_eq(eds[i].name, name)) {
            gui_window_focus(eds[i].win);
            return eds[i].win;
        }
    }

    int slot = -1;
    int opened = 0;

    for (int i = 0; i < ED_COUNT; i++) {
        if (eds[i].used)
            opened++;
        else if (slot < 0)
            slot = i;
    }

    if (slot < 0) return 0;

    editor_t *e = &eds[slot];

    e->name[0] = 0;
    e_copy(e->name, name, sizeof e->name);

    e->modified = 0;
    e->discard_armed = 0;
    e->msg[0] = 0;
    e->cur = e->want = e->top = e->left = 0;

    {
        char *p = e->title;
        const char *pre = "Edit: ";

        while (*pre) *p++ = *pre++;

        for (const char *n = e->name;
             *n && p < e->title + sizeof e->title - 4; n++)
            *p++ = *n;

        *p = 0;
    }

    gui_window_t *win = gui_window_create(
        90 + opened * 30,
        60 + opened * 30,
        640, 420,
        e->title
    );

    if (win)
        gui_window_set_icon(win, GUI_ICON_EDITOR);

    if (!win) return 0;

    e->used = 1;
    e->win = win;

    load_file(e);

    int cw, ch;
    gui_window_client(win, 0, 0, &cw, &ch);

    e->cols = cw / 8;
    e->rows = ch / 18;

    gui_window_set_resizable(win, 1);
    gui_window_set_ops(win, &ed_ops, e);
    gui_window_focus(win);

    return win;
}
