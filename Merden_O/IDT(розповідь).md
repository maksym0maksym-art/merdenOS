[[IDT(розповідь)]]
початок для [[IDT(code)]]
#розповідь 
# Таблиця переривчастого дескриптора

Від OSDev Вікі

(Перенаправлено з [IDT](https://wiki.osdev.org/index.php?title=IDT&redirect=no "IDT"))

[Перейти до навігації](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#mw-head) [Перейти до пошуку](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#searchInput)

**Таблиця Interrupt Descriptor** (**IDT**) — це двійкова структура даних, характерна для архітектури [IA-32](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IA32_Architecture_Family "Сім'я Архітектура IA32") [та x86-64](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/X86-64 "Х86-64"). Це [захищений режим і](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Protected_Mode "Захищений режим") аналог довгого режиму [до Реального режиму](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Real_Mode "Реальний режим") Переривання векторної таблиці ([IVT](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IVT "IVT")), що повідомляє процесору, де розташовані [процедури переривання обслуговування](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Interrupt_Service_Routines "Процедури Переривчастого Обслуговування") (ISR) (один на вектор переривання). Він схожий [на Глобальну таблицю дескрипторів](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Global_Descriptor_Table "Глобальна таблиця дескрипторів") за структурою.

Записи IDT називаються воротами. Він може містити переривчасті ворота, ворота завдань і ворота пастки.

Перш ніж реалізувати IDT, переконайтеся, що у вас є робочий GDT.

## Зміст

- [1 IDTR](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#IDTR)
- [2 Структура на ІА-32](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#Structure_on_IA-32)
    - [2.1 Таблиця](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#Table)
    - [2.2 Дескриптор воріт](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#Gate_Descriptor)
    - [2.3 Приклад коду](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#Example_Code)
- [3 Структура на x86-64](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#Structure_on_x86-64)
    - [3.1 Стіл](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#Table_2)
    - [3.2 Дескриптор воріт](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#Gate_Descriptor_2)
    - [3.3 Приклад коду](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#Example_Code_2)
- [4 предмети IDT](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#IDT_items)
- [5 Типи воріт](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#Gate_Types)
    - [5.1 Переривчасті ворота](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#Interrupt_Gate)
    - [5.2 Пасткові ворота](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#Trap_Gate)
    - [5.3 Ворота завдань](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#Task_Gate)
- [6 Див. Також](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#See_Also)
    - [6.1 Статті](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#Articles)
    - [6.2 Зовнішні посилання](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/IDT#External_References)

## IDTR

Місцезнаходження **IDT** зберігається в **IDTR** (реєстр **IDT**). Це завантажується за допомогою інструкції з монтажу **LIDT**, аргумент якої є вказівником на структуру **IDT** Descriptor:

- **Розмір:** На один менше розміру **IDT** в байтах.
- **Офсет** : Лінійна адреса **таблиці Interrupt Descriptor** (не фізична адреса, застосовується підказка).

Зауважте, що обсяг даних, завантажених **LIDT**, відрізняється в 32-розрядному та 64-розрядному режимах, **Offset** має довжину 4 байти в 32-розрядному режимі та 8 байт у 64-розрядному режимі.

Це схоже **[на GDT](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/GDT "ГДТ")**, за винятком:

- Перший запис (при нульовому зсуві) використовується **в IDT**.
- Існує 256 векторів переривань (0..255), тому **IDT** повинен мати 256 записів, кожен запис відповідає певному вектору переривання.
- Хоча **IDT** може містити більше 256 записів, вони ігноруються.
- Хоча **IDT** може містити менше 256 записів, будь-які записи, які не присутні (через ту чи іншу причину), генеруватимуть **[вихід із загального захисту](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Exceptions#General_Protection_Fault "Винятки")**, коли буде зроблено спробу отримати до них доступ. В ідеалі **IDT** повинен містити достатньо записів, щоб цю несправність (яка сама по собі є вектором переривання) могла бути оброблена.

Для отримання додаткової інформації див. **Розділ 2.4.3: Реєстр таблиць інтерпунктних описувачів IDTR** **та рисунок 2-6: Реєстри управління пам'яттю** посібника розробника програмного забезпечення Intel, Том 3-A.

## Структура на ІА-32

### Таблиця

На 32-розрядних процесорах записи **в IDT** мають довжину 8 байт і утворюють таблицю, як це:


Відповідний запис для даного **Переривчастого вектора** вказується в пам'яті шляхом масштабування вектора на 8 і додавання його до значення в полі **Офсет** **ВПД**.



- **Офсет:** 32-розрядне значення, розділене на дві частини. Він представляє адресу точки входу **[в процедуру Interrupt Service.](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Interrupt_Service_Routines "Процедури Переривчастого Обслуговування")**
- **Селектор:** **[Селектор сегментів](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Segment_Selector "Селектор сегментів")** з кількома полями, які повинні вказувати на дійсний сегмент коду у вашому **[GDT](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/GDT "ГДТ")**.
- **Тип воріт:** 4-бітне значення, яке визначає тип воріт цього **Переривати Дескриптор** представляють. Існує п'ять дійсних значень типу:
    - **0b0101** або **0x5** : Ворота завдань, зауважте, що в цьому випадку значення **Offset** не використовується і має бути встановлено на нуль.
    - **0b0110** або **0x6** : 16-бітні переривчасті ворота
    - **0b0111** або **0x7** : 16-бітні ворота-пастки
    - **0b1110** або **0xE** : 32-розрядні переривчасті ворота
    - **0b1111**0b1111 або **0xF** : 32-розрядні ворота-пастки
- **DPL:** 2-бітне значення, яке визначає **[рівні привілеїв процесора](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Security#Rings "Безпека")**, які дозволяють отримати доступ до цього переривання за допомогою інструкції **INT.** Апаратні переривання ігнорують цей механізм.
- **P:** Теперішній біт. Повинен бути встановлений **1**(1) для того, щоб дескриптор був дійсним.

Для отримання додаткової інформації див. **Розділ 7.11: Дескриптори IDT** **та малюнок 7-2: Дескриптори воріт IDT** [посібника розробника програмного забезпечення Intel, Том 3-А](https://www.intel.com/content/www/us/en/architecture-and-technology/64-ia-32-architectures-software-developer-vol-3a-part-1-manual.html).

### Приклад коду

C Структура:
```
struct InterruptDescriptor32 {
      uint16_t offset_1 ; // зміщення бітів 0..15
   uint16_t  ; // селектор сегментів коду в GDT або LDT
   uint8_t zero ; // невикористаний, встановлений на 0
   uint8_t  ; // тип затвора, dpl і p поля
   uint16_t offset_2 ; // зміщення бітів 16..31
};
```



Приклад значень _type_attributes_, які люди, ймовірно, використовують (припускаючи, що DPL дорівнює 0):

- 32-розрядні **переривчасті ворота: 0x8E** (p=1, dpl=0b00, type=0b1110 => **type_attributes=0b1000_1110=0x8E**)
- 32-розрядні **ворота-пастки: 0x8F** (p=1, dpl=0b00, type=0b1111 => **type_attributes=1000_1111b=** 0x8F)
- **Ворота завдання: 0x85** (р=1, dpl=0b00, type=0b0101 => **type_attributes=0b1000_0101=** 0x85)
- **Зсув:** 64-бітне значення, розділене на три частини. Він представляє адресу точки входу **[в процедуру Interrupt Service.](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Interrupt_Service_Routines "Процедури Переривчастого Обслуговування")**
- **Селектор:** **[Селектор сегментів](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Segment_Selector "Селектор сегментів")** з кількома полями, які повинні вказувати на дійсний сегмент коду у вашому **[GDT](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/GDT "ГДТ")**.
- **IST:** A 3-bit value which is an offset into the **Interrupt Stack Table**, which is stored in the **[Task State Segment](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Task_State_Segment "Task State Segment")**. If the bits are all set to zero, the **Interrupt Stack Table** is not used.
- **Тип воріт:** 4-бітне значення, яке визначає тип воріт цього **Переривати Дескриптор** represents. In long mode there are two valid type values:
    - **0b1110** or **0xE**: 64-bit Interrupt Gate
    - **0b1111** or **0xF**: 64-bit Trap Gate
- **DPL:** 2-бітне значення, яке визначає **[рівні привілеїв процесора](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Security#Rings "Безпека")**, які дозволяють отримати доступ до цього переривання за допомогою інструкції **INT.** Апаратні переривання ігнорують цей механізм.
- **P:** Теперішній біт. Повинен бути встановлений **1**(1) для того, щоб дескриптор був дійсним.

In your **[Interrupt Service Routines](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Interrupt_Service_Routines "Процедури Переривчастого Обслуговування")**, remember to return from the interrupt using the **IRETQ** instruction instead of **IRET**, as assemblers will not translate that for you. Many 64-bit IDT related problems on the forum are caused by that missing 'Q'. Don't let this happen to you.

For more information, see **Section 6.14.1: 64-Bit Mode IDT** and **Figure 6-8: 64-Bit IDT Gate Descriptors** of the Intel Software Developer Manual, Volume 3-A.

### Приклад коду

C Структура:

struct InterruptDescriptor64 {
      uint16_t offset_1 ; // зміщення бітів 0..15
   uint16_t  ; // селектор сегментів коду в GDT або LDT
   uint8_t ist ; // біти 0..2 утримує зміщення таблиці Interrupt Stack, решту бітів нуль.
   uint8_t  ; // тип затвора, dpl і p поля
   uint16_t offset_2 ; // зміщення бітів 16..31
   uint32_t offset_3 ; // зміщення бітів 32..63
   uint32_t нуль ; // зарезервовано
};

Приклад значень _type_attributes_, які люди, ймовірно, використовують (припускаючи, що DPL дорівнює 0):

- 64-розрядні **переривчасті ворота: 0x8E** (p=1, dpl=0b00, type=0b1110 => **type_attributes=0b1000_1110=0x8E**)
- 64-розрядні **ворота-пастки: 0x8F** (p=1, dpl=0b00, type=0b1111 => **0x8F**type_attributes=1000_1111b=0x8F)

## Предмети IDT

Спочатку створений для Вікіпедії. [[1]](https://en.wikipedia.org/w/index.php?title=Template:X86_protected_mode_interrupts&oldid=1283313564)

Скопійовано з: [[2]](https://wiki.jwo.cz/wiki/x86_protected_mode_interrupts)

Ця таблиця містить інформацію, зібрану з _посібника з програмного забезпечення Intel® 64 та IA-32 Architectures Software Developer's Volume 3 (3A, 3B, 3C & 3D):_ Перегляд _посібника з системного програмування_ з квітня 2022 року.

  

## Типи воріт

В основному існує два види переривань: ті, які відбуваються, коли виконання коду стикається **[з виключенням](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Exceptions "Винятки")** через поганий код, або ті, які відбуваються для обробки подій, не пов'язаних з виконанням коду в даний час. У першому випадку доречно зберегти адресу _інструкції,_ що _виконується в даний час_, щоб її можна було повторно винести, вони називаються **пастками**. У другому випадку доречно зберегти адресу _наступної_ інструкції, щоб виконання можна було відновити там, де воно зупинилося. Вони можуть бути викликані IRQ або іншою апаратною подією або використанням інструкції **INT**. Ще одна відмінність, яку слід зазначити, полягає в тому, що з **пастками** нові переривання можуть виникнути під час служби, але коли процесор обслуговує IRQ, подальші переривання маскуються до надсилання сигналу **End of Interrupt**. Те, як подається певне переривання, залежить від того, які ворота ви поклали у в'їзд IDT.

### Переривчаті ворота

**Для** визначення **[процедури переривання](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Interrupt_Service_Routines "Процедури Переривчастого Обслуговування")** **використовується «Переривчасті ворота».** Наприклад, коли інструкція з монтажу **INT 50** виконується під час роботи в захищеному режимі, процесор шукає 50-й запис (розташований на 50 * 8) **в IDT.** Потім завантажуються значення **«Селектор»** **і «Офсет»** Interrupt Gate. **Селектор і** **Офсет** використовуються для виклику **програми Interrupt Service.** Коли виконується інструкція **IRET**, процесор повертається з переривання. Якщо процесор працював у 32-розрядному режимі, а вказаний селектор - 16-бітний, то процесор перейде в 16-бітному захищеному режимі після виклику **ISR**. Щоб повернутися в цьому випадку, слід використовувати інструкцію **O32 IRET**, інакше процесор не знатиме, що він повинен повернути 32-розрядну версію (читаючи 32-розрядні значення [зі стека](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Stack "Stack") замість 16 біт).

### Пастки воріт

**Ворота-пастки** повинні використовуватися для обробки **[винятків](file:///home/maksm_y/%D0%A1%D1%82%D1%96%D0%BB%D1%8C%D0%BD%D0%B8%D1%86%D1%8F/osdev_wiki\(1\)/wiki.osdev.org/Exceptions "Винятки")**. Коли виникає такий виняток, іноді на стеку може бути розміщено код помилки, який слід з'явитися перед поверненням з переривання.

**Ворота-пастки** та **переривчасті ворота** схожі, а їх дескриптори структурно однакові, що відрізняються лише полем **Gate Type**. Різниця полягає в тому, що для **переривчастих воріт** переривання автоматично відключаються при вході та поновлюються **на IRET**, тоді як це не відбувається **для Trap Gates**.

### Ворота завдань

**Ворота завдань**