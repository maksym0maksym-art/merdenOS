#ifndef USBFS_H
#define USBFS_H

#include <stdint.h>

/*
 * Склейка USB-флешка (xHCI + MSC) і IDE-диск + FAT32/exFAT для shell, терміналу та редактора.
 * Команди: `usb help`. Шляхи в редакторі: "usb:/dir/file.txt" (відносні — від `usb cd`).
 * IDE-диски (legacy ATA, див. IDE_drive.h) монтуються при старті автоматично і працюють як флешки,
 * тільки називаються hd0:, hd1:, ...
 */

/* Номери дисків в API нижче: флешка usbN — це N (0..99), IDE-диск hdN — USBFS_IDE_BASE + N */
#define USBFS_IDE_BASE     100
#define USBFS_IS_IDE(d)    ((d) >= USBFS_IDE_BASE)

/* Обробляє команду, що починається з "usb". Повертає 1, якщо команду оброблено. */
int usbfs_command(const char *cmd);

int usbfs_mounted(void);
/* Змонтувати флешки, знайдені при старті (викликати після mouse_init) */
void usbfs_autostart(void);
/* Активний USB-диск (після `usb0:` / `usb scan`): звичайні ls/cd/cat/... працюють на ньому */
int usbfs_active(void);
/* Рядок для промпта ("usb0:/docs "), порожній для системного диска */
const char *usbfs_prompt(void);
/* Шлях для `edit`: при активному USB-диску додає префікс "usb:" (buf — тимчасовий буфер) */
const char *usbfs_edit_path(const char *arg, char *buf, int sz);
/* Читання/запис цілого файлу для редактора. path — з префіксом "usb:" або без. Повертають FAT_E_* (0 — ok). */
int usbfs_read(const char *path, char *buf, uint32_t cap, uint32_t *len);
int usbfs_write(const char *path, const char *buf, uint32_t n);
const char *usbfs_strerror(int rc);

/* ---- API для GUI-провідника (нічого не друкують) ---- */
typedef struct {
    char     name[256];     /* UTF-8 */
    uint8_t  is_dir;
    uint32_t size;
} usbfs_entry_t;

int  usbfs_drive_count(void);                                   /* скільки дисків змонтовано (флешки + IDE) */
int  usbfs_drive_name(int drive, char *buf, int sz);            /* "usb0" / "hd0" (без двокрапки); повертає довжину */
int  usbfs_drive_at(int i, char *label, int sz);                /* номер диска i-го змонтованого (-1 — нема); label = мітка або назва ФС */
int  usbfs_drive_space(int drive, uint32_t *free_kb, uint32_t *total_kb);
int  usbfs_list(int drive, const char *path, usbfs_entry_t *out, int max, int *count);   /* 0 або FAT_E_* */
int  usbfs_remove(int drive, const char *path);                 /* файл або порожній каталог */
int  usbfs_make(int drive, const char *path, int is_dir);
int  usbfs_rename(int drive, const char *from, const char *to);
int  usbfs_rescan(void);                                        /* знайти й змонтувати флешки та IDE-диски; повертає кількість змонтованих */

#endif
