#include <syscall.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    // install_apps.sh 会把用户程序统一打到 /bin/ 目录里，
    // 因此这里直接映射一个确定存在的普通文件来测试文件映射逻辑。
    int32_t fd = open("/bin/test_mmap_file", O_RDONLY);
    if (fd < 0) {
        printf("test_mmap_file: open failed\n");
        return 1;
    }

    int32_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) {
        printf("test_mmap_file: bad file size=%d\n", file_size);
        close(fd);
        return 1;
    }
    lseek(fd, 0, SEEK_SET);

    uint32_t map_len = (file_size > 5000) ? 5000 : (uint32_t)file_size;
    char expect[16];
    memset(expect, 0, sizeof(expect));
    if (read(fd, expect, sizeof(expect)) != (int32_t)sizeof(expect)) {
        printf("test_mmap_file: pre-read failed\n");
        close(fd);
        return 1;
    }
    lseek(fd, 0, SEEK_SET);

    char* p = (char*)mmap(NULL, map_len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        printf("test_mmap_file: mmap failed\n");
        close(fd);
        return 1;
    }

    if (memcmp(expect, p, sizeof(expect)) != 0) {
        printf("test_mmap_file: first bytes mismatch\n");
        munmap(p, map_len);
        close(fd);
        return 1;
    }

    printf("test_mmap_file: mmap addr=%x\n", (uint32_t)p);
    printf("test_mmap_file: first4=%x %x %x %x\n",
           (uint8_t)p[0], (uint8_t)p[1], (uint8_t)p[2], (uint8_t)p[3]);

    if (map_len > 4096) {
        char expected_page_byte = 0;
        lseek(fd, 4096, SEEK_SET);
        if (read(fd, &expected_page_byte, 1) == 1) {
            printf("test_mmap_file: page1=%x expected=%x\n",
                   (uint8_t)p[4096], (uint8_t)expected_page_byte);
        }
    }

    if (munmap(p, map_len) != 0) {
        printf("test_mmap_file: munmap failed\n");
        close(fd);
        return 1;
    }

    close(fd);
    printf("test_mmap_file: done\n");
    return 0;
}
