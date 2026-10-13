#include <stdint.h>
#include "syntax_c.h"
#include "gui.h"

#ifndef HL_DARK
#define HL_DARK 0
#endif

uint32_t hl_color(int cls) {
#if HL_DARK
    switch (cls) {
    case HL_KEYWORD: return 0xC678DD;
    case HL_TYPE:    return 0xE5C07B;
    case HL_NUMBER:  return 0xD19A66;
    case HL_STRING:  return 0x98C379;
    case HL_COMMENT: return 0x7F848E;
    case HL_PREPROC: return 0xE06C75;
    case HL_FUNC:    return 0x61AFEF;
    case HL_PUNCT:   return 0x56B6C2;
    default:         return 0xABB2BF;
    }
#else
    switch (cls) {
    case HL_KEYWORD: return 0x9B2393;
    case HL_TYPE:    return 0x0B6E99;
    case HL_NUMBER:  return 0xA45A00;
    case HL_STRING:  return 0x2E7D32;
    case HL_COMMENT: return 0x7A8088;
    case HL_PREPROC: return 0x8B5E00;
    case HL_FUNC:    return 0x2457C6;
    case HL_PUNCT:   return 0x455A64;
    default:         return 0x202020;
    }
#endif
}

static const char *const kw_list[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue",
    "return", "goto", "sizeof", "typedef", "struct", "union", "enum", "static", "extern",
    "const", "volatile", "register", "inline", "restrict", "auto",
    "NULL", "true", "false", "asm", "__asm__", "__attribute__",
    0
};

static const char *const type_list[] = {
    "int", "char", "short", "long", "float", "double", "void", "signed", "unsigned",
    "bool", "_Bool", "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t", "FILE",
    0
};

static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alnum(char c) { return is_alpha(c) || is_digit(c); }

static int word_in(const char *s, int n, const char *const *list) {
    for (; *list; list++) {
        const char *w = *list;
        int i = 0;
        while (i < n && w[i] && w[i] == s[i]) i++;
        if (i == n && w[i] == 0) return 1;
    }
    return 0;
}

/* uint8_t, uint32_t, mouse_state_t ... — усе, що закінчується на _t */
static int ends_with_t(const char *s, int n) { return n > 2 && s[n - 2] == '_' && s[n - 1] == 't'; }

static int only_space_before(const char *s, int i) {
    for (int k = 0; k < i; k++) if (s[k] != ' ' && s[k] != '\t') return 0;
    return 1;
}

static int is_punct(char c) {
    switch (c) {
    case '+': case '-': case '*': case '/': case '%': case '=': case '<': case '>':
    case '!': case '&': case '|': case '^': case '~': case '?': case ':': case ';':
    case ',': case '.': case '(': case ')': case '{': case '}': case '[': case ']':
        return 1;
    default:
        return 0;
    }
}

#define SET(i, v) do { if (out) out[(i)] = (uint8_t)(v); } while (0)

static void classify(const char *s, int len, hl_state_t *st, uint8_t *out) {
    int i = 0;

    if (st->in_comment) {
        while (i < len) {
            SET(i, HL_COMMENT);
            if (s[i] == '*' && i + 1 < len && s[i + 1] == '/') {
                SET(i + 1, HL_COMMENT);
                i += 2;
                st->in_comment = 0;
                break;
            }
            i++;
        }
        if (st->in_comment) return;
    }

    while (i < len) {
        char c = s[i];

        if (c == ' ' || c == '\t') { SET(i, HL_DEFAULT); i++; continue; }

        /* коментарі */
        if (c == '/' && i + 1 < len && s[i + 1] == '/') {
            while (i < len) { SET(i, HL_COMMENT); i++; }
            return;
        }
        if (c == '/' && i + 1 < len && s[i + 1] == '*') {
            SET(i, HL_COMMENT); SET(i + 1, HL_COMMENT);
            i += 2;
            st->in_comment = 1;
            while (i < len) {
                SET(i, HL_COMMENT);
                if (s[i] == '*' && i + 1 < len && s[i + 1] == '/') {
                    SET(i + 1, HL_COMMENT);
                    i += 2;
                    st->in_comment = 0;
                    break;
                }
                i++;
            }
            continue;
        }
        if (c == '#' && only_space_before(s, i)) {
            int j = i;
            while (j < len) {
                if (s[j] == '/' && j + 1 < len && (s[j + 1] == '/' || s[j + 1] == '*')) break;
                SET(j, HL_PREPROC);
                j++;
            }
            for (int k = i + 1; k < j; k++) {
                if (s[k] == '<' || s[k] == '"') {
                    char endc = (s[k] == '<') ? '>' : '"';
                    int e = k + 1;
                    while (e < j && s[e] != endc) e++;
                    int stop = (e < j) ? e + 1 : j;
                    for (int m = k; m < stop; m++) SET(m, HL_STRING);
                    break;
                }
            }
            i = j;
            continue;
        }

        if (c == '"' || c == '\'') {
            char q = c;
            SET(i, HL_STRING);
            i++;
            while (i < len) {
                SET(i, HL_STRING);
                if (s[i] == '\\' && i + 1 < len) { SET(i + 1, HL_STRING); i += 2; continue; }
                if (s[i] == q) { i++; break; }
                i++;
            }
            continue;
        }

        /* числа: 42, 0xFF, 1.5f, 10UL, .5 */
        if (is_digit(c) || (c == '.' && i + 1 < len && is_digit(s[i + 1]))) {
            while (i < len && (is_alnum(s[i]) || s[i] == '.')) { SET(i, HL_NUMBER); i++; }
            continue;
        }

        /* ідентифікатори: ключові слова, типи, виклики функцій */
        if (is_alpha(c)) {
            int st0 = i;
            while (i < len && is_alnum(s[i])) i++;
            int n = i - st0;
            int cls = HL_DEFAULT;
            if (word_in(s + st0, n, kw_list)) cls = HL_KEYWORD;
            else if (word_in(s + st0, n, type_list) || ends_with_t(s + st0, n)) cls = HL_TYPE;
            else {
                int k = i;
                while (k < len && (s[k] == ' ' || s[k] == '\t')) k++;
                if (k < len && s[k] == '(') cls = HL_FUNC;
            }
            for (int k = st0; k < i; k++) SET(k, cls);
            continue;
        }

        SET(i, is_punct(c) ? HL_PUNCT : HL_DEFAULT);
        i++;
    }
}

void hl_init(hl_state_t *st) { st->in_comment = 0; }

void hl_classify(const char *s, int len, hl_state_t *st, uint8_t *out) { classify(s, len, st, out); }

void hl_scan(const char *s, int len, hl_state_t *st) { classify(s, len, st, 0); }

#define HL_MAX_LINE 1024
static uint8_t hl_cls[HL_MAX_LINE];

int hl_draw_line(int x, int y, const char *s, int len, hl_state_t *st) {
    if (len > HL_MAX_LINE) len = HL_MAX_LINE;
    classify(s, len, st, hl_cls);
    for (int i = 0; i < len; i++, x += 8) {
        if (s[i] == ' ') continue;
        gui_draw_char(x, y, s[i], hl_color(hl_cls[i]));
    }
    return x;
}

static int ends_with(const char *s, const char *suf) {
    int n = 0, m = 0;
    while (s[n]) n++;
    while (suf[m]) m++;
    if (m > n) return 0;
    for (int i = 0; i < m; i++) if (s[n - m + i] != suf[i]) return 0;
    return 1;
}

int hl_is_c_file(const char *name) {
    if (!name) return 0;
    return ends_with(name, ".c") || ends_with(name, ".h") || ends_with(name, ".cc") ||
           ends_with(name, ".cpp") || ends_with(name, ".hpp");
}