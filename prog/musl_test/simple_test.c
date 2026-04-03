#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    int pid = getpid();
    printf("my pid is: %d\n", pid);

    int *ptr = (int *) malloc(sizeof(int) * 32);
    char *ptr2 = (char *) malloc(1024 * 1024);
    char *ptr3 = (char *) malloc(4 * 1024 * 1024);

    if (!ptr || !ptr2 || !ptr3) {
        printf("malloc failed\n");
        return 1;
    }

    for (int i = 0; i < 1024 * 1024; i++) {
        ptr2[i] = 77;
    }

    for (int i = 0; i < 4 * 1024 * 1024; i += 4096) {
        ptr3[i] = 88;
    }
    ptr3[4 * 1024 * 1024 - 1] = 99;

    free(ptr);
    free(ptr2);
    free(ptr3);
    return pid;
}

