section .multiboot
align 8

MB2_MAGIC    equ 0xE85250D6
MB2_ARCH     equ 0
MB2_LENGTH   equ header_end - header_start
MB2_CHECKSUM equ -(MB2_MAGIC + MB2_ARCH + MB2_LENGTH)

header_start:

    dd MB2_MAGIC
    dd MB2_ARCH
    dd MB2_LENGTH
    dd MB2_CHECKSUM

    ; Framebuffer request
    align 8

    dw 5
    dw 0
    dd 20
    dd 1090 
    dd 768
    dd 32

    ; End tag
    align 8

    dw 0
    dw 0
    dd 8

header_end


section .text

bits 32

global _start
extern kernel_main

_start:

    cli

    ; GRUB Multiboot2:
    ; EAX = 0x36D76289
    ; EBX = address of Multiboot2 information structure

    mov esp, stack_top

    push ebx
    push eax

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang


section .bss

align 16

stack_bottom:
    resb 16384

stack_top: