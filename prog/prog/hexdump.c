#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"

#define LINE_SIZE 16
#define SCREEN_LINES 16  // 每显示 16 行暂停一次

int32_t line_count = 0;

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: hd <file_or_dev> [offset] [length]\n");
        exit(-1);
    }

    char* path = argv[1];
    int32_t offset = 0;
    int32_t length = 256;

    if (argc > 2) {
        if (!atoi_dep(argv[2], &offset)) {
            printf("hd: invalid offset %s\n", argv[2]);
            exit(-1);
        }
    }
    if (argc > 3) {
        if (!atoi_dep(argv[3], &length)) {
            printf("hd: invalid length %s\n", argv[3]);
            exit(-1);
        }
    }

    int32_t fd = open(path, O_RDONLY);

    if (fd == -1) {
        printf("hd: open %s failed!\n", path);
        return -1;
    }

    if (lseek(fd, offset, SEEK_SET) == -1) {
        printf("hd: lseek to %d failed!\n", offset);
        close(fd);
        return -1;
    }

    uint8_t* buf = malloc(length);
    if (!buf) {
        printf("hd: malloc failed\n");
        close(fd);
        return -1;
    }

    int32_t read_bytes = read(fd, buf, length);
    if (read_bytes <= 0) {
        printf("hd: read failed or EOF\n");
    } else {
        printf("Dumping %s: %d bytes from offset %d\n", path, read_bytes, offset);
        
        for (int32_t i = 0; i < read_bytes; i++) {
            if (i % LINE_SIZE == 0) {
                // 分页逻辑
                if (line_count != 0 && line_count % SCREEN_LINES == 0) {
                    printf("\n-- press any key to continue --");
                    char pause_buf[2];
                    read(0, pause_buf, 1); // 从标准输入（键盘）读一个字符，起到阻塞作用
                }

                char addr_buf[16];
                itoa(offset + i, addr_buf, 16);
                printf("\n%s: ", addr_buf);
                line_count++;
            }

            char hex_buf[16];
            itoa(buf[i], hex_buf, 16);
            // 手动补齐两位对齐，如果 itoa 不支持自动补 0
            if (buf[i] < 0x10) printf("0"); 
            printf("%s ", hex_buf);

            if ((i + 1) % 8 == 0 && (i + 1) % LINE_SIZE != 0) {
                printf(" ");
            }

            if ((i + 1) % LINE_SIZE == 0 || (i + 1) == read_bytes) {
                // 最后一行补空格对齐 ASCII 预览
                if ((i + 1) == read_bytes && (read_bytes % LINE_SIZE != 0)) {
                    int spaces = LINE_SIZE - (read_bytes % LINE_SIZE);
                    for (int s = 0; s < spaces; s++) printf("   ");
                    if (spaces >= 8) printf(" ");
                }
                printf(" |");
                int start = (i / LINE_SIZE) * LINE_SIZE;
                for (int j = start; j <= i; j++) {
                    if (buf[j] >= 32 && buf[j] <= 126) printf("%c", buf[j]);
                    else printf(".");
                }
                printf("|");
            }
        }
        printf("\n");
    }

    free(buf);
    close(fd);
    return 0;
}