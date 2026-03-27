#include <syscall.h>
#include <string.h>
#include <stdint.h>

// 用户申请的内存块最少8字节
#define USER_MALLOC_ALIGN 8

// 实际布局是 [chunk header][user payload][chunk header][user payload]
// 用户实际拿到的是后面的 payload
struct malloc_chunk {
    uint32_t size; // 用户可用大小，不包括头部，纯payload大小
    uint32_t free; // 当前块是否空闲
    struct malloc_chunk* next;
};

static struct malloc_chunk* heap_head = NULL;

static uint32_t align_up(uint32_t size) {
    return (size + USER_MALLOC_ALIGN - 1) & ~(USER_MALLOC_ALIGN - 1);
}

// 使用简单的首次适应
static struct malloc_chunk* find_free_chunk(uint32_t size) {
    struct malloc_chunk* cur = heap_head;
    while (cur != NULL) {
        if (cur->free && cur->size >= size) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

// 对拿到的chunk进行切分，比如用户申请20B，首次适应给了个32B的块
// 那么就调用这个函数，从这个32B中切出20B给用户，剩下的12B挂回空闲链表
static void split_chunk(struct malloc_chunk* chunk, uint32_t size) {
    // chunk->size 取到的是payload的大小
    uint32_t remain = chunk->size - size;
    // 由于我们最小的一个payload也要8字节，并且我们还要腾出来空间放头部信息malloc_chunk
    // 因此剩下的空间必须要大于 8字节+头部 的大小才有需要切分的必要，要是太小就不用切分了
    if (remain <= sizeof(struct malloc_chunk) + USER_MALLOC_ALIGN) {
        return;
    }
    // 我们移动到头部的末尾（chunk + 1）再加上一个分配出去的 payload （size） 的距离
    // 在新的位置防止新的头部，然后将头部之外的剩下区域作为payload以便实现拆分
    struct malloc_chunk* next = (struct malloc_chunk*)((uint8_t*)(chunk + 1) + size);
    next->size = remain - sizeof(struct malloc_chunk);
    next->free = 1;
    next->next = chunk->next;

    chunk->size = size;
    chunk->next = next;
}

static struct malloc_chunk* request_chunk(uint32_t size) {
    uint32_t total_size = sizeof(struct malloc_chunk) + size;
    struct malloc_chunk* chunk = (struct malloc_chunk*)sbrk((int32_t)total_size);
    if (chunk == (void*)-1) {
        return NULL;
    }

    chunk->size = size;
    chunk->free = 0;
    chunk->next = NULL;

    if (heap_head == NULL) {
        heap_head = chunk;
    } else { // 插入空闲块到尾部
        struct malloc_chunk* tail = heap_head;
        while (tail->next != NULL) {
            tail = tail->next;
        }
        tail->next = chunk;
    }

    return chunk;
}

// 用于合并相邻空闲块
static void coalesce_chunks(void) {
    struct malloc_chunk* cur = heap_head;
    while (cur != NULL && cur->next != NULL) {
        uint8_t* cur_end = (uint8_t*)(cur + 1) + cur->size;
        // 如果当前块和当前块的下一个块都空，并且当前块的元信息加payload达到的末端地址和下一个块的首地址相邻
        // 那么就合并他们，合并操作很简单，就是把当前块的size增加一个元数据和下一个块payload的大小进就行
        // 由于我们的遍历都是依赖于 sizeof(struct malloc_chunk) 和 size 属性来进行的
        // 因此修改了当前快的size后，下次遍历时自然就可以越过后面这个被合并进去的块
        if (cur->free && cur->next->free && cur_end == (uint8_t*)cur->next) {
            cur->size += sizeof(struct malloc_chunk) + cur->next->size;
            cur->next = cur->next->next;
            continue;
        }
        cur = cur->next;
    }
}

// 如果链表尾部正好是一段空闲块，并且它贴着当前 brk，
// 那么可以真正把这段 heap 还给内核，而不是只留在用户态 free list 里复用。
static void trim_heap_tail(void) {
    while (heap_head != NULL) {
        struct malloc_chunk* prev = NULL;
        struct malloc_chunk* tail = heap_head;

        // 直接跑到最后一个节点，检查其是否到堆顶brk
        // 按理来说，我们现在存的各种 malloc_chunk 元数据他都是按照地址顺序来存的
        // 或者换句话说，这些 malloc_chunk 本来就是在整个地址空间上从低高打桩
        // 因此这些 malloc_chunk 天然就是具有顺序性的，不用担心 tail 不是最高地址处
        // 它和真正意义上的空闲链表还不太一样
        while (tail->next != NULL) {
            prev = tail;
            tail = tail->next;
        }

        // 尾部要是不空闲的话那肯定不能收缩，直接返回
        if (!tail->free) {
            return;
        }

        uint32_t tail_start = (uint32_t)tail;
        uint32_t tail_total_size = sizeof(struct malloc_chunk) + tail->size;
        uint32_t cur_brk = (uint32_t)brk(NULL);

        // 只有真正位于 heap 顶端的空闲尾块，才能安全地缩回去。
        // cur_brk 是堆顶，如果 tail_start + tail_total_size 不等于堆顶那么就不是位于尾端的
        if (tail_start + tail_total_size != cur_brk) {
            return;
        }

        // 我们后面的sbrk有一个int32_t的强转操作，因此此处要验证一下防止其强转溢出
        if (tail_total_size > 0x7fffffffU) {
            return;
        }

        if (sbrk(-((int32_t)tail_total_size)) == (void*)-1) {
            return;
        }

        // 特殊处理一下头节点
        if (prev == NULL) {
            heap_head = NULL;
        } else {
            prev->next = NULL;
        }
    }
}

// 进行虚拟地址的扩堆
// brk 的语义是将虚拟地址推到某个绝对的地址值
// sbrk是将地址相对的移动多少字节，封装一个sbrk写malloc写起来更顺
void* sbrk(int32_t increment) {
    uint32_t old_brk = (uint32_t)brk(NULL);
    // 使用64位类型是防止出现溢出无法判断
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

void* malloc(uint32_t size) {
    if (size == 0) {
        return NULL;
    }

    // 按八字节对齐
    size = align_up(size);

    struct malloc_chunk* chunk = find_free_chunk(size);
    if (chunk != NULL) { // 如果找到空闲块了，切分后返回
        split_chunk(chunk, size);
        chunk->free = 0;
        // 我们跳过元信息，返回payload，chunk + 1就是跳过元信息的操作
        return (void*)(chunk + 1);
    }

    // 如果没有找到空闲块，我们通过sbrk扩堆
    chunk = request_chunk(size);
    if (chunk == NULL) {
        return NULL;
    }

    return (void*)(chunk + 1);
}

void free(void* ptr) {
    if (ptr == NULL) {
        return;
    }

    struct malloc_chunk* chunk = ((struct malloc_chunk*)ptr) - 1;
    chunk->free = 1;
    coalesce_chunks();
    trim_heap_tail();
}

void* calloc(uint32_t nmemb, uint32_t size) {
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

    // ((struct malloc_chunk*)ptr) - 1 将指针从payload头部移动到元数据头部
    struct malloc_chunk* chunk = ((struct malloc_chunk*)ptr) - 1;
    if (chunk->size >= size) {
        return ptr;
    }

    void* new_ptr = malloc(size);
    if (new_ptr == NULL) {
        return NULL;
    }

    // 把旧数据拷贝到新地址，然后释放旧地址返回新地址
    memcpy(new_ptr, ptr, chunk->size);
    free(ptr);
    return new_ptr;
}
