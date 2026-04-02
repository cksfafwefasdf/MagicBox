#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*
    模拟编译器中的一些简单的内存操作
*/

#define PAGE_SIZE       4096
#define SRC_SIZE        (3 * 1024 * 1024)
#define WORKBUF_COUNT   4
#define TOKEN_COUNT     12000
#define SYMBOL_COUNT    3000

struct token {
    char *text;
    int len;
    int kind;
};

struct symbol {
    char *name;
    int value;
};

static void die(const char *msg) {
    printf("FAIL: %s\n", msg);
    _exit(1);
}

static uint32_t prng_state = 1;
static uint32_t myrand(void) {
    prng_state = prng_state * 1103515245u + 12345u;
    return prng_state;
}

static char random_ident_char(int pos) {
    if (pos == 0) return 'a' + (myrand() % 26);
    uint32_t r = myrand() % 36;
    return r < 26 ? ('a' + r) : ('0' + (r - 26));
}

static char *make_ident(int min_len, int max_len) {
    int len = min_len + (myrand() % (max_len - min_len + 1));
    char *s = malloc((size_t)len + 1);
    if (!s) die("make_ident malloc");
    for (int i = 0; i < len; i++) s[i] = random_ident_char(i);
    s[len] = '\0';
    return s;
}

int main(void) {
    printf("compiler-like stress start, pid=%d\n", getpid());

    /* 模拟把一个较大的源码/中间文本读进内存 */
    char *src = malloc(SRC_SIZE);
    if (!src) die("src malloc");

    for (size_t off = 0; off < SRC_SIZE; off += PAGE_SIZE) {
        src[off] = (char)('A' + ((off / PAGE_SIZE) % 26));
    }
    src[SRC_SIZE - 1] = '\n';

    /* 再把整块内容填成更像源码的样子 */
    for (size_t i = 0; i < SRC_SIZE - 64; i += 64) {
        src[i + 0] = 'i';
        src[i + 1] = 'n';
        src[i + 2] = 't';
        src[i + 3] = ' ';
        src[i + 4] = 'x';
        src[i + 5] = (char)('0' + (i % 10));
        src[i + 6] = ' ';
        src[i + 7] = '=';
        src[i + 8] = ' ';
        src[i + 9] = '1';
        src[i + 10] = ';';
        src[i + 11] = '\n';
    }

    printf("source buffer prepared\n");

    /* 模拟 token 数组，动态增长 */
    int tok_cap = 256;
    int tok_cnt = 0;
    struct token *tokens = malloc(sizeof(struct token) * (size_t)tok_cap);
    if (!tokens) die("tokens malloc");

    for (int i = 0; i < TOKEN_COUNT; i++) {
        if (tok_cnt == tok_cap) {
            tok_cap *= 2;
            struct token *p = realloc(tokens, sizeof(struct token) * (size_t)tok_cap);
            if (!p) die("tokens realloc");
            tokens = p;
        }

        tokens[tok_cnt].text = make_ident(4, 40);
        tokens[tok_cnt].len = (int)strlen(tokens[tok_cnt].text);
        tokens[tok_cnt].kind = (int)(myrand() % 16);

        /* 真实读写 token 内容 */
        if (tokens[tok_cnt].len < 4) die("token len");
        tokens[tok_cnt].text[0] = 't';
        tokens[tok_cnt].text[tokens[tok_cnt].len - 1] ^= 1;

        tok_cnt++;
    }

    printf("tokens built: %d\n", tok_cnt);

    /* 模拟符号表，名字较短但数量多 */
    struct symbol *syms = malloc(sizeof(struct symbol) * SYMBOL_COUNT);
    if (!syms) die("symbols malloc");

    for (int i = 0; i < SYMBOL_COUNT; i++) {
        syms[i].name = make_ident(3, 24);
        syms[i].value = (int)myrand();

        if (!syms[i].name[0]) die("symbol name empty");
        syms[i].name[0] = 's';
    }

    printf("symbols built: %d\n", SYMBOL_COUNT);

    /* 模拟几个大工作缓冲区（预处理输出、汇编输出、临时对象缓存等） */
    char *workbuf[WORKBUF_COUNT];
    size_t worksz[WORKBUF_COUNT] = {
        512 * 1024,
        1024 * 1024,
        2 * 1024 * 1024,
        3 * 1024 * 1024
    };

    for (int i = 0; i < WORKBUF_COUNT; i++) {
        workbuf[i] = malloc(worksz[i]);
        if (!workbuf[i]) die("workbuf malloc");

        /* 跨页写，逼出真实物理页分配 */
        for (size_t off = 0; off < worksz[i]; off += PAGE_SIZE) {
            workbuf[i][off] = (char)(0x30 + i);
        }
        workbuf[i][worksz[i] - 1] = (char)(0x60 + i);
    }

    printf("work buffers allocated\n");

    /* 再做一轮“语义分析/重写”式 realloc */
    for (int i = 0; i < tok_cnt; i += 5) {
        int new_len = tokens[i].len + 80 + (myrand() % 120);
        char *p = realloc(tokens[i].text, (size_t)new_len + 1);
        if (!p) die("token text realloc");
        tokens[i].text = p;

        for (int j = tokens[i].len; j < new_len; j++) {
            tokens[i].text[j] = 'A' + (char)(j % 26);
        }
        tokens[i].text[new_len] = '\0';
        tokens[i].len = new_len;
    }

    printf("token realloc phase done\n");

    /* 校验几类内存都真的可读 */
    for (int i = 0; i < tok_cnt; i += 257) {
        if (tokens[i].text == NULL || tokens[i].len <= 0) die("token verify");
        volatile char c = tokens[i].text[0];
        (void)c;
    }

    for (int i = 0; i < SYMBOL_COUNT; i += 113) {
        if (syms[i].name == NULL || syms[i].name[0] != 's') die("symbol verify");
    }

    for (int i = 0; i < WORKBUF_COUNT; i++) {
        for (size_t off = 0; off < worksz[i]; off += PAGE_SIZE) {
            if (workbuf[i][off] != (char)(0x30 + i)) die("workbuf page verify");
        }
        if (workbuf[i][worksz[i] - 1] != (char)(0x60 + i)) die("workbuf tail verify");
    }

    printf("verification done\n");

    /* 打散释放顺序，模拟编译阶段结束时各种对象回收 */
    for (int i = 0; i < tok_cnt; i += 2) {
        free(tokens[i].text);
        tokens[i].text = NULL;
    }

    for (int i = WORKBUF_COUNT - 1; i >= 0; i--) {
        free(workbuf[i]);
        workbuf[i] = NULL;
    }

    for (int i = 0; i < SYMBOL_COUNT; i++) {
        free(syms[i].name);
        syms[i].name = NULL;
    }

    for (int i = 1; i < tok_cnt; i += 2) {
        free(tokens[i].text);
        tokens[i].text = NULL;
    }

    free(tokens);
    free(syms);
    free(src);

    printf("compiler-like stress passed\n");
    return 0;
}

