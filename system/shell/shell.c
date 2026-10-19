#include "../../standarts_lib/stdio.h"
#include "../../kernel/headers/vga.h"
#include "../../kernel/headers/keyboard.h"
#include "../../kernel/headers/system.h"
#include "../../kernel/headers/kernel-panic.h"
#include "../../kernel/code/drivers/io.h"
#include "shell.h"
#include "../../kernel/headers/acpi.h"
#include "../../kernel/headers/bmp.h"
#include "../../kernel/images/image.h"
#include "../../kernel/headers/rtc.h"
#include "../top_panel/top_panel.h"
#include "../../kernel/headers/ram_files.h"
#include "../../kernel/headers/hardware.h"
#include "../../kernel/headers/framebuffer.h"
#include "../../kernel/headers/IDE_drive.h"
#include "../../kernel/headers/mouse_cmd.h"
#include "../../MerdenLanguage/merden.h"
#include "graphic/console.h"
#include "../GUI/gui.h"
#include "../../kernel/headers/usbfs.h"
#include "../../kernel/headers/klog.h"
#include "../../kernel/headers/schluder.h"
#include "../../kernel/headers/png.h"
#include "../../kernel/headers/memory.h"

#define CHAR_WIDTH 8
#define CHAR_HEIGHT 18 

#define IDE_ATA    0x00
#define IDE_ATAPI  0x01

#define HISTORY_SIZE 16

int shell_cursor_pos = 0;   

int shell_col = 0;
int shell_row = 0;

static int prompt_start_col = 0;

/* ---- прототипи (щоб порядок визначень не мав значення) ---- */
void shell_print(char *text);
void shell_print_int(int num);
int shell_atoi(const char *str);
void shell_calculator(char *command);
void shell_print_color(char *text, uint32_t color);
void shell_redraw_line(const char *buf, int length, int new_cursor_pos);
void shell_history_add(const char *cmd);
void show_cursor(void);
void hide_cursor(void);
void shell_execute_command(char* command);
void shell_print_f(char* text);
void move_cursor(int x, int y);
static void prompt_dir(char *out, int n);
void uptime_mark_boot(void);
uint64_t hardware_get_ram_installed_bytes(void);   /* має бути і в hardware.h */

#define PROMPT_USER  "rose"
#define PROMPT_HOST  "rose"
#define PROMPT_LINE  0x0000FF      /* лінії, дужки, ">>" — колір, яким був старий промт */
#define PROMPT_NAME  0x0000FF      /* імена, як у Kali, того ж кольору */
#define PROMPT_DIR   0xFFFFFF      /* директорія біла, як ~ у Kali */

/* "usb0:/dir" без кінцевого пробілу, а без USB-диска — "/" */
static void prompt_dir(char *out, int n)
{
    const char *drv = usbfs_prompt();                 /* "usb0:/dir " з пробілом в кінці, або "" */
    int len = 0;
    while (drv[len] && len < n - 1) { out[len] = drv[len]; len++; }
    while (len > 0 && out[len - 1] == ' ') len--;
    out[len] = '\0';
    if (len == 0) { out[0] = '/'; out[1] = '\0'; }
}

void shell_prompt(){
    char dir[128];
    prompt_dir(dir, sizeof dir);

    /*  ┌──{rose🌹rose}-{usb0:/}
     *  └─>>                      */
    shell_print_color(GUI_BOX_TL_STR GUI_BOX_H_STR GUI_BOX_H_STR "{", PROMPT_LINE);
    shell_print_color(PROMPT_USER, PROMPT_NAME);
    shell_print_color(GUI_ROSE_STR, PROMPT_NAME);       /* троянда має власні кольори */
    shell_print_color(PROMPT_HOST, PROMPT_NAME);
    shell_print_color("}-{", PROMPT_LINE);
    shell_print_color(dir, PROMPT_DIR);
    shell_print_color("}\n", PROMPT_LINE);
    shell_print_color(GUI_BOX_BL_STR GUI_BOX_H_STR ">> ", PROMPT_LINE);

    prompt_start_col = shell_col;                        /* 5 клітинок: └ ─ > > пробіл */
}

#define PROMPT "liveuser@merden$ "
#define BUFFER_SIZE 256

static char history[HISTORY_SIZE][BUFFER_SIZE];
static int  history_count =  0; 
static int  history_head  =  0;  
static int  history_pos   = -1;  

int command_index = 0;
char command_buffer[BUFFER_SIZE];

void welcome_to_Merden()
{   
    shell_print_color("/*=======================\n", 0x00FF00);
    shell_print_color(":  Welcome to Merden    :\n", 0x00FF00);
    shell_print_color("=======================*/\n", 0x00FF00);
}

#define EDITOR_MAX 1024 
static int  editor_mode = 0;
static char editor_filename[64];
static char editor_buffer[EDITOR_MAX];
static int  editor_len = 0;
static int  editor_line_number = 1;
static int  editor_saved_row = 0;   // позиція курсора шела до входу в редактор
static int  editor_saved_col = 0;

void shell_calculator(char *command)
{
    char *p = command + 5; // після "calc "

    while (*p == ' ')
        p++;

    int first = shell_atoi(p);

    if (*p == '-')
        p++;

    while (*p >= '0' && *p <= '9')
        p++;

    while (*p == ' ')
        p++;

    char operation = *p;
    if (*p)
        p++;

    while (*p == ' ')
        p++;

    int second = shell_atoi(p);
    int result = 0;

    switch (operation)
    {
        case '+':
            result = first + second;
            break;

        case '-':
            result = first - second;
            break;

        case '*':
            result = first * second;
            break;

        case '/':
            if (second == 0)
            {
                shell_print("Error: division by zero.\n");
                return;
            }

            result = first / second;
            break;

        default:
            shell_print("Usage: calc <number> <+|-|*|/> <number>\n");
            return;
    }

    shell_print_int(result);
    shell_print("\n");
}

short cursor_visible = 0;

void shell_print_int(int num)
{
    char buf[16];
    itoa(num, buf);
    shell_print(buf);
}

/* ---- планувальник: команди `tasks` і `tasktest` ---- */

static void tasks_pad(int used, int width)
{
    while (used++ < width)
        shell_print(" ");
}

static void tasks_print_num(int n, int width)
{
    int digits = 1;
    for (int t = n; t >= 10; t /= 10)
        digits++;
    shell_print_int(n);
    tasks_pad(digits, width);
}

static void tasks_print_str(const char *s, int width)
{
    int len = 0;
    while (s[len])
        len++;
    shell_print((char *)s);
    tasks_pad(len, width);
}

static void cmd_tasks(void)
{
    sched_info_t info;

    shell_print("ID  NAME            STATE     RUNS    STACK(used/size)\n");
    for (int slot = 0; slot < MAX_TASKS; slot++)
    {
        if (!scheduler_info(slot, &info))
            continue;

        tasks_print_num(info.id, 4);
        tasks_print_str(info.name, 16);
        tasks_print_str(task_state_name(info.state), 10);
        tasks_print_num((int)info.runs, 8);

        if (info.stack_size)
        {
            shell_print_int((int)info.stack_used);
            shell_print("/");
            shell_print_int((int)info.stack_size);
        }
        else
        {
            shell_print("boot stack");
        }
        shell_print("\n");
    }
    shell_print("Tasks alive: ");
    shell_print_int(scheduler_count());
    shell_print(" of ");
    shell_print_int(MAX_TASKS);
    shell_print("\n");
}

static void demo_task_a(void)
{
    for (int i = 0; i < 5; i++)
    {
        shell_print("[task A] tick ");
        shell_print_int(i);
        shell_print("\n");
        task_sleep_ms(400);
    }
}

static void demo_task_b(void)
{
    for (int i = 0; i < 3; i++)
    {
        shell_print("[task B] tick ");
        shell_print_int(i);
        shell_print("\n");
        task_sleep_ms(700);
    }
}

/* ---- команда `png <файл>`: показує PNG з флешки на весь екран; будь-яка клавіша — вихід ---- */

static void png_wait_key(void)
{
    while (inb(0x64) & 1)                       /* викинути все, що вже накопичилось */
        inb(0x60);

    for (;;)
    {
        unsigned char st = inb(0x64);
        if (st & 1)
        {
            unsigned char sc = inb(0x60);
            if (!(st & 0x20) && !(sc & 0x80) && sc != 0xE0)   /* 0x20 — байт PS/2-миші, 0x80 — відпускання клавіші */
                return;
        }
        sched_yield();
    }
}

static void cmd_png(const char *arg)
{
    while (*arg == ' ')
        arg++;
    if (!*arg)
    {
        shell_print("Usage: png <file.png>   (e.g. png usb0:/pic.png)\n");
        return;
    }

    const uint32_t cap = 8u * 1024u * 1024u;
    uint8_t *buf = (uint8_t *)kmalloc(cap);
    if (!buf)
    {
        shell_print("png: not enough memory\n");
        return;
    }

    uint32_t len = 0;
    int rc = usbfs_read(arg, (char *)buf, cap, &len);
    if (rc)
    {
        shell_print("png: cannot read file: ");
        shell_print((char *)usbfs_strerror(rc));
        shell_print("\n");
        kfree(buf);
        return;
    }

    png_info_t info;
    rc = png_info(buf, len, &info);
    if (rc != PNG_OK)
    {
        shell_print("png: ");
        shell_print((char *)png_strerror(rc));
        shell_print("\n");
        kfree(buf);
        return;
    }

    console_enter_alt();
    clear_screen(0x000000);
    rc = png_draw_fit(buf, len, 0, 0, framebuffer_width, framebuffer_height);
    if (rc == PNG_OK)
        png_wait_key();
    console_exit_alt();

    if (rc != PNG_OK)
    {
        shell_print("png: ");
        shell_print((char *)png_strerror(rc));
        shell_print("\n");
    }
    kfree(buf);
}

static void cmd_tasktest(void)
{
    if (add_tasks(demo_task_a, "demo_a") < 0 || add_tasks(demo_task_b, "demo_b") < 0)
        shell_print("tasktest: cannot create tasks (table full or out of memory)\n");
    else
        shell_print("tasktest: started demo_a and demo_b, run `tasks` to see them\n");
}

int shell_atoi(const char *str)
{
    int num = 0;
    int sign = 1;

    if (*str == '-')
    {
        sign = -1;
        str++;
    }

    while (*str >= '0' && *str <= '9')
    {
        num = num * 10 + (*str - '0');
        str++;
    }

    return num * sign;
}

int strncmp(const char *a, const char *b, int n)
{
    for(int i = 0; i < n; i++)
    {
        if (a[i] != b[i])
        {
            return a[i] - b[i];
        }

        if (a[i] == '\0')
        {
            return 0;
        }

    }
    return 0;
}

void shell_print(char* text)
{
    console_puts_color(text, COLOR_WHITE);
}

/* UTF-8 🌹 (F0 9F 8C B9) -> два символи GUI_ROSE_STR; решта байтів без змін */
static void shell_print_utf8(const char *s)
{
    char buf[128];
    int n = 0;
    while (*s) {
        if (n > (int)sizeof buf - 3) { buf[n] = '\0'; shell_print(buf); n = 0; }
        if ((unsigned char)s[0] == 0xF0 && (unsigned char)s[1] == 0x9F &&
            (unsigned char)s[2] == 0x8C && (unsigned char)s[3] == 0xB9) {
            buf[n++] = (char)GUI_CH_ROSE_L;
            buf[n++] = (char)GUI_CH_ROSE_R;
            s += 4;
        } else {
            buf[n++] = *s++;
        }
    }
    buf[n] = '\0';
    shell_print(buf);
}

void shell_init()
{
        uptime_mark_boot();   // запам'ятовуємо момент старту для uptime у fetch
        clear_screen(0x000000);
        console_reset();

        shell_row = 1;   // одразу під верхньою панеллю (TOP_ROWS = 1)
        shell_col = 0;

        command_index = 0;
        command_buffer[0] = '\0';
        draw_top_panel();
        welcome_to_Merden();   // 3 рядки з \n -> після нього shell_row == 4
        shell_prompt();
}

char cursor_saved;

void move_cursor(int x, int y)
{
   (void)x;
   (void)y;
}

void shell_putchar(char c)
{
    hide_cursor();

    if (editor_mode)
    {
        if (editor_len < EDITOR_MAX - 1)
        {
            editor_buffer[editor_len++] = c;
            editor_buffer[editor_len] = '\0';

            char s[2] = {c, '\0'};
            shell_print(s);
        }
        show_cursor();
        return;
    }

    if (command_index >= BUFFER_SIZE - 1)
    {
        show_cursor();
        return;
    }

    // Зсуваємо хвіст рядка праворуч, щоб вставити символ саме в cursor_pos
    for (int i = command_index; i > shell_cursor_pos; i--)
        command_buffer[i] = command_buffer[i - 1];

    command_buffer[shell_cursor_pos] = c;
    command_index++;
    shell_cursor_pos++;
    command_buffer[command_index] = '\0';

    shell_redraw_line(command_buffer, command_index, shell_cursor_pos);
}

void shell_print_color(char *text, uint32_t color)
{
    console_puts_color(text, color);
}

void shell_backspace()
{
    if (editor_mode)
    {
        if (editor_len > 0)
        {
            hide_cursor();

            editor_len--;
            editor_buffer[editor_len] = '\0';

            shell_col--;

            print_one_part(" ", shell_row, shell_col);
            cell_set(shell_row, shell_col, ' ', COLOR_WHITE);
            move_cursor(shell_row, shell_col);
            clear_char(shell_col * CHAR_WIDTH, shell_row * CHAR_HEIGHT);

            show_cursor();
        }
        return;
    }

    if (shell_cursor_pos <= 0)
        return;   // нема чого стирати зліва — заразом захищає промт

    for (int i = shell_cursor_pos - 1; i < command_index - 1; i++)
        command_buffer[i] = command_buffer[i + 1];

    command_index--;
    shell_cursor_pos--;
    command_buffer[command_index] = '\0';

    shell_redraw_line(command_buffer, command_index, shell_cursor_pos);
}

void show_cursor()
{
    if (cursor_visible || !console_is_live())
        return;

    cursor_saved = cell_get(shell_row, shell_col);

    clear_char(shell_col * CHAR_WIDTH,
               shell_row * CHAR_HEIGHT);

    print_one_part_color("_", shell_row, shell_col,  0x0000FF);

    cursor_visible = 1;
}

void hide_cursor()
{
    if (!cursor_visible)
        return;

    clear_char(shell_col * CHAR_WIDTH,
               shell_row * CHAR_HEIGHT);

    char s[2];
    s[0] = cursor_saved;
    s[1] = '\0';

    print_one_part(s, shell_row, shell_col);

    cursor_visible = 0;
}

void blink_cursor()
{
    if (cursor_visible)
    {
        hide_cursor();
    }
    else
    {
        show_cursor();
    }
}

char* shell_trim(char* str)
{
    if (!str) return str;

    // 1. Пропускаємо пробіли на початку
    char* start = str;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
    {
        start++;
    }

    // Якщо рядок весь складався з пробілів
    if (*start == '\0')
    {
        str[0] = '\0';
        return str;
    }

    // 2. Якщо початок зсунувся - переносимо рядок на початок буфера
    if (start != str)
    {
        int i = 0;
        while (start[i] != '\0')
        {
            str[i] = start[i];
            i++;
        }
        str[i] = '\0';
    }

    // 3. Обрізаємо пробіли з кінця
    int len = 0;
    while (str[len] != '\0') len++;

    while (len > 0 &&
          (str[len - 1] == ' ' || str[len - 1] == '\t' ||
           str[len - 1] == '\r' || str[len - 1] == '\n'))
    {
        str[len - 1] = '\0';
        len--;
    }

    return str;
}

void shell_enter()
{
    hide_cursor();

    if (editor_mode)
    {
        if (editor_len < EDITOR_MAX - 1)
        {
            editor_buffer[editor_len++] = '\n';
            editor_buffer[editor_len] = '\0';
        }

        shell_print("\n");

        editor_line_number++;
        char num_prefix[16];
        itoa(editor_line_number, num_prefix);
        shell_print(num_prefix);
        shell_print("| ");

        show_cursor();
        return;
    }

    command_buffer[command_index] = '\0';

    shell_trim(command_buffer);

    shell_print("\n");
    shell_history_add(command_buffer);   
    shell_execute_command(command_buffer);

    command_index = 0;
    shell_cursor_pos = 0; 
    command_buffer[0] = '\0';

    if (!editor_mode)   // якщо команда щойно ввімкнула редактор — промт не показуємо
    {
        shell_prompt();
        show_cursor();
    }
}

// Компактне лого (26x15), розбите на окремі рядки — щоб можна було
// друкувати його поряд з характеристиками, рядок в рядок.
static const char *logo_lines[] = {
    "              ..          ",
    "        .......           ",
    "      #+=##@#             ",
    "    ##*####               ",
    "   ##%####%         ---   ",
    "  ***####@  ##    ####==  ",
    "  **###################=* ",
    "     @######%%##########=+",
    "        @##    #@ ######+=",
    "       ###########   ###+*",
    "           ####  %@   @#++",
    "*#*#@   #######        #+*",
    " ***###########        ++ ",
    "   *+++#########       *  ",
    "       +++++++++          ",
};
#define LOGO_LINES     (sizeof(logo_lines) / sizeof(logo_lines[0]))
#define FETCH_LOGO_W   26
#define FETCH_GAP      2

static int fetch_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void fetch_append(char *dst, const char *src)
{
    int i = fetch_strlen(dst);
    int j = 0;
    while (src[j]) dst[i++] = src[j++];
    dst[i] = '\0';
}

static void fetch_append_int(char *dst, int value)
{
    char buf[16];
    itoa(value, buf);
    fetch_append(dst, buf);
}

/* ---------------- кольоровий fetch ---------------- */
#define FETCH_USER      "liveuser"
#define FETCH_HOST      "merden"
#define FETCH_LABEL     0xFF3B3B   /* підписи й ім'я, як у neofetch */
#define FETCH_WHITE     0xFFFFFF
#define FETCH_BLOCK     "###"      /* "клітинка" палітри; якщо у шрифті є суцільний блок — підстав його */
#define FETCH_FIELDS    9
#define FETCH_MAX_DISKS 8
#define FETCH_LINE_MAX  80

/* Палітра 16 кольорів (перший — майже чорний, щоб було видно на чорному фоні) */
static const uint32_t fetch_palette[16] = {
    0x2A2A2A, 0xCC0000, 0x4E9A06, 0xC4A000, 0x3465A4, 0x75507B, 0x06989A, 0xD3D7CF,
    0x555753, 0xEF2929, 0x8AE234, 0xFCE94F, 0x729FCF, 0xAD7FA8, 0x34E2E2, 0xEEEEEC,
};

/* Колір символу лого: щільніші символи яскравіші — виходить об'ємна форма */
static uint32_t fetch_logo_color(char c)
{
    switch (c)
    {
        case '@': return 0xFF5A5A;
        case '%': return 0xF03C3C;
        case '#': return 0xD82A2A;
        case '*': return 0xB81E1E;
        case '+': return 0x982222;
        case '=': return 0x802424;
        case '-': return 0x702828;
        case '.': return 0x602424;
    }
    return FETCH_WHITE;
}

/* Друкує один рядок лого рівно в FETCH_LOGO_W + FETCH_GAP клітинок, групуючи однакові кольори */
static void fetch_print_logo_line(const char *s)
{
    char run[FETCH_LOGO_W + FETCH_GAP + 1];
    int n = 0, ended = 0;
    uint32_t cur = FETCH_WHITE;

    for (int i = 0; i < FETCH_LOGO_W + FETCH_GAP; i++)
    {
        char c = ' ';
        if (!ended && i < FETCH_LOGO_W)
        {
            if (s[i]) c = s[i];
            else      ended = 1;
        }

        uint32_t color = (c == ' ' && n) ? cur : fetch_logo_color(c);

        if (n && color != cur)
        {
            run[n] = '\0';
            shell_print_color(run, cur);
            n = 0;
        }

        cur = color;
        run[n++] = c;
    }

    if (n)
    {
        run[n] = '\0';
        shell_print_color(run, cur);
    }
}

static const char *const fetch_labels[FETCH_FIELDS] = {
    "OS", "Kernel", "Uptime", "Arch", "Host", "CPU", "Memory", "Display", "Shell"
};

static char fetch_val_up[FETCH_LINE_MAX];
static char fetch_val_host[FETCH_LINE_MAX];
static char fetch_val_cpu[FETCH_LINE_MAX];
static char fetch_val_mem[FETCH_LINE_MAX];
static char fetch_val_res[FETCH_LINE_MAX];

static const char *const fetch_values[FETCH_FIELDS] = {
    "MerdenOS", "Rose", fetch_val_up, "i686",
    fetch_val_host, fetch_val_cpu, fetch_val_mem, fetch_val_res,
    "merden-sh"
};

static char fetch_disk[FETCH_MAX_DISKS][FETCH_LINE_MAX];
static int  fetch_disk_count;

/* Додає src в кінець dst, не виходячи за cap байтів (разом з '\0') */
static void fetch_append_cap(char *dst, const char *src, int cap)
{
    int i = fetch_strlen(dst);
    int j = 0;
    while (src[j] && i < cap - 1) dst[i++] = src[j++];
    dst[i] = '\0';
}

/* "119 GB" / "512 MB" */
static void fetch_append_size_mb(char *dst, unsigned int mb)
{
    char num[16];
    if (mb >= 1024)
    {
        itoa((int)(mb / 1024), num);
        fetch_append_cap(dst, num, FETCH_LINE_MAX);
        fetch_append_cap(dst, " GB", FETCH_LINE_MAX);
    }
    else
    {
        itoa((int)mb, num);
        fetch_append_cap(dst, num, FETCH_LINE_MAX);
        fetch_append_cap(dst, " MB", FETCH_LINE_MAX);
    }
}

/* Збирає список усіх дисків: IDE/ATA, ATAPI (CD/DVD) та USB-флешки */
static void fetch_collect_disks(void)
{
    fetch_disk_count = 0;

    for (int i = 0; i < 4 && fetch_disk_count < FETCH_MAX_DISKS; i++)
    {
        if (!ide_devices[i].Reserved)
            continue;

        char *line = fetch_disk[fetch_disk_count];
        line[0] = '\0';

        if (ide_devices[i].Type == IDE_ATAPI)
        {
            fetch_append_cap(line, "CD/DVD", FETCH_LINE_MAX);
        }
        else
        {
            fetch_append_cap(line, "HDD ", FETCH_LINE_MAX);
            fetch_append_size_mb(line, (unsigned int)(ide_devices[i].Size / 2048)); /* сектори по 512 Б */
        }

        /* модель без кінцевих пробілів */
        char model[44];
        int m = 0;
        while (ide_devices[i].Model[m] && m < (int)sizeof model - 1)
        {
            model[m] = ide_devices[i].Model[m];
            m++;
        }
        while (m > 0 && model[m - 1] == ' ') m--;
        model[m] = '\0';

        if (m > 0)
        {
            fetch_append_cap(line, "  ", FETCH_LINE_MAX);
            fetch_append_cap(line, model, FETCH_LINE_MAX);
        }

        fetch_disk_count++;
    }

    for (int i = 0; i < usbfs_drive_count() && fetch_disk_count < FETCH_MAX_DISKS; i++)
    {
        int d = usbfs_drive_at(i, 0, 0);
        if (d < 0) continue;

        char *line = fetch_disk[fetch_disk_count];
        char num[16];
        itoa(d, num);

        line[0] = '\0';
        fetch_append_cap(line, "USB usb", FETCH_LINE_MAX);
        fetch_append_cap(line, num, FETCH_LINE_MAX);
        fetch_append_cap(line, ":", FETCH_LINE_MAX);

        fetch_disk_count++;
    }
}

/* ---------------- uptime (за RTC, з точністю до хвилини) ---------------- */
static long boot_minutes = -1;

/* кількість днів від 1970-01-01 до заданої дати (алгоритм days_from_civil) */
static long days_from_civil(int y, int m, int d)
{
    if (m <= 2) y--;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static long rtc_minutes_now(void)
{
    RTC_TIME t = rtc_get_time();
    int y = t.year < 100 ? t.year + 2000 : t.year;
    return days_from_civil(y, t.month, t.day) * 1440L + t.hour * 60L + t.minute;
}

void uptime_mark_boot(void)
{
    if (boot_minutes < 0)
        boot_minutes = rtc_minutes_now();
}

/* "2 days" / "1 hour" — з комою між частинами */
static void fetch_append_unit(char *dst, long n, const char *one, const char *many, int *first)
{
    if (!*first)
        fetch_append_cap(dst, ", ", FETCH_LINE_MAX);

    char num[16];
    itoa((int)n, num);
    fetch_append_cap(dst, num, FETCH_LINE_MAX);
    fetch_append_cap(dst, n == 1 ? one : many, FETCH_LINE_MAX);
    *first = 0;
}

static void fetch_build_uptime(void)
{
    long up = 0;
    if (boot_minutes >= 0)
        up = rtc_minutes_now() - boot_minutes;
    if (up < 0)
        up = 0;

    long d = up / 1440, h = (up % 1440) / 60, m = up % 60;
    int first = 1;

    fetch_val_up[0] = '\0';
    if (d) fetch_append_unit(fetch_val_up, d, " day",  " days",  &first);
    if (h) fetch_append_unit(fetch_val_up, h, " hour", " hours", &first);
    if (m || first) fetch_append_unit(fetch_val_up, m, " min", " mins", &first);
}

static void build_fetch_lines(void)
{
    fetch_build_uptime();

    fetch_val_host[0] = '\0';
    fetch_append(fetch_val_host, (char *)hardware_get_computer_name());

    fetch_val_cpu[0] = '\0';
    fetch_append(fetch_val_cpu, (char *)hardware_get_cpu_brand());
    fetch_append(fetch_val_cpu, " (");
    fetch_append_int(fetch_val_cpu, hardware_get_logical_cpus());
    fetch_append(fetch_val_cpu, "C)");

    fetch_val_mem[0] = '\0';
    {
        int usable_mb    = (int)(hardware_get_ram_bytes() / (1024 * 1024));
        int installed_mb = (int)(hardware_get_ram_installed_bytes() / (1024 * 1024));

        if (installed_mb > usable_mb)
        {
            /* "4096 MB (3843 MB usable)" — скільки стоїть фізично і скільки реально доступно */
            fetch_append_int(fetch_val_mem, installed_mb);
            fetch_append(fetch_val_mem, " MB (");
            fetch_append_int(fetch_val_mem, usable_mb);
            fetch_append(fetch_val_mem, " MB usable)");
        }
        else
        {
            fetch_append_int(fetch_val_mem, usable_mb);
            fetch_append(fetch_val_mem, " MB");
        }
    }

    fetch_val_res[0] = '\0';
    fetch_append_int(fetch_val_res, (int)framebuffer_width);
    fetch_append(fetch_val_res, "x");
    fetch_append_int(fetch_val_res, (int)framebuffer_height);

    fetch_collect_disks();
}

static int fetch_info_rows(void)
{
    /* user@host, ----, поля, диски, порожній рядок, 2 рядки палітри */
    return 2 + FETCH_FIELDS + fetch_disk_count + 3;
}

/* Правий стовпчик: рядок i (без переведення рядка в кінці) */
static void fetch_print_info(int i)
{
    int disks_end = 2 + FETCH_FIELDS + fetch_disk_count;

    if (i == 0)
    {
        shell_print_color(FETCH_USER, FETCH_LABEL);
        shell_print_color("@", FETCH_WHITE);
        shell_print_color(FETCH_HOST, FETCH_LABEL);
    }
    else if (i == 1)
    {
        char dashes[40];
        int n = fetch_strlen(FETCH_USER) + 1 + fetch_strlen(FETCH_HOST);
        if (n > (int)sizeof dashes - 1) n = (int)sizeof dashes - 1;
        for (int k = 0; k < n; k++) dashes[k] = '-';
        dashes[n] = '\0';
        shell_print_color(dashes, FETCH_WHITE);
    }
    else if (i < 2 + FETCH_FIELDS)
    {
        int f = i - 2;
        shell_print_color((char *)fetch_labels[f], FETCH_LABEL);
        shell_print_color(": ", FETCH_WHITE);
        shell_print_color((char *)fetch_values[f], FETCH_WHITE);
    }
    else if (i < disks_end)
    {
        shell_print_color("Disk", FETCH_LABEL);
        shell_print_color(": ", FETCH_WHITE);
        shell_print_color(fetch_disk[i - 2 - FETCH_FIELDS], FETCH_WHITE);
    }
    else if (i == disks_end + 1 || i == disks_end + 2)
    {
        int row = i - (disks_end + 1);          /* 0 — темні, 1 — яскраві */
        for (int k = 0; k < 8; k++)
            shell_print_color(FETCH_BLOCK, fetch_palette[row * 8 + k]);
    }
    /* disks_end — порожній рядок між полями й палітрою */
}

void print_fetch(void)
{
    build_fetch_lines();

    int info_rows = fetch_info_rows();
    int rows = (int)LOGO_LINES;
    if (info_rows > rows)
        rows = info_rows;

    shell_print("\n");

    for (int i = 0; i < rows; i++)
    {
        fetch_print_logo_line(i < (int)LOGO_LINES ? logo_lines[i] : "");

        if (i < info_rows)
            fetch_print_info(i);

        shell_print("\n");
    }

    shell_print("\n");
}

void shell_editor_start(const char *filename)
{
    if(!filename || filename[0] == '\0')
    {
        shell_print("Usage: edit <filename>\n");
        return;
    }

    editor_saved_row = shell_row;   // сюди повернемось після виходу з редактора
    editor_saved_col = shell_col;

    console_enter_alt();            // чистий екран, історія шела не займана

    shell_row = 1;                  // одразу під верхньою панеллю
    shell_col = 0;

    editor_mode = 1;
    
    strcpy(editor_filename, filename);
    
    editor_buffer[0] = '\0';
    editor_len = 0;
    editor_line_number = 1;

    FS_FILE *existing = fs_find(filename);

    //малювання нижньої панелі
    draw_bottom_panel(filename, existing == 0);

    shell_print("Edit mode: ");
    shell_print((char *)filename);
    
    if (!existing)
    {
        shell_print("  [create file(");
        shell_print((char *)filename);
        shell_print(")]");
    }

    shell_print("--\n");

    if (existing)
    {
        int i = 0;
        char num_prefix[16];

        itoa(editor_line_number, num_prefix);
        shell_print(num_prefix);
        shell_print("| ");
    
        while (existing->data[i] != '\0' && i < EDITOR_MAX - 1)
        {
            char ch = existing->data[i];
            editor_buffer[i] = ch;

            if (ch == '\n')
            {
                shell_print("\n");   // перехід на новий рядок перед номером
                editor_line_number++;
                itoa(editor_line_number, num_prefix);
                shell_print(num_prefix);
                shell_print("| ");
            }
            else
            {
                char s[2] = {ch, '\0'};
                shell_print(s);
            }

            i++;
            
            editor_buffer[i] = '\0';
            editor_len = i;
        }
    }
}

void shell_editor_save_and_exit(void)
{
    if (!editor_mode) 
        return;   // нема чого зберігати й нема з чого виходити

    FS_FILE *f = fs_get_or_create(editor_filename);
    int save_failed = (f == 0);

    if (f)
    {
        int i = 0;
        while (editor_buffer[i] != '\0' && i < 1023)
        {
            f->data[i] = editor_buffer[i];
            i++;
        }
        f->data[i] = '\0';
        f->size = (unsigned int)i;
    }

    editor_mode = 0;
    editor_len = 0;
    editor_buffer[0] = '\0';

    console_exit_alt();             // повернути старий екран з усіма командами
    shell_row = editor_saved_row;
    shell_col = editor_saved_col;

    if (save_failed)
        shell_print("Error: could not save (storage full)\n");
    else
        shell_print("Saved.\n");

    shell_prompt();
    show_cursor();
}

void shell_editor_cancel(void)
{
    if (!editor_mode) return;

    editor_mode = 0;
    editor_len = 0;
    editor_buffer[0] = '\0';

    console_exit_alt();             // повернути старий екран з усіма командами
    shell_row = editor_saved_row;
    shell_col = editor_saved_col;

    shell_print("Cancelled (not saved).\n");
    shell_prompt();
    show_cursor();
}

void shell_history_add(const char *cmd)
{
    if (cmd[0] == '\0')
        return;   // не зберігаємо порожні команди

    strcpy(history[history_head], cmd);
    history_head = (history_head + 1) % HISTORY_SIZE;

    if (history_count < HISTORY_SIZE)
        history_count++;

    history_pos = -1;
}

void shell_redraw_line(const char *buf, int length, int new_cursor_pos)
{
    hide_cursor();

    for (int i = prompt_start_col; i < SCREEN_COLS; i++)
    {
        print_one_part(" ", shell_row, i);
        cell_set(shell_row, i, ' ', COLOR_WHITE);
    }

    shell_col = prompt_start_col;
    for (int i = 0; i < length; i++)
    {
        char s[2] = {buf[i], '\0'};
        print_one_part(s, shell_row, shell_col);
        cell_set(shell_row, shell_col, buf[i], COLOR_WHITE);
        shell_col++;
    }

    shell_col = prompt_start_col + new_cursor_pos;
    shell_cursor_pos = new_cursor_pos;

    show_cursor();
}

void shell_cursor_left(void)
{
    if (editor_mode) return;

    if (shell_cursor_pos > 0)
    {
        hide_cursor();
        shell_cursor_pos--;
        shell_col = prompt_start_col + shell_cursor_pos;
        show_cursor();
    }
}

void shell_cursor_right(void)
{
    if (editor_mode) return;

    if (shell_cursor_pos < command_index)
    {
        hide_cursor();
        shell_cursor_pos++;
        shell_col = prompt_start_col + shell_cursor_pos;
        show_cursor();
    }
}

void shell_history_up(void)
{
    if (editor_mode || history_count == 0) return;

    if (history_pos == -1)
        history_pos = 0;
    else if (history_pos < history_count - 1)
        history_pos++;
    else
        return;

    int idx = (history_head - 1 - history_pos + HISTORY_SIZE) % HISTORY_SIZE;

    strcpy(command_buffer, history[idx]);
    command_index = strlen(command_buffer);

    shell_redraw_line(command_buffer, command_index, command_index);
}

void shell_history_down(void)
{
    if (editor_mode || history_pos == -1) return;

    if (history_pos == 0)
    {
        history_pos = -1;
        command_buffer[0] = '\0';
        command_index = 0;
        shell_redraw_line(command_buffer, 0, 0);
        return;
    }

    history_pos--;

    int idx = (history_head - 1 - history_pos + HISTORY_SIZE) % HISTORY_SIZE;
    strcpy(command_buffer, history[idx]);
    command_index = strlen(command_buffer);

    shell_redraw_line(command_buffer, command_index, command_index);
}

void power_off(void)
{
    shell_print("Shutting down...\n");

    static int acpi_checked   = 0;
    static int acpi_available = 0;

    if (!acpi_checked)
    {
        acpi_available = acpi_init();
        acpi_checked   = 1;
    }

    if (acpi_available)
        acpi_shutdown();

    outw(0x604,  0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);

    shell_print("Shutdown failed - halting CPU.\n");

    asm volatile("cli");
    while (1)
        asm volatile("hlt");
}

static char savelog_buf[24576];
static uint32_t savelog_len; 

static void savelog_sink(int level, const char *line)
{
    (void)level;
    while(*line && savelog_len < sizeof savelog_buf - 2) savelog_buf[savelog_len++] = *line++;
    savelog_buf[savelog_len++] = '\n';
}

static void cmd_savelog(void)
{
    savelog_len = 0;
    klog_dump(savelog_sink);
    if (usbfs_drive_count() == 0) (usbfs_rescan());
    if (usbfs_drive_count() == 0) { shell_print("savelog: no USB flash drive mounted (FAT32)\n"); return; }

    int saved = 0;
    for (int i = 0; i < usbfs_drive_count(); i++) {
        int d = usbfs_drive_at(i, 0, 0);
        if (d < 0) continue;
        char path[24] = "usb0:/merden.log";
        path[3] = (char)('0' + d);
        int rc = usbfs_write(path, savelog_buf, savelog_len);
        if (rc == 0) { shell_print("saved "); shell_print(path); shell_print("\n"); saved++; }
        else { shell_print("savelog: "); shell_print(path); shell_print(": "); shell_print((char *)usbfs_strerror(rc)); shell_print("\n"); }
    }
    if (!saved) shell_print("savelog: could not write to any drive\n");
}

void shell_execute_command(char* command)
{
    if (usbfs_command(command))
        return;

    if ((strcmp(command, "help") == 0) || (strcmp(command, "help ") == 0))
    {
        shell_print("Avaible commands:\n");
        shell_print("> clear or cls:     -    clear screen\n");
        shell_print("> fastfetch:        -    systeminfo\n");
        shell_print("> about:            -    about of system\n");
        shell_print("> tasks:            -    list scheduler tasks\n");
        shell_print("> tasktest:         -    start two demo background tasks\n");
        shell_print("> png <file>:       -    show a PNG from a USB drive (any key to exit)\n");
        shell_print("> mem:              -    memory info \n");
        shell_print("> echo:             -    print text(in future for files)\n");
        shell_print("> shutdown:         -    shutdown your computer\n");
        shell_print("> interrput:        -    trigger a forced termination of procceses\n");
        shell_print("> cat <file_name>:  -    print file data\n");
        shell_print("> list:             -    list files in the current directory\n");
        shell_print("> create:           -    create a new file in current directory\n");
        shell_print("> doge:             -    show fullscreen image (press ESC to return)\n");
        shell_print("> edit <File name>  -    edit contents file(ctrl + s to save)(ctrl + q to exit)\n");
        shell_print("> hardware:         -    show hardware info\n");
        shell_print("> IDE:              -    IDE Drive info\n");
        shell_print("> mouse:            -    test USB mouse (ESC to stop)\n");
        shell_print("> IDE_debug:        -    Debug IDE device\n");
        shell_print("> run<filename.mrd> -    run merden-file\n");
        shell_print("> usb help          -    USB flash (FAT32/exFAT): usb0: switches drive, then ls/cat/write/rm/mkdir/mv/cp/edit\n");
    }
    else if ((strcmp(command, "clear") == 0) || (strcmp(command, "clear ") == 0))
    {
        clear_screen(0x000000);
        console_reset();
        shell_col = 0;
        shell_row = 1;
        draw_top_panel();
    }
    else if ((strcmp(command, "cls") == 0)   || (strcmp(command, "cls ") == 0))
    {
        clear_screen(0x000000);
        console_reset();
        shell_col = 0;
        shell_row = 1;
        draw_top_panel();
    }
    else if ((strcmp(command, "about") == 0) || (strcmp(command, "about ") == 0))
    {
        shell_print("MerdenOS      -    version 1.0\n");
        shell_print("kernel:       -    Rose, version 1.0\n");
        shell_print("Mode:         -    32bits protected mode\n");
        shell_print("Author:       -    Maksm_y\n");
    }
    else if ((strcmp(command, "interrput") == 0) || (strcmp(command, "interrput ") == 0))
    {
        asm volatile("cli");
        while(1)
        {
            asm("hlt");
        }
    }
    else if ((strcmp(command, "tasks") == 0) || (strcmp(command, "tasks ") == 0))
    {
        cmd_tasks();
    }
    else if (strncmp(command, "png ", 4) == 0 || strcmp(command, "png") == 0)
    {
        cmd_png(command + (command[3] ? 4 : 3));
    }
    else if ((strcmp(command, "tasktest") == 0) || (strcmp(command, "tasktest ") == 0))
    {
        cmd_tasktest();
    }
    else if ((strcmp(command, "mem") == 0) || (strcmp(command, "mem ") == 0))
    {
        shell_print("Memory: basic/RMM draft\n");
    }
    else if((strcmp(command, "time") == 0) || (strcmp(command, "time ") == 0))
    {
        RTC_TIME time = rtc_get_time();

        if (time.hour < 10)
        {
            shell_print("0");
        }

        shell_print_int(time.hour);
        shell_print(":");

        if (time.minute < 10)
        {
            shell_print("0");
        }

        shell_print_int(time.minute);
        shell_print("\n");

    }
    else if (strcmp(command, "dmesg") == 0)
    { 
        klog_dmesg(); 
        return; 
    }
    else if (strcmp(command, "savelog") == 0) 
    { 
        cmd_savelog(); 
        return; 
    }
    else if ((strcmp(command, "date") == 0 ) || (strcmp(command, "date ") == 0))
    {
        RTC_TIME time = rtc_get_time();

        if (time.day < 10)
        {
            shell_print("0");
        }

        shell_print_int(time.day);
        shell_print(".");

        if (time.month < 10)
        {
            shell_print("0");
        }

        shell_print_int(time.month);
        shell_print(".");

        shell_print_int(time.year);

        shell_print("\n");
    }
    else if((strcmp(command, "fetch") == 0) || (strcmp(command, "fetch ") == 0))   
    {
        print_fetch();
    }
    else if ((strcmp(command, "shutdown ") == 0) || (strcmp(command, "shutdown") == 0))
    {
        shell_print("shuting down");
        power_off();
    }
    else if((strncmp(command, "calc ", 5) == 0))
    {
        shell_calculator(command);
    }
    else if((strcmp(command, "fastfetch ") == 0) || (strcmp(command, "fastfetch") == 0))
    {
        print_fetch();
    }
    else if (strcmp(command, "gui") == 0)
    {
        cmd_gui();
        shell_execute_command("cls");
    
    }
    else if ((strcmp(command, "list") == 0) || (strcmp(command, "list ") == 0))
    {

        FS_FILE *list = fs_get_files();

        for(int i =0; i < fs_get_file_count(); i++)   
        {
            shell_print((char *)list[i].name);
            shell_print("\n");
        }
    }
    else if (strncmp(command, "run ", 4) == 0)
    {
        const char *file_name = command + 4;
        const char *data = fs_read(file_name);
        if (data)
            merden_run(data);
        else
            shell_print("File not found\n");
    }
    else if (strncmp(command, "cat ", 4) == 0) 
    {
        const char *file_name = command + 4;
        const char *data = fs_read(file_name);

        if(data) 
        {
            shell_print_utf8(data);
            shell_print("\n");
        }
        else
        {
            shell_print("File not found\n");
        }
    }
    else if ((strcmp(command, "mouse") == 0) || (strcmp(command, "mouse ") == 0))
    {
        cmd_mouse_test();
    }
    else if (strncmp(command, "create ", 7) == 0)
    {
        const char *file_name = command + 7;

        if(fs_create(file_name))
        {
            shell_print("complete!\n");
        }
        else
        {
            shell_print("Failed to complete! :(\n");
        }
    }
    else if (strncmp(command, "edit ", 5) == 0)
    {
        char eb[300];
        shell_editor_start((char *)usbfs_edit_path(command + 5, eb, sizeof eb));
    }
    else if(strcmp(command, "hardware") == 0)
    {
        hardware_print_info();
    }
    else if(strcmp(command, "IDE") == 0)
    {   
        static unsigned char drive_buf[512];
        ide_read_sectors(0, 1, 0, drive_buf);

        if (package[0] == 0)
            t_printf("MBR signature: %x %x\n", drive_buf[510], drive_buf[511]);
        else
            t_printf("ide_read_sectors failed, code %d\n", package[0]);
    }
    else if(strcmp(command, "IDE_debug") == 0)
    {
        int count = 0;
        for (int i = 0; i < 4; i++)
            if (ide_devices[i].Reserved)
                count++;

        if (count == 0)
        {
            t_printf("IDE: no drives found\n");
            return;
        }

        for (int i = 0; i < 4; i++)
        {
            if (!ide_devices[i].Reserved)
                continue;

            t_printf("Found %s Drive %dGB - %s\n",
                (const char *[]){"ATA", "ATAPI"}[ide_devices[i].Type],
                ide_devices[i].Size / 1024 / 1024 / 2,
                ide_devices[i].Model);
        }
    }
    else if (command[0] == 'e' &&
        command[1] == 'c' &&
        command[2] == 'h' &&
        command[3] == 'o' &&
        command[4] == ' ') {
        shell_print_utf8(command + 5);
        shell_print("\n");
    }
    else if (command[0] == '\0') {
        return;
    }
    else
    {

        shell_print("Uncnown command: ");
        shell_print(command);
        shell_print(", Type <help> to see available commands");
        shell_print("\n");
    
    }
}

void shell_restart(void)
{
    hide_cursor();

    command_index = 0;
    shell_cursor_pos = 0;
    command_buffer[0] = '\0';

    shell_print("^C\n");
    shell_prompt();

    show_cursor();
}
