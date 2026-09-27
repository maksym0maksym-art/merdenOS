# MerdenOS — Technical Documentation

**Author:** Maksm_y
**License:** MerdenOS Development Public License 1.0 (MDPL-1.0), © 2026 Maksm_y
**Repository:** https://github.com/maksym0maksym-art/merdenOS
**Architecture:** x86 (i686), 32-bit protected mode
**Kernel name:** Rose, version 1.0

This documentation is based on the project's source code.

---

## 1. General Description

MerdenOS is an educational/hobby operating system that boots via **GRUB** using the **Multiboot2** protocol, runs in 32-bit protected mode, and outputs graphics through a **linear framebuffer** (obtained from the Multiboot2 tag, rather than the old VGA text mode). The system implements its own command interpreter (shell), a top status panel with a clock, keyboard drivers, RTC, serial port, as well as standard-generation disk drivers — IDE (PIO), under whose control the system currently works with a RAM file system (in the future, **FAT32** and **NTFS** support are planned).

### Top-Level Components

| Component | Description |
|---|---|
| Bootloader | GRUB2 (Multiboot2) + a separate EFI bootloader (`bootx64.c`, `efi_loader.c`) |
| Kernel (`kernel/`) | GDT/IDT/PIC initialization, interrupt handling, memory descriptor |
| Drivers (`kernel/code/drivers/`) | VGA, framebuffer, keyboard, RTC, COM port, IDE |
| File system (`kernel/filesystems/`) | RAM files implementation (+ FAT12/FAT16 drafts that remain notes only) |
| Shell (`system/shell/`) | command interpreter, pseudo-graphic "logo", calculator |
| Top panel (`system/top_panel/`) | status bar with clock/date at the top of the screen |
| Standard library (`standarts_lib/`) | own minimal `stdio` (`t_printf`, `strcmp`, `itoa`, etc.) |
| `futures/` | a separate GUI-application prototype in C#/Avalonia (.NET 9), unrelated to the kernel |

---

## 2. Boot Process

1. **GRUB** loads `kernel.bin`, built and linked according to `kernel/linker/linker.ld`, using the Multiboot2 scheme (the header is described in `kernel/boot/boot.asm`, requesting a framebuffer tag).
2. The entry point `_start` (asm) sets up the stack (`stack_bottom`/`stack_top`, 16 KB) and calls `kernel_main(magic, mbi_addr)`.
3. `kernel_main` (`kernel/kernel.c`):
   - checks the Multiboot2 magic number (`0x36D76289`);
   - calls `framebuffer_init(mbi_addr)`, which finds the framebuffer-type tag in the Multiboot2 information and stores the address/width/height/pitch/bpp;
   - calls `main_g()` from `GDT.c` — a demonstrational construction of GDT descriptors (prints them to the console; there is no separate `lgdt` call in this file — the actual GDT loading happens in the assembly code `GDT.asm`/`GDT_realmode.asm`, which are linked separately);
   - clears the screen and calls `shell_init()`;
   - enters an infinite loop of `keyboard_handler()` + `blink_cursor()` (keyboard handling and command-line cursor blinking).
4. The code below the infinite loop (`ide_init`, reading the MBR via `ide_read_sectors`) is "dead" — unreachable, since the `while(1)` above never terminates. This is a fragment of an unfinished integration with the IDE driver.

There is also a separate EFI path (`kernel/boot/bootx64.c`, `kernel/boot/efi_loader.c`) for booting directly via UEFI, as an alternative to GRUB (which is not yet finished).

---

## 3. Kernel: GDT, IDT, PIC

### 3.1 GDT (Global Descriptor Table)
Files: `kernel/code/GDT/GDT.c`, `GDT.asm`, `GDT_realmode.asm`, `kernel/headers/GDT.h`.

- `struct GDT { base, limit, access_byte, flags }` — describes a single descriptor.
- `encode_Gdt_entry()` — encodes the `GDT` structure into the 8-byte hardware descriptor format.
- `create_descriptor(base, limit, flag)` — an alternative way of building a 64-bit descriptor (demonstrational, with console printing via `t_printf`).
- Access-rights constants are defined for ring 0 and ring 3 code/data segments (`GDT_CODE_ACCESS_PL0/PL3`, `GDT_DATA_ACCESS_PL0/PL3`) and granularity/size flags (`GDT_FLAGS`).
- Actual loading of the table (`lgdt`) is performed in separate `.asm` files for real mode (`GDT_realmode.asm`) and protected mode (`GDT.asm`).

### 3.2 IDT (Interrupt Descriptor Table)
Files: `kernel/IDT/IDT.c`, `IDT.asm`, `kernel/headers/IDT.h`.

- `idt_set_gate(num, base, sel, flags)` fills an entry of the interrupt vector table (256 entries, 16-byte aligned).
- `idt_init()` initializes the table, sets `idt_ptr` (limit+base), and loads it via the external function `idt_load` (implemented in assembly).
- `isr_stub_table` — a table of interrupt handler stubs (asm), `exception_handler()` — the exception handler (marked `noreturn`).

### 3.3 PIC (8259 Programmable Interrupt Controller)
Files: `kernel/code/PIC/PIC.c`, `kernel/headers/PIC.h`.

- Standard master/slave PIC re-initialization (ICW1–ICW4) to remap IRQ vectors so they don't overlap with CPU exceptions.
- `PIC_sendEOI(irq)` sends the "end of interrupt" signal. *(Note: there is a bug in the current implementation — the signal is always sent only to `PIC2_COMMAND`, even for IRQs from the master PIC (`irq < 8`); the command to `PIC1_COMMAND` is not sent.)*

---

## 4. Drivers (`kernel/code/drivers/`)

| File | Purpose |
|---|---|
| `frambuffer.c` | Initialization of the linear framebuffer from the Multiboot2 tag; basic primitives (`put_pixel`, `fill_rect`, font character drawing, `scrol_screen` — screen scrolling) |
| `keyboard.c` | Keyboard interrupt handler: scancode table `keyboard_map[128]`, support for Shift/Ctrl/Caps Lock/Esc, passing characters to the shell |
| `rtc.c` | Reading the current time/date from the CMOS RTC (`rtc_get_time()` → `RTC_TIME {hour, minute, day, month, year}`) |
| `COM.c` | Serial port handling (for serial log/debug output) |
| `io.c` / `io.h` | Low-level `inb/outb` wrappers for port I/O |
| `disk.c` | Stub/entry point for the disk layer (11 lines) |
| `disk/init.asm`, `disk/read.asm` | Disk reading via BIOS int13h (real mode, used at the boot stage) |
| `IDE_drive.c` | Full-fledged PIO IDE/ATA driver (disk identification, `ide_read_sectors`, error handling via `package[]`) |
| `ATAPI.c` | Empty stub file for a future ATAPI (CD/DVD) driver |

---

## 5. File System (Present Future)
Currently, a test RAM file system is running, but ...

The `Merden_O/` folder (Obsidian notes) also contains separate draft documents: `File-system(fat12).md`, `File-system(fat16).md`, `File-system(fat 32).md` — these are the author's project notes that have not yet transitioned into code implementation (the code only contains `kernel/filesystems/fat12/init.asm` — a FAT12 initialization stub for the boot sector).

## 6. Command Shell

File: `system/shell/shell.c`, header `shell.h`.

- Startup: `shell_init()` clears the screen, draws the top panel (`draw_top_panel()`), and prints the prompt `liveuser@merden$`.
- Character input is handled via `shell_putchar()`/`shell_backspace()`, the command buffer is `command_buffer[256]`.
- The cursor blinks via `blink_cursor()` (called from the main kernel loop), visually implemented as an `_` character that is periodically drawn and hidden (`show_cursor`/`hide_cursor`), preserving the character underneath the cursor.
- Screen scrolling — `scrol_screen()` (framebuffer), triggered when `shell_row` goes beyond the visible area (`SCREEN_ROW = 42`, `SCREEN_COLS = 128`).
- Command execution — `shell_execute_command()`, string comparison via the custom `strcmp`/`strncmp`.

### Supported Commands

| Command | Action |
|---|---|
| `help` | list of available commands |
| `clear`, `cls` | clear the screen |
| `about` | system information (name, version, kernel, mode, author) |
| `tasks` | stub ("Scheluder: enabled") |
| `mem` | memory information stub |
| `time` | current time (from RTC) |
| `date` | current date (from RTC) |
| `fetch` | prints an ASCII logo of the system |
| `fastfetch` | prints a compact info card `merdenfetch()` (Rose kernel, arch i686, merden-sh shell) |
| `shutdown` | shutdown (via QEMU ACPI port `outw(0xF4, 0x00)`) |
| `interrput` | forced CPU halt (`cli`+`hlt` in an infinite loop) |
| `calc <a> <+|-|*|/> <b>` (invoked via `calc `) | simple integer calculator |
| `echo <text>` | prints text |
| `cat/create/list` | working with RAM files |

## 7. Top Panel

File: `system/top_panel/top_panel.c`.

`draw_top_panel()` draws a green bar across the full width of the screen; on the left it writes "MerdenOS|", on the right — the time and date (`HH:MM DD.MM.YY`) obtained from the RTC.

---

## 8. Standard Library (`standarts_lib/`)

A minimalist libc replacement for an OS-less environment (`stdio.c`/`stdio.h`, `stdio_s.c`/`stdio_s.h`):

- `t_printf` — a custom `printf` implementation supporting basic specifiers (`%d`, `%x`, `%s`, `%c`, and similar; judging by usage in the code: `%.16llX`, `%u`, `%.8s`);
- `t_putchar`, `t_print_int`, `t_printf_hex` — helper output functions;
- string functions: `strcmp`, `strcpy`, `strcat`, `strlen`, `itoa`, and others used throughout the kernel and shell.

## 9. Error Handling

File: `kernel/code/errors/kernel-panic.c`, header `kernel-panic.h`, notes `Merden_O/ErrorInfoTable.md`, `Merden_O/Kernel-panic.md`.

`kernel_panic(error_code)` — prints a critical error message and halts the system; used, in particular, in `fs_read()` when attempting to read a non-existent file (code `FOS_ERROR_UNKNOWN`).

---

## 10. Fonts and Text Output

Files: `kernel/fonts/font.c`, `fonts.h`, `Uni2-Fixed18.psf`, `psf2c.c`/`psf2c` (a utility for converting a PSF font into a C array).

A bitmap PC Screen Font (PSF2) is used, converted into a byte array (`fonts.h`), which is used to draw characters directly into the framebuffer (character size 8×18 pixels, hence `CHAR_WIDTH`/`CHAR_HEIGHT` in the shell and top panel).

---

## 11. The `futures/` Project (Separate from the Kernel)

The `futures/` directory contains a **separate .NET 9 / Avalonia UI project** (`App.axaml`, `MainWindow.axaml`, `Program.cs`) — a desktop prototype, not integrated into the OS code itself (a mockup of a future desktop application for creating a "work mode" (disabling user-selected applications while work mode is enabled; work mode is disabled after completing 10 exercises at the 1st–2nd–3rd grade level)). It compiles and runs separately from the OS kernel.

---

## 13. Build and Run

File: `Makefile` (and an outdated copy `m.txt`).

### Compilation (`make all`)
- Compiler: `gcc -m32 -ffreestanding -fno-pie -fno-pic -O0 -fno-stack-protector` (32-bit code with no dependency on the host OS).
- Assembly modules are built via `nasm -f elf32`.
- Linking: `ld -m elf_i386 -nostdlib -T kernel/linker/linker.ld` into a single `build/bin/kernel.bin`.
- An ISO image is formed via `grub-mkrescue` with a minimal `grub.cfg` (`multiboot2 /boot/kernel.bin`).
- An empty disk `disk.img` is created (64 MB, `qemu-img create -f raw`) — used as a SATA/floppy disk for future testing of disk and file operations.

### QEMU Run Targets

| Target | Description |
|---|---|
| `make run` | `qemu-system-i386`, `q35` machine, ISO + `disk.img` as floppy, but output to terminal (curses) |
| `make clean` | removes the `build/` directory |

File `OVMF_VARS.fd` — UEFI firmware variables for QEMU, `qemu.log` — log of the last run.

---

## 13. Repository Structure (brief)

```
MerdenOS/
├── kernel/
│   ├── boot/            — entry point, Multiboot2, EFI bootloader
│   ├── code/
│   │   ├── GDT/, IDT/... (IDT/ separately in kernel/IDT)
│   │   ├── PIC/         — interrupt controller
│   │   ├── drivers/     — VGA, framebuffer, keyboard, RTC, COM, IDE, ATAPI (empty)
│   │   ├── errors/      — kernel panic
│   │   └── system.c     — halt/poweroff/delay
│   ├── filesystems/     — Ram-files (+ fat12/init.asm stub)
│   ├── fonts/           — PSF2 bitmap font + converter
│   ├── headers/         — all kernel .h files
│   └── linker/linker.ld
├── standarts_lib/       — custom mini-libc (stdio)
├── system/
│   ├── shell/           — command shell
│   └── top_panel/       — top panel with clock
├── configs/config.asm
├── futures/             — separate .NET/Avalonia GUI application prototype (not integrated)
├── Merden_O/            — Obsidian notes (author's project documentation, .md)
├── build/               — build artifacts (kernel.bin, MerdenOS.iso, .o files)
├── disk.img, OVMF_VARS.fd, qemu.log — files for running/testing in QEMU
├── Makefile / m.txt
├── LICENSE (MDPL-1.0), COPYRIGHT
└── MerdenOS.kdev4 / .gpr / .geany / .vs / .vscode — IDE project files
```

---

## Known Bugs / Issues

- **`PIC_sendEOI(irq)`** always sends the EOI signal to `PIC2_COMMAND` only, even for IRQs from the master PIC (`irq < 8`) — the command to `PIC1_COMMAND` is never sent (see section 3.3).
- **Dead code after the boot loop**: the code following `keyboard_handler()` + `blink_cursor()` in `kernel_main` (`ide_init`, MBR reading via `ide_read_sectors`) is unreachable, since the preceding `while(1)` never terminates (see section 2, item 4).
- **EFI boot path is unfinished**: `bootx64.c`/`efi_loader.c` exist as an alternative to GRUB but are not yet completed (see section 2).
- **`ATAPI.c` is an empty stub** — no actual ATAPI (CD/DVD) driver implementation exists yet (see section 4).
- **`disk.c`** is only a stub/entry point (11 lines), not a full disk-layer implementation (see section 4).
- **FAT12/FAT16/FAT32 file systems are not implemented** — only design notes exist, plus a single unfinished stub, `kernel/filesystems/fat12/init.asm` (see section 5).
- **Shell stub commands**: `tasks` and `mem` do not return real data — they print placeholder/stub output only (see section 6).
- **`futures/` (.NET/Avalonia project) is fully disconnected from the kernel** — it is a separate prototype with no integration into the OS itself (see section 11)