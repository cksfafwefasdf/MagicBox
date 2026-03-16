#include <file.h>
#include <fs_types.h>
#include <unistd.h>
#include <file_table.h>
#include <interrupt.h>
#include <inode.h>
#include <stdio-kernel.h>
#include <fifo.h>
#include <tty.h>
#include <ide.h>
#include <sifs_file.h>
#include <device.h>
#include <debug.h>
#include <ext2_file.h>

/*
    该文件中对file的操作与具体的文件系统无关
    属于VFS范畴
*/

// if success, then return 0
// else return -1
int32_t file_close(struct file* file){
	if (file == NULL || file->fd_inode == NULL) {
        return -1;
    }

	file->f_count--;
	// 只有当这个文件是以“独占写”或“执行”模式打开，且我们要彻底释放它时才恢复
	if (file->fd_inode->i_open_cnts == 1 && file->f_count == 1) {
        file->fd_inode->write_deny = false;
    }

	// 只有当 file 的 f_count 为 0 时
	// 说明这个全局表项可以清空了
	// 因此，改该全局表项对应的 inode 的打开数量也要减1
	// 这个打开计数是在 inode_close 中减少的
	if (file->f_count == 0) {

		// 如果是以写权限打开的文件，需要将write_deny置为false
		// 否则缓存上的inode他的write_deny会一直为true，下次缓存命中时，即使没有进程在写
		// 其他进程也不能写！必须要判断当前的这个file是否是在写才行
		// 否则会出现一个进程在写，另一个进程在读，这个读进程退出后直接将这个写进程的write_deny置为false的清空
		// 其实更好的办法应该是要将其换成写计数而不是一个bool的写保护
		// 不然难以处理多进程写的问题
		if((file->fd_flag&O_RDWR)||(file->fd_flag&O_WRONLY)){
			file->fd_inode->write_deny = false;
		}

        // 这时才真正去減少 Inode 的计数
        // 因为这个“打开文件句柄”已经彻底沒人用了
        inode_close(file->fd_inode); 
        
        // 此时将指針置空是安全的，因为没有 FD 指向这里了
        file->fd_inode = NULL; 
        file->fd_pos = 0;
        file->f_op = NULL;
        file->fd_flag = 0;
    }
	return 0;
}

int32_t file_open(struct partition* part, uint32_t inode_no,uint8_t flag){
	int32_t fd_idx = get_free_slot_in_global();
	if(fd_idx==-1){
		printk("exceed max open files\n");
		return -1;
	} 
	// file_open 将一个全局描述符绑定到了一个inode上
	// 因此 i_open_cnt 需要加一，这个加一在 inode_open 中进行
	file_table[fd_idx].fd_inode = inode_open(part,inode_no);
	file_table[fd_idx].fd_pos = 0;
	file_table[fd_idx].f_count = 1;

	file_table[fd_idx].fd_flag = flag;

	bool *write_deny = &(file_table[fd_idx].fd_inode->write_deny);

	if(flag&O_WRONLY||flag&O_RDWR){
		enum intr_status old_status = intr_disable();
		if(!(*write_deny)){
			*write_deny = true;
			intr_set_status(old_status);
		}else{
			intr_set_status(old_status);
			printk("file can't be write now,try again later!\n");
			return -1;
		}
	}

	enum file_types type = file_table[fd_idx].fd_inode->i_type;

	struct super_block* sb = file_table[fd_idx].fd_inode->i_sb;

	switch (type) {
        case FT_PIPE:
			file_table[fd_idx].f_op = &pipe_file_operations;
			break;

		case FT_FIFO: // 具名管道和匿名管道在读写逻辑上是完全一样的
            // 管道逻辑
            file_table[fd_idx].f_op = &fifo_file_operations;
			break;

        case FT_CHAR_SPECIAL: {
            // 字符设备分发
            uint32_t major = MAJOR(file_table[fd_idx].fd_inode->i_rdev);
            if (major == TTY_MAJOR) {
                file_table[fd_idx].f_op = &tty_file_operations;
            }else{
				printk("file_open: this file do not have f_op\n");
			}

            // 以后在此可以扩展其他字符设备
            break;
        }

        case FT_BLOCK_SPECIAL:
			file_table[fd_idx].f_op = &ide_file_operations;
			break;

        case FT_REGULAR:
			if(sb->s_magic==SIFS_FS_MAGIC_NUMBER){
				file_table[fd_idx].f_op = &sifs_file_file_operations;
			}else if(sb->s_magic==EXT2_MAGIC_NUMBER){
				file_table[fd_idx].f_op = &ext2_file_operations;
			}else{
				PANIC("unknown file type!");
			}
			
			break;

        case FT_DIRECTORY:
			if(sb->s_magic==SIFS_FS_MAGIC_NUMBER){
				file_table[fd_idx].f_op = &sifs_dir_file_operations;
			}else if(sb->s_magic==EXT2_MAGIC_NUMBER){
				file_table[fd_idx].f_op = &ext2_file_operations;
			}else{
				PANIC("unknown file type!");
			}
			break;

        default:
            printk("sys_read: unknown file type %d!\n", type);
            PANIC("unknown file type!");
    }

	return pcb_fd_install(fd_idx);
}