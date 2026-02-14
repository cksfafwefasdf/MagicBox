#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "stdint.h"

// 测试具名管道的读写以及观察后台进程

int main() {
    printf("FIFO Test Start...\n");

	signal(2, (void*)0);
    // 确保创建一个 FIFO
    mkfifo("/data/fifo_test");
	
    pid_t pid_writer = fork();
	
    if (pid_writer == 0) {
        // 子进程 A，写者 
        printf("Writer Process (PID:%d) attempting to open FIFO...\n", getpid());
        int32_t fd = open("/data/fifo_test", O_WRONLY);
        if (fd != -1) {
            printf("Writer Process: Opened FIFO, writing data...\n");
            write(fd, "Message from Writer!", 21);
            close(fd);
            printf("Writer Process: Data sent and closed.\n");
        }
        exit(0);
    }

    pid_t pid_reader = fork();
    if (pid_reader == 0) {
        // 子进程 B：读者 
        printf("Reader Process (PID:%d) attempting to open FIFO...\n", getpid());
        int32_t fd = open("/data/fifo_test", O_RDONLY);
        if (fd != -1) {
            printf("Reader Process: Opened FIFO, reading data...\n");
            char buf[64] = {0};
            read(fd, buf, 21);
            printf("Reader Process: Received: %s\n", buf);
            close(fd);
        }
        exit(0);
    }

    // 父进程等待两个孩子

	// 加一个耗时操作，这样的话，当我们运行 prog_pipe & 时，按下回车
	// 我们就有时间使用ps命令看到各个后台进程，否则执行的太快了我们都没时间输入 ps
	for(volatile uint32_t i = 0; i < 1000000000; i++); 

    int status;
    wait(&status);
    wait(&status);

    printf("FIFO Test Finished. Cleaning up...\n");
}