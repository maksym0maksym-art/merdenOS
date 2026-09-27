%ifndef FAT12_INIT
%define FAT12_INIT

%include "configs/config.asm"

fat12_init:
    push ax
    push bx
    push cx
    push dx
    push es

    xor ax, ax
    mov es, ax

    mov ax, [es:BOOTIX_LOAD_ADDR + 11]
    
%endif