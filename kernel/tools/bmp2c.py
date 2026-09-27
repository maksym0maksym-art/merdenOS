#!/usr/bin/env python3
"""
bmp2c.py — конвертує 24-bit BMP-файл у C-масив байтів для вбудовування
в ядро MerdenOS (за аналогією з psf2c для шрифтів).

Використання:
    python3 bmp2c.py input.bmp output.c symbol_name

Згенерований .c файл містить:
    const uint8_t <symbol_name>_data[] = { ... };  // сирі байти BMP-файлу
    const unsigned int <symbol_name>_size = N;

Рендерити цей масив у ядрі можна функцією bmp_draw() з bmp.c/bmp.h —
вона сама розбирає BMP-заголовок, тому оригінальні байти файлу
зберігаються як є (нічого додатково не декодується на етапі збірки).
"""

import sys


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} input.bmp output.c symbol_name")
        sys.exit(1)

    in_path, out_path, symbol = sys.argv[1], sys.argv[2], sys.argv[3]

    with open(in_path, "rb") as f:
        data = f.read()

    if data[0:2] != b"BM":
        print("Помилка: це не BMP-файл (немає сигнатури 'BM')")
        sys.exit(1)

    with open(out_path, "w") as f:
        f.write("// Автоматично згенеровано bmp2c.py — не редагувати вручну\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"const uint8_t {symbol}_data[] = {{\n")

        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            line = ", ".join(f"0x{b:02X}" for b in chunk)
            f.write(f"    {line},\n")

        f.write("};\n\n")
        f.write(f"const unsigned int {symbol}_size = {len(data)};\n")

    header_path = out_path.rsplit(".", 1)[0] + ".h"
    guard = symbol.upper() + "_IMG_H"

    with open(header_path, "w") as f:
        f.write("// Автоматично згенеровано bmp2c.py — не редагувати вручну\n")
        f.write(f"#ifndef {guard}\n#define {guard}\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"extern const uint8_t {symbol}_data[];\n")
        f.write(f"extern const unsigned int {symbol}_size;\n\n")
        f.write("#endif\n")

    print(f"OK: {in_path} ({len(data)} байт) -> {out_path} + {header_path}, символ '{symbol}_data'")


if __name__ == "__main__":
    main()