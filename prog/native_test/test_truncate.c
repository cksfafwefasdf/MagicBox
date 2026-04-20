#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <unitype.h>

// 块大小为 1024 字节 (1KB)
#define BLOCK_SIZE 1024
// 写入 15 个块，超过 Ext2 的 12 个直接块限制，会用到 1 个一级间接块
#define TEST_BLOCKS 15
#define TEST_SIZE (TEST_BLOCKS * BLOCK_SIZE)

void print_fs_status() {
    struct statfs sfs;
    if (statfs("/", &sfs) == 0) {
        printf("   [FS Info] Free Blocks: %d\n", sfs.f_bfree);
    }
}

int main() {
    char* path = "/large_file.bin";
    struct stat st;
    
    printf("--- Ext2 Indirect Block Truncate Test ---\n");

    // 记录初始空闲块数
    printf("1. Checking initial filesystem state...\n");
    print_fs_status();

    // 创建文件并写入 15KB
    int fd = open(path, O_CREATE | O_RDWR);
    char* buf = malloc(BLOCK_SIZE);
    memset(buf, 0x55, BLOCK_SIZE);

    printf("2. Writing %d KB to %s...\n", TEST_BLOCKS, path);
    for (int i = 0; i < TEST_BLOCKS; i++) {
        write(fd, buf, BLOCK_SIZE);
    }
    close(fd);

    // 检查写入后的状态
    stat(path, &st);
    printf("   File created. Size: %d, Blocks: %d (512B sectors)\n", (uint32_t)st.st_size, st.st_blocks);
    printf("   Status after write:\n");
    print_fs_status();

    // 执行完全截断
    printf("3. Executing truncate(path, 0)...\n");
    if (truncate(path, 0) == 0) {
        printf("   Truncate successful.\n");
    } else {
        printf("   Truncate failed!\n");
        return -1;
    }

    // 最终验证：检查空闲块是否完整回滚
    // 预期：空闲块数应该增加 (15个数据块 + 1个一级间接块) = 16 个块
    printf("4. Checking final filesystem state...\n");
    stat(path, &st);
    printf("   Final File: Size: %d, Blocks: %d\n", st.st_size, st.st_blocks);
    print_fs_status();

    printf("\nConclusion: If 'Free Blocks' increased by %d, recursion is PERFECT.\n", TEST_BLOCKS + 1);

    free(buf);
    return 0;
}