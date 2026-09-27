#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>

void welcome_to_Merden();

void shell_print(char* text);

void shell_init();

void shell_init();

void move_cursor(int x, int y);

static inline void outb(uint16_t port, uint8_t val);

void shell_putchar(char c);

void shell_backspace();

void shell_enter();

void shell_execute_command(char* command);

int strncmp(const char *a, const char *b, int n);

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

void shell_prompt();

void shell_restart(void);

void show_cursor();

void shell_print_color(char *text, uint32_t color);

void hide_cursor();

void blink_cursor(void);

void shell_editor_start(const char *file_name);

void shell_editor_save_and_exit(void);

void shell_editor_cancel(void);

void shell_cursor_left(void);

void shell_cursor_right(void);

void shell_history_up(void);

void shell_history_down(void);

void shell_history_add(const char *cmd);

void shell_redraw_line(const char *buf, int length, int new_cursor_pos);
#endif