#include "../../headers/bmp.h"
#include "../../headers/framebuffer.h"
#include "../../../standarts_lib/stdio.h"

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int32_t read_i32(const uint8_t *p)
{
    return (int32_t)read_u32(p);
}

/* Перевіряє заголовок і повертає 0 при успіху, заповнюючи параметри. */
static int bmp_parse_header(
    const uint8_t *data,
    uint32_t data_size,
    uint32_t *out_pixel_offset,
    uint32_t *out_width,
    uint32_t *out_height,
    int *out_top_down,
    uint32_t *out_row_size
)
{
    if (!data || data_size < 54)
    {
        t_printf("bmp: file too small\n");
        return -1;
    }

    if (data[0] != 'B' || data[1] != 'M')
    {
        t_printf("bmp: bad signature\n");
        return -1;
    }

    uint32_t pixel_offset = read_u32(data + 10);
    int32_t  width        = read_i32(data + 18);
    int32_t  height_raw   = read_i32(data + 22);
    uint16_t bpp          = read_u16(data + 28);
    uint32_t compression  = read_u32(data + 30);

    if (bpp != 24 || compression != 0)
    {
        t_printf("bmp: unsupported format (bpp=%d, comp=%d)\n", bpp, compression);
        return -1;
    }

    if (width <= 0)
    {
        t_printf("bmp: invalid width\n");
        return -1;
    }

    int top_down = 0;
    uint32_t height;

    if (height_raw < 0)
    {
        top_down = 1;
        height = (uint32_t)(-height_raw);
    }
    else
    {
        height = (uint32_t)height_raw;
    }

    if (height == 0)
    {
        t_printf("bmp: invalid height\n");
        return -1;
    }

    uint32_t width_u = (uint32_t)width;
    uint32_t row_size = ((width_u * 3 + 3) / 4) * 4; /* вирівнювання рядка на 4 байти */

    /* Перевірка, що заявлені дані дійсно вміщуються в буфер */
    uint64_t needed = (uint64_t)pixel_offset + (uint64_t)row_size * (uint64_t)height;

    if (needed > data_size)
    {
        t_printf("bmp: truncated pixel data\n");
        return -1;
    }

    *out_pixel_offset = pixel_offset;
    *out_width  = width_u;
    *out_height = height;
    *out_top_down = top_down;
    *out_row_size = row_size;

    return 0;
}

void bmp_get_size(const uint8_t *data, uint32_t data_size,
                   uint32_t *out_width, uint32_t *out_height)
{
    uint32_t pixel_offset, width, height, row_size;
    int top_down;

    if (bmp_parse_header(data, data_size, &pixel_offset, &width, &height,
                          &top_down, &row_size) != 0)
    {
        if (out_width)  *out_width = 0;
        if (out_height) *out_height = 0;
        return;
    }

    if (out_width)  *out_width = width;
    if (out_height) *out_height = height;
}

int bmp_draw(const uint8_t *data, uint32_t data_size, uint32_t x, uint32_t y)
{
    uint32_t pixel_offset, width, height, row_size;
    int top_down;

    if (bmp_parse_header(data, data_size, &pixel_offset, &width, &height,
                          &top_down, &row_size) != 0)
    {
        return -1;
    }

    for (uint32_t row = 0; row < height; row++)
    {
        /* BMP за замовчуванням зберігає рядки знизу вгору (bottom-up) */
        uint32_t src_row = top_down ? row : (height - 1 - row);

        const uint8_t *row_ptr = data + pixel_offset + (uint64_t)src_row * row_size;

        for (uint32_t col = 0; col < width; col++)
        {
            const uint8_t *px = row_ptr + col * 3;

            uint8_t b = px[0];
            uint8_t g = px[1];
            uint8_t r = px[2];

            uint32_t color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

            put_pixel(x + col, y + row, color);
        }
    }

    return 0;
}

int bmp_draw_scaled(const uint8_t *data, uint32_t data_size,
                     uint32_t x, uint32_t y,
                     uint32_t dst_w, uint32_t dst_h)
{
    uint32_t pixel_offset, width, height, row_size;
    int top_down;

    if (bmp_parse_header(data, data_size, &pixel_offset, &width, &height,
                          &top_down, &row_size) != 0)
    {
        return -1;
    }

    if (dst_w == 0 || dst_h == 0)
    {
        t_printf("bmp: invalid destination size\n");
        return -1;
    }

    for (uint32_t dy = 0; dy < dst_h; dy++)
    {
        /* найближчий вихідний рядок (nearest-neighbor по вертикалі) */
        uint32_t src_y = (dy * height) / dst_h;
        if (src_y >= height)
        {
            src_y = height - 1;
        }

        uint32_t src_row = top_down ? src_y : (height - 1 - src_y);
        const uint8_t *row_ptr = data + pixel_offset + (uint64_t)src_row * row_size;

        for (uint32_t dx = 0; dx < dst_w; dx++)
        {
            /* найближчий вихідний стовпець (nearest-neighbor по горизонталі) */
            uint32_t src_x = (dx * width) / dst_w;
            if (src_x >= width)
            {
                src_x = width - 1;
            }

            const uint8_t *px = row_ptr + src_x * 3;

            uint8_t b = px[0];
            uint8_t g = px[1];
            uint8_t r = px[2];

            uint32_t color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

            put_pixel(x + dx, y + dy, color);
        }
    }

    return 0;
}

int bmp_draw_fullscreen(const uint8_t *data, uint32_t data_size)
{
    return bmp_draw_scaled(data, data_size, 0, 0,
                            framebuffer_width, framebuffer_height);
}
