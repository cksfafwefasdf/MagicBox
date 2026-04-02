#include <syscall.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint32_t old_brk = (uint32_t)sbrk(0);
    uint32_t len = 3 * 4096 + 123;
    char* p = (char*)mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

    printf("test_mmap: initial brk=%x\n", old_brk);
    if (p == MAP_FAILED) {
        printf("test_mmap: mmap failed\n");
        return 1;
    }

    printf("test_mmap: mmap addr=%x\n", (uint32_t)p);
    printf("test_mmap: brk after mmap=%x\n", (uint32_t)sbrk(0));

    memset(p, 0x41, len);
    p[0] = 'M';
    p[4096] = 'A';
    p[8192] = 'P';
    p[len - 1] = '!';

    printf("test_mmap: bytes=%c%c%c%c\n", p[0], p[4096], p[8192], p[len - 1]);

    // 进一步测试部分 munmap：
    // 先把中间一页挖掉，再重新 mmap 一页，按当前高地址反向搜索策略，
    // 新映射应当优先复用这个最高的页级空洞。
    if (munmap(p + 4096, 4096) != 0) {
        printf("test_mmap: partial munmap failed\n");
        return 1;
    }

    char* mid = (char*)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mid == MAP_FAILED) {
        printf("test_mmap: remap middle page failed\n");
        return 1;
    }
    printf("test_mmap: remap addr=%x expected=%x\n", (uint32_t)mid, (uint32_t)(p + 4096));
    if (mid != p + 4096) {
        printf("test_mmap: remap hole mismatch\n");
        return 1;
    }
    mid[0] = 'R';
    printf("test_mmap: remap byte=%c\n", mid[0]);

    if (munmap(p, len) != 0) {
        printf("test_mmap: munmap failed\n");
        return 1;
    }

    printf("test_mmap: brk after munmap=%x\n", (uint32_t)sbrk(0));
    printf("test_mmap: done\n");
    return 0;
}
