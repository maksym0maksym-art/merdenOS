#include "../../headers/keyboard.h"
#include "../../headers/vga.h"
#include "../../../system/shell/shell.h"
#include "../../headers/framebuffer.h"
#include "../../headers/system.h"
#include "../../../system/shell/graphic/console.h"
#include "../../../standarts_lib/stdio.h"
#include "io.h"
#include "../../headers/mouse.h"
#include "../../../standarts_lib/charset.h"

#define LEFT_SHIFT_PRESS 0x2A
#define LEFT_SHIFT_RELEASE 0xAA

#define RIGHT_SHIFT_PRESS   0x36
#define RIGHT_SHIFT_RELEASE 0xB6
#define ALT_PRESS           0x38
#define ALT_RELEASE         0xB8

#define LEFT_CTRL_PRESS    0x1D
#define LEFT_CTRL_RELEASE  0x9D

#define ESC_PRESS          0x01

#define CAPS_LOCK_PRESSED 0x3a 

extern void draw_top_panel(void);

#define true 1
#define false 0

void wait(long c)
{
    for (volatile uint32_t i = 0; i < c; i++);
}

char keyboard_map[128] = {
    [0x01] = 27,

    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',
    [0x0D] = '=',

    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1A] = '[',
    [0x1B] = ']',

    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = ';',
    [0x28] = '\'',
    [0x29] = '`',

    [0x2B] = '\\',

    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',

    [0x39] = ' ',
};

char keyboard_shift_map[128] = {
    [0x02] = '!',
    [0x03] = '@',
    [0x04] = '#',
    [0x05] = '$',
    [0x06] = '%',
    [0x07] = '^',
    [0x08] = '&',
    [0x09] = '*',
    [0x0A] = '(',
    [0x0B] = ')',

    [0x0C] = '_',
    [0x0D] = '+',

    [0x10] = 'Q',
    [0x11] = 'W',
    [0x12] = 'E',
    [0x13] = 'R',
    [0x14] = 'T',
    [0x15] = 'Y',
    [0x16] = 'U',
    [0x17] = 'I',
    [0x18] = 'O',
    [0x19] = 'P',

    [0x1A] = '{',
    [0x1B] = '}',

    [0x1E] = 'A',
    [0x1F] = 'S',
    [0x20] = 'D',
    [0x21] = 'F',
    [0x22] = 'G',
    [0x23] = 'H',
    [0x24] = 'J',
    [0x25] = 'K',
    [0x26] = 'L',

    [0x27] = ':',
    [0x28] = '"',
    [0x29] = '~',

    [0x2B] = '|',

    [0x2C] = 'Z',
    [0x2D] = 'X',
    [0x2E] = 'C',
    [0x2F] = 'V',
    [0x30] = 'B',
    [0x31] = 'N',
    [0x32] = 'M',

    [0x33] = '<',
    [0x34] = '>',
    [0x35] = '?',

    [0x39] = ' ',
};


char keyboard_caps_map[128] = {
    [0x10] = 'Q',
    [0x11] = 'W',
    [0x12] = 'E',
    [0x13] = 'R',
    [0x14] = 'T',
    [0x15] = 'Y',
    [0x16] = 'U',
    [0x17] = 'I',
    [0x18] = 'O',
    [0x19] = 'P',

    [0x1E] = 'A',
    [0x1F] = 'S',
    [0x20] = 'D',
    [0x21] = 'F',
    [0x22] = 'G',
    [0x23] = 'H',
    [0x24] = 'J',
    [0x25] = 'K',
    [0x26] = 'L',

    [0x2C] = 'Z',
    [0x2D] = 'X',
    [0x2E] = 'C',
    [0x2F] = 'V',
    [0x30] = 'B',
    [0x31] = 'N',
    [0x32] = 'M',
};


/* Українська розкладка (як у Windows), байти CP1251 */
char keyboard_map_ua[128] = {
    [0x01] = 27,
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',
    [0x0D] = '=',
    [0x10] = (char)0xE9,   /* й */
    [0x11] = (char)0xF6,   /* ц */
    [0x12] = (char)0xF3,   /* у */
    [0x13] = (char)0xEA,   /* к */
    [0x14] = (char)0xE5,   /* е */
    [0x15] = (char)0xED,   /* н */
    [0x16] = (char)0xE3,   /* г */
    [0x17] = (char)0xF8,   /* ш */
    [0x18] = (char)0xF9,   /* щ */
    [0x19] = (char)0xE7,   /* з */
    [0x1A] = (char)0xF5,   /* х */
    [0x1B] = (char)0xBF,   /* ї */
    [0x1E] = (char)0xF4,   /* ф */
    [0x1F] = (char)0xB3,   /* і */
    [0x20] = (char)0xE2,   /* в */
    [0x21] = (char)0xE0,   /* а */
    [0x22] = (char)0xEF,   /* п */
    [0x23] = (char)0xF0,   /* р */
    [0x24] = (char)0xEE,   /* о */
    [0x25] = (char)0xEB,   /* л */
    [0x26] = (char)0xE4,   /* д */
    [0x27] = (char)0xE6,   /* ж */
    [0x28] = (char)0xBA,   /* є */
    [0x29] = '\'',
    [0x2B] = (char)0xB4,   /* ґ */
    [0x2C] = (char)0xFF,   /* я */
    [0x2D] = (char)0xF7,   /* ч */
    [0x2E] = (char)0xF1,   /* с */
    [0x2F] = (char)0xEC,   /* м */
    [0x30] = (char)0xE8,   /* и */
    [0x31] = (char)0xF2,   /* т */
    [0x32] = (char)0xFC,   /* ь */
    [0x33] = (char)0xE1,   /* б */
    [0x34] = (char)0xFE,   /* ю */
    [0x35] = '.',
    [0x39] = ' ',
};

char keyboard_shift_map_ua[128] = {
    [0x02] = '!',
    [0x03] = '"',
    [0x04] = (char)0xB9,   /* № */
    [0x05] = ';',
    [0x06] = '%',
    [0x07] = ':',
    [0x08] = '?',
    [0x09] = '*',
    [0x0A] = '(',
    [0x0B] = ')',
    [0x0C] = '_',
    [0x0D] = '+',
    [0x10] = (char)0xC9,   /* Й */
    [0x11] = (char)0xD6,   /* Ц */
    [0x12] = (char)0xD3,   /* У */
    [0x13] = (char)0xCA,   /* К */
    [0x14] = (char)0xC5,   /* Е */
    [0x15] = (char)0xCD,   /* Н */
    [0x16] = (char)0xC3,   /* Г */
    [0x17] = (char)0xD8,   /* Ш */
    [0x18] = (char)0xD9,   /* Щ */
    [0x19] = (char)0xC7,   /* З */
    [0x1A] = (char)0xD5,   /* Х */
    [0x1B] = (char)0xAF,   /* Ї */
    [0x1E] = (char)0xD4,   /* Ф */
    [0x1F] = (char)0xB2,   /* І */
    [0x20] = (char)0xC2,   /* В */
    [0x21] = (char)0xC0,   /* А */
    [0x22] = (char)0xCF,   /* П */
    [0x23] = (char)0xD0,   /* Р */
    [0x24] = (char)0xCE,   /* О */
    [0x25] = (char)0xCB,   /* Л */
    [0x26] = (char)0xC4,   /* Д */
    [0x27] = (char)0xC6,   /* Ж */
    [0x28] = (char)0xAA,   /* Є */
    [0x29] = '~',
    [0x2B] = (char)0xA5,   /* Ґ */
    [0x2C] = (char)0xDF,   /* Я */
    [0x2D] = (char)0xD7,   /* Ч */
    [0x2E] = (char)0xD1,   /* С */
    [0x2F] = (char)0xCC,   /* М */
    [0x30] = (char)0xC8,   /* И */
    [0x31] = (char)0xD2,   /* Т */
    [0x32] = (char)0xDC,   /* Ь */
    [0x33] = (char)0xC1,   /* Б */
    [0x34] = (char)0xDE,   /* Ю */
    [0x35] = ',',
    [0x39] = ' ',
};


/* ---------- розкладки ---------- */

int keyboard_layout = KBD_LAYOUT_EN;

/*
 * Скан-код -> символ (CP1251) з урахуванням активної розкладки.
 *  shift       — затиснутий Shift;
 *  caps        — CapsLock (міняє регістр тільки в літерних клавіш, цифри й розділові не чіпає);
 *  force_latin — Ctrl/Alt затиснуті: гарячі клавіші завжди латиницею, незалежно від розкладки.
 */
char keyboard_translate(unsigned char sc, int shift, int caps, int force_latin)
{
    if (sc >= 128)
        return 0;

    const char *base = keyboard_map;
    const char *shf  = keyboard_shift_map;

    if (keyboard_layout == KBD_LAYOUT_UA && !force_latin)
    {
        base = keyboard_map_ua;
        shf  = keyboard_shift_map_ua;
    }

    int upper = shift ? 1 : 0;
    if (caps && cs_is_letter((unsigned char)base[sc]))
        upper = !upper;

    char c = upper ? shf[sc] : base[sc];
    if (!c && upper)
        c = base[sc];
    return c;
}

/*
 * Перемикання розкладки по Ctrl+Shift або Alt+Shift (як у Windows): спрацьовує, коли
 * ці модифікатори відпущено, якщо між ними не натискали жодної іншої клавіші.
 * Викликати після кожної зміни модифікаторів (other_key = 0) і при кожному натисканні
 * звичайної клавіші (other_key = 1). Повертає 1, якщо розкладку щойно змінено.
 */
int keyboard_layout_track(int ctrl, int alt, int shift, int other_key)
{
    static int armed = 0;

    if (other_key)
    {
        armed = 0;
        return 0;
    }

    if (shift && (ctrl || alt))
    {
        armed = 1;
        return 0;
    }

    if (armed)
    {
        armed = 0;
        keyboard_layout ^= 1;
        return 1;
    }

    return 0;
}


unsigned char inb(unsigned short port) 
{
	unsigned char result;

	asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));

	return result;
}

void keyboard_handler() 
{
	static int left_shift_pressed = false;
	static int capslock_enabled = false;
    static int left_ctrl_pressed = false;
    static int alt_pressed = false;

		unsigned char kbd_status = inb(0x64);

		/* Клавіатура й PS/2-мишка ділять один вихідний буфер. Біт 0x20 = байт від мишки:
		 * віддаємо його драйверу мишки, інакше він вважався б скан-кодом клавіші. */
		if ((kbd_status & 0x21) == 0x21)
		{
			ps2_mouse_feed(inb(0x60));
			return;
		}

		if(kbd_status & 1) 
		{
			unsigned char scancode = inb(0x60);

            static int e0 = 0;

            if (scancode == 0xE0)
            {
                e0 = 1;
                return;
            }

            if (e0)
            {
                e0 = 0;

                switch (scancode)
                {
                    case 0x48:
                        if (left_ctrl_pressed)
                            console_scroll_view(+1);
                        else
                            shell_history_up();
                        outb(0x20, 0x20);
                        break;
 
                    case 0x50:
                        if (left_ctrl_pressed)
                            console_scroll_view(-1);
                        else
                            shell_history_down();
                        outb(0x20, 0x20);
                        break;

                    case 0x4B: // ←
                        shell_cursor_left();
                        outb(0x20, 0x20);
                        break;

                    case 0x4D: // →
                        shell_cursor_right();
                        outb(0x20, 0x20);
                        break;
                        case 0x49:
                        console_scroll_view(+VIEW_ROWS / 2);
                        outb(0x20, 0x20);
                        break;
 
                    case 0x51:
                        console_scroll_view(-VIEW_ROWS / 2);
                        outb(0x20, 0x20);
                        break;

                }
                return;
            }

			if (scancode & 0x80)
			{
				int mod = 1;

				if (scancode == LEFT_SHIFT_RELEASE || scancode == RIGHT_SHIFT_RELEASE)
					left_shift_pressed = false;
				else if (scancode == LEFT_CTRL_RELEASE)
					left_ctrl_pressed = false;
				else if (scancode == ALT_RELEASE)
					alt_pressed = false;
				else
					mod = 0;

				if (mod && keyboard_layout_track(left_ctrl_pressed, alt_pressed, left_shift_pressed, 0))
					draw_top_panel();   /* оновити індикатор розкладки */

				outb(0x20, 0x20);
				return ;
			}

			if (scancode == LEFT_SHIFT_PRESS || scancode == RIGHT_SHIFT_PRESS)
			{
				left_shift_pressed = true;
				keyboard_layout_track(left_ctrl_pressed, alt_pressed, left_shift_pressed, 0);
				return;
			}
			if (scancode == LEFT_CTRL_PRESS)
            {
                left_ctrl_pressed = true;
                keyboard_layout_track(left_ctrl_pressed, alt_pressed, left_shift_pressed, 0);
                return;
            }
			if (scancode == ALT_PRESS)
			{
				alt_pressed = true;
				keyboard_layout_track(left_ctrl_pressed, alt_pressed, left_shift_pressed, 0);
				return;
			}

			keyboard_layout_track(0, 0, 0, 1);  
			if (scancode == CAPS_LOCK_PRESSED)
			{
				capslock_enabled = !capslock_enabled;
                return ;
                outb(0x20, 0x20);
			}
            if (scancode == 0x0E)
            {
                shell_backspace();
                return;
                shell_print("s");
                outb(0x20, 0x20);
            }
            if (scancode == 0x1C)
            {
                shell_enter();
                return;
                outb(0x20, 0x20);
            }

            if (left_ctrl_pressed)
            {
                switch(scancode)
                {
                    case 0x26:
                        shell_execute_command("cls");
                        shell_prompt();
                        return;
                        outb(0x20, 0x20);

                    case 0x2E:
                        shell_restart();
                        outb(0x20, 0x20);
                        
                    case 0x1F: // Ctrl + S (Зберегти і вийти)
                        shell_editor_save_and_exit();
                        outb(0x20, 0x20);
                        return;

                    case 0x10: // Ctrl + Q (Скасувати і вийти)
                        shell_editor_cancel();
                        outb(0x20, 0x20);
                        return;

                }
            }

			char c = keyboard_translate(scancode, left_shift_pressed, capslock_enabled,
			                            left_ctrl_pressed || alt_pressed);
            if (c)
            {
                shell_putchar(c);
            }
		}
	}