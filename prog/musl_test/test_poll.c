#include <stdio.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>

int main() {
    // 将标准输入设置为非阻塞模式
    // 这是配合 poll 使用的标准操作
    int flags = fcntl(0, F_GETFL, 0);
    fcntl(0, F_SETFL, flags | O_NONBLOCK);

    printf("--- TTY Poll Test Start ---\n");
    printf("Hint: Type something and press Enter. Both Father and Son are watching!\n");

    pid_t pid = fork();

    // 父子进程运行相同的逻辑：用 poll 监控 stdin
    struct pollfd fds[1];
    fds[0].fd = 0;          // 标准输入
    fds[0].events = POLLIN; // 关注读就绪

    for (int i = 0; i < 5; i++) {
        // 等待事件，超时时间为 5000ms
        int ret = poll(fds, 1, 5000);

        if (ret < 0) {
            perror("poll error");
            break;
        } else if (ret == 0) {
            printf("[%s] Poll timeout, still waiting...\n", pid == 0 ? "Son" : "Father");
            continue;
        }

        // 检查是否是真的读就绪
        if (fds[0].revents & POLLIN) {
            char buf[64];
            memset(buf, 0, sizeof(buf));
            
            // 尝试读取
            ssize_t n = read(0, buf, sizeof(buf) - 1);
            if (n > 0) {
                printf("[%s] Got it! Content: %s", pid == 0 ? "Son" : "Father", buf);
            } else if (n < 0) {
                if (errno == EAGAIN) {
                    // 这种情况就是你担心的：被另一个进程抢先读走了！
                    printf("[%s] Ah! Someone else took the data! (EAGAIN)\n", 
                           pid == 0 ? "Son" : "Father");
                } else {
                    perror("read error");
                }
            }
        }
    }

    if (pid != 0) {
        wait(NULL);
        printf("--- Test Finished ---\n");
    }

    return 0;
}