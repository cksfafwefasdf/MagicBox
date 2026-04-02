#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*
    该程序运行时，在用户态中的malloc至少会占用36MB的内存
    由于我们目前采用的还是双池分配策略，用户会固定吃掉低端内存的一半
    因此我们至少需要36*2=72MB的内存才能运行这个程序
    考虑到程序某些其他部分可能还会占用一些用户态内存，需要的内存肯定会大于72MB
    经过测试，至少要将-m参数设置为77才能成功通过测试，内存设为76MB时会内存耗尽崩溃
*/

#define SMALL_CNT   4000
#define MEDIUM_CNT   512
#define LARGE_CNT      8

#define PAGE_SIZE   4096

static void fail(const char *msg) {
    printf("FAIL: %s\n", msg);
    _exit(1);
}

static uint32_t pattern32(uint32_t x) {
    return x * 2654435761u + 0x9e3779b9u;
}

int main(void) {
    pid_t pid = getpid();
    printf("allocator stress start, pid=%d\n", pid);

    void *small[SMALL_CNT];
    char *medium[MEDIUM_CNT];
    char *large[LARGE_CNT];

    memset(small, 0, sizeof(small));
    memset(medium, 0, sizeof(medium));
    memset(large, 0, sizeof(large));

    /* 大量小对象，模拟 token / AST node / symbol entry */
    for (int i = 0; i < SMALL_CNT; i++) {
        size_t sz = 8 + (i % 96);   /* 8 ~ 103 bytes */
        small[i] = malloc(sz);
        if (!small[i]) fail("small malloc");

        memset(small[i], (i ^ 0x5a) & 0xff, sz);
        ((unsigned char *)small[i])[0] = (unsigned char)i;
        ((unsigned char *)small[i])[sz - 1] = (unsigned char)(i >> 3);
    }

    printf("small allocations done\n");

    /* 中型字符串块，模拟源码缓冲/预处理/符号名 */
    for (int i = 0; i < MEDIUM_CNT; i++) {
        size_t sz = 128 + (i % 512); /* 128 ~ 639 bytes */
        medium[i] = malloc(sz);
        if (!medium[i]) fail("medium malloc");

        for (size_t j = 0; j < sz - 1; j++) {
            medium[i][j] = 'a' + (char)((i + j) % 26);
        }
        medium[i][sz - 1] = '\0';
    }

    printf("medium allocations done\n");

    /* 对中型块做 realloc，模拟动态增长的文本缓冲 */
    for (int i = 0; i < MEDIUM_CNT; i++) {
        size_t old_sz = 128 + (i % 512);
        size_t new_sz = old_sz + 300 + (i % 200);
        char *p = realloc(medium[i], new_sz);
        if (!p) fail("medium realloc grow");
        medium[i] = p;

        for (size_t j = old_sz - 1; j < new_sz - 1; j++) {
            medium[i][j] = 'A' + (char)((i + j) % 26);
        }
        medium[i][new_sz - 1] = '\0';
    }

    printf("medium realloc done\n");

    /* 大块缓冲区，模拟大文件/目标文件/临时工作区 */
    for (int i = 0; i < LARGE_CNT; i++) {
        size_t sz = (size_t)(i + 1) * 1024 * 1024; /* 1MB ~ 8MB */
        large[i] = malloc(sz);
        if (!large[i]) fail("large malloc");

        /* 每页都碰一下，确保不是只建立 VMA，而是真 fault in */
        for (size_t off = 0; off < sz; off += PAGE_SIZE) {
            large[i][off] = (char)(i + 1);
        }
        large[i][sz - 1] = (char)(0x70 + i);

        /* 再读一遍，确保映射和内容都真正确 */
        for (size_t off = 0; off < sz; off += PAGE_SIZE) {
            if (large[i][off] != (char)(i + 1)) {
                fail("large page verify");
            }
        }
        if (large[i][sz - 1] != (char)(0x70 + i)) {
            fail("large tail verify");
        }
    }

    printf("large allocations done\n");

    /* 校验小对象 */
    for (int i = 0; i < SMALL_CNT; i++) {
        size_t sz = 8 + (i % 96);
        if (((unsigned char *)small[i])[0] != (unsigned char)i) {
            fail("small verify head");
        }
        if (((unsigned char *)small[i])[sz - 1] != (unsigned char)(i >> 3)) {
            fail("small verify tail");
        }
    }

    /* 校验中型字符串 */
    for (int i = 0; i < MEDIUM_CNT; i++) {
        if (medium[i][0] == '\0') {
            fail("medium verify");
        }
        size_t len = strlen(medium[i]);
        if (len < 100) {
            fail("medium string too short");
        }
    }

    printf("verification done\n");

    /* 打散释放顺序，模拟真实编译器生命周期 */
    for (int i = 0; i < SMALL_CNT; i += 2) {
        free(small[i]);
        small[i] = NULL;
    }
    for (int i = 0; i < MEDIUM_CNT; i += 3) {
        free(medium[i]);
        medium[i] = NULL;
    }
    for (int i = LARGE_CNT - 1; i >= 0; i--) {
        free(large[i]);
        large[i] = NULL;
    }

    printf("partial/free large done\n");

    for (int i = 1; i < SMALL_CNT; i += 2) {
        free(small[i]);
        small[i] = NULL;
    }
    for (int i = 0; i < MEDIUM_CNT; i++) {
        if (medium[i]) {
            free(medium[i]);
            medium[i] = NULL;
        }
    }

    printf("allocator stress passed\n");
    return 0;
}

