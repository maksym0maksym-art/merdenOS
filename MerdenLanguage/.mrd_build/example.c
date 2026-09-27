#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

int main(void) {
    #ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    #endif
    const char * md_value_0 = "Maksm_y";
    printf("Hello world, %s\n", md_value_0);
    if ((strcmp(md_value_0, "Maksm_y") == 0)) {
        printf("%s\n", "Привіт, Макс");
    }
    else {
        printf("%s\n", "Яке твоє ім'я?");
    }
    for (long long md_loop_0 = 0; md_loop_0 < (3); ++md_loop_0) {
        printf("%s\n", "Hello");
    }
    return 0;
}
