#ifndef __FS_SIFS_DIR_H
#define __FS_SIFS_DIR_H
#include "stdint.h"
#include "stdbool.h"
#include "unistd.h"
#include "fs_types.h"

struct partition;


// 我们现在取消了 dir 结构体，我们将 dir 结构体并入到 file 结构体中
// 将 dir 也看做一种文件，然后通过文件类型来分发操作
// 这是同步会磁盘时，针对不同的文件系统所设置的不同磁盘镜像结构体
struct sifs_dir_entry {
    char filename[MAX_FILE_NAME_LEN];
    uint32_t i_no;
    enum file_types f_type;
};

extern struct inode* root_dir_inode;

extern bool sifs_search_dir_entry(struct partition* part,struct inode* dir_inode,const char* name,struct sifs_dir_entry* p_de);
// 我们将 dir 的关闭逻辑并入 file_close 中
// extern void dir_close(struct inode* inode);
// 同样的，这个open函数也可以废弃了，直接放到file_open里面来处理
// 对于像是 search_file 这样的函数，我们直接移除了里面原本对于 dir_open 和 dir_close 的调用
// 直接对父目录的 inode 进行 inode_open 和 inode_close 操作
// 因为在 search_file 的过程中，其实我们并不需要 fd_pos 这个游标，本质上其实也就是在操作他的inode，因此我们直接改成对inode的操作
// 而对于 open 操作，我们才使用 file_open 来进行统一控制
// extern struct dir* dir_open(struct partition* part,uint32_t inode_no);
extern void open_root_dir(struct partition* part);
extern void sifs_create_dir_entry(char* filename, uint32_t inode_no, enum file_types file_type, struct sifs_dir_entry* p_de);
extern bool sifs_sync_dir_entry(struct inode* parent_inode, struct sifs_dir_entry* p_de, void* io_buf);
extern bool sifs_delete_dir_entry(struct partition* part, struct inode* parent_inode, uint32_t inode_no, void* io_buf);
extern int32_t sifs_dir_read(struct file* file, struct dirent* de);
extern int32_t sifs_dir_remove(struct inode* parent_inode, struct inode* child_inode);
extern bool sifs_dir_is_empty(struct inode* dir_inode);
extern void close_root_dir(struct partition* part);

#endif