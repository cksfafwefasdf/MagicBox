#ifndef __INCLUDE_MAGICBOX_FS_TYPES_H
#define __INCLUDE_MAGICBOX_FS_TYPES_H
#include <stdint.h>
#include <unistd.h>
#include <sifs_inode.h>
#include <sifs_sb.h>
#include <pipe.h>
#include <ext2_sb.h>
#include <ext2_inode.h>

/*
	虚拟文件系统层的头文件
	主要用于存储和具体文件系统无关的
	VFS 数据结构
*/

// the max number of the file in each partitions is 4096
#define MAX_FILES_PER_PART 4096
// each sector has 512 B, which equals to 512*8=4096 b
#define BITS_PER_SECTOR 4096 

#define ADDR_BYTES_32BIT 4 // size of the 32 bits address pointor

// 匿名 inode 的 i_no
#define ANONY_I_NO 0xffffffff

#define FS_REQUIRES_DEV      0x01 // 需要物理设备（如 SIFS, Ext2）
#define FS_SINGLE            0x02 // 整个系统只有一个实例（如 procfs） 
#define FS_NOMOUNT           0x04 // 不允许用户从外部挂载

#define SECTOR_SIZE 512

// 内存inode
// VFS 直接操作的inode 对象
struct inode{
	enum file_types i_type;
	// 在目录文件中 i_size 标记的是该目录文件目前所达到的最大逻辑偏移量。
	// 对于目录文件而言，这个值只增不减，只会越变越大，但是对于普通文件而言他是会减小的！
	// 目录文件i_size只增不减的目的是为了避免目录“空洞”问题，比如 1 2 3 中，删除了 2，这样就剩下 1 X 3，中间的X是空洞
	// 如果我们将 i_size 减小的话，那么我们在 readdir 当中的逻辑就不好写了，因为此时我们的 i_size 的大小只是两个目录项，这样的话 readdir 读两次就停止了
	// 根本识别不到第三个 3 了！因此为了便于实现 readdir ，我们需要让探测的序列是 “连续” 的，因此我们删除文件时，就只是回收这部分空间，但是 i_size 并不减小
	// 这么干看起来很奇怪，但是其实linux的ext4文件系统也是这么实现的！可以做实验验证
	// 这个只增不减的原理其实有点类似于用线性探测法来解决哈希冲突，在线性哈希表中，如果删除元素，也是只是将文件标记为可覆盖，不会真将其删除
	// 否则会影响在这个文件之后的其他文件的查找，这里的道理也类似
	uint32_t i_size;
	uint32_t i_rdev; // 这个 inode 表示哪一个设备（针对设备inode使用）
	uint32_t i_no;
	uint32_t i_dev; // 这个 inode 存在哪个持久化设备上
	// 有多少个全局打开文件表项指向这个inode
	uint32_t i_open_cnts;
	// write operation will cause concurrent safty problem
	// so make write_deny true, before write the file. 
	bool write_deny;
	// this tag is used for the 'already opened inode queue'
	// to prevent redundant reads of inodes from the disk.

	struct super_block* i_sb;      // 指向该 inode 所属分区的 VFS 超级块
    struct inode* i_mount;         // 向下隧道，如果我是挂载点(如 /mnt)，我指向被挂载分区的根 inode
    struct inode* i_mount_at;      // 向上隧道，如果我是被挂载分区的根，我指向父分区的挂载点 inode

	struct inode_operations* i_op;

	struct dlist_elem lru_tag; // 哈希表节点：用于根据 (i_dev, i_no) 快速找到 inode
    struct dlist_elem hash_tag;  // LRU节点：用于当缓冲区满时，决定踢掉哪个 inode
	union{
		struct sifs_inode_info sifs_i;
		struct pipe_inode_info pipe_i;
		struct ext2_inode_info ext2_i; 
	};
};

struct file{
	uint32_t fd_pos;
	uint32_t fd_flag;
	struct inode* fd_inode;
	// enum file_types f_type;
	// f_count 用于表示有多少局部FD指向此全局表中的 file，主要用于处理fork和dup2
	// inode 的 i_open_cnts 表示有多少个全局 file结构指向同一个inode
	// 通过多设置一个 f_count，将inode的生命周期管理和file的生命周期管理分开了
	uint32_t f_count;     
	struct file_operations* f_op; // 指向该文件的具体操作集
};

struct path_search_record{
	char searched_path[MAX_PATH_LEN];
	struct inode* parent_inode; // 父目录的inode
	enum file_types file_type;
    uint32_t i_dev; // 所在设备号
};

// 用于 vfs，抽象文件操作
// 不同的文件类型（例如块设备文件、字符设备文件、普通文件、目录文件、pipe文件、fifo文件）会对应不同的操作集
struct file_operations {
	int (*lseek) (struct inode *, struct file *, int32_t, int32_t);
	int (*read) (struct inode *, struct file *, char *, int);
	int (*write) (struct inode *, struct file *, char *, int);
	int (*readdir) (struct inode *, struct file *, struct dirent *, int);
	// int (*select) (struct inode *, struct file *, int, select_table *);
	int (*ioctl) (struct inode *, struct file *, uint32_t, uint32_t);
	int (*mmap) (struct inode *, struct file *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
	int (*open) (struct inode *, struct file *);
	// 由 close 操作中，打开计数为0时调用
	int32_t (*release) (struct inode *, struct file *);
	// int (*fsync) (struct inode *, struct file *); // 将内存中的缓存强制同步回磁盘
};

// 不同文件系统有不同的inode操作集
// 同一文件系统，根据不同的inode类型，分别填充这个inode操作集合里的不同函数
// 例如 dir inode 会有 lookup, create, mkdir, rmdir, unlink 等
// 设备 inode 通常几乎全是 NULL，它只是一个占位符。
// 当 VFS 看到这个 Inode 时，它知道这不需要文件系统去做什么元数据操作，只需要去调用它的 i_fop（指向 TTY 或磁盘驱动）就可以了。
// 普通文件 inode 它负责管理文件本身的数据块。一般包含函数：truncate (截断文件), bmap (逻辑块转物理块)。
// 普通文件inode 没有 lookup/mkdir，因为普通文件不是容器，VFS 根本不会对它调用这些函数
// 总的来说，同一文件系统主要分成 设备inode，目录inode和普通文件inode三种
struct inode_operations {
	struct file_operations * default_file_ops;
	// 创建普通文件
	int (*create) (struct inode *,char *,int,int);
	// 在给定的目录中，寻找名字为name的子项
	// VFS 在解析路径 /a/b/c 时，会先拿到 / 的 inode，然后调用 root_inode->i_op->lookup(root, "a", ...) 得到 a 的 inode，如此递归。
	int (*lookup) (struct inode *,char *,int,struct inode **); 
	// 硬链接
	// int (*link) (struct inode *,struct inode *,const char *,int); 
	// 删除文件
	int (*unlink) (struct inode *,char *,int);
	// 符号链接
	// int (*symlink) (struct inode *,char *,int,const char *);
	// 创建子目录文件
	int (*mkdir) (struct inode *,char *,int,int);
	int (*rmdir) (struct inode *,char *,int);
	// 创建设备节点
	int (*mknod) (struct inode *,char *,int,int,int);
	// 将一个文件从 A 路径改为 B 路径。
	// 如果是在同一个目录下改名，只需修改目录项。
	// 如果是跨目录移动（比如把 /a/f1 移动到 /b/f2），则需要从 A 目录删除项，在 B 目录增加项。它是原子性的保证。
	// 参数里有两个 inode。第一个是源目录，第二个是目标目录。
	int (*rename) (struct inode *,char *,int,struct inode *,char *,int);
	// 用户态调用（如 readlink 命令）。它把符号链接文件里的“内容”（即指向的路径字符串）读出来。
	// int (*readlink) (struct inode *,char *,int);
	// 路径解析时调用。
	// 当访问 /mnt/link_to_a 时，路径解析引擎发现这是一个软链接，就会调用 follow_link。
	// 它会告诉 VFS 别停下，继续去搜这个软链接指向的那个目标 inode
	// int (*follow_link) (struct inode *,struct inode *,int,int,struct inode **);
	// 块映射, 传入文件的逻辑块号（例如：文件的第 0, 1, 2 个块），返回该块在磁盘上的物理扇区号（LBA）。
	// 最典型的是 Swap（交换分区）。
	// 当内核需要把内存页换出到文件时，它不能通过文件系统层去写（那太慢且容易死锁）
	// 它会通过 bmap 预先查好所有物理块位置，直接用驱动写裸扇区。
	// 这个函数主要是用于解析间址块的
	int (*bmap) (struct inode *,int);
	// 截断文件（比如 open 时带了 O_TRUNC）。
	// 它负责释放文件占用的磁盘块，并把 i_size 置为 0。
	void (*truncate) (struct inode *);
	// 检查当前进程是否有权对该 inode 执行特定的操作（读、写、执行）
	// int (*permission) (struct inode *, int);
};

// 在我们的实现中，为了简单起见，一个文件系统只会对应一个 super_operations
// 在同一文件系统下不会和inode一样再进一步拆分！
struct super_operations {
	// 根据 inode->i_no，去磁盘上找到对应的扇区，读取原始数据并填充 struct inode 的其他字段（如大小、块映射、时间等）。
	// 在linux中，当调用 iget(sb, nr) 时，如果内存缓存里没有，VFS 就会触发这个调用。
	void (*read_inode) (struct inode *);
	// 属性变更通知。当文件的权限（Chmod）或所有者（Chown）发生变化时，VFS 会通过这个函数通知文件系统，这块元数据变了，准备一下同步操作
	// int (*notify_change) (int flags, struct inode *);
	// 同步元数据。把内存中被修改过的 inode 属性（比如刚 write 完，文件变大了）写回磁盘。
	// 这就是当前系统中一直手动调用的 inode_sync 的规范写法。
	void (*write_inode) (struct inode *);
	// 释放。当 Inode 的引用计数降为 0，准备从内存中销毁时调用。可以在这里做一些收尾工作。
	void (*put_inode) (struct inode *);
	// 卸载（Umount）时调用。负责释放该文件系统占用的所有内核资源，关闭磁盘驱动
	void (*put_super) (struct super_block *);
	// 同步超级块。比如 SIFS 里的位图（Bitmap）变了、空闲块数变了，这个函数负责把内存里的 super_block 结构写回磁盘的第一号扇区（或者对应的位置）。
	void (*write_super) (struct super_block *);
	// 查询统计信息。比如用户输入 df -h 命令时，VFS 就会调用这个函数来查询，这个分区总共多大？还剩多少空闲块？Inode 用了多少？
	void (*statfs) (struct super_block *, struct statfs *);
	// 重新挂载。比如原本是只读挂载（Read-Only），现在想改成读写（Read-Write），这个函数负责修改挂载标志位而不需要重新解析整个分区。
	// int (*remount_fs) (struct super_block *, int *, char *);
};

struct super_block {
    // VFS 通用字段
    uint32_t s_dev; // 超级块所存储在的逻辑设备号
    uint32_t s_block_size; 
    uint32_t s_root_ino; // 该超级块的根目录的 inode 的编号
	uint32_t s_magic;
    struct inode* s_root_inode; // 指向该超级块的根目录的 inode

	struct super_operations* s_op;
    
    // VFS 对不同文件系统打的补丁
    union {
        struct sifs_sb_info sifs_info; // 此时这里就包含了运行时的状态
        struct ext2_sb_info ext2_info;
    };
};

// 用来定义超级块的填充操作
// 主要用于填充 info 结构体
struct file_system_type {
    char *name;
    int flags; // 使用宏，如 FS_REQUIRES_DEV, 用于表示这个文件系统是否对应真实的设备，例如pipe文件系统就不对应真实的设备，他是内存文件系统
    struct super_block *(*read_super) (struct super_block *, void *, int);
    struct dlist_elem fs_elem; // 用于建立一个文件系统链表，以便于后续动态加载更多文件系统
};
#endif
