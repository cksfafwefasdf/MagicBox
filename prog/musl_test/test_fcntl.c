#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

// 打印标志位的辅助函数
void print_flags(const char* label, int flags) {
    printf("%s: 0x%x (", label, flags);
    if ((flags & 3) == O_RDONLY) printf("O_RDONLY ");
    if (flags & O_APPEND)        printf("O_APPEND ");
    if (flags & O_NONBLOCK)      printf("O_NONBLOCK ");
    if (flags & O_CREAT)         printf("O_CREAT ");
    printf(")\n");
}

int main() {
    printf("=== Syscall Translation Layer Test ===\n");

    // 测试 open 的翻译
    // 故意带上多个标志位：Linux O_RDWR(2) | O_APPEND(0x400) | O_NONBLOCK(0x800)
    int fd = open("/dev/tty0", O_RDWR | O_APPEND | O_NONBLOCK);
    if (fd < 0) {
        printf("Open failed! fd: %d\n", fd);
        return 1;
    }

    // 获取标志位，验证内核是否成功转回了 Linux 格式
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        printf("F_GETFL failed!\n");
    } else {
        print_flags("After Open (F_GETFL)", flags);
        
        // 验证关键位
        if ((flags & O_NONBLOCK) && (flags & O_APPEND) && ((flags & 3) == O_RDWR)) {
            printf(">> SUCCESS: Open flags translated and recovered perfectly.\n");
        } else {
            printf(">> ERROR: Flags mismatch! Check your do_open or do_fcntl(F_GETFL).\n");
        }
    }

    // 测试 F_SETFL 的翻译
    // 尝试在运行时去掉 O_APPEND，加上 O_NONBLOCK
    printf("\nSetting flags to O_RDWR | O_NONBLOCK (removing O_APPEND)...\n");
    if (fcntl(fd, F_SETFL, O_RDWR | O_NONBLOCK) == -1) {
        printf("F_SETFL failed!\n");
    } else {
        int new_flags = fcntl(fd, F_GETFL);
        print_flags("After SETFL (F_GETFL)", new_flags);

        if ((new_flags & O_NONBLOCK) && !(new_flags & O_APPEND)) {
            printf(">> SUCCESS: F_SETFL translation works.\n");
        } else {
            printf(">> ERROR: F_SETFL failed to update flags correctly.\n");
        }
    }

    close(fd);
    return 0;
}