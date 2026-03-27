#include <syscall.h>
#include <string.h>
#include <stdint.h>

#define USER_MALLOC_ALIGN 8
#define USER_PG_SIZE 4096
#define USER_ARENA_DESC_CNT 7
#define USER_ARENA_MAX_BLOCK 1024

struct mem_block;

struct arena {
    struct mem_block_desc* desc;
    uint32_t cnt;
    uint32_t large;
};

struct mem_block {
    struct mem_block* next;
};

struct mem_block_desc {
    uint32_t block_size;
    uint32_t block_per_arena;
    struct mem_block* free_list;
};

static struct mem_block_desc u_block_descs[USER_ARENA_DESC_CNT];
static int malloc_inited = 0;

static uint32_t align_up(uint32_t size, uint32_t align) {
    return (size + align - 1) & ~(align - 1);
}

static struct arena* block2arena(struct mem_block* block) {
    return (struct arena*)((uint32_t)block & ~(USER_PG_SIZE - 1));
}

static struct mem_block* arena2block(struct arena* arena, uint32_t idx) {
    uint32_t base = (uint32_t)(arena + 1);
    return (struct mem_block*)(base + idx * arena->desc->block_size);
}

static uint32_t arena_usable_size(struct arena* arena) {
    if (arena->large) {
        return arena->cnt * USER_PG_SIZE - sizeof(struct arena);
    }
    return arena->desc->block_size;
}

static void block_desc_init(void) {
    uint32_t block_size = 16;
    for (uint32_t i = 0; i < USER_ARENA_DESC_CNT; i++) {
        u_block_descs[i].block_size = block_size;
        u_block_descs[i].block_per_arena = (USER_PG_SIZE - sizeof(struct arena)) / block_size;
        u_block_descs[i].free_list = NULL;
        block_size <<= 1;
    }
    malloc_inited = 1;
}

// 进行虚拟地址的扩堆
// brk 的语义是将虚拟地址推到某个绝对的地址值
// sbrk 是将地址相对地移动多少字节，封装一个 sbrk 写 malloc 更顺。
void* sbrk(int32_t increment) {
    uint32_t old_brk = (uint32_t)brk(NULL);
    int64_t candidate_brk = (int64_t)old_brk + (int64_t)increment;
    if (candidate_brk < 0 || candidate_brk > 0xffffffffLL) {
        return (void*)-1;
    }

    uint32_t new_brk = (uint32_t)candidate_brk;
    uint32_t actual_brk = (uint32_t)brk((void*)new_brk);

    if (actual_brk != new_brk) {
        return (void*)-1;
    }
    return (void*)old_brk;
}

static struct arena* alloc_large_arena(uint32_t size) {
    uint32_t total_size = sizeof(struct arena) + size;
    uint32_t page_cnt = (total_size + USER_PG_SIZE - 1) / USER_PG_SIZE;
    uint32_t map_len = page_cnt * USER_PG_SIZE;

    struct arena* arena = (struct arena*)mmap(NULL, map_len,
                                              PROT_READ | PROT_WRITE,
                                              MAP_PRIVATE | MAP_ANON,
                                              -1, 0);
    if (arena == MAP_FAILED) {
        return NULL;
    }

    arena->desc = NULL;
    arena->cnt = page_cnt;
    arena->large = 1;
    return arena;
}

static void* alloc_small(uint32_t size) {
    uint32_t desc_idx = 0;
    while (desc_idx < USER_ARENA_DESC_CNT && size > u_block_descs[desc_idx].block_size) {
        desc_idx++;
    }
    if (desc_idx == USER_ARENA_DESC_CNT) {
        return NULL;
    }

    struct mem_block_desc* desc = &u_block_descs[desc_idx];
    if (desc->free_list == NULL) {
        struct arena* arena = (struct arena*)sbrk(USER_PG_SIZE);
        if (arena == (void*)-1) {
            return NULL;
        }

        arena->desc = desc;
        arena->large = 0;
        arena->cnt = desc->block_per_arena;

        for (uint32_t i = 0; i < desc->block_per_arena; i++) {
            struct mem_block* block = arena2block(arena, i);
            block->next = desc->free_list;
            desc->free_list = block;
        }
    }

    // 弹出队头
    struct mem_block* block = desc->free_list;
    desc->free_list = block->next;

    struct arena* arena = block2arena(block);
    arena->cnt--;
    memset(block, 0, desc->block_size);
    return block;
}

void* malloc(uint32_t size) {
    if (size == 0) {
        return NULL;
    }
    if (!malloc_inited) {
        block_desc_init();
    }

    size = align_up(size, USER_MALLOC_ALIGN);
    if (size > USER_ARENA_MAX_BLOCK) {
        struct arena* arena = alloc_large_arena(size);
        if (arena == NULL) {
            return NULL;
        }
        // 移动到 payload
        return (void*)(arena + 1);
    }

    return alloc_small(size);
}

void free(void* ptr) {
    if (ptr == NULL) {
        return;
    }

    struct arena* arena = (struct arena*)((uint32_t)ptr & ~(USER_PG_SIZE - 1));
    if (arena->large) {
        munmap((void*)arena, arena->cnt * USER_PG_SIZE);
        return;
    }

    struct mem_block* block = (struct mem_block*)ptr;
    block->next = arena->desc->free_list;
    arena->desc->free_list = block;
    arena->cnt++;

    // 当 arena 内所有小块都空闲时，直接把整个页缩回 heap。
    // 由于 arena 元信息和 payload 混在同一页，且 arena 来自 sbrk 按页增长，
    // 只有当该 arena 恰好位于 brk 尾部时，才能真正 shrink。
    if (arena->cnt == arena->desc->block_per_arena) {
        uint32_t arena_addr = (uint32_t)arena;
        uint32_t cur_brk = (uint32_t)brk(NULL);
        if (arena_addr + USER_PG_SIZE == cur_brk) {
            struct mem_block** cur = &arena->desc->free_list;
            while (*cur != NULL) {
                if ((uint32_t)(*cur) >= arena_addr && (uint32_t)(*cur) < arena_addr + USER_PG_SIZE) {
                    *cur = (*cur)->next;
                } else {
                    cur = &(*cur)->next;
                }
            }
            sbrk(-USER_PG_SIZE);
        }
    }
}

void* calloc(uint32_t nmemb, uint32_t size) {
    if (nmemb != 0 && size > 0xffffffffU / nmemb) {
        return NULL;
    }
    uint32_t total_size = nmemb * size;
    void* ptr = malloc(total_size);
    if (ptr != NULL) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

void* realloc(void* ptr, uint32_t size) {
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    struct arena* arena = (struct arena*)((uint32_t)ptr & ~(USER_PG_SIZE - 1));
    uint32_t old_size = arena_usable_size(arena);
    if (old_size >= size) {
        return ptr;
    }

    void* new_ptr = malloc(size);
    if (new_ptr == NULL) {
        return NULL;
    }

    memcpy(new_ptr, ptr, old_size);
    free(ptr);
    return new_ptr;
}
