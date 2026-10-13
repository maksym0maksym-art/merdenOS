/* exFAT: суміжні файли/каталоги (NoFatChain), створені іншими ОС. Образ — mkexfat.py --prefill */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "exfat_fs.h"
static int fd; static uint32_t bsz;
static int bd_read(void *c, uint32_t lba, uint32_t n, void *b) { (void)c; return pread(fd, b, (size_t)n * bsz, (off_t)lba * bsz) == (ssize_t)((size_t)n * bsz) ? 0 : -1; }
static int bd_write(void *c, uint32_t lba, uint32_t n, const void *b) { (void)c; return pwrite(fd, b, (size_t)n * bsz, (off_t)lba * bsz) == (ssize_t)((size_t)n * bsz) ? 0 : -1; }
static int bd_flush(void *c) { (void)c; return fsync(fd); }
static xf_vol_t vol; static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void pat(uint8_t *b, uint32_t n, uint32_t k) { for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)(i * k + 3); }
int main(int argc, char **argv) {
    fd = open(argv[1], O_RDWR); bsz = atoi(argv[2]);
    struct stat st; fstat(fd, &st);
    blockdev_t bd = { 0, bsz, (uint32_t)(st.st_size / bsz), bd_read, bd_write, bd_flush, 0 };
    int rc = xf_mount(&vol, &bd, -1, 0); CHECK(rc == 0, "mount %d", rc);
    uint32_t cb = vol.cluster_bytes, size = 3 * cb + 123;
    uint8_t *buf = malloc(size + 100000), *exp = malloc(size + 100000);
    uint32_t len;
    rc = xf_read_file(&vol, "/pre.bin", buf, size + 10, &len); pat(exp, size, 7);
    CHECK(rc == 0 && len == size && !memcmp(buf, exp, size), "read PRE.BIN rc=%d len=%u", rc, len);
    xf_file_t f; rc = xf_open(&vol, "/nfdir/inner.bin", 0, &f); CHECK(rc == 0, "open inner");
    uint32_t isz = 2 * cb + 5; pat(exp, isz, 5);
    CHECK(xf_fread(&f, 0, buf, isz) == (int)isz && !memcmp(buf, exp, isz), "inner content");
    /* дописати в суміжний файл -> перетворення на ланцюжок */
    uint8_t tail[5000]; pat(tail, sizeof tail, 11);
    rc = xf_append_file(&vol, "/PRE.BIN", tail, sizeof tail); CHECK(rc == 0, "append %d", rc);
    pat(exp, size, 7); memcpy(exp + size, tail, sizeof tail);
    rc = xf_read_file(&vol, "/PRE.BIN", buf, size + 100000, &len);
    CHECK(rc == 0 && len == size + sizeof tail && !memcmp(buf, exp, len), "after append rc=%d len=%u", rc, len);
    /* багато файлів у NoFatChain-каталозі -> ріст */
    for (int i = 0; i < 300; i++) {
        char p[64]; snprintf(p, sizeof p, "/NFDIR/file_number_%03d.txt", i);
        rc = xf_write_file(&vol, p, p, strlen(p)); CHECK(rc == 0, "write %s: %d", p, rc);
    }
    rc = xf_read_file(&vol, "/nfdir/file_number_299.txt", buf, 100, &len);
    CHECK(rc == 0 && len == 26 && !memcmp(buf, "/NFDIR/file_number_299.txt", 26), "readback");
    /* перезапис суміжного файлу меншим вмістом */
    rc = xf_write_file(&vol, "/nfdir/inner.bin", "xyz", 3); CHECK(rc == 0, "rewrite inner %d", rc);
    rc = xf_read_file(&vol, "/nfdir/inner.bin", buf, 100, &len); CHECK(rc == 0 && len == 3 && !memcmp(buf, "xyz", 3), "inner small");
    /* усічення суміжного файлу */
    rc = xf_open(&vol, "/PRE.BIN", 0, &f); CHECK(rc == 0, "open");
    CHECK(xf_ftruncate(&f, cb + 1) == 0, "trunc"); CHECK(xf_fclose(&f) == 0, "close");
    rc = xf_rmdir(&vol, "/NFDIR"); CHECK(rc == FAT_E_NOTEMPTY, "rmdir nonempty %d", rc);
    for (int i = 0; i < 300; i++) { char p[64]; snprintf(p, sizeof p, "/NFDIR/file_number_%03d.txt", i); rc = xf_unlink(&vol, p); CHECK(rc == 0, "unlink %s %d", p, rc); }
    CHECK(xf_unlink(&vol, "/nfdir/inner.bin") == 0, "unlink inner");
    CHECK(xf_rmdir(&vol, "/nfdir") == 0, "rmdir");
    CHECK(xf_unlink(&vol, "/pre.bin") == 0, "unlink pre");
    uint32_t t, fr, c; xf_statfs(&vol, &t, &fr, &c); xf_flush(&vol);
    printf("nofat test: free %u/%u, %d failures\n", fr, t, fails);
    free(buf); free(exp);
    return fails != 0;
}
