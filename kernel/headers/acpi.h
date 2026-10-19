#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

/* Знайти таблиці ACPI. mbi — адреса multiboot2-структури (з неї береться RSDP на UEFI).
 * Повертає 1, якщо вимкнення через ACPI (S5) можливе. Можна викликати повторно. */
int acpi_init_mbi(uint32_t mbi);
int acpi_init(void);

/* Обидві функції не повертаються: вимкнення / перезавантаження комп'ютера */
void acpi_shutdown(void);
void acpi_reboot(void);

#endif
