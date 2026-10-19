import sys

def load(path):
    d = open(path, 'rb').read()
    if d[0] != 0x36 or d[1] != 0x04:
        sys.exit("not a PSF1 font")
    mode, cs = d[2], d[3]
    n = 512 if mode & 1 else 256
    glyphs = [d[4 + i * cs: 4 + (i + 1) * cs] for i in range(n)]
    uni = {}
    if mode & 2:
        t = d[4 + n * cs:]
        i = 0
        for g in range(n):
            while True:
                v = int.from_bytes(t[i:i + 2], 'little'); i += 2
                if v == 0xFFFF:
                    break
                if v == 0xFFFE:      # послідовності нам не потрібні
                    continue
                uni.setdefault(v, g)
    return glyphs, uni, cs

def main():
    glyphs, uni, cs = load(sys.argv[1])
    rows = []
    for b in range(256):
        if b < 0x80:
            g = glyphs[b]
        else:
            try:
                u = ord(bytes([b]).decode('cp1251'))
                gi = uni.get(u)
            except UnicodeDecodeError:       # 0x98 в CP1251 не визначено
                gi = None
            if gi is None:
                gi = uni[ord('?')]
            g = glyphs[gi]
        rows.append(g)

    out = []
    out.append('#include "fonts.h"\n')
    out.append('/* Згенеровано kernel/tools/psf2font_cp1251.py з Uni2-Fixed18.psf.\n'
               ' * 0x00..0x7F — ASCII, 0x80..0xFF — CP1251 (кирилиця: А-Я 0xC0.., а-я 0xE0.., Ё A8, ё B8,\n'
               ' * Є AA, є BA, І B2, і B3, Ї AF, ї BF, Ґ A5, ґ B4). */')
    out.append('const uint8_t font8x%d[256][%d] = {' % (cs, cs))
    for b, g in enumerate(rows):
        out.append('    {' + ', '.join('0x%02X' % x for x in g) + '}' + (',' if b != 255 else ''))
    out.append('};')
    print('\n'.join(out))

main()
