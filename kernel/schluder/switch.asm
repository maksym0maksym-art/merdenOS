[bits 32]

global switch_context

section .text
switch_context:
    mov eax, [esp + 4]      ; old_esp
    mov edx, [esp + 8]      ; new_esp
    push ebp
    push ebx
    push esi
    push edi
    mov [eax], esp
    mov esp, edx
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
