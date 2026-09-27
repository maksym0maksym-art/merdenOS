EFI_CC = gcc
EFI_CRFLAGS = -I /usr/include/efi -I /usr/include/efi/x86_64 -fPIC -fshort-wchar -mno-red-zone -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers

CFLAGS = -m32 -ffreestanding -fno-pie -fno-pic -g -O0 -fno-stack-protector
LDFLAGS = -m elf_i386 -nostdlib -T kernel/linker/linker.ld

GUI_CFLAGS    = $(CFLAGS) -DGUI_BUILD
SERIAL_CFLAGS = $(CFLAGS) -DSERIAL_BUILD

all:
	mkdir -p build/obj build/bin build/iso/boot/grub build/iso/EFI/BOOT

	gcc $(CFLAGS) -c kernel/kernel.c -o build/obj/kernel.o
	gcc $(CFLAGS) -c kernel/code/drivers/vga.c -o build/obj/vga.o
	gcc $(CFLAGS) -c kernel/code/drivers/keyboard.c -o build/obj/keyboard.o
	gcc $(CFLAGS) -c kernel/code/errors/kernel-panic.c -o build/obj/kp.o
	gcc $(CFLAGS) -c standarts_lib/stdio.c -o build/obj/stdio.o
	gcc $(CFLAGS) -c kernel/code/GDT/GDT.c -o build/obj/GDT.o
	gcc $(CFLAGS) -c kernel/IDT/IDT.c -o build/obj/IDT.o
	gcc $(CFLAGS) -c kernel/code/system.c -o build/obj/system.o
	gcc $(CFLAGS) -c kernel/code/PIC/PIC.c -o build/obj/PIC.o
	gcc $(CFLAGS) -c system/shell/shell.c -o build/obj/shell.o
	gcc $(CFLAGS) -c kernel/code/drivers/frambuffer.c -o build/obj/framebuffer.o
	gcc $(CFLAGS) -c kernel/fonts/font.c -o build/obj/font.o
	gcc $(CFLAGS) -c kernel/code/drivers/rtc.c -o build/obj/rtc.o
	gcc $(CFLAGS) -c system/top_panel/top_panel.c -o build/obj/top_panel.o
	gcc $(CFLAGS) -c kernel/code/drivers/IDE_drive.c -o build/obj/IDE.o
	gcc $(CFLAGS) -c kernel/filesystems/ram_files.c -o build/obj/ram_files.o
	gcc $(CFLAGS) -c kernel/code/drivers/bmp.c -o build/obj/bmp.o
	gcc $(CFLAGS) -c kernel/code/drivers/mushka/pci.c -o build/obj/pci.o
	gcc $(CFLAGS) -c kernel/code/drivers/mushka/uhci.c -o build/obj/uhci.o
	gcc $(CFLAGS) -c kernel/code/drivers/mushka/usb.c -o build/obj/usb.o
	gcc $(CFLAGS) -c kernel/code/drivers/mushka/usb_mem.c -o build/obj/usb_mem.o
	gcc $(CFLAGS) -c kernel/code/drivers/mushka/hid_mouse.c -o build/obj/hid_mouse.o
	gcc $(CFLAGS) -c kernel/code/drivers/mushka/mouse_cmd.c -o build/obj/mouse_cmd.o
	gcc $(CFLAGS) -c system/shell/graphic/console.c -o build/obj/console.o
	gcc $(CFLAGS) -c kernel/images/image.c -o build/obj/image.o
	gcc $(CFLAGS) -c kernel/code/drivers/ACPI.c -o build/obj/acpi.o
	gcc $(CFLAGS) -c kernel/code/drivers/hardware.c -o build/obj/hardware.o
	gcc $(CFLAGS) -c kernel/boot/debug_power.c -o build/obj/db_p.o
	gcc $(CFLAGS) -c MerdenLanguage/merden.c -o build/obj/merden.o 
	nasm -f elf32 kernel/boot/boot.asm -o build/obj/boot.elf
	nasm -f elf32 kernel/IDT/IDT.asm -o build/obj/IDT_ASM.elf
	nasm -f elf32 kernel/code/GDT/GDT_realmode.asm -o build/obj/gd_rm.elf
	nasm -f elf32 kernel/code/GDT/GDT.asm -o build/obj/gd_pm.elf
	nasm -f elf32 kernel/code/drivers/mushka/idt_flush.asm
	python3 kernel/tools/bmp2c.py kernel/tools/doge_1090x818.bmp kernel/images/image.c doge

	ld $(LDFLAGS) \
		build/obj/merden.o \
		build/obj/boot.elf \
		build/obj/mouse_cmd.o \
		build/obj/acpi.o \
		build/obj/console.o \
		build/obj/db_p.o \
		build/obj/hid_mouse.o \
		build/obj/usb_mem.o \
		build/obj/usb.o \
		build/obj/uhci.o \
		build/obj/pci.o \
		build/obj/ram_files.o \
		build/obj/bmp.o \
		build/obj/hardware.o \
        build/obj/image.o \
		build/obj/top_panel.o \
		build/obj/rtc.o \
		build/obj/framebuffer.o \
		build/obj/font.o \
		build/obj/kernel.o \
		build/obj/vga.o \
		build/obj/keyboard.o \
		build/obj/kp.o \
		build/obj/system.o \
		build/obj/stdio.o \
		build/obj/GDT.o \
		build/obj/IDT.o \
		build/obj/IDT_ASM.elf \
		build/obj/gd_rm.elf \
		build/obj/gd_pm.elf \
		build/obj/PIC.o \
		build/obj/shell.o \
		build/obj/IDE.o \
		-o build/bin/kernel.bin

	cp build/bin/kernel.bin build/iso/boot/kernel.bin

	#qemu-img create -f raw disk.img 64M

	echo 'set timeout=0' > build/iso/boot/grub/grub.cfg
	echo 'insmod vbe' >> build/iso/boot/grub/grub.cfg
	echo 'insmod vga' >> build/iso/boot/grub/grub.cfg
	echo 'set gfxpayload=1024x768x32' >> build/iso/boot/grub/grub.cfg
	echo 'menuentry "MerdenOS" {' >> build/iso/boot/grub/grub.cfg
	echo '    multiboot2 /boot/kernel.bin' >> build/iso/boot/grub/grub.cfg
	echo '    boot' >> build/iso/boot/grub/grub.cfg
	echo '}' >> build/iso/boot/grub/grub.cfg
	grub-mkrescue -o build/MerdenOS.iso build/iso

clean:
	rm -rf build

run: 
	qemu-system-i386 \
		-machine q35 \
		-m 512M \
		-cdrom build/MerdenOS.iso \
		-drive file=disk.img,if=floppy,format=raw \
		-boot d