#ifndef SYNTAX_C_H
#define SYNTAX_C_H

#include <stdint.h>

/* Підсвітка синтаксису C для редактора.
 *
 * Підсвічувач працює по рядках і тримає між ними один біт стану (чи ми всередині
 * блокового коментаря slash-star ... star-slash), тому рядки треба обробляти по порядку.
 *
 * Мінімальне підключення в paint редактора:
 *
 *     hl_state_t st; hl_init(&st);
 *     for (int i = 0; i < nlines; i++) {
 *         if (i < first_visible)  hl_scan(line[i], len[i], &st);                 // лише оновити стан
 *         else if (i < last_vis)  hl_draw_line(x, y + (i - first_visible) * 18,
 *                                              line[i], len[i], &st);            // намалювати кольорами
 *     }
 *
 * Для файлів не-C просто малюйте як раніше: hl_is_c_file(filename) каже, чи вмикати підсвітку.
 */

typedef struct { int in_comment; } hl_state_t;

enum {
    HL_DEFAULT = 0,
    HL_KEYWORD,
    HL_TYPE,
    HL_NUMBER,
    HL_STRING,
    HL_COMMENT,
    HL_PREPROC,
    HL_FUNC,
    HL_PUNCT,
    HL_CLASS_COUNT
};

void     hl_init(hl_state_t *st);

/* Розмічає рядок: out[i] = клас символу i (HL_*). len може бути без завершального нуля. */
void     hl_classify(const char *s, int len, hl_state_t *st, uint8_t *out);

/* Те саме без виводу — щоб просто промотати стан через рядки над видимою областю. */
void     hl_scan(const char *s, int len, hl_state_t *st);

uint32_t hl_color(int cls);

/* Малює рядок через gui_draw_char: 8 px на символ, кольори за класами. Повертає x після рядка. */
int      hl_draw_line(int x, int y, const char *s, int len, hl_state_t *st);

/* 1, якщо ім'я файлу закінчується на .c .h .cc .cpp .hpp */
int      hl_is_c_file(const char *name);

#endif /* SYNTAX_C_H */