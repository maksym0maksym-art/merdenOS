#include "../../headers/vga.h"
#include "../../headers/system.h"
#include "../../headers/kernel-panic.h"


#define STR_BITS(str) ((sizeof(str) -1)* 8)

typedef struct
{
	int code;
	const char *message;
}ErrorInfo;

char smile[] = ":(";

ErrorInfo ErrorInfoTable[] = 
{
	{FOS_ERROR_UNKNOWN, "Unknown_error(0xF0001)"},
	{FOS_ERROR_SCREEN_POSITION, "Invalid screen position(0xF0002)"},
	{FOS_ERROR_NULL_POINTER, "Kernel didn't see null pointer(0xF0003)"},
	{FOS_ERROR_VGA, "VGA ports error(0xF0004)"},
	{FOS_ERROR_BOOT, "Ann error occurred while booting(0xF0005)"}
};

const char *get_error_message(int error_code)
{
	for(int i = 0;i < 5;i++)
{
		if (ErrorInfoTable[i].code == error_code)
		{
			return ErrorInfoTable[i].message;
		}
	}
	return "Unknown error code(0xF006)";
}

void kernel_error(int code, const char* message)
{
	print_center("Error: ", 12);
	print_center(message, 13);
}

void kernel_panic(int error_code) 
{
	clear_screen(0x000000);
	asm volatile("cli");
	//*disable_cursor();

	const char *msg = get_error_message(error_code);

	print_center("KERNEL PANIC!", 20);
	print_center("Please, rebot youre computer", 21);
	print_center(msg, 22);

	asm volatile("hlt");
}