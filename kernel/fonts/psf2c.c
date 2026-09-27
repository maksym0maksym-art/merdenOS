#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct __attribute__((packed))
{
    uint16_t magic;
    uint8_t mode;
    uint8_t charsize;
} PSF1Header;

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("Usage: %s font.psf\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f)
    {
        perror("fopen");
        return 1;
    }

    PSF1Header hdr;
    fread(&hdr, sizeof(hdr), 1, f);

    if (hdr.magic != 0x0436)
    {
        printf("Not a PSF1 font!\n");
        return 1;
    }

    int glyphs = 256;

    printf("#include <stdint.h>\n\n");
    printf("const uint8_t font8x%d[%d][%d] = {\n",
           hdr.charsize, glyphs, hdr.charsize);

    for (int g = 0; g < glyphs; g++)
    {
        printf("    {");

        for (int y = 0; y < hdr.charsize; y++)
        {
            uint8_t b;
            fread(&b, 1, 1, f);

            printf("0x%02X", b);

            if (y != hdr.charsize - 1)
                printf(", ");
        }

        printf("}");

        if (g != glyphs - 1)
            printf(",");

        printf("\n");
    }

    printf("};\n");

    fclose(f);
    return 0;
}