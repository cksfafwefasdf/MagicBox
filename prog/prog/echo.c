#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
// echo hello_world_this_is_my_os -f /test.txt

int main(int argc, char** argv) {
    if ((argc == 2 && !strcmp(argv[1],"-h"))||argc < 2) {
        printf("usage: echo [string] [-f filename]\n");
        return -1;
    }

    int fd = 1;
    int final_argc = argc;

    if (argc >= 4 && strcmp(argv[argc - 2], "-f") == 0) {
        char* path = argv[argc - 1];

        // 第一次尝试：直接打开 (O_WRONLY)
        fd = open(path, O_WRONLY); 

        if (fd == -1) {
            // 第二次尝试：创建 (O_WRONLY | O_CREATE)
            fd = open(path, O_WRONLY | O_CREATE);
        }

        if (fd == -1) {
            printf("echo: open/create %s failed\n", path);
            return -1;
        }
        final_argc = argc - 2;
    }

    for (int i = 1; i < final_argc; i++) {
        write(fd, argv[i], strlen(argv[i]));
        if (i < final_argc - 1) {
            write(fd, " ", 1);
        }
    }

    write(fd, "\n", 1);

    if (fd != 1) {
        close(fd);
    }

    return 0;
}