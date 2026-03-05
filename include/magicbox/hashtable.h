#ifndef __DEVICE_HASHTABLE_H
#define __DEVICE_HASHTABLE_H
#include "dlist.h"

// 黄金分割数 0.618033...
// 具有一个奇妙的数学特性：它是最“无理”的无理数。
// 用它作为乘数，可以保证在连续的输入（
// 比如 LBA 为 1, 2, 3, 4...）下，
// 产生的哈希结果在 32 位或 64 位空间内具有最大的不相关性。
#define HASH_GOLDEN_RATIO_32 0x61C88647
#define HASH_GOLDEN_RATIO_64 0x9E3779B97F4A7C15

typedef uint32_t (*hash_callback)(void* arg);

// 为了实现机制和策略分离，我们的hashtable类里面不加锁
// 并发安全性应该由哈希表的调用者保证
struct hashtable{
	struct dlist* buckets; // bucket 数组，用于存放所有的桶
	uint32_t bucket_nr;
	uint32_t elem_nr;
	hash_callback hash_func; // 用于计算backet索引
	func_condition condition; // 用于bucket内的元素查找
};

extern void hash_free(struct hashtable *hash); 
extern void hash_remove_elem(struct hashtable *hash, struct dlist_elem* pelem);
extern void hash_remove(struct hashtable *hash, void *arg);
extern void hash_insert(struct hashtable *hash, void *arg, struct dlist_elem* pelem);
extern struct dlist_elem* hash_find(struct hashtable *hash, void *arg);
extern void hash_init(struct hashtable *hash,uint32_t bucket_nr, hash_callback hash_func, func_condition condition);

#endif