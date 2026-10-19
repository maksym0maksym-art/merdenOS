#ifndef ROSE_API_H
#define ROSE_API_H

#include <stdint.h>

#define ROSE_API_VERSION 1u
#define MEXE_FLAG_ROSE_API (1u << 0)

typedef struct {
    uint32_t version;
    uint32_t struct_size;

    void (*write)(const char *text);

} rose_api_t;

typedef int (*rose_program_api_t)(const rose_api_t *api);

#endif