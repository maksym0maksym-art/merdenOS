#include "../../kernel/headers/framebuffer.h"
#include "../../kernel/headers/rtc.h"
#include "../../standarts_lib/stdio.h"

#define CHAR_WIDTH 8
#define CHAR_HEIGHT 18

#define PANEL_HEIGHT 18

/*void drw_top_panel(void)
{

    put_pixel(10, 10, 0xFF0000);

    fill_rect(0, 0, framebuffer_width, PANEL_HEIGHT, 0x00FF00);

    RTC_TIME t = rtc_get_time();
    
    char buf[15];

    buf[0] = (t.hour / 10) + '0';
    buf[1] = (t.hour % 10) + '0';
    buf[2] = ':';

    buf[3] = (t.minute / 10) + '0';
    buf[4] = (t.minute % 10) + '0';

    buf[5] = ' ';

    buf[6] = (t.day / 10) + '0';
    buf[7] = (t.day % 10) + '0';
    buf[8] = '.';

    buf[9] = (t.month / 10) + '0';
    buf[10] = (t.month % 10) + '0';
    buf[11] = '.';

    buf[12] = (t.year / 10) + '0';
    buf[13] = (t.year % 10) + '0';

    buf[14] = '\0';

    uint32_t x = framebuffer_width - (15 * CHAR_WIDTH);

    color_draw_string("MerdenOS|", 0, 0, 0x000000);

    color_draw_string(buf, x, 0, 0x000000);

}*/

/*void draw_clock_panel(void)
{
    RTC_TIME t = rtc_get_time();

    char buf[17];

    buf[0] = '|';

    buf[1] = (t.hour / 10) + '0';
    buf[2] = (t.hour % 10) + '0';
    buf[3] = ':';
    buf[4] = (t.minute / 10) + '0';
    buf[5] = (t.minute % 10) + '0';
    buf[6] = ' ';
    buf[7] = (t.day / 10) + '0';
    buf[8] = (t.day % 10) + '0';
    buf[9] = '.';
    buf[10] = (t.month / 10) + '0';
    buf[11] = (t.month % 10) + '0';
    buf[12] = '.';
    buf[13] = (t.year / 10) + '0';
    buf[14] = (t.year % 10) + '0';
    buf[15] = '|';
    buf[16] = '\0';

    uint32_t x = framebuffer_width - (15 * CHAR_WIDTH);

    color_draw_string_bg(buf, x, 0, 0x000000, 0x00FF00);
}
*/

void draw_clock_panel(void)
{
    RTC_TIME t = rtc_get_time();

    char date_buf[11];

    date_buf[0] = '|';
    date_buf[1] = (t.day / 10) + '0';
    date_buf[2] = (t.day % 10) + '0';
    date_buf[3] = '.';
    date_buf[4] = (t.month / 10) + '0';
    date_buf[5] = (t.month % 10) + '0';
    date_buf[6] = '.';
    date_buf[7] = (t.year / 10) + '0';
    date_buf[8] = (t.year % 10) + '0';
    date_buf[9] = '|';
    date_buf[10] = '\0';

    uint32_t date_width = 10 * CHAR_WIDTH;
    uint32_t date_x = (framebuffer_width - date_width) / 2;

    color_draw_string_bg(date_buf, date_x, 0, 0x000000, 0x00FF00);

    /* --- час, окремо, праворуч --- */
    char time_buf[7];

    time_buf[0] = '|';
    time_buf[1] = (t.hour / 10) + '0';
    time_buf[2] = (t.hour % 10) + '0';
    time_buf[3] = ':';
    time_buf[4] = (t.minute / 10) + '0';
    time_buf[5] = (t.minute % 10) + '0';
    time_buf[6] = '\0';

    uint32_t time_width = 6 * CHAR_WIDTH;
    uint32_t time_x = framebuffer_width - time_width;

    color_draw_string_bg(time_buf, time_x, 0, 0x000000, 0x00FF00);
}

void draw_top_panel(void)
{
    fill_rect(0, 0, framebuffer_width, PANEL_HEIGHT, 0x00FF00);
    color_draw_string_bg("MerdenOS|", 0, 0, 0x000000, 0x00FF00);
    draw_clock_panel();
}

#define BOTTOM_PANEL_HEIGHT 18

void draw_bottom_panel(const char *filename, int is_new_file)
{
    uint32_t y = framebuffer_height - BOTTOM_PANEL_HEIGHT;

    fill_rect(0, y, framebuffer_width, BOTTOM_PANEL_HEIGHT, 0xFFFFFF);

    if (is_new_file)
    {
        const char *prefix = "create file(";
        color_draw_string_bg(prefix, 0, y, 0x000000, 0xFFFFFF);

        uint32_t x = strlen(prefix) * CHAR_WIDTH;
        color_draw_string_bg(filename, x, y, 0x000000, 0xFFFFFF);

        int len = strlen(filename);
        color_draw_string_bg(")", x + (len * CHAR_WIDTH), y, 0x000000, 0xFFFFFF);
    }
    else
    {
        color_draw_string_bg(filename, 0, y, 0x000000, 0xFFFFFF);
    }
}

void clear_bottom_panel(void)
{
    if (framebuffer_height < BOTTOM_PANEL_HEIGHT) {
        return; // або clamp, залежно від бажаної поведінки
    }
    uint32_t y = framebuffer_height - BOTTOM_PANEL_HEIGHT;
    fill_rect(0, y, framebuffer_width, BOTTOM_PANEL_HEIGHT, 0x000000);
}