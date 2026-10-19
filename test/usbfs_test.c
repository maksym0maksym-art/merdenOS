/* Host-тест склейки usbfs.c: три "флешки" (FAT32, exFAT, порожня), перемикання дисків usb0:/usb1:/ram:,
 * звичайні ls/cd/cat/... на активному диску, копіювання між дисками, eject.
 * Плюс IDE: hd0 (FAT32) монтується при старті сам, hd1 (порожній) — ні, але підхоплюється після "форматування"
 * (usbfs_rescan), hd2 — CD (ATAPI), його пропускаємо. Аргументи: u0 u1 u2 ide0 ide1_blank ide1_formatted. */
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
#include "IDE_drive.h"

int usb_debug;
static char outbuf[1 << 20]; static size_t outn;
int klog_booting(void) { return 0; }
void klog_raw(const char *t) { (void)t; }
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

/* ---- заглушка IDE: два ATA-диски на файлах-образах і один ATAPI ---- */
struct ide_devices_s ide_devices[IDE_MAX_DRIVES];
static int ide_fd[2] = { -1, -1 };
static blockdev_t ibd[2];
static int ide_idx(void *c) { return (int)((struct ide_devices_s *)c - ide_devices); }
static int ide_rd(void *c, uint32_t lba, uint32_t n, void *b) { return pread(ide_fd[ide_idx(c)], b, (size_t)n * 512, (off_t)lba * 512) == (ssize_t)(n * 512) ? 0 : -1; }
static int ide_wr(void *c, uint32_t lba, uint32_t n, const void *b) { return pwrite(ide_fd[ide_idx(c)], b, (size_t)n * 512, (off_t)lba * 512) == (ssize_t)(n * 512) ? 0 : -1; }
static int ide_fl(void *c) { (void)c; return 0; }
blockdev_t *ide_blockdev(int n) {
    if (n < 0 || n > 1 || !ide_devices[n].Reserved || ide_devices[n].Type != IDE_ATA) return 0;
    ibd[n].ctx = &ide_devices[n]; ibd[n].block_size = 512;
    ibd[n].block_count = (uint32_t)(lseek(ide_fd[n], 0, SEEK_END) / 512);
    ibd[n].read = ide_rd; ibd[n].write = ide_wr; ibd[n].flush = ide_fl; ibd[n].read_only = 0;
    return &ibd[n];
}

static int fails;
static void cmd(const char *c) { outn = 0; outbuf[0] = 0; usbfs_command(c); }
static int handled(const char *c) { outn = 0; outbuf[0] = 0; return usbfs_command(c); }
#define EXPECT(sub) do { if (!strstr(outbuf, sub)) { fails++; printf("FAIL line %d: output lacks '%s'\n--- output ---\n%s--------------\n", __LINE__, sub, outbuf); } } while (0)
#define NOTEXPECT(sub) do { if (strstr(outbuf, sub)) { fails++; printf("FAIL line %d: output has '%s'\n--- output ---\n%s--------------\n", __LINE__, sub, outbuf); } } while (0)
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(int argc, char **argv) {
    (void)argc;
    for (int i = 0; i < 3; i++) { fds[i] = open(argv[1 + i], O_RDWR); if (fds[i] < 0) { perror("open"); return 2; } }
    if (argc < 7) { fprintf(stderr, "usage: usbfs_test u0 u1 u2 ide0 ide1_blank ide1_formatted\n"); return 2; }
    ide_fd[0] = open(argv[4], O_RDWR); ide_fd[1] = open(argv[5], O_RDWR);
    int ide1_fmt = open(argv[6], O_RDWR);
    if (ide_fd[0] < 0 || ide_fd[1] < 0 || ide1_fmt < 0) { perror("open ide"); return 2; }
    for (int i = 0; i < 2; i++) { ide_devices[i].Reserved = 1; ide_devices[i].Type = IDE_ATA; ide_devices[i].channel = (unsigned char)i; strcpy((char *)ide_devices[i].Model, i ? "TESTDISK1" : "TESTDISK0"); }
    ide_devices[2].Reserved = 1; ide_devices[2].Type = IDE_ATAPI; strcpy((char *)ide_devices[2].Model, "TESTCD");

    /* старт системи: mouse_init вже взяв флешки одним скан-проходом; autostart тільки монтує, без повторного скану */
    xhci_scan(0, 0); nscans = 0;
    outn = 0; outbuf[0] = 0; usbfs_autostart();
    EXPECT("Mounted usb0:"); EXPECT("Mounted usb1:"); CHECK(usbfs_active()); CHECK(nscans == 0);
    EXPECT("Mounted hd0: TESTDISK0  FAT32");                              /* IDE: автомонтування при старті */
    EXPECT("hd1: (TESTDISK1): no FAT32/exFAT file system");               /* порожній диск — просто повідомлення */
    NOTEXPECT("TESTCD");                                                  /* CD (ATAPI) мовчки пропущено */
    CHECK(strcmp(usbfs_prompt(), "usb0:/ ") == 0);                        /* IDE не перехоплює поточний диск */
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


    /* ================= IDE: hd0: працює як флешка ================= */
    cmd("hd0:"); EXPECT("Current drive: hd0:"); NOTEXPECT("Mounted hd0:"); CHECK(strcmp(usbfs_prompt(), "hd0:/ ") == 0);
    cmd("mkdir ide"); EXPECT("ok"); cmd("cd ide"); CHECK(strcmp(usbfs_prompt(), "hd0:/ide ") == 0);
    cmd("write note.txt on ide disk\\nline2"); EXPECT("ok");
    cmd("cat note.txt"); EXPECT("on ide disk"); EXPECT("line2");
    cmd("pwd"); EXPECT("hd0:/ide");
    cmd("ls"); EXPECT("note.txt"); EXPECT("hd0:");
    cmd("usb1:"); cmd("hd0:"); cmd("cd ide");                                  /* usb1 з попередніх тестів витягнута — монтуємо знову */
    cmd("cp hd0:/ide/note.txt usb1:/fromide.txt"); EXPECT("ok");              /* IDE -> флешка (exFAT) */
    cmd("cat usb1:/fromide.txt"); EXPECT("on ide disk");
    cmd("cp usb0:/docs/hello.txt hd0:/ide/fromusb.txt"); EXPECT("ok");        /* флешка -> IDE */
    cmd("cat fromusb.txt"); EXPECT("Hello from FAT");
    cmd("mv fromusb.txt moved.txt"); EXPECT("ok"); cmd("rm moved.txt"); EXPECT("ok");
    cmd("hd:"); EXPECT("Current drive: hd0:");                                /* \"hd:\" = поточний / hd0: */

    /* `usb scan` скидає USB-контролер, але IDE-диск не відмонтовує */
    int sc = nscans;
    cmd("usb scan"); CHECK(nscans == sc + 1); EXPECT("Mounted usb0:"); NOTEXPECT("Mounted hd0:");
    cmd("cat hd0:/ide/note.txt"); EXPECT("on ide disk");
    cmd("hd0:"); EXPECT("Current drive: hd0:"); NOTEXPECT("Mounted hd0:");

    /* API для GUI-провідника */
    int found_ide = 0, found_usb = 0;
    for (int i = 0; i < usbfs_drive_count(); i++) { int d = usbfs_drive_at(i, 0, 0); if (USBFS_IS_IDE(d)) found_ide++; else found_usb++; }
    CHECK(found_ide == 1 && found_usb == 2);
    char dn[16];
    CHECK(usbfs_drive_name(USBFS_IDE_BASE, dn, sizeof dn) == 3 && !strcmp(dn, "hd0"));
    CHECK(usbfs_drive_name(USBFS_IDE_BASE + 12, dn, sizeof dn) == 4 && !strcmp(dn, "hd12"));
    CHECK(usbfs_drive_name(2, dn, sizeof dn) == 4 && !strcmp(dn, "usb2"));
    CHECK(usbfs_drive_name(USBFS_IDE_BASE, dn, 3) == 2 && !strcmp(dn, "hd"));     /* обрізання без переповнення */
    uint32_t fkb = 0, tkb = 0;
    CHECK(usbfs_drive_space(USBFS_IDE_BASE, &fkb, &tkb) == 0 && tkb > 0 && fkb <= tkb);
    static usbfs_entry_t ents[16]; int cnt = 0;
    CHECK(usbfs_list(USBFS_IDE_BASE, "/ide", ents, 16, &cnt) == 0 && cnt == 1 && !strcmp(ents[0].name, "note.txt"));
    CHECK(usbfs_make(USBFS_IDE_BASE, "/ide/sub", 1) == 0);
    CHECK(usbfs_rename(USBFS_IDE_BASE, "/ide/sub", "/ide/sub2") == 0);
    CHECK(usbfs_remove(USBFS_IDE_BASE, "/ide/sub2") == 0);
    CHECK(usbfs_list(USBFS_IDE_BASE + 1, "/", ents, 16, &cnt) != 0);              /* hd1 ще не змонтований */

    /* редактор: hd0:/файл і \"usb:\" (поточний диск) */
    CHECK(strcmp(usbfs_edit_path("hd0:/x.txt", eb, sizeof eb), "hd0:/x.txt") == 0);
    CHECK(strcmp(usbfs_edit_path("y.txt", eb, sizeof eb), "usb:y.txt") == 0);
    CHECK(usbfs_write("hd0:/ide/ed.txt", "ide!", 4) == 0);
    CHECK(usbfs_read("hd0:/ide/ed.txt", buf, 100, &len) == 0 && len == 4 && !memcmp(buf, "ide!", 4));
    CHECK(usbfs_write("usb:ed2.txt", "cur", 3) == 0);                            /* поточний диск — hd0 */
    CHECK(usbfs_read("hd0:/ide/ed2.txt", buf, 100, &len) == 0 && len == 3);
    CHECK(usbfs_read("hd5:/nope", buf, 100, &len) != 0);

    /* команди IDE-інструментів (disks, hd read ...) не перехоплюються usbfs, навіть коли поточний диск — hd0 */
    CHECK(!handled("hd read 0 0")); CHECK(!handled("disks")); CHECK(!handled("hd mbr 0"));
    CHECK(!handled("hdparm"));

    /* неіснуючі/незмонтовані диски */
    cmd("hd3:"); EXPECT("No such IDE disk"); EXPECT("hd3:");
    cmd("cat hd3:/x"); EXPECT("is not mounted");
    cmd("cat hd1:/x"); EXPECT("is not mounted");

    /* hd1 \"відформатували\" (підміна образу) — провідник/`usb scan` у GUI викликають usbfs_rescan */
    ide_fd[1] = ide1_fmt;
    int before = usbfs_drive_count();
    CHECK(usbfs_rescan() == before + 1);
    cmd("hd1:"); EXPECT("Current drive: hd1:"); CHECK(strcmp(usbfs_prompt(), "hd1:/ ") == 0);
    cmd("info_placeholder");                                                      /* не наша команда: нічого не ламає */
    cmd("write x.txt exfat on ide"); EXPECT("ok"); cmd("cat x.txt"); EXPECT("exfat on ide");
    cmd("cp hd1:/x.txt hd0:/ide/fromhd1.txt"); EXPECT("ok");                      /* між двома IDE-дисками (exFAT -> FAT32) */
    cmd("cat hd0:/ide/fromhd1.txt"); EXPECT("exfat on ide");

    /* eject IDE-диска: лише відмонтування, жодного USB eject */
    int ej[3] = { ejected[0], ejected[1], ejected[2] };
    cmd("usb eject"); EXPECT("hd1: unmounted"); CHECK(ejected[0] == ej[0] && ejected[1] == ej[1] && ejected[2] == ej[2]); CHECK(!usbfs_active());
    cmd("usb eject 0"); EXPECT("usb0: ejected"); CHECK(ejected[0]);                                  /* цифра = флешка, не IDE */
    cmd("hd0:"); EXPECT("Current drive: hd0:");
    cmd("usb eject"); EXPECT("hd0: unmounted"); CHECK(!usbfs_active());
    cmd("hd0:"); EXPECT("Mounted hd0: TESTDISK0  FAT32"); EXPECT("Current drive: hd0:");   /* повторне монтування за назвою */
    cmd("cat hd0:/ide/note.txt"); EXPECT("on ide disk");                          /* дані пережили unmount/mount */

    printf("usbfs test: %d failure(s)\n", fails);
    return fails != 0;
}
