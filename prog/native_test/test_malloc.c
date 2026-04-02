#include <syscall.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/*
    用于测试用户态 malloc 是否正常
    主要用于测试大对象的申请和释放是否正常
    以及对于尾部空间释放的 trim 操作是否正常
*/

static int g_fail = 0;

static void wait_key(const char* stage) {
    char ch = 0;
    printf("test_malloc: %s -- press any key to continue\n", stage);
    read(stdin_no, &ch, 1);
}

static void check(int cond, const char* msg) {
    if (cond) {
        printf("[OK]   %s\n", msg);
    } else {
        printf("[FAIL] %s\n", msg);
        g_fail = 1;
    }
}

static void fill_pattern(uint8_t* buf, uint32_t size, uint8_t seed) {
    for (uint32_t i = 0; i < size; i++) {
        buf[i] = (uint8_t)(seed + i);
    }
}

static int verify_pattern(uint8_t* buf, uint32_t size, uint8_t seed) {
    for (uint32_t i = 0; i < size; i++) {
        if (buf[i] != (uint8_t)(seed + i)) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    uint32_t brk0 = (uint32_t)sbrk(0);
    printf("test_malloc: initial brk = %x\n", brk0);

    uint8_t* small = (uint8_t*)malloc(24);
    check(small != NULL, "malloc(24) should succeed");
    if (small == NULL) return -1;
    fill_pattern(small, 24, 0x10);
    check(verify_pattern(small, 24, 0x10), "small allocation should be writable");

    uint32_t brk1 = (uint32_t)sbrk(0);
    printf("test_malloc: brk after small alloc = %x\n", brk1);
    check(brk1 > brk0, "small allocation should grow heap");
    wait_key("after small allocation");

    uint32_t big_size = 128 * 1024 + 137;
    uint8_t* big = (uint8_t*)malloc(big_size);
    check(big != NULL, "large malloc should succeed");
    if (big == NULL) return -1;
    fill_pattern(big, big_size, 0x33);
    check(verify_pattern(big, big_size, 0x33), "large allocation should be writable");

    uint32_t brk2 = (uint32_t)sbrk(0);
    printf("test_malloc: brk after large alloc = %x\n", brk2);
    check(brk2 == brk1, "large allocation should not grow heap (mmap-backed)");
    check((uint32_t)big >= 0x08048000U + 0x1000000U, "large allocation should come from mmap area");

    free(big);
    uint32_t brk3 = (uint32_t)sbrk(0);
    printf("test_malloc: brk after freeing tail large block = %x\n", brk3);
    check(brk3 == brk1, "freeing mmap-backed large block should not affect brk");
    wait_key("after large allocation/free");

    uint8_t* a = (uint8_t*)malloc(64);
    uint8_t* b = (uint8_t*)malloc(96);
    uint8_t* c = (uint8_t*)malloc(128);
    check(a != NULL && b != NULL && c != NULL, "three small allocations should succeed");
    if (a == NULL || b == NULL || c == NULL) return -1;

    fill_pattern(a, 64, 0x01);
    fill_pattern(b, 96, 0x21);
    fill_pattern(c, 128, 0x41);

    uint32_t brk4 = (uint32_t)sbrk(0);
    printf("test_malloc: brk after a/b/c alloc = %x\n", brk4);
    check(brk4 >= brk1, "small arena allocations should succeed even if multiple size classes consume extra arena pages");

    free(b);
    uint32_t brk5 = (uint32_t)sbrk(0);
    printf("test_malloc: brk after freeing middle block b = %x\n", brk5);
    check(brk5 == brk4, "freeing middle block should not trim heap");

    free(c);
    uint32_t brk6 = (uint32_t)sbrk(0);
    printf("test_malloc: brk after freeing tail block c = %x\n", brk6);
    check(brk6 <= brk5, "freeing tail-side arena blocks may trim if that size-class arena becomes fully free");

    free(a);
    uint32_t brk7 = (uint32_t)sbrk(0);
    printf("test_malloc: brk after freeing a = %x\n", brk7);
    check(brk7 == brk1, "freeing remaining arena blocks should keep the first heap page reserved");
    wait_key("after arena free sequence");

    uint8_t* reuse1 = (uint8_t*)malloc(80);
    check(reuse1 != NULL, "malloc after trim should still succeed");
    if (reuse1 == NULL) return -1;
    fill_pattern(reuse1, 80, 0x55);
    check(verify_pattern(reuse1, 80, 0x55), "reused allocation should be writable");
    uint32_t brk_reuse = (uint32_t)sbrk(0);
    printf("test_malloc: brk after reuse alloc = %x\n", brk_reuse);
    check(brk_reuse >= brk7, "reused small allocation should remain valid after earlier arena trim");

    free(reuse1);
    free(small);

    uint32_t brk8 = (uint32_t)sbrk(0);
    printf("test_malloc: final brk = %x\n", brk8);
    check(brk8 == brk0, "all small allocations freed should return brk to initial value");

    if (g_fail) {
        printf("test_malloc: FAILED\n");
        return -1;
    }

    printf("test_malloc: PASSED\n");
    return 0;
}
