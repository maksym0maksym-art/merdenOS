[[Kernel-panic]]
#код 
```c
#include "../../headers/vga.h"

#include "../../headers/system.h"

#include "../../headers/kernel-panic.h"

  

#define FOS_ERROR_UNCNOWN 0xF0001

#define FOS_ERROR_SCREEN_POSITION 0xF0002

#define FOS_ERROR_NULL_POINTER 0xF0003

#define FOS_ERROR_VGA 0xF0004

#define FOS_ERROR_BOOT 0xF0005

  

#define STR_BITS(str) ((sizeof(str) -1)* 8)

  

typedef struct

{

int code;

const char *message;

}ErrorInfo;

  

char smile[] = ":(";

  

ErrorInfo ErrorInfoTable[] =

{

{FOS_ERROR_UNCNOWN, "Uncnown_error(0xF0001)"},

{FOS_ERROR_SCREEN_POSITION, "Invalid screen position(0xF0002)"},

{FOS_ERROR_NULL_POINTER, "Kernel didn't see null pointer(0xF0003)"},

{FOS_ERROR_VGA, "VGA ports error(0xF0004)"},

{FOS_ERROR_BOOT, "Ann error occured while booting(0xF0005)"}

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

return "Uncnown error code(0xF006)";

}
```