#ifndef KernelPanic_H
#define KernelPanic_H


const char *get_error_message(int error_code);

void kernel_error(int code, const char* message);

void kernel_panic(int error_code);


#define FOS_ERROR_UNKNOWN 0xF0001
#define FOS_ERROR_SCREEN_POSITION 0xF0002
#define FOS_ERROR_NULL_POINTER 0xF0003
#define FOS_ERROR_VGA 0xF0004
#define FOS_ERROR_BOOT 0xF0005


#endif