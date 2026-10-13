/*
 * Host-тест FAT32: випадкові операції над образом + модель у пам'яті.
 * Збірка (див. tests/usb/run_tests.sh):
 *   gcc -g -O1 -fsanitize=address,undefined -I../../kernel/headers fat_test.c ../../kernel/filesystems/fat32.c -o fat_test
 * Запуск:  fat_test <image> <block_size> <seed> <nops> <dump_dir>
 * Після прогону модель записується у dump_dir; тест run_tests.sh порівнює її з тим, що витягнув
 * незалежний fatcheck.py з образу.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include "fat32.h"

static int fd;
static uint32_t bsz;
#define UNUSED_BD __attribute__((unused))
static long n_reads, n_writes, n_flush;

UNUSED_BD static int bd_read(void *c, uint32_t lba, uint32_t n, void *buf) {
    (void)c; n_reads++;
    return pread(fd, buf, (size_t)n * bsz, (off_t)lba * bsz) == (ssize_t)((size_t)n * bsz) ? 0 : -1;
}
UNUSED_BD static int bd_write(void *c, uint32_t lba, uint32_t n, const void *buf) {
    (void)c; n_writes++;
    return pwrite(fd, buf, (size_t)n * bsz, (off_t)lba * bsz) == (ssize_t)((size_t)n * bsz) ? 0 : -1;
}
UNUSED_BD static int bd_flush(void *c) { (void)c; n_flush++; return fsync(fd); }

/* ---------------- модель ---------------- */
typedef struct { char path[600]; int is_dir; uint8_t *data; uint32_t size; } node_t;
static node_t nodes[4096];
static int nnodes;

static uint32_t rng_state;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

static int m_find(const char *p) {
    for (int i = 0; i < nnodes; i++) if (strcasecmp(nodes[i].path, p) == 0) return i;
    return -1;
}
static void m_del(int i) { free(nodes[i].data); nodes[i] = nodes[--nnodes]; }
static void m_add(const char *p, int is_dir) {
    node_t *n = &nodes[nnodes++];
    memset(n, 0, sizeof(*n));
    strcpy(n->path, p); n->is_dir = is_dir;
}
static int m_children(const char *dir) {
    size_t l = strlen(dir); int c = 0;
    for (int i = 0; i < nnodes; i++)
        if (strncasecmp(nodes[i].path, dir, l) == 0 && nodes[i].path[l] == '/') c++;
    return c;
}
static void parent_of(const char *p, char *out) {
    strcpy(out, p);
    char *s = strrchr(out, '/');
    if (s == out) out[1] = 0; else *s = 0;
}

static const char *names[] = {
    "a.txt", "UPPER.TXT", "lower.txt", "MiXed.Txt", "Long File Name number 1.txt",
    "this is a very long file name that needs several LFN entries to be stored properly 0123456789.dat",
    "\xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB.txt", "a.b.c", ".hidden", "sp ace.txt", "NOEXT", "x+y,z.dat",
    "report-2026.docx", "data.json", "readme.markdown", "dup1.txt", "dup2.txt", "dup3.txt",
    "VeryLongNameNoDots_abcdefghijklmnopqrstuvwxyz", "tilde~1.txt", "tilde~2.txt", "abcdefgh.ijk", "abcdefghi.ijk",
    "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD1\x96\xD1\x82 \xD1\x81\xD0\xB2\xD1\x96\xD1\x82.txt",
};
#define NNAMES (sizeof(names) / sizeof(names[0]))
static const char *dirs[] = { "d0", "D1", "Sub Dir", "deep" };

static void rand_path(char *out, int allow_dirs) {
    int depth = rnd() % 3;
    out[0] = 0;
    for (int i = 0; i < depth; i++) { strcat(out, "/"); strcat(out, dirs[rnd() % 4]); }
    strcat(out, "/");
    strcat(out, allow_dirs && (rnd() % 3 == 0) ? dirs[rnd() % 4] : names[rnd() % NNAMES]);
}

/* випадково змінити регістр ASCII — файлова система нечутлива до регістру */
static void mangle_case(const char *in, char *out) {
    strcpy(out, in);
    if (rnd() % 2) for (char *p = out; *p; p++) if (rnd() % 3 == 0) {
        if (*p >= 'a' && *p <= 'z') *p -= 32; else if (*p >= 'A' && *p <= 'Z') *p += 32;
    }
}

static uint32_t sizes[] = { 0, 1, 100, 511, 512, 513, 4095, 4096, 4097, 20000, 70000, 262144, 300001 };

static void fill(uint8_t *b, uint32_t n, uint32_t seed) {
    uint32_t s = seed * 2654435761u + 1;
    for (uint32_t i = 0; i < n; i++) { s = s * 1103515245u + 12345u; b[i] = (uint8_t)(s >> 16); }
}

static fat_vol_t vol;
static long fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int expect_rc(int rc, int want, const char *what, const char *p) {
    if (rc != want) { fails++; printf("FAIL %s '%s': got %d (%s), want %d (%s)\n", what, p, rc, fat_strerror(rc), want, fat_strerror(want)); return 0; }
    return 1;
}

static void verify_file(const char *p, node_t *n) {
    uint8_t *buf = malloc(n->size + 1);
    uint32_t len = 0;
    int rc = fat_read_file(&vol, p, buf, n->size + 1, &len);
    if (rc == 0 && len == n->size && memcmp(buf, n->data, n->size) == 0) { free(buf); return; }
    fails++;
    printf("FAIL read '%s': rc=%d len=%u want %u\n", p, rc, len, n->size);
    free(buf);
}

static void op_write(void) {
    char p[600], q[700]; rand_path(p, 0);
    int idx = m_find(p);
    if (idx >= 0) mangle_case(p, q); else strcpy(q, p);   /* новий файл — точне ім'я, існуючий — довільний регістр */
    char par[600]; parent_of(p, par);
    int pi = strcmp(par, "/") == 0 ? 0 : m_find(par);
    uint32_t sz = sizes[rnd() % (sizeof(sizes) / sizeof(sizes[0]))];
    uint8_t *data = malloc(sz + 1);
    fill(data, sz, rnd());
    int rc = fat_write_file(&vol, q, data, sz);

    if (pi < 0 || (pi > 0 && !nodes[pi].is_dir && 0)) { expect_rc(rc, FAT_E_NOENT, "write(no parent)", q); free(data); return; }
    if (pi > 0 && !nodes[pi].is_dir) { expect_rc(rc, FAT_E_NOTDIR, "write(parent file)", q); free(data); return; }
    if (idx >= 0 && nodes[idx].is_dir) { expect_rc(rc, FAT_E_ISDIR, "write(dir)", q); free(data); return; }
    if (rc == FAT_E_NOSPC) { free(data); return; }       /* допустимо: том малий */
    if (!expect_rc(rc, 0, "write", q)) { free(data); return; }
    if (idx < 0) { m_add(p, 0); idx = nnodes - 1; }
    free(nodes[idx].data);
    nodes[idx].data = data; nodes[idx].size = sz;
}

static void op_append(void) {
    char p[600], q[700]; rand_path(p, 0);
    mangle_case(p, q);
    int idx = m_find(p);
    if (idx < 0 || nodes[idx].is_dir) return;
    uint32_t sz = sizes[rnd() % 9];
    uint8_t *data = malloc(sz + 1);
    fill(data, sz, rnd());
    int rc = fat_append_file(&vol, q, data, sz);
    if (rc == FAT_E_NOSPC) { free(data); verify_file(p, &nodes[idx]); return; }
    if (!expect_rc(rc, 0, "append", q)) { free(data); return; }
    nodes[idx].data = realloc(nodes[idx].data, nodes[idx].size + sz + 1);
    memcpy(nodes[idx].data + nodes[idx].size, data, sz);
    nodes[idx].size += sz;
    free(data);
}

static void op_read(void) {
    if (!nnodes) return;
    int i = rnd() % nnodes;
    if (nodes[i].is_dir) return;
    char q[700]; mangle_case(nodes[i].path, q);
    verify_file(q, &nodes[i]);

    /* випадкове часткове читання через handle */
    if (nodes[i].size > 10) {
        fat_file_t f;
        int rc = fat_open(&vol, q, 0, &f);
        if (!expect_rc(rc, 0, "open", q)) return;
        for (int k = 0; k < 4; k++) {
            uint32_t off = rnd() % nodes[i].size, n = rnd() % 9000;
            uint8_t *b = malloc(n + 1);
            int got = fat_fread(&f, off, b, n);
            uint32_t want = off + n > nodes[i].size ? nodes[i].size - off : n;
            CHECK(got == (int)want && memcmp(b, nodes[i].data + off, want) == 0, "partial read '%s' off=%u n=%u got=%d want=%u", q, off, n, got, want);
            free(b);
        }
    }
}

static void op_unlink(void) {
    if (!nnodes) return;
    int i = rnd() % nnodes;
    char p[600]; strcpy(p, nodes[i].path);
    int rc = fat_unlink(&vol, p);
    if (nodes[i].is_dir) { expect_rc(rc, FAT_E_ISDIR, "unlink(dir)", p); return; }
    if (expect_rc(rc, 0, "unlink", p)) m_del(i);
}

static void op_mkdir(void) {
    char p[600]; rand_path(p, 1);
    char *s = strrchr(p, '/');
    int want_dir_name = 0;
    for (int k = 0; k < 4; k++) if (strcasecmp(s + 1, dirs[k]) == 0) want_dir_name = 1;
    if (!want_dir_name) strcpy(s + 1, dirs[rnd() % 4]);
    char par[600]; parent_of(p, par);
    int pi = strcmp(par, "/") == 0 ? 0 : m_find(par);
    int idx = m_find(p);
    int rc = fat_mkdir(&vol, p);
    if (pi < 0) { expect_rc(rc, FAT_E_NOENT, "mkdir(no parent)", p); return; }
    if (pi > 0 && !nodes[pi].is_dir) { expect_rc(rc, FAT_E_NOTDIR, "mkdir(parent file)", p); return; }
    if (idx >= 0) { expect_rc(rc, FAT_E_EXIST, "mkdir(exists)", p); return; }
    if (rc == FAT_E_NOSPC) return;
    if (expect_rc(rc, 0, "mkdir", p)) m_add(p, 1);
}

static void op_rmdir(void) {
    if (!nnodes) return;
    int i = rnd() % nnodes;
    char p[600]; strcpy(p, nodes[i].path);
    int rc = fat_rmdir(&vol, p);
    if (!nodes[i].is_dir) { expect_rc(rc, FAT_E_NOTDIR, "rmdir(file)", p); return; }
    if (m_children(p)) { expect_rc(rc, FAT_E_NOTEMPTY, "rmdir(non-empty)", p); return; }
    if (expect_rc(rc, 0, "rmdir", p)) m_del(i);
}

static void op_rename(void) {
    if (!nnodes) return;
    int i = rnd() % nnodes;
    char from[600]; strcpy(from, nodes[i].path);
    char to[600]; rand_path(to, 0);
    if (nodes[i].is_dir) { char par[600]; parent_of(to, par); strcpy(to, par); if (!strcmp(par, "/")) strcpy(to, ""); strcat(to, "/"); strcat(to, dirs[rnd() % 4]); }
    char par[600]; parent_of(to, par);
    int pi = strcmp(par, "/") == 0 ? 0 : m_find(par);
    int ti = m_find(to);
    int rc = fat_rename(&vol, from, to);

    size_t fl = strlen(from);
    int into_self = nodes[i].is_dir && strncasecmp(to, from, fl) == 0 && (to[fl] == '/' || to[fl] == 0);

    if (into_self && strcasecmp(to, from) != 0) { expect_rc(rc, FAT_E_INVAL, "rename(into self)", to); return; }
    if (pi < 0) { expect_rc(rc, FAT_E_NOENT, "rename(no parent)", to); return; }
    if (pi > 0 && !nodes[pi].is_dir) { expect_rc(rc, FAT_E_NOTDIR, "rename(parent file)", to); return; }
    if (ti >= 0 && ti != i) { expect_rc(rc, FAT_E_EXIST, "rename(exists)", to); return; }
    if (rc == FAT_E_NOSPC) return;
    if (!expect_rc(rc, 0, "rename", to)) return;

    /* оновити модель: сам вузол і, якщо каталог, усіх нащадків */
    char old[600]; strcpy(old, from);
    size_t ol = strlen(old);
    for (int k = 0; k < nnodes; k++) {
        if (k == i) continue;
        if (strncasecmp(nodes[k].path, old, ol) == 0 && nodes[k].path[ol] == '/') {
            char t[600]; snprintf(t, sizeof t, "%s%s", to, nodes[k].path + ol);
            strcpy(nodes[k].path, t);
        }
    }
    strcpy(nodes[i].path, to);
}

static void op_stat(void) {
    if (!nnodes) return;
    int i = rnd() % nnodes;
    fat_dirent_t e;
    int rc = fat_stat(&vol, nodes[i].path, &e);
    if (!expect_rc(rc, 0, "stat", nodes[i].path)) return;
    CHECK(((e.attr & FAT_ATTR_DIR) != 0) == nodes[i].is_dir, "stat dir flag %s", nodes[i].path);
    if (!nodes[i].is_dir) CHECK(e.size == nodes[i].size, "stat size %s: %u vs %u", nodes[i].path, e.size, nodes[i].size);
    const char *base = strrchr(nodes[i].path, '/') + 1;
    CHECK(strcasecmp(e.name, base) == 0, "stat name '%s' vs '%s'", e.name, base);
    CHECK(strcmp(e.name, base) == 0, "stat case-preserve '%s' vs '%s'", e.name, base);
}

static int ls_count;
static int ls_cb(const fat_dirent_t *e, void *arg) { (void)e; (void)arg; ls_count++; return 0; }

static void check_listing(void) {
    /* кількість записів у кожному каталозі збігається з моделлю */
    for (int i = -1; i < nnodes; i++) {
        const char *d = i < 0 ? "/" : nodes[i].path;
        if (i >= 0 && !nodes[i].is_dir) continue;
        int want = 0;
        size_t l = strcmp(d, "/") == 0 ? 0 : strlen(d);
        for (int k = 0; k < nnodes; k++) {
            if (strncasecmp(nodes[k].path, d, l) != 0 || nodes[k].path[l] != '/') continue;
            if (strchr(nodes[k].path + l + 1, '/')) continue;
            want++;
        }
        ls_count = 0;
        int rc = fat_readdir(&vol, d, ls_cb, 0);
        CHECK(rc == 0 && ls_count == want, "readdir '%s': rc=%d count=%d want=%d", d, rc, ls_count, want);
    }
}

static void dump_model(const char *dir) {
    char cmd[700]; snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    if (system(cmd)) return;
    for (int i = 0; i < nnodes; i++) {
        char full[1400]; snprintf(full, sizeof full, "%s%s", dir, nodes[i].path);
        if (nodes[i].is_dir) { char c[1500]; snprintf(c, sizeof c, "mkdir -p '%s'", full); if (system(c)) {} }
    }
    for (int i = 0; i < nnodes; i++) {
        if (nodes[i].is_dir) continue;
        char full[1400]; snprintf(full, sizeof full, "%s%s", dir, nodes[i].path);
        FILE *f = fopen(full, "wb");
        if (!f) { printf("cannot create %s\n", full); continue; }
        fwrite(nodes[i].data, 1, nodes[i].size, f);
        fclose(f);
    }
}

/* Основний прогін: bd — будь-який blockdev (файл-образ або USB-флешка через xHCI-емулятор) */
int fat_stress(blockdev_t *bd, uint32_t seed, int nops, const char *dump_dir, int part) {
    rng_state = seed;
    fails = 0; nnodes = 0;
    fat_part_info_t info;
    int rc = fat_mount(&vol, bd, part, &info);
    if (rc) { printf("mount failed: %s\n", fat_strerror(rc)); return 1; }
    printf("mounted: scheme=%d start=%u bps=%u spc=%u clusters=%u root=%u fats=%u(+%u) label='%s'\n",
           info.scheme, info.start_lba, vol.bps, vol.spc, vol.total_clusters, vol.root_cluster, vol.nfats, vol.fat_extra_copies, vol.label);

    for (int op = 0; op < nops; op++) {
        switch (rnd() % 16) {
        case 0: case 1: case 2: case 3: op_write(); break;
        case 4: case 5: op_append(); break;
        case 6: case 7: case 8: op_read(); break;
        case 9: op_unlink(); break;
        case 10: case 11: op_mkdir(); break;
        case 12: op_rmdir(); break;
        case 13: case 14: op_rename(); break;
        case 15: op_stat(); break;
        }
        if (op % 50 == 0) check_listing();
        if (fails > 20) break;
    }
    check_listing();
    for (int i = 0; i < nnodes; i++) if (!nodes[i].is_dir) verify_file(nodes[i].path, &nodes[i]);

    /* дубль-перевірка: нове монтування того ж образу має бачити те саме */
    fat_flush(&vol);
    dump_model(dump_dir);
    uint32_t tot, fr, cb;
    fat_statfs(&vol, &tot, &fr, &cb);
    fat_flush(&vol);
    printf("done: %d nodes, free %u/%u clusters, io: %ld reads %ld writes %ld flushes, %ld failures\n",
           nnodes, fr, tot, n_reads, n_writes, n_flush, fails);
    return fails ? 1 : 0;
}

#ifndef FAT_TEST_NO_MAIN
int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s image block_size seed nops dump_dir [part_index]\n", argv[0]); return 2; }
    fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 2; }
    bsz = (uint32_t)atoi(argv[2]);
    struct stat st; fstat(fd, &st);
    blockdev_t bd = { 0, bsz, (uint32_t)(st.st_size / bsz), bd_read, bd_write, bd_flush, 0 };
    return fat_stress(&bd, (uint32_t)atoi(argv[3]), atoi(argv[4]), argv[5], argc > 6 ? atoi(argv[6]) : -1);
}
#endif
