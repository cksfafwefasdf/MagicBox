#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <syscall.h>

// 测试 ioctl 是否能正常读到磁盘扇区数和大小

#define BLKGETSIZE   0x1245
#define BLKGETSIZE64 0x80081272


int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: disk_test <device_path>\n");
        return -1;
    }

    // 打开设备文件
    // 格式化通常需要写权限，测试读取只需 O_RDONLY
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        printf("Error: Cannot open %s (Check if device node exists)\n", argv[1]);
        return -1;
    }

    printf("Querying device information for: %s\n", argv[1]);
    printf("------------------------------------------\n");

    // 获取 32 位扇区数 (BLKGETSIZE)
    uint32_t sectors = 0;
    if (ioctl(fd, BLKGETSIZE, &sectors) == 0) {
        printf("[SUCCESS] BLKGETSIZE: %x sectors\n", sectors);
    } else {
        // 如果返回 -1，记得检查内核 ide_ioctl 是否返回了 -ENODEV 或 -EINVAL
        printf("[FAILED]  BLKGETSIZE ioctl error.\n");
    }

    // 获取 64 位字节数 (BLKGETSIZE64)
    uint64_t size_in_bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &size_in_bytes) == 0) {
        printf("[SUCCESS] BLKGETSIZE64: %x bytes\n", size_in_bytes);
        
        // 换算成更易读的单位
        uint32_t size_mb = (uint32_t)(size_in_bytes / (1024 * 1024));
        printf("Calculated Capacity: %x MB\n", size_mb);
    } else {
        printf("[FAILED]  BLKGETSIZE64 ioctl error.\n");
    }

    close(fd);
    return 0;
}