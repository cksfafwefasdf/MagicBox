struct test_node {
    uint32_t key;            /* 模拟 b_blocknr */
    struct dlist_elem tag;   /* 链接标签 */
};

/* 1. 哈希函数：直接返回 key */
uint32_t test_hash(void* arg) {
    return (uint32_t)arg;
}

/* 2. 匹配函数 */
bool test_condition(struct dlist_elem* elem, void* arg) {
    uint32_t key = (uint32_t)arg;
    struct test_node* node = member_to_entry(struct test_node, tag, elem);
    return node->key == key;
}

void test_hashtable() {
    struct hashtable ht;
    struct test_node nodes[20];
    int i;

    /* 1. 初始化，给个小的桶数，方便制造冲突 */
    hash_init(&ht, 4, test_hash, test_condition);

    /* 2. 插入数据 */
    for (i = 0; i < 10; i++) {
        nodes[i].key = i;
        hash_insert(&ht, (void*)nodes[i].key, &nodes[i].tag);
    }

    /* 3. 验证查找 */
    for (i = 0; i < 10; i++) {
        struct dlist_elem* e = hash_find(&ht, (void*)i);
        if (e == NULL) {
            // 打印错误：未找到
            PANIC("not found!\n");
        } else {
            struct test_node* n = member_to_entry(struct test_node, tag, e);
            if (n->key != i) {
                // 打印错误：数据不匹配
                PANIC("number not match!\n");
            }
        }
    }

    /* 4. 测试删除 */
    hash_remove(&ht, (void*)5);
    if (hash_find(&ht, (void*)5) != NULL) {
        // 打印错误：删除失败
    }

    /* 5. 制造严重的哈希冲突 */
    // key 4, 8, 12 对 4 取模都是 0
    nodes[10].key = 4; hash_insert(&ht, (void*)4, &nodes[10].tag);
    nodes[11].key = 8; hash_insert(&ht, (void*)8, &nodes[11].tag);
    
    if (hash_find(&ht, (void*)8) == NULL) {
        // 打印错误：冲突查找失败
        PANIC("err in hashtable!\n");
    }

    // 如果能跑到这里没死机没报错，说明你的工具类过关了！
    printk("hash success!\n");
}

void test_buffer_hash_collision() {
    printk("--- Starting Buffer Hash Collision & Search Test ---\n");

    struct disk dummy_disk;
    struct buffer_key key1 = {100, &dummy_disk};
    struct buffer_key key2 = {228, &dummy_disk}; // 故意找一个可能在同一个桶的 key (100 + 128)
    struct buffer_key key_not_exists = {999, &dummy_disk};

    // 1. 获取两个 buffer_head 实例（模拟从池子里拿）
    struct buffer_head* bh1 = &global_ide_buffer.cache_pool[0];
    struct buffer_head* bh2 = &global_ide_buffer.cache_pool[1];

    // 2. 手动设置身份并插入哈希表
    bh1->b_dev = &dummy_disk;
    bh1->b_blocknr = 100;
    hash_insert(&global_ide_buffer.hash_table, &key1, &bh1->hash_tag);

    bh2->b_dev = &dummy_disk;
    bh2->b_blocknr = 228;
    hash_insert(&global_ide_buffer.hash_table, &key2, &bh2->hash_tag);

    // 3. 开始查找测试
    printk("Searching for LBA 100...\n");
    struct dlist_elem* e1 = hash_find(&global_ide_buffer.hash_table, &key1);
    if (e1) {
        struct buffer_head* res1 = member_to_entry(struct buffer_head, hash_tag, e1);
        if (res1->b_blocknr == 100) printk("  Result: SUCCESS (Found 100)\n");
        else printk("  Result: ERROR (Wrong block found: %d)\n", res1->b_blocknr);
    } else {
        printk("  Result: FAILED (100 not found)\n");
    }

    printk("Searching for LBA 228 (Collision check)...\n");
    struct dlist_elem* e2 = hash_find(&global_ide_buffer.hash_table, &key2);
    if (e2) {
        struct buffer_head* res2 = member_to_entry(struct buffer_head, hash_tag, e2);
        if (res2->b_blocknr == 228) printk("  Result: SUCCESS (Found 228)\n");
        else printk("  Result: ERROR (Wrong block found: %d)\n", res2->b_blocknr);
    }

    // 4. 查找不存在的块
    printk("Searching for non-exists LBA 999...\n");
    struct dlist_elem* e3 = hash_find(&global_ide_buffer.hash_table, &key_not_exists);
    if (e3 == NULL) {
        printk("  Result: SUCCESS (Correctly returned NULL)\n");
    } else {
        printk("  Result: FAILED (Returned something for non-exists key)\n");
    }
}

// 将HASH_SIZE改成8，调用此测试，以便于查看在有大量hash碰撞的情况下
// 我们是否能正确找到元素
void test_high_collision_search() {
    printk("--- High Collision Search Test (HASH_SIZE=8) ---\n");

    struct disk dummy_disk;
    struct buffer_key key_a = {1, &dummy_disk}; // 假设落在桶 1
    struct buffer_key key_b = {9, &dummy_disk}; // 1 + 8, 极大概率也落在桶 1
    struct buffer_key key_c = {17, &dummy_disk};// 1 + 16, 极大概率也落在桶 1

    // 1. 模拟从池中拿出 3 个 buffer_head
    struct buffer_head* bh_a = &global_ide_buffer.cache_pool[10];
    struct buffer_head* bh_b = &global_ide_buffer.cache_pool[11];
    struct buffer_head* bh_c = &global_ide_buffer.cache_pool[12];

    // 2. 赋予身份并插入（此时它们都挤在同一个桶的链表里）
    bh_a->b_dev = &dummy_disk; bh_a->b_blocknr = 1;
    hash_insert(&global_ide_buffer.hash_table, &key_a, &bh_a->hash_tag);

    bh_b->b_dev = &dummy_disk; bh_b->b_blocknr = 9;
    hash_insert(&global_ide_buffer.hash_table, &key_b, &bh_b->hash_tag);

    bh_c->b_dev = &dummy_disk; bh_c->b_blocknr = 17;
    hash_insert(&global_ide_buffer.hash_table, &key_c, &bh_c->hash_tag);

    // 3. 验证“穿透”查找：即使 key_a 在链表最前面，找 key_c 也必须成功
    printk("Testing lookup for LBA 17 (Deep in the chain)...\n");
    struct dlist_elem* e = hash_find(&global_ide_buffer.hash_table, &key_c);
    
    if (e) {
        struct buffer_head* res = member_to_entry(struct buffer_head, hash_tag, e);
        if (res->b_blocknr == 17) {
            printk("  SUCCESS: Found LBA 17 deep in the bucket!\n");
        } else {
            printk("  ERROR: Found wrong LBA: %d\n", res->b_blocknr);
        }
    } else {
        printk("  FAILED: Could not find LBA 17!\n");
    }

    // 4. 验证删除后的查找
    hash_remove_elem(&bh_b->hash_tag);
    if (hash_find(&global_ide_buffer.hash_table, &key_b) == NULL) {
        printk("  SUCCESS: LBA 9 removed and confirmed gone.\n");
    }
}