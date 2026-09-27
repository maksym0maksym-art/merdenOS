%ifndef DISK_INIT
%define DISK_INIT

disk_init:
    push ax

    mov [disk_current_drive], dl

    push es
    mov ah, 8h
    int 0x13
    pop es
    js .done

    and cl, 0x3F
    xor ch, ch

    inc dh

    movzx ax, dh
    mov [disk_heads], ax
    mov [disk_spt], cx

.done:
    pop ax
    ret

disk_current_drive: db 0
disk_spt:           dw 0
disk_heads:         dw 0

%endif