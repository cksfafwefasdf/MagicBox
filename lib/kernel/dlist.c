#include "dlist.h"
#include "interrupt.h"
#include "stdint.h"
#include "stdbool.h"
#include "debug.h"
#include "print.h"

void dlist_init(struct dlist* plist){
	plist->head.prev = NULL;
	plist->head.next = &plist->tail;
	plist->tail.prev = &plist->head;
	plist->tail.next = NULL;
}

void dlist_insert_front(struct dlist_elem* front,struct dlist_elem* elem){
	// close intr to ensure the atomicity of the operation
	enum intr_status old_status = intr_disable();
	front->prev->next = elem;

	elem->prev = front->prev;
	elem->next = front;

	front->prev = elem;
	
	intr_set_status(old_status);
}

void dlist_push_front(struct dlist* plist,struct dlist_elem* elem){
	dlist_insert_front(plist->head.next,elem);
}

void dlist_push_back(struct dlist* plist,struct dlist_elem* elem){
	dlist_insert_front(&plist->tail,elem);
}

void dlist_remove(struct dlist_elem* pelem){
	enum intr_status old_status = intr_disable();

	pelem->prev->next = pelem->next;
	pelem->next->prev = pelem->prev;

	intr_set_status(old_status);
}

struct dlist_elem* dlist_pop_front(struct dlist* plist){
	struct dlist_elem* elem = plist->head.next;
	
	dlist_remove(elem);
	return elem;
}

bool dlist_find(struct dlist* plist,struct dlist_elem* obj_elem){
	struct dlist_elem* elem = plist->head.next;
	while(elem!=&plist->tail){
		if(elem==obj_elem) return true;
		elem = elem->next;
	}
	return false;
}

bool dlist_empty(struct dlist* plist){
	return (plist->head.next==&plist->tail?true:false);
}

uint32_t dlist_len(struct dlist* plist){
	struct dlist_elem* elem = plist->head.next;
	uint32_t length = 0;
	while(elem!=&plist->tail){
		length++;
		elem = elem->next;
	}
	return length;
}

struct dlist_elem* dlist_traversal(struct dlist* plist,func_condition condition,void* arg){
	struct dlist_elem* elem = plist->head.next;
	uint32_t count = 0; // 安全计数器
	if(dlist_empty(plist)){
		return NULL;
	}
	while(elem!=&plist->tail){

		if (count++ > 100000000) { 
            put_str("\n!!!! DEAD LOOP DETECTED !!!!\n");
			// 假设你的 buffer_head 很大，我们看看 elem 附近的内存
			uint32_t* fault_ptr = (uint32_t*)elem;
			put_str("Memory at elem: ");
			for(int i=0; i<4; i++) {
				put_int(fault_ptr[i]); put_str(" ");
			}
			PANIC("Look at the memory content!");
        }

		if(condition(elem,arg)){
			return elem;
		}
		elem = elem->next;
	}
	return NULL;
}

