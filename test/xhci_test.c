/* Host-тест драйвера xHCI + USB MSC + FAT32 на емуляторі. Запуск: xhci_test image_template */
#define FAT_TEST_NO_MAIN
#include "fat_test.c"
#include <malloc.h>
#include <sys/wait.h>
#include "emu_xhci.h"
#include "xhci.h"
#include "usb_msc.h"
#include "xhci_hid.h"

static int bad;
#undef CHECK
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

static int copy_file(const char *a, const char *b) {
    char cmd[512]; snprintf(cmd, sizeof cmd, "cp %s %s", a, b); return system(cmd);
}

static int scenario(const char *tmpl, int csz, int speed, int bios, int kbd_first, uint32_t seed, int nops) {
    char img[256]; snprintf(img, sizeof img, "/tmp/merden_xhci_%d_%d_%d.img", csz, speed, bios);
    copy_file(tmpl, img);
    printf("== csz=%d speed=%d bios=%d kbd_first=%d\n", csz, speed, bios, kbd_first);
    long b0 = bad;
    emu_reset();
    emu_power_on(csz, bios);
    if (kbd_first) { emu_add_kbd(1, 3, 8); emu_add_msc(3, speed, img, 512); }
    else           { emu_add_msc(speed >= 4 ? 3 : 1, speed, img, 512); emu_add_kbd(2, 2, 8); }
    usb_msc_forget_all();
    int n = xhci_scan(usb_msc_probe, 0);
    CHECK(n == 1, "xhci_scan returned %d (want 1)", n);
    CHECK(usb_msc_count() == 1, "msc_count=%d", usb_msc_count());
    CHECK(emu.bad_run == 0 && emu.param_errors == 0 && emu.bad_cmds == 0 && emu.bad_trbs == 0 &&
          emu.bad_cbw == 0 && emu.crossed_64k == 0 && emu.writes_while_off == 0 && emu.bad_doorbells == 0,
          "emu errors: run=%ld param=%ld cmds=%ld trbs=%ld cbw=%ld 64k=%ld off=%ld db=%ld",
          emu.bad_run, emu.param_errors, emu.bad_cmds, emu.bad_trbs, emu.bad_cbw, emu.crossed_64k,
          emu.writes_while_off, emu.bad_doorbells);
    if (bios) CHECK(emu.handoffs >= 1, "BIOS handoff not performed");
    if (usb_msc_count() != 1) return 1;
    usb_msc_t *m = usb_msc_get(0);
    CHECK(m->block_size == 512 && m->block_count > 1000, "bsz=%u cnt=%u", m->block_size, m->block_count);
    blockdev_t bd = { m, m->block_size, m->block_count, usb_msc_read, usb_msc_write, usb_msc_sync, 0 };
    char dump[256]; snprintf(dump, sizeof dump, "/tmp/merden_xhci_dump_%d_%d_%d", csz, speed, bios);
    int r = fat_stress(&bd, seed, nops, dump, -1);
    CHECK(r == 0, "fat_stress failed");
    CHECK(emu.bad_cbw == 0 && emu.crossed_64k == 0 && emu.bad_trbs == 0, "post-stress emu errors");
    printf("   cmds=%ld cbws=%ld bulk_tds=%ld ctrl=%ld stalls=%ld clear_halts=%ld bot_resets=%ld\n",
           emu.cmds, emu.cbws, emu.bulk_tds, emu.control_xfers, emu.stalls, emu.clear_halts, emu.bot_resets);
    xhci_shutdown();
    /* незалежна перевірка образу */
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "python3 %s/fatcheck.py %s --quiet --extract %s.ex && diff -r %s %s.ex", getenv("USBTEST_DIR") ? getenv("USBTEST_DIR") : ".", img, dump, dump, dump);
    CHECK(system(cmd) == 0, "fatcheck/diff mismatch");
    return bad != b0;
}

static int fault_tests(const char *tmpl) {
    char img[] = "/tmp/merden_xhci_fault.img";
    long b0 = bad;
    printf("== fault injection\n");
    copy_file(tmpl, img);
    emu_reset(); emu_power_on(64, 0); emu_add_msc(3, 4, img, 512);
    usb_msc_forget_all();
    CHECK(xhci_scan(usb_msc_probe, 0) == 1, "scan");
    usb_msc_t *m = usb_msc_get(0);
    if (!m) return 1;
    static uint8_t buf[8192], buf2[8192];
    int r = usb_msc_read(m, 0, 8, buf);
    CHECK(r == 0, "baseline read %d", r);
    /* STALL на bulk-in */
    emu_msc_inject(3, 1, 0, 0);
    r = usb_msc_read(m, 0, 8, buf2);
    CHECK(r == 0 && !memcmp(buf, buf2, 4096), "read after IN stall: %s", usb_msc_err_str(r));
    /* STALL на bulk-out */
    for (int i = 0; i < 4096; i++) buf2[i] = (uint8_t)(i * 7);
    emu_msc_inject(3, 0, 1, 0);
    r = usb_msc_write(m, 100, 8, buf2);
    CHECK(r == 0, "write after OUT stall: %s", usb_msc_err_str(r));
    memset(buf, 0, sizeof buf);
    r = usb_msc_read(m, 100, 8, buf);
    CHECK(r == 0 && !memcmp(buf, buf2, 4096), "readback after stalled write");
    /* втрачений пакет — таймаут і відновлення */
    emu_msc_inject(3, 0, 0, 1);
    r = usb_msc_read(m, 0, 8, buf);
    printf("   dropped IN → %s\n", usb_msc_err_str(r));
    r = usb_msc_read(m, 0, 8, buf);
    CHECK(r == 0, "read after recovery: %s", usb_msc_err_str(r));
    /* захист від запису */
    emu_msc_set(3, 0, 1, 0);
    r = usb_msc_write(m, 200, 1, buf2);
    CHECK(r != 0, "write on WP media must fail");
    emu_msc_set(3, 0, 0, 0);
    /* відключення */
    printf("   fault counters: stalls=%ld clear_halts=%ld bot_resets=%ld ep_resets=%ld set_deqs=%ld dropped=%ld\n", emu.stalls, emu.clear_halts, emu.bot_resets, emu.ep_resets, emu.set_deqs, emu.dropped);
    CHECK(emu.stalls >= 2 && emu.clear_halts >= 2, "injected stalls were not exercised");
    emu_unplug(3);
    r = usb_msc_read(m, 0, 1, buf);
    CHECK(r != 0, "read after unplug must fail");
    xhci_shutdown();

    /* порожній карт-рідер */
    copy_file(tmpl, img);
    emu_reset(); emu_power_on(32, 0); emu_add_msc(1, 3, img, 512); emu_msc_set(1, 1, 0, 0);
    usb_msc_forget_all();
    int n = xhci_scan(usb_msc_probe, 0);
    printf("   no-medium scan → %d devices, msc=%d\n", n, usb_msc_count());
    xhci_shutdown();

    /* лише клавіатура — пристрій має бути відпущений */
    emu_reset(); emu_power_on(32, 0); emu_add_kbd(2, 3, 8);
    usb_msc_forget_all();
    n = xhci_scan(usb_msc_probe, 0);
    CHECK(n == 0 && usb_msc_count() == 0, "kbd only: n=%d msc=%d", n, usb_msc_count());
    CHECK(emu.slots_disabled >= 1, "slot of non-MSC not disabled");
    xhci_shutdown();
    return bad != b0;
}


static int probe_both(xhci_dev_t *d, void *arg) {
    if (usb_msc_probe(d, arg)) return 1;
    return xhci_hid_probe(d, arg);
}

static int mouse_tests(const char *tmpl) {
    long b0 = bad;
    char img[] = "/tmp/merden_xhci_mouse.img";
    for (int csz = 32; csz <= 64; csz += 32) for (int speed = 1; speed <= 4; speed += (speed == 1 ? 1 : 1)) {
        if (speed == 2) continue;
        printf("== mouse csz=%d speed=%d (+flash)\n", csz, speed);
        copy_file(tmpl, img);
        emu_reset(); emu_power_on(csz, 0);
        emu_add_mouse(speed == 4 ? 3 : 1, speed, speed == 4 ? 9 : 8);
        emu_add_msc(speed == 4 ? 4 : 2, speed == 4 ? 4 : 3, img, 512);
        usb_msc_forget_all(); xhci_hid_forget_all();
        int n = xhci_scan(probe_both, 0);
        CHECK(n == 2, "scan kept %d (want 2)", n);
        CHECK(xhci_mouse_count() == 1 && usb_msc_count() == 1, "mouse=%d msc=%d", xhci_mouse_count(), usb_msc_count());
        CHECK(emu.set_protocol == 1, "SET_PROTOCOL not sent");
        CHECK(emu.param_errors == 0 && emu.bad_trbs == 0 && emu.bad_cmds == 0, "emu errors");
        if (xhci_mouse_count() != 1 || usb_msc_count() != 1) continue;
        int dx, dy, wh; uint8_t bt;
        CHECK(xhci_mouse_read(&dx, &dy, &wh, &bt) == 0, "phantom report");
        int mport = speed == 4 ? 3 : 1;
        emu_mouse_move(mport, 5, -3, 1, 0);
        CHECK(xhci_mouse_read(&dx, &dy, &wh, &bt) == 1 && dx == 5 && dy == -3 && bt == 1 && wh == 0, "report1 dx=%d dy=%d bt=%d", dx, dy, bt);
        emu_mouse_move(mport, -127, 127, 5, -1);
        CHECK(xhci_mouse_read(&dx, &dy, &wh, &bt) == 1 && dx == -127 && dy == 127 && bt == 5 && wh == -1, "report2");
        CHECK(xhci_mouse_read(&dx, &dy, &wh, &bt) == 0, "extra report");
        /* події миші під час bulk-передач флешки не мають губитись */
        usb_msc_t *m = usb_msc_get(0);
        static uint8_t buf[4096];
        int sent = 0, got = 0, sdx = 0, rdx = 0;
        for (int i = 0; i < 200; i++) {
            emu_mouse_move(mport, 1 + i % 7, 0, i & 1, 0); sent++; sdx += 1 + i % 7;
            for (int j = 0; j < 4096; j++) buf[j] = (uint8_t)(i + j);
            CHECK(usb_msc_write(m, 100 + i * 8, 8, buf) == 0, "write %d", i);
            static uint8_t rb[4096];
            CHECK(usb_msc_read(m, 100 + i * 8, 8, rb) == 0 && !memcmp(rb, buf, 4096), "readback %d", i);
            if (i % 3 == 0) emu_mouse_move(mport, 0, 1, 0, 0), sent++;
            while (xhci_mouse_read(&dx, &dy, &wh, &bt)) { got++; rdx += dx; }
        }
        while (xhci_mouse_read(&dx, &dy, &wh, &bt)) { got++; rdx += dx; }
        CHECK(got == sent && rdx == sdx, "mouse events lost during bulk: sent %d got %d (dx %d/%d)", sent, got, sdx, rdx);
        printf("   intr_reports=%ld sent=%d got=%d\n", emu.intr_reports, sent, got);
        /* помилка interrupt-ендпоінта не вбиває мишу */
        xhci_shutdown();
    }
    /* відключення миші не повинно зависати */
    emu_reset(); emu_power_on(32, 0); emu_add_mouse(1, 3, 8);
    xhci_hid_forget_all();
    CHECK(xhci_mouse_init() == 1, "xhci_mouse_init");
    emu_unplug(1);
    int dx, dy, wh; uint8_t bt;
    for (int i = 0; i < 5; i++) xhci_mouse_read(&dx, &dy, &wh, &bt);
    xhci_shutdown();
    return bad != b0;
}

int main(int argc, char **argv) {
    mallopt(M_MMAP_THRESHOLD, 1 << 30); mallopt(M_TOP_PAD, 256 << 20);
    if (argc < 2) { fprintf(stderr, "usage: %s template.img\n", argv[0]); return 2; }
    int nops = argc > 2 ? atoi(argv[2]) : 300;
    int failed = 0;
    failed += scenario(argv[1], 32, 4, 0, 0, 11, nops);
    failed += scenario(argv[1], 64, 4, 1, 1, 12, nops);
    failed += scenario(argv[1], 32, 3, 1, 0, 13, nops);
    failed += scenario(argv[1], 64, 3, 0, 1, 14, nops);
    failed += fault_tests(argv[1]);
    failed += mouse_tests(argv[1]);
    printf(bad ? "\nXHCI TESTS: %d FAILURES\n" : "\nXHCI TESTS: ALL OK\n", bad);
    return bad != 0;
}
