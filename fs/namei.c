#include <namei.h>
#include <fs.h>
#include <unitype.h>
#include <fs_types.h>
#include <errno.h>
#include <debug.h>
#include <inode.h>
#include <stdio.h>
#include <stdio-kernel.h>
#include <ide.h>

// search_file 函数所使用的状态机定义
typedef enum {
    STATE_START,            // 初始化起点
    STATE_PARSE_COMPONENT,  // 解析路径中的下一个名字 (usr, bin等)
    STATE_LOOKUP,           // 在磁盘/缓存中查找该名字
    STATE_CHECK_TYPE,       // 检查查到的 Inode 类型（目录？链接？挂载点？）
    STATE_FOLLOW_LINK,      // 处理符号链接
    STATE_DONE,             // 解析成功结束
    STATE_ERROR             // 出错结束
} path_state_t;

char* _path_parse(char* pathname,char* name_store){
	if(pathname[0]=='/'){
		while(*(++pathname)=='/');
	}

	while(*pathname!='/'&&*pathname!=0){
		*name_store++ = *pathname++;
	}

	if(pathname[0]==0){
		return NULL;
	}
	return pathname;
}

int32_t path_depth_cnt(char* pathname){
	ASSERT(pathname!=NULL);
	char* p = pathname;
	char name[MAX_FILE_NAME_LEN];

	uint32_t depth = 0;

	p = _path_parse(p,name);
	while(name[0]){
		depth++;
		memset(name,0,MAX_FILE_NAME_LEN);
		if(p){
			p=_path_parse(p,name);
		}
	}
	return depth;
}

// search_file 只关闭中间搜索过程中的“中间父目录”，最终的父目录无论如何他都不关闭
// 由调用者关闭
// follow_link 用于指明是否跟踪link，有些时候我们需要判断链接本身是否存在，而不是判断链接指向的文件是否存在
// 这时我们需要将其置为 false
// 例如，如果返回 -ENOENT 我们需要区分是“真没坑位”还是“断链占坑”
// 这是因为对于一个符号链接而言，有两种情况都会导致 search_file 返回 ENOENT
// 一种是 符号链接文件本身不存在，还有一种是符号链接所指向的文件不存在
// 我们的 search_file 函数功能比较简单，只会一条路走到底
// 当它去跟踪一个符号链接时，他会一路走到底，当发现符号链接指向的目标不存在时，他会返回 -ENOENT
// 因此我们只通过 -ENOENT 是无法直接判断出到底是符号链接本身不存在还是符号链接指向的文件不存在的
// 对于followlink的传入值而言
// 消费型函数 (open, stat, mount, chdir)：通常传入 true，因为它们关心路径指向的那个“实体内容”。
// 管理型/创建型函数 (mknod, mkdir, symlink, unlink, rename)：必须传入 false，因为它们操作的是目录里的那个“名字（Entry）”本身。
int32_t search_file(char* pathname, struct path_search_record* searched_record, bool follow_link) {

    // search_file 接收到的必须是绝对地址
    ASSERT(pathname[0] == '/');

    char path_buf[MAX_PATH_LEN];
    memset(searched_record, 0, sizeof(struct path_search_record));
    memcpy(path_buf, pathname, MAX_PATH_LEN);

    path_state_t state = STATE_START;
    struct inode* curr_inode = NULL;  
    struct inode* next_inode = NULL;  
    char* p = path_buf;
    char name[MAX_FILE_NAME_LEN];
    int symlink_depth = 0;

    while (state != STATE_DONE && state != STATE_ERROR) {
        switch (state) {
            case STATE_START:
                if (curr_inode && curr_inode != root_dir_inode) inode_close(curr_inode);
                curr_inode = inode_open(root_part, root_dir_inode->i_no);
                p = path_buf;
                // 跳过所有开头的斜杠，防止 // 导致的空组件
                while (*p == '/') p++; 
                next_inode = NULL;
                // 初始化 record，默认指向根
                searched_record->parent_inode = curr_inode;
                searched_record->i_dev = curr_inode->i_dev;
                memset(searched_record->searched_path, 0, MAX_PATH_LEN);
                // 初始化一个根目录
                searched_record->searched_path[0] = '/'; // 设为 "/"
                searched_record->searched_path[1] = '\0';
                state = STATE_PARSE_COMPONENT;
                break;

            case STATE_PARSE_COMPONENT:
                while (*p == '/') p++;
                if (*p == 0) { // 根目录或者以 / 结尾
                    searched_record->file_type = FT_DIRECTORY;
                    state = STATE_DONE;
                    break;
                }
                memset(name, 0, MAX_FILE_NAME_LEN);
                char* name_start = p;
                while (*p && *p != '/') p++;
                int len = p - name_start;
                memcpy(name, name_start, len);

                if (searched_record->searched_path[0] == '\0') {
                    // 只有在路径完全为空时（通常不应发生），才初始化为 /
                    strcpy(searched_record->searched_path, "/");
                } 

                // 如果当前路径不是只有 "/"，则需要补一个 "/"
                uint32_t sp_len = strlen(searched_record->searched_path);
                if (sp_len > 1 && searched_record->searched_path[sp_len - 1] != '/') {
                    strcat(searched_record->searched_path, "/");
                }

                strcat(searched_record->searched_path, name);
                state = STATE_LOOKUP;
                break;

            case STATE_LOOKUP:
                // printk("Lookup: name='%s' in parent_ino=%d\n", name, curr_inode->i_no);
                // 处理 ".." 向上穿透
                if (len == 2 && memcmp(name, "..", 2) == 0) {
                    if (curr_inode->i_no == curr_inode->i_sb->s_root_ino && curr_inode->i_mount_at) {
                        struct inode* old = curr_inode;
                        struct inode* mp = curr_inode->i_mount_at;
                        curr_inode = inode_open(get_part_by_rdev(mp->i_dev), mp->i_no);
                        if (old != root_dir_inode) inode_close(old);
                    }
                }

                // 磁盘查找
                if (curr_inode->i_op->lookup(curr_inode, name, len, &next_inode) != 0) {
                    // 没找到，保持 curr_inode 为 parent_inode
                    searched_record->parent_inode = curr_inode;
                    searched_record->i_dev = curr_inode->i_dev;
                    state = STATE_ERROR;
                } else {
                    state = STATE_CHECK_TYPE;
                }
                break;

            case STATE_CHECK_TYPE:
                //  处理向下跳转 (进入挂载分区的根)
                while (next_inode->i_mount != NULL) {
                    struct inode* mounted_root = next_inode->i_mount;
                    inode_open(get_part_by_rdev(mounted_root->i_dev), mounted_root->i_no);
                    inode_close(next_inode);
                    next_inode = mounted_root;
                }

                // 检查符号链接
                if (next_inode->i_type == FT_SYMLINK) {
                    // 判断是否为路径的最后一个组件（通过 p 指针是否到头来判断）
                    bool is_last = (*p == 0);

                    // 如果不是最后一个组件，必须 Follow（为了穿透中间路径）
                    // 如果是最后一个组件，则取决于调用者的意愿 (follow_last)
                    if (!is_last || follow_link) {
                        state = STATE_FOLLOW_LINK;
                    } else {
                        // 命中了，不跟随最后一个链接，直接返回链接自己的 Inode
                        searched_record->file_type = FT_SYMLINK;
                        searched_record->parent_inode = curr_inode;
                        uint32_t link_ino = next_inode->i_no;
                        inode_close(next_inode);
                        return link_ino;
                    }
                } else if (*p == 0) { // 路径终点判断
                    searched_record->file_type = next_inode->i_type;
                    searched_record->i_dev = next_inode->i_dev; // 确保是跳转后的设备号
                    searched_record->parent_inode = curr_inode; 
                    uint32_t final_ino = next_inode->i_no;
                    inode_close(next_inode); 
                    return final_ino; // 直接返回，避免 DONE 状态的复杂逻辑
                } else { // 推进到下一层目录
                    if (curr_inode != root_dir_inode) inode_close(curr_inode);
                    curr_inode = next_inode;
                    next_inode = NULL;
                    // 更新 record，当前的 next 变成了下一轮的 parent
                    searched_record->parent_inode = curr_inode;
                    searched_record->i_dev = curr_inode->i_dev;
                    state = STATE_PARSE_COMPONENT;
                }
                break;

            case STATE_FOLLOW_LINK:
                // 防止出现循环链接
                if (++symlink_depth > 8) {
                    printk("search_file: Too many levels of symbolic links!\n");
                    // 即使出错，也要把当前的现场交给 record，防止调用者误判为“找不到父目录”
                    searched_record->parent_inode = curr_inode;
                    state = STATE_ERROR; 
                    return -ELOOP; 
                }

                char link_target[MAX_PATH_LEN] = {0};

                int link_len = next_inode->i_op->readlink(next_inode, link_target, MAX_PATH_LEN);

                if (link_len <= 0) {
                    state = STATE_ERROR;
                    break;
                }

                inode_close(next_inode);

                // 拼凑新路径，link_target + 剩下的 p
                char temp[MAX_PATH_LEN];
                // 如果 p 后面还有内容，且不是以 / 开头，才补 /
                char* sep = (*p != 0 && *p != '/') ? "/" : "";
                // 如访问 /dir/link1/file1 ，link1指向 dir2/dir （相对路径） 时，我们最终会拼接成 /dir/dir2/dir/file1
                // 如访问 /dir/link1/file1 ，link1指向 /dir2/dir （绝对路径）时，我们会拼接成 /dir2/dir/file1，最开始已经解析完了的上下文会被丢弃，然后我们必须从头开始解析
                sprintf(temp, "%s%s%s", link_target, sep, p);

                memcpy(path_buf, temp, MAX_PATH_LEN);

                if (link_target[0] == '/') {
                    state = STATE_START; // 绝对路径链接，重启，重新从根目录开始搜寻
                    // 只清空路径记录，不要动其他元数据，以便出问题时debug起来简单点
                    memset(searched_record->searched_path, 0, MAX_PATH_LEN);
                } else {
                    // 相对路径链接，原地重启解析
                    // 我们需要把 path_buf 中已经解析掉的部分去掉，换成符号链接解析出来的内容
                    p = path_buf; 
                    state = STATE_PARSE_COMPONENT;
                    // 还需要手动把已经记录在 searched_path 里的符号链接所在位置的符号链接 dir_lnk 给删了
                    char* last_slash = strrchr(searched_record->searched_path, '/');
                    if (last_slash) *last_slash = '\0';
                }
                break;
        }
    }
    // 处理以 / 结尾的情况
    if (state == STATE_DONE) {
        searched_record->i_dev = curr_inode->i_dev;
        return curr_inode->i_no;
    }
    return -ENOENT;
}

// 将路径转换为绝对路径
void make_abs_pathname(const char* pathname, char* abs_path) {
    if (pathname[0] == '/') {
        // 如果已经是绝对路径，直接拷贝
        strcpy(abs_path, pathname);
    } else {
        // 如果是相对路径，先获取当前目录
        sys_getcwd(abs_path, MAX_PATH_LEN);
        
        // 针对根目录 "/" 做特殊处理，避免拼成 "//name"
        if (strcmp(abs_path, "/") != 0) {
            strcat(abs_path, "/");
        }
        strcat(abs_path, pathname);
    }
}