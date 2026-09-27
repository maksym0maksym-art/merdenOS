#include "../../headers/keyboard.h"
#include "../../headers/vga.h"
#include "../../../system/shell/shell.h"
#include "../../headers/framebuffer.h"
#include "../../headers/system.h"
#include "../../../system/shell/graphic/console.h"
#include "../../../standarts_lib/stdio.h"
#include "io.h"

#define LEFT_SHIFT_PRESS 0x2A
#define LEFT_SHIFT_RELEASE 0xAA

#define LEFT_CTRL_PRESS    0x1D
#define LEFT_CTRL_RELEASE  0x9D

#define ESC_PRESS          0x01

#define CAPS_LOCK_PRESSED 0x3a 

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

		if(inb(0x64) & 1) 
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
				if (scancode == LEFT_SHIFT_RELEASE)
				{
					left_shift_pressed = false;
                    outb(0x20, 0x20);
				}
				if (scancode == LEFT_CTRL_RELEASE)
                {
                    left_ctrl_pressed = false;
                    outb(0x20, 0x20);
                }

                return ;
			}

			if (scancode == LEFT_SHIFT_PRESS) 
			{
				left_shift_pressed = true;
                return;
                outb(0x20, 0x20);
			}
			if (scancode == LEFT_CTRL_PRESS)
            {
                left_ctrl_pressed = true;
                return;
                outb(0x20, 0x20);
            }
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

			char c = 0;

			if(left_shift_pressed)
			{
				c = keyboard_shift_map[scancode];
			}
			else if(capslock_enabled) 
			{
				c = keyboard_caps_map[scancode];
			}
			else
			{
				c = keyboard_map[scancode];
			}
            if (c)
            {
                shell_putchar(c);
            }
		}
	}