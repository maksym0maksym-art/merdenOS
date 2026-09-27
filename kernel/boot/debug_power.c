#include "../../standarts_lib/stdio.h"

void debug_power()
{
    print_brackets_green("[0.000000] GRUB: multiboot2 header found, magic 0x36D76289", 0, 0);
    delay();
    print_brackets_green("[0.000512] kernel_main: multiboot magic verified", 1, 0);
    delay();
    print_brackets_green("[0.000889] framebuffer_init: linear framebuffer mapped (128x42 text grid)", 2, 0);
    delay();
    print_brackets_green("[0.001203] GDT: ring0/ring3 code + data descriptors loaded", 3, 0);
    delay();
    print_brackets_green("[0.001547] IDT: 256 vectors installed, isr stubs bound", 4, 0);
    delay();
    print_brackets_green("[0.001820] PIC: master/slave remapped, IRQs cleared", 5, 0);
    delay();
    print_brackets_green("[0.002114] shell_init: merden-sh attached to tty0", 6, 0);
    delay();
    print_brackets_green("[0.002340] top_panel: clock/date attached (RTC)", 7, 0);
    delay();
    print_brackets_green("[boot complete]", 8, 0);
    delay();
}