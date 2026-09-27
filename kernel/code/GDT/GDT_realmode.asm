gdtr:
    dw 0    
    dd 0    

setGdt:
    xor eax, eax
    mov ax, ds
    shl eax, 4
    add eax, GDT            
    mov [gdtr + 2], eax     

    mov eax, GDT_end
    sub eax, GDT
    dec eax                 
    mov [gdtr], ax          

    lgdt [gdtr]
    ret

GDT:
    dq 0                    
GDT_end: