#ifndef __LIB_KERNEL_DLIST_H
#define __LIB_KERNEL_DLIST_H
#include "../../kernel/global.h"
#include "../stdbool.h"
#include "../stdint.h"

// offset() is used to compute the offset of the member in struct
// ((struct_type*)0)->member_name is meaningless, but compiler can compute the offset correctly
#define offset(struct_type,member_name) (int)(&(((struct_type*)0)->member_name))
// member_to_entry is used to compute the beginning of the struct
#define member_to_entry(struct_type,member_name,member_ptr) (struct_type*)((int)member_ptr-offset(struct_type,member_name))
#define elem2entry(type,elem_ptr) (type*)(0xfffff000&(int)elem_ptr)

struct dlist_elem{
	struct dlist_elem* prev;
	struct dlist_elem* next;
};

struct dlist{
	// head and tail are both NULL node
	struct dlist_elem head;
	struct dlist_elem tail;
};

typedef bool (func_condition)(struct dlist_elem*,int arg);

void dlist_init(struct dlist* plist);
// insert [elem] in front of the [front]
void dlist_insert_front(struct dlist_elem* front,struct dlist_elem* elem);
// insert [elem] after [plist->head]
void dlist_push_front(struct dlist* plist,struct dlist_elem* elem);
void dlist_iterate(struct dlist* plist);
void dlist_push_back(struct dlist* plist,struct dlist_elem* elem);
// remove [pelem] from the dlist,this operation will not release the space
void dlist_remove(struct dlist_elem* pelem);
struct dlist_elem* dlist_pop_front(struct dlist* plist);
bool dlist_empty(struct dlist* plist);
uint32_t dlist_len(struct dlist* plist);
bool dlist_find(struct dlist* plist,struct dlist_elem* obj_elem);
// traverse the dlist and check if each element satisfies the condition provided by [condition] 
// if satisfy, return this elem. ohterwise, return NULL
struct dlist_elem* dlist_traversal(struct dlist* plist,func_condition condition,int arg);
#endif