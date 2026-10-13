#include <stdint.h>
#include "../gui.h"
#include "../paint.h"
#include "../explorer.h"
#include "../../../kernel/headers/mouse.h"
#include "../../../kernel/headers/framebuffer.h"
#include "../../../kernel/images/wallpaper.h"
#include "../../shell/shell.h"
#include "../../shell/graphic/console.h"

/* Команда `gui` у шелі: стільниця зі шпалерами, панель завдань і меню «Пуск». */

static void launch_terminal(void) { gui_terminal_open(); }
static void launch_editor(void)   { gui_editor_open("untitled.txt"); }
static void launch_calc(void)     { gui_calc_open(); }
static void launch_paint(void)    { paint_open(); }
static void launch_explorer(void) { gui_explorer_open(); }
static void exit_gui(void)        { gui_quit(); }

void cmd_gui(void) {
    static int mouse_ready = 0;
    if (!mouse_ready) {
        shell_print("Init mouse...\n");
        if (!mouse_init()) {
            shell_print("GUI needs a mouse: USB UHCI or PS/2 not found\n");
            return;
        }
        mouse_ready = 1;
    }

    console_enter_alt();

    if (!gui_init()) {
        console_exit_alt();
        shell_print("GUI needs a 32-bit framebuffer up to 1920x1080\n");
        return;
    }

    mouse_set_bounds((int32_t)framebuffer_width, (int32_t)framebuffer_height);
    gui_set_wallpaper_bmp(wallpaper_data, wallpaper_size);
    gui_set_launcher(launch_terminal);              /* Ctrl+Alt+T */

    gui_menu_add_ex("Terminal",    GUI_ICON_TERMINAL, launch_terminal);
    gui_menu_add_ex("Text Editor", GUI_ICON_EDITOR,   launch_editor);
    gui_menu_add_ex("Calculator",  GUI_ICON_CALC,     launch_calc);
    gui_menu_add_ex("Paint",       GUI_ICON_PAINT,    launch_paint);
    gui_menu_add_ex("Explorer",    GUI_ICON_FOLDER,   launch_explorer);
    gui_menu_add_ex("Exit GUI",    GUI_ICON_POWER,    exit_gui);

    mouse_set_pos((int32_t)(framebuffer_width / 2), (int32_t)(framebuffer_height / 2));
    mouse_cursor_show();
    gui_run();
    mouse_cursor_hide();

    console_exit_alt();
    shell_print("GUI closed.\n");
}