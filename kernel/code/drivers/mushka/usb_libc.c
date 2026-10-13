/*
 * Мінімальні mem*-функції для ядра. gcc має право сам вставити виклик
 * memcpy/memset (копіювання структур, ініціалізація масивів), а libc у ядра немає.
 * Символи weak: якщо в проєкті з'явиться інша реалізація (наприклад AHCI.c), конфлікту не буде.
 */
#include <stddef.h>

__attribute__((weak)) void *memset(void *dest, int value, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    while (n--) *d++ = (unsigned char)value;
    return dest;
}

__attribute__((weak)) void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dest;
}

__attribute__((weak)) void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) { while (n--) *d++ = *s++; }
    else       { d += n; s += n; while (n--) *--d = *--s; }
    return dest;
}

__attribute__((weak)) int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
