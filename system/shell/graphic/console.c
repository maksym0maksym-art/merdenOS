#include <stdint.h>
#include "console.h"
#include "../../../kernel/headers/framebuffer.h"   // framebuffer_*, color_draw_char_bg()

extern int shell_row, shell_col;   // курсор шелла (shell.c)
void hide_cursor(void);            // shell.c
void show_cursor(void);            // shell.c

extern void clear_screen(uint32_t color);
extern void draw_top_panel(void);

// Клітинка = символ (старші 8 біт) + колір RGB (молодші 24 біти)
#define MAKE_CELL(ch, color) (((uint32_t)(uint8_t)(ch) << 24) | ((uint32_t)(color) & 0xFFFFFF))
#define CELL_CH(cell)        ((char)((cell) >> 24))
#define CELL_COLOR(cell)     ((cell) & 0xFFFFFF)
#define BLANK_CELL           MAKE_CELL(' ', COLOR_WHITE)

static uint32_t hist[HISTORY_ROWS][SCREEN_COLS];   // історія шела
static uint32_t alt_buf[SCREEN_ROW][SCREEN_COLS];  // "чужий" екран (редактор)

static int live_top    = 0;
static int view_offset = 0;

static int alt_active    = 0;
static int alt_saved_top = 0;

static uint32_t *line_ptr(int abs_line)
{
    return hist[abs_line % HISTORY_ROWS];   // abs_line завжди >= 0
}

void console_reset(void)
{
    for (int l = 0; l < HISTORY_ROWS; l++)
        for (int c = 0; c < SCREEN_COLS; c++)
            hist[l][c] = BLANK_CELL;

    live_top    = 0;
    view_offset = 0;
}

void cell_set(int row, int col, char ch, uint32_t color)
{
    if (row < TOP_ROWS || row >= SCREEN_ROW || col < 0 || col >= SCREEN_COLS)
        return;

    if (alt_active)
    {
        alt_buf[row][col] = MAKE_CELL(ch, color);
        return;
    }

    line_ptr(live_top + row - TOP_ROWS)[col] = MAKE_CELL(ch, color);
}

char cell_get(int row, int col)
{
    if (row < TOP_ROWS || row >= SCREEN_ROW || col < 0 || col >= SCREEN_COLS)
        return ' ';

    if (alt_active)
        return CELL_CH(alt_buf[row][col]);

    return CELL_CH(line_ptr(live_top + row - TOP_ROWS)[col]);
}

/* ---------- малювання екрана з буфера історії шела ---------- */

static void view_redraw(void)
{
    int bottom = live_top + VIEW_ROWS - 1 - view_offset;   // останній видимий рядок
    int oldest = live_top + VIEW_ROWS - HISTORY_ROWS;      // найстаріший, що ще в кільці
    if (oldest < 0)
        oldest = 0;

    for (int r = 0; r < VIEW_ROWS; r++)
    {
        int      abs_line = bottom - (VIEW_ROWS - 1) + r;
        uint32_t y        = (TOP_ROWS + r) * CHAR_HEIGHT;

        for (int c = 0; c < SCREEN_COLS; c++)
        {
            uint32_t cell = (abs_line >= oldest) ? line_ptr(abs_line)[c] : BLANK_CELL;

            color_draw_char_bg(CELL_CH(cell), c * CHAR_WIDTH, y,
                               CELL_COLOR(cell), 0x000000);
        }
    }
}

/* ---------- швидка прокрутка екрана (пікселі) ---------- */

static void scroll_pixels(void)
{
    uint32_t *fb     = (uint32_t *)(uintptr_t)framebuffer_addr;
    uint32_t  stride = framebuffer_pitch / 4;
    uint32_t  first  = TOP_ROWS * CHAR_HEIGHT;      // під верхньою панеллю
    uint32_t  last   = SCREEN_ROW * CHAR_HEIGHT;    // кінець текстової області

    for (uint32_t y = first; y < last - CHAR_HEIGHT; y++)
    {
        uint32_t *dst = fb + y * stride;
        uint32_t *src = fb + (y + CHAR_HEIGHT) * stride;

        for (uint32_t x = 0; x < framebuffer_width; x++)
            dst[x] = src[x];
    }

    for (uint32_t y = last - CHAR_HEIGHT; y < last; y++)
    {
        uint32_t *row = fb + y * stride;

        for (uint32_t x = 0; x < framebuffer_width; x++)
            row[x] = 0x000000;
    }
}

void scrol_screen(void)
{
    if (alt_active)
    {
        // Скролимо лише локальний буфер редактора — історія шела не чіпається.
        for (int r = TOP_ROWS; r < SCREEN_ROW - 1; r++)
            for (int c = 0; c < SCREEN_COLS; c++)
                alt_buf[r][c] = alt_buf[r + 1][c];

        for (int c = 0; c < SCREEN_COLS; c++)
            alt_buf[SCREEN_ROW - 1][c] = BLANK_CELL;

        scroll_pixels();
        return;
    }

    console_snap_to_bottom();   // пікселі зсуваємо тільки на живому екрані

    live_top++;

    // новий нижній рядок займає слот найстарішого — очищаємо його
    uint32_t *fresh = line_ptr(live_top + VIEW_ROWS - 1);
    for (int c = 0; c < SCREEN_COLS; c++)
        fresh[c] = BLANK_CELL;

    scroll_pixels();
}

/* ---------- гортання історії шела ---------- */

int console_is_live(void)
{
    return view_offset == 0;
}

void console_scroll_view(int delta)
{
    if (alt_active)
        return;   // поки активний "чужий" екран (редактор) — історію шела не гортаємо

    int max = HISTORY_ROWS - VIEW_ROWS;
    if (live_top < max)
        max = live_top;             // далі початку історії гортати нікуди

    int target = view_offset + delta;
    if (target < 0)   target = 0;
    if (target > max) target = max;
    if (target == view_offset)
        return;

    if (view_offset == 0)
        hide_cursor();              // прибрати курсор, поки ще на живому екрані

    view_offset = target;
    view_redraw();

    if (view_offset == 0)
        show_cursor();
}

void console_snap_to_bottom(void)
{
    if (view_offset != 0)
        console_scroll_view(-view_offset);
}

/* ---------- "чужий" екран (редактор тощо) ---------- */

void console_enter_alt(void)
{
    if (alt_active)
        return;

    console_snap_to_bottom();   // якщо саме гортали історію — повернутись
    hide_cursor();

    alt_saved_top = live_top;   // щоб на виході повернути точнісінько той самий вигляд
    alt_active    = 1;

    for (int r = 0; r < SCREEN_ROW; r++)
        for (int c = 0; c < SCREEN_COLS; c++)
            alt_buf[r][c] = BLANK_CELL;

    clear_screen(0x000000);
    draw_top_panel();
}

void console_exit_alt(void)
{
    if (!alt_active)
        return;

    alt_active = 0;
    live_top   = alt_saved_top;

    clear_screen(0x000000);
    draw_top_panel();
    view_redraw();              // перемалювати точнісінько те, що було до входу
}

/* ---------- вивід тексту ---------- */

static void con_putc(char c, uint32_t color)
{
    if (c == '\n')
    {
        shell_row++;
        shell_col = 0;
    }
    else
    {
        cell_set(shell_row, shell_col, c, color);
        color_draw_char_bg(c, shell_col * CHAR_WIDTH, shell_row * CHAR_HEIGHT,
                           color, 0x000000);

        if (++shell_col >= SCREEN_COLS)
        {
            shell_col = 0;
            shell_row++;
        }
    }

    while (shell_row >= SCREEN_ROW)
    {
        scrol_screen();
        shell_row--;
    }
}

void console_puts_color(const char *s, uint32_t color)
{
    if (!alt_active)
        console_snap_to_bottom();   // новий вивід шела завжди повертає до кінця

    hide_cursor();

    while (*s)
        con_putc(*s++, color);
}

void console_putc(char c)
{
    if (!alt_active)
        console_snap_to_bottom();

    hide_cursor();
    con_putc(c, COLOR_WHITE);
}