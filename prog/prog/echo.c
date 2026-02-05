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
    char abs_path[512] = {0}; // 用于存储合成后的绝对路径

    if (argc >= 4 && strcmp(argv[argc - 2], "-f") == 0) {
        char* filename = argv[argc - 1];

        // --- 核心：相对路径转绝对路径逻辑 ---
        if (filename[0] != '/') {
            if (getcwd(abs_path, 512) == NULL) {
                printf("echo: getcwd failed\n");
                return -1;
            }
            // 确保不是在根目录下重复加斜杠
            if (strcmp(abs_path, "/") != 0) {
                strcat(abs_path, "/");
            }
            strcat(abs_path, filename);
        } else {
            strcpy(abs_path, filename);
        }

        // 第一次尝试：直接打开 (O_WRONLY)
        fd = open(abs_path, O_WRONLY); 

        if (fd == -1) {
            // 第二次尝试：创建 (O_WRONLY | O_CREATE)
            fd = open(abs_path, O_WRONLY | O_CREATE);
        }

        if (fd == -1) {
            printf("echo: open/create %s failed\n", abs_path);
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