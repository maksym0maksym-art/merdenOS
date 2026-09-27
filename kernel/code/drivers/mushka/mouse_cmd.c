#include "../../../headers/mouse_cmd.h"
#include "../../../headers/head_mouse.h"
#include "../io.h"
#include "../../../../standarts_lib/stdio.h"

static hid_mouse_t g_shell_mouse;
static int g_shell_mouse_ready = 0;

void cmd_mouse_test(void)
{
    if (!g_shell_mouse_ready)
    {
        shell_print("Init USB mouse...\n");

        if (!hid_mouse_init(&g_shell_mouse))
        {
            shell_print("Mouse not found (no UHCI controller or no device on port)\n");
            return;
        }

        g_shell_mouse_ready = 1;
        t_printf("Mouse ready. endpoint=%d max_packet=%d\n",
                  g_shell_mouse.endpoint_addr, g_shell_mouse.max_packet);
    }

    shell_print("Polling mouse, press ESC to stop...\n");

    mouse_report_t report;

    for (;;)
    {
        if (inb(0x64) & 1)
        {
            unsigned char scancode = inb(0x60);
            if (scancode == 0x01)
            {
                shell_print("Stopped.\n");
                break;
            }
        }

        if (hid_mouse_poll(&g_shell_mouse, &report))
        {
            t_printf("dx=%d dy=%d btn=%c%c%c wheel=%d\n",
                      report.dx, report.dy,
                      (report.buttons & 0x01) ? 'L' : '-',
                      (report.buttons & 0x02) ? 'R' : '-',
                      (report.buttons & 0x04) ? 'M' : '-',
                      report.wheel);
        }
    }
}