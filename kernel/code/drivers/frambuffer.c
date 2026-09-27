#include <stdint.h>
#include "../../headers/framebuffer.h"
#include "../../fonts/fonts.h"

#define SCREEN_COLS 128
#define SCREEN_ROWS 42
#define HISTORY_ROWS 1000

typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} multiboot2_info_t;

typedef struct {
    uint32_t type;
    uint32_t size;
} multiboot2_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;

    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
} multiboot2_tag_framebuffer_t;

uint64_t framebuffer_addr = 0;
uint32_t framebuffer_width = 0;
uint32_t framebuffer_height = 0;
uint32_t framebuffer_pitch = 0;
uint8_t framebuffer_bpp = 0;

void framebuffer_init(uint32_t mbi_addr)
{
    multiboot2_info_t *mbi = (multiboot2_info_t *)mbi_addr;

    multiboot2_tag_t *tag =
        (multiboot2_tag_t *)(mbi_addr + 8);

    while (tag->type != 0)
    {
        if (tag->type == 8)
        {
            multiboot2_tag_framebuffer_t *fb =
                (multiboot2_tag_framebuffer_t *)tag;

            framebuffer_addr   = fb->framebuffer_addr;
            framebuffer_width  = fb->framebuffer_width;
            framebuffer_height = fb->framebuffer_height;
            framebuffer_pitch  = fb->framebuffer_pitch;
            framebuffer_bpp    = fb->framebuffer_bpp;

            return;
        }

        tag = (multiboot2_tag_t *)
            ((uint8_t *)tag + ((tag->size + 7) & ~7));
    }
}

extern uint64_t framebuffer_addr;
extern uint32_t framebuffer_width;
extern uint32_t framebuffer_height;
extern uint32_t framebuffer_pitch;

void fill_screen(uint32_t color)
{
    uint8_t *fb = (uint8_t *)(uintptr_t)framebuffer_addr;

    for (uint32_t y = 0; y < framebuffer_height; y++)
    {
        uint32_t *row = (uint32_t *)(fb + y * framebuffer_pitch);

        for (uint32_t x = 0; x < framebuffer_width; x++)
        {
            row[x] = color;
        }
    }
}

void put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (x >= framebuffer_width || y >=framebuffer_height)
    {
        return;
    }

    uint32_t *fb = (uint32_t *)(uintptr_t)framebuffer_addr;

    fb[(framebuffer_pitch / 4) * y + x] = color;
}

void draw_char(char c, uint32_t x, uint32_t y)
{
    const uint8_t *glyph = font8x18[(uint8_t)c]; 
    uint32_t color = 0xFFFFFFFF;

    for(int row = 0; row < 18; row++)
    {
        uint8_t bits = glyph[row];
        
        for(int col = 0; col < 8; col++)
        {
            //if (bits & (0x80 >> col))
            //{   
            uint32_t pixel_color = (bits & (0x80 >> col)) ? color : 0x00000000;
            put_pixel(x + col, y + row, pixel_color);
            //}
        }
    }
}
//0x00FF00
void color_draw_char(char c, uint32_t x, uint32_t y, uint32_t color)
{
    const uint8_t *glyph = font8x18[(uint8_t)c]; 

    for(int row = 0; row < 18; row++)
    {
        uint8_t bits = glyph[row];
        
        for(int col = 0; col < 8; col++)
        {
            /*if (bits & (0x80 >> col))
            {
            }*/
            uint32_t pixel_color = (bits & (0x80 >> col)) ? color : 0x00000000;
            put_pixel(x + col, y + row, pixel_color);
        }
    }
}

void color_draw_char_bg(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color)
{
    const uint8_t *glyph = font8x18[(uint8_t)c];

    for(int row = 0; row < 18; row++)
    {
        uint8_t bits = glyph[row];

        for(int col = 0; col < 8; col++)
        {
            uint32_t pixel_color = (bits & (0x80 >> col)) ? fg_color : bg_color;
            put_pixel(x + col, y + row, pixel_color);
        }
    }
}

void color_draw_string_bg(const char *str, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color)
{
    while(*str)
    {
        color_draw_char_bg(*str, x, y, fg_color, bg_color);
        x += 8;
        str++;
    }
}

void  fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t py = 0; py < h; py++)
    {
        for (uint32_t px = 0; px < w; px++)
        {
            put_pixel(x + px, y + py, color);
        }
    }
}

void color_draw_string(const char *str, uint32_t x, uint32_t y, uint32_t color)
{
    while(*str)
    {
        color_draw_char(*str, x, y, color);
        x += 8;
        str++;
    }
}