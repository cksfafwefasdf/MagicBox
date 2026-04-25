#include <syscall.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define TEST_FILE "/io_test.tmp"
#define BUF_SIZE 512       // 每次写入一个块的大小
#define ITERATIONS 50000    // 写入次数

void io_perf_test() {
    char* buf = malloc(BUF_SIZE);
    memset(buf, 'x', BUF_SIZE);

    printf("Starting I/O Performance Test...\n");
    printf("Test Configuration: %d iterations of %d bytes\n\n", ITERATIONS, BUF_SIZE);

    // 连续写入 (测量异步写回速度)
    uint32_t start_time = time();
    
    int32_t fd = open(TEST_FILE, 0x02 | 0x08); // 假设 0x02 为 O_RDWR, 0x08 为 O_CREAT
    if (fd < 0) {
        printf("Error: Failed to open file.\n");
        return;
    }

    printf("Step 1: Writing data to buffer cache...\n");
    for (int i = 0; i < ITERATIONS; i++) {
        write(fd, buf, BUF_SIZE);
        if (i % 500 == 0) printf("  Written %d blocks...\n", i);
    }
    
    uint32_t write_done_time = time();
    uint32_t write_duration = write_done_time - start_time;
    printf(">> Async Write Duration: %d seconds\n", write_duration);

    // 强制同步 (测量真实刷盘速度) 
    printf("\nStep 2: Calling sync() to flush dirty blocks...\n");
    uint32_t sync_start = time();
    
    sync(); // 触发系统调用，唤醒 sync_thread
    
    // 因为 sync 是非阻塞的，这里我们可以紧接着 close
    // 观察控制台是否在持续输出 "sync_thread: write sectors..."
    close(fd);
    
    uint32_t sync_end = time();
    printf(">> sync() Syscall latency: %d seconds (Asynchronous)\n", sync_end - sync_start);

    // 覆盖写 (命中缓存测试) 
    printf("\nStep 3: Overwriting the same block repeatedly...\n");
    fd = open(TEST_FILE, 0x02);
    start_time = time();
    for (int i = 0; i < ITERATIONS * 2; i++) {
        lseek(fd, 0, SEEK_SET); // SEEK_SET
        write(fd, buf, BUF_SIZE);
    }
    close(fd);
    uint32_t overwrite_duration = time() - start_time;
    printf(">> Repeated Overwrite Duration: %d seconds\n", overwrite_duration);

    unlink(TEST_FILE);
    free(buf);
    printf("\nTest Complete.\n");
}

int main(int argc, char** argv) {
    io_perf_test();
    return 0;
}