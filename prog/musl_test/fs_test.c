#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <dirent.h>

int main() {
    printf("--- [1] Testing PID & Basic I/O ---\n");
    printf("Current PID: %d\n", getpid()); // 触发 SYS_getpid (20)

    // --- 文件操作部分 ---
    printf("\n--- [2] Testing File System (Open/Write/Close) ---\n");
    const char* filename = "/test_musl.txt";
    
    // 触发 SYS_open (5)
    int fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open failed");
    } else {
        printf("Open '%s' success, fd: %d\n", filename, fd);

        // 触发 SYS_write (4) 或 SYS_writev (146)
        const char* msg = "Hello from MagicBox OS via Musl-gcc!\n";
        ssize_t bytes = write(fd, msg, strlen(msg));
        printf("Written %d bytes to fd %d\n", bytes, fd);

        // 触发 SYS_lseek (19)
        off_t offset = lseek(fd, 0, SEEK_SET);
        printf("Lseek back to %lld\n", offset);

        // 触发 SYS_read (3)
        char read_buf[64] = {0};
        read(fd, read_buf, sizeof(read_buf));
        printf("Read back: %s", read_buf);

        // 触发 SYS_fstat64 (197)
        struct stat st;
        if (fstat(fd, &st) == 0) {
            printf("Fstat: Size = %lld, Inode = %llu\n", st.st_size, st.st_ino);
        }

        // 触发 SYS_close (6)
        close(fd);
        printf("File closed.\n");
    }

    // // --- 内存管理部分 ---
    printf("\n--- [3] Testing Memory (Malloc/Free/Brk) ---\n");
    // 触发 SYS_brk (45) 或 SYS_mmap2 (192)
    void* ptr = malloc(1024 * 4); 
    if (ptr) {
        strcpy(ptr, "Dynamic memory is working!");
        printf("Malloc address: %p, content: %s\n", ptr, (char*)ptr);
        free(ptr);
        printf("Free success.\n");
    }

    // --- 目录操作部分 ---
    printf("\n--- [4] Testing Directory (Opendir/Getdents) ---\n");
    // 触发 SYS_open (5) 并带 O_DIRECTORY 标志，接着触发 SYS_getdents64 (220)
    DIR* dir = opendir("/");
    if (dir) {
        struct dirent* de;
        printf("Contents of '/':\n");
        while ((de = readdir(dir)) != NULL) {
            printf("  - %s (type: %d)\n", de->d_name, de->d_type);
        }
        closedir(dir);
    } else {
        perror("opendir / failed");
    }

    printf("\n--- [5] Testing Final Exit ---\n");
    // 触发 SYS_exit_group (252)
    return 0;
}