%include "kernel/code/drivers/disk/init.asm"

disk_read:
    push cx
    push dx
    push di
    push ax

    mov dl, [disk_current_drive]

    push cx 
    call .lba_to_chs
    pop ax

    mov ah, 2h
    mov di, 3


.retry:
    pusha
    stc
    int 0x13
    jnc .done

    popa
    call .disk_reset

    dec di
    jnz .retry

.fail:
    pop ax
    pop di 
    pop dx
    pop cx 

    jmp read_error

.done: 
    popa 
    
    pop ax
    pop di
    pop dx
    pop cx
    ret

.lba_to_chs: 
    push ax
    push dx

    xor dx, dx
    div word [disk_spt]
    inc dx
    mov cx, dx    

    xor dx, dx
    div word [disk_heads]
    mov dh, dl

    mov ch, al
    shl ah, 6
    or cl, ah

    pop ax
    mov dl, al
    pop ax

    ret 

.disk_reset:
    pusha

    mov dl, [disk_current_drive]

    xor ax, ax
    stc
    int 0x13
    jc read_error
    
    popa 
    ret

read_error:
    mov si, err_read_faill
    jmp error_handler

err_read_faill: db '[!] read failled', 0
error_handler: dw 0