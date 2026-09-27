[bits 32]

; Експортуємо мітки для C-файлу
global idt_load
global isr_stub_table

; Імпортуємо C-функцію для обробки паніки ядра (якщо знадобиться)
extern exception_handler

; Макрос для обробників БЕЗ коду помилки
%macro isr_no_err_stub 1
isr_stub_%1:
    push 0                 ; Заштовхуємо dummy error code (0)
    push %1                ; Заштовхуємо номер переривання
    pushad                 ; Зберігаємо загальні регістри
    call exception_handler ; Викликаємо твій C-обробник
    popad                  ; Відновлюємо регістри
    add esp, 8             ; Очищаємо стек від номера та error code
    iret                   ; Повертаємося з переривання
%endmacro

; Секція коду: генеруємо самі функції обробників
section .text

%assign i 0
%rep 32
    isr_no_err_stub i
%assign i i+1
%endrep

; Функція завантаження IDT
idt_load:
    mov edx, [esp + 4]     ; Беремо аргумент (вказівник на дескриптор) зі стеку
    lidt [edx]             ; Завантажуємо таблицю
    sti                    ; Вмикаємо апаратні переривання
    ret

; Секція даних: генеруємо масив адрес для C
section .data
align 4
isr_stub_table:
%assign i 0
%rep 32
    dd isr_stub_%+i
%assign i i+1
%endrep