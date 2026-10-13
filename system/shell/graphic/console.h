#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

#ifndef CHAR_WIDTH
#define CHAR_WIDTH    8
#endif
#ifndef CHAR_HEIGHT
#define CHAR_HEIGHT   18
#endif

#define MAX_COLS      320
#define MAX_ROWS      120
extern int console_cols;
extern int console_rows;
#define SCREEN_COLS   console_cols
#define SCREEN_ROW    console_rows
#define TOP_ROWS      1                         // рядок 0 — верхня панель
#define VIEW_ROWS     (SCREEN_ROW - TOP_ROWS)   // скільки рядків тексту на екрані
#define HISTORY_ROWS  1000                      // скільки рядків пам'ятаємо (>= VIEW_ROWS)

#define COLOR_WHITE   0xFFFFFF

// --- вивід тексту (єдина точка входу для shell і t_putchar) ---
void console_reset(void);                                   // очистити історію
void console_putc(char c);                                  // білий символ
void console_puts_color(const char *s, uint32_t color);     // рядок заданим кольором

typedef void (*console_sink_fn)(char c, uint32_t color);
void console_set_sink(console_sink_fn fn);

// --- клітинки "живого" екрана (row = екранний рядок, 0 — панель) ---
void cell_set(int row, int col, char ch, uint32_t color);
char cell_get(int row, int col);

void console_init_geometry(void);

// --- гортання історії шела ---
void console_scroll_view(int delta);
void console_snap_to_bottom(void);
int  console_is_live(void);      

void console_enter_alt(void);
void console_exit_alt(void);    

void scrol_screen(void);
#endif