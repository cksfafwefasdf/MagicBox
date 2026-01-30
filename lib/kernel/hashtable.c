#include "hashtable.h"
#include "memory.h"
#include "stdint.h"
#include "dlist.h"
#include "stdbool.h"
#include "debug.h"

void hash_init(struct hashtable *hash,uint32_t bucket_nr, hash_callback hash_func, func_condition condition){
	uint32_t bucket_idx;
	hash->bucket_nr = bucket_nr;
	hash->buckets = (struct dlist*) kmalloc(bucket_nr*sizeof(struct dlist));
	hash->elem_nr = 0;
	if(NULL==hash->buckets){
		PANIC("hash->buckets is NULL!\n");
	}
	hash->hash_func = hash_func;
	hash->condition = condition;
	for(bucket_idx=0;bucket_idx<bucket_nr;bucket_idx++){
		dlist_init(&hash->buckets[bucket_idx]);
	}
}

static inline uint32_t get_bucket(struct hashtable* hash,void* arg){
	// 取模，防止溢出
	return (hash->hash_func(arg)) % hash->bucket_nr;
}


struct dlist_elem* hash_find(struct hashtable *hash, void *arg){
	// 先获取桶索引，在此处强制取模一次，防止创建hash函数时忘记了
	uint32_t bucket_idx = get_bucket(hash,arg);
	// 到相应的桶中查找元素
	struct dlist_elem* elem = dlist_traversal(&hash->buckets[bucket_idx], hash->condition, arg);
	return elem;
}

void hash_insert(struct hashtable *hash, void *arg, struct dlist_elem* pelem){
	uint32_t bucket_idx = get_bucket(hash,arg);
	if(NULL!=dlist_traversal(&hash->buckets[bucket_idx],hash->condition,arg)){
		return;
	}
	dlist_push_back(&hash->buckets[bucket_idx],pelem);
	hash->elem_nr++;
}

void hash_remove(struct hashtable *hash, void *arg){
	uint32_t bucket_idx = get_bucket(hash,arg);
	struct dlist_elem* felem = dlist_traversal(&hash->buckets[bucket_idx],hash->condition,arg);
	if(NULL==felem) return;
	dlist_remove(felem);
	hash->elem_nr--;
}

/* O(1) 版本的删除：直接剥离节点，不需要 Key，也不需要重新计算哈希 */
void hash_remove_elem(struct hashtable *hash,struct dlist_elem* pelem) {
    if (pelem != NULL) {
        dlist_remove(pelem);
		hash->elem_nr--;
    }
}



void hash_free(struct hashtable *hash){
	if(NULL!=hash->buckets) kfree(hash->buckets);
}
