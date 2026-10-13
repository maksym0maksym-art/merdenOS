/* Host-тест склейки usbfs.c: три "флешки" (FAT32, exFAT, порожня), перемикання дисків usb0:/usb1:/ram:,
 * звичайні ls/cd/cat/... на активному диску, копіювання між дисками, eject. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include "usbfs.h"
#include "usb_msc.h"
#include "xhci.h"
#include "xhci_hid.h"
#include "rtc.h"
#include "usb_dbg.h"

int usb_debug;
static char outbuf[1 << 20]; static size_t outn;
void shell_print(char *t) { size_t l = strlen(t); if (outn + l < sizeof outbuf) { memcpy(outbuf + outn, t, l); outn += l; outbuf[outn] = 0; } }

static usb_msc_t msc[3]; static int fds[3]; static int nmsc;
int usb_msc_count(void) { return nmsc; }
usb_msc_t *usb_msc_get(int i) { return i >= 0 && i < nmsc ? &msc[i] : 0; }
void usb_msc_forget_all(void) { nmsc = 0; }
int usb_msc_read(void *c, uint32_t lba, uint32_t n, void *b) { usb_msc_t *m = c; int fd = fds[m - msc]; return pread(fd, b, (size_t)n * 512, (off_t)lba * 512) == (ssize_t)(n * 512) ? 0 : -1; }
int usb_msc_write(void *c, uint32_t lba, uint32_t n, const void *b) { usb_msc_t *m = c; int fd = fds[m - msc]; return pwrite(fd, b, (size_t)n * 512, (off_t)lba * 512) == (ssize_t)(n * 512) ? 0 : -1; }
int usb_msc_sync(void *c) { (void)c; return 0; }
static int ejected[3];
int usb_msc_eject(usb_msc_t *m) { ejected[m - msc] = 1; return 0; }
const char *usb_msc_err_str(int e) { (void)e; return "msc error"; }
int usb_msc_probe(xhci_dev_t *d, void *a) { (void)d; (void)a; return 0; }
int xhci_hid_probe(xhci_dev_t *d, void *a) { (void)d; (void)a; return 0; }
void xhci_hid_forget_all(void) {}
void usb_time_init(void) {}
static int nscans;
int xhci_scan(xhci_probe_fn p, void *a) {
    (void)p; (void)a; nmsc = 3; nscans++;
    for (int i = 0; i < 3; i++) {
        memset(&msc[i], 0, sizeof msc[i]); msc[i].used = 1; msc[i].block_size = 512;
        struct { off_t sz; } s; s.sz = lseek(fds[i], 0, SEEK_END); msc[i].block_count = (uint32_t)(s.sz / 512);
        strcpy(msc[i].vendor, i == 0 ? "FatCo" : i == 1 ? "ExCo" : "Blank"); strcpy(msc[i].product, "Stick"); strcpy(msc[i].rev, "1.0");
    }
    return 3;
}
void xhci_print_info(void) { shell_print("xhci info\n"); }
int xhci_seen_count(void) { return 0; }
const xhci_seen_t *xhci_seen_get(int i) { (void)i; return 0; }
const char *xhci_speed_str(int s) { (void)s; return "?"; }
RTC_TIME rtc_get_time(void) { RTC_TIME t = { 10, 20, 12, 5, 10, 26 }; return t; }
uint8_t rtc_read(uint8_t r) { (void)r; return 0; }

static int fails;
static void cmd(const char *c) { outn = 0; outbuf[0] = 0; usbfs_command(c); }
static int handled(const char *c) { outn = 0; outbuf[0] = 0; return usbfs_command(c); }
#define EXPECT(sub) do { if (!strstr(outbuf, sub)) { fails++; printf("FAIL line %d: output lacks '%s'\n--- output ---\n%s--------------\n", __LINE__, sub, outbuf); } } while (0)
#define NOTEXPECT(sub) do { if (strstr(outbuf, sub)) { fails++; printf("FAIL line %d: output has '%s'\n--- output ---\n%s--------------\n", __LINE__, sub, outbuf); } } while (0)
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(int argc, char **argv) {
    (void)argc;
    for (int i = 0; i < 3; i++) { fds[i] = open(argv[1 + i], O_RDWR); if (fds[i] < 0) { perror("open"); return 2; } }

    /* старт системи: mouse_init вже взяв флешки одним скан-проходом; autostart тільки монтує, без повторного скану */
    xhci_scan(0, 0); nscans = 0;
    outn = 0; outbuf[0] = 0; usbfs_autostart();
    EXPECT("Mounted usb0:"); EXPECT("Mounted usb1:"); CHECK(usbfs_active()); CHECK(nscans == 0);
    cmd("usb1:"); cmd("usb0:"); cmd("usb info"); CHECK(nscans == 0);
    cmd("ram:");
    CHECK(!handled("ls"));                                 /* системний диск: не наше */
    CHECK(strcmp(usbfs_prompt(), "") == 0);
    cmd("usb scan"); CHECK(nscans == 1);
    EXPECT("Mounted usb0:"); EXPECT("FAT32"); EXPECT("Mounted usb1:"); EXPECT("exFAT");
    EXPECT("no FAT32/exFAT file system");                  /* порожня флешка */
    EXPECT("Current drive: usb0:");
    CHECK(usbfs_active()); CHECK(strcmp(usbfs_prompt(), "usb0:/ ") == 0);

    /* звичайні команди без префікса "usb" */
    cmd("mkdir docs"); EXPECT("ok");
    cmd("cd docs"); CHECK(strcmp(usbfs_prompt(), "usb0:/docs ") == 0);
    cmd("write hello.txt Hello from FAT\\nsecond line"); EXPECT("ok");
    cmd("cat hello.txt"); EXPECT("Hello from FAT"); EXPECT("second line");
    cmd("ls"); EXPECT("hello.txt"); EXPECT("1 file(s)");
    cmd("pwd"); EXPECT("usb0:/docs");

    /* перехід на exFAT-диск */
    cmd("usb1:"); EXPECT("Current drive: usb1:"); CHECK(strcmp(usbfs_prompt(), "usb1:/ ") == 0);
    cmd("ls"); EXPECT("0 file(s)");
    cmd("mkdir Папка"); EXPECT("ok");
    cmd("cd Папка"); cmd("write файл.txt exFAT hello\\nline2"); EXPECT("ok");
    cmd("cat ФАЙЛ.TXT"); EXPECT("exFAT hello"); EXPECT("line2");
    cmd("touch a.bin"); EXPECT("ok");
    cmd("touch a.bin"); EXPECT("ok");
    cmd("create a.bin"); EXPECT("already exists");

    /* копіювання між дисками, шляхи з префіксом */
    cmd("cp usb0:/docs/hello.txt usb1:/Папка/copy.txt"); EXPECT("ok");
    cmd("cat copy.txt"); EXPECT("Hello from FAT");
    cmd("cp copy.txt usb0:/docs/back.txt"); EXPECT("ok");
    cmd("cat usb0:/docs/back.txt"); EXPECT("Hello from FAT");
    cmd("ls usb0:/docs"); EXPECT("hello.txt"); EXPECT("back.txt"); EXPECT("2 file(s)");
    cmd("mv usb0:/docs/back.txt usb1:/x.txt"); EXPECT("cannot move between drives");
    cmd("mv copy.txt renamed.txt"); EXPECT("ok");
    cmd("ls"); EXPECT("renamed.txt"); NOTEXPECT("copy.txt");
    cmd("rm renamed.txt"); EXPECT("ok");
    cmd("cd .."); CHECK(strcmp(usbfs_prompt(), "usb1:/ ") == 0);
    cmd("rmdir Папка"); EXPECT("not empty");

    /* usb-префікс теж працює, довгі шляхи в промпті скорочуються */
    cmd("usb ls"); EXPECT("usb1:");
    cmd("usb0:"); cmd("mkdir averyveryverylongdirectoryname1"); cmd("cd averyveryverylongdirectoryname1");
    cmd("mkdir averyveryverylongdirectoryname2"); cmd("cd averyveryverylongdirectoryname2");
    CHECK(strlen(usbfs_prompt()) < 40); CHECK(strstr(usbfs_prompt(), "...") != 0);
    cmd("cd /"); 

    /* редактор */
    char eb[300];
    CHECK(strcmp(usbfs_edit_path("notes.txt", eb, sizeof eb), "usb:notes.txt") == 0);
    CHECK(strcmp(usbfs_edit_path("usbnotes", eb, sizeof eb), "usb:usbnotes") == 0);
    CHECK(strcmp(usbfs_edit_path("usb1:/x", eb, sizeof eb), "usb1:/x") == 0);
    CHECK(strcmp(usbfs_edit_path("ram:rf", eb, sizeof eb), "rf") == 0);
    char buf[100]; uint32_t len = 0;
    CHECK(usbfs_write("usb1:/ed.txt", "edited", 6) == 0);
    CHECK(usbfs_read("usb1:/ed.txt", buf, 100, &len) == 0 && len == 6 && !memcmp(buf, "edited", 6));
    CHECK(usbfs_write("usb:ed0.txt", "zero", 4) == 0);                   /* поточний usb0 */
    cmd("cat ed0.txt"); EXPECT("zero");

    /* info */
    cmd("usb info"); EXPECT("* usb0:"); EXPECT("exFAT"); EXPECT("not mounted");

    /* назад на системний диск */
    cmd("ram:"); EXPECT("ram:"); CHECK(!usbfs_active()); CHECK(!handled("ls")); CHECK(strcmp(usbfs_prompt(), "") == 0);
    /* usb: повертає */
    cmd("usb1:"); CHECK(usbfs_active());
    cmd("usb eject 1"); EXPECT("usb1: ejected"); CHECK(ejected[1]); CHECK(!usbfs_active());
    cmd("usb0:"); EXPECT("Current drive: usb0:");
    cmd("usb1:"); /* ejected: перемонтується за запитом (емулятор) */
    cmd("usb eject"); EXPECT("ejected");

    printf("usbfs test: %d failure(s)\n", fails);
    return fails != 0;
}
