#include <stdio.h>
#include <stdint.h>
#include <syscall.h>
#include <termios.h>
#include <ioctl.h>
#include <unitype.h>

int main() {
    struct termios old_term, new_term;

    // 获取当前 TTY 的属性
    if (ioctl(stdin_no, TCGETS, (uint32_t)&old_term) == -1) {
        printf("Error: Cannot get termios\n");
        return 1;
    }

    // 设置新属性：关闭规范模式 (ICANON) 和回显 (ECHO)
    new_term = old_term;
    new_term.c_lflag &= ~(ICANON | ECHO);

    if (ioctl(stdin_no, TCSETS, (uint32_t)&new_term) == -1) {
        printf("Error: Cannot set termios\n");
        return 1;
    }

    printf("--- Raw Mode Test (Press 'q' to quit) ---\n");
    printf("Try pressing Arrow Keys to see ANSI sequences:\n");

    char c;
    while (1) {
        // 在非规范模式下，read 应该在按下按键后立刻返回，不需要等回车
        int n = read(stdin_no, &c, 1);
        if (n > 0) {
            if (c == 'q') break;

            // 打印字符的十六进制值，方便观察转义序列
            // 比如向上键你会看到: 1b 5b 41
            if (c < 32 || c > 126) {
                printf("[0x%x] ", (uint8_t)c);
            } else {
                printf("%c ", c);
            }

        }
    }

    // 恢复原始设置（非常重要，否则 Shell 也会变不正常）
    ioctl(stdin_no, TCSETS, (uint32_t)&old_term);
    printf("\n--- Test Finished ---\n");

    return 0;
}