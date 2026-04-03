#include <string.h>
#include <syscall.h>
#include <stdio.h>
#include <assert.h>
#include <unitype.h>
#include <stdint.h>
#include <stdbool.h>
#include "buildin_cmd.h"
#include "shell.h"

char* path_parse(char* pathname,char* name_store){
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

static void wash_path(char* old_abs_path,char* new_abs_path){
	assert(old_abs_path[0]=='/');
	char name[MAX_FILE_NAME_LEN] = {0};
	char* sub_path = old_abs_path;
	sub_path = path_parse(sub_path,name);

	if(name[0]==0){
		new_abs_path[0] = '/';
		new_abs_path[1] = 0;
		return;
	}

	new_abs_path[0] = 0;
	strcat(new_abs_path,"/");
	while(name[0]){
		if(!strcmp("..",name)){
			char* slash_ptr = strrchr(new_abs_path,'/');
			if(slash_ptr!=new_abs_path){
				*slash_ptr = 0;
			}else{
				*(slash_ptr+1) = 0;
			}
		}else if(strcmp(".",name)){
			if(strcmp(new_abs_path,"/")){
				strcat(new_abs_path,"/");
			}
			strcat(new_abs_path,name);
		}

		memset(name,0,MAX_FILE_NAME_LEN);
		if(sub_path){
			sub_path = path_parse(sub_path,name);
		}
	}
}

void make_clear_abs_path(char* path,char* final_path){
	char abs_path[MAX_PATH_LEN] = {0};
	if(path[0]!='/'){
		memset(abs_path,0,MAX_PATH_LEN);
		if(getcwd(abs_path,MAX_PATH_LEN)!=NULL){
			if(!((abs_path[0]=='/')&&(abs_path[1]==0))){
				strcat(abs_path,"/");
			}
		}
	}
	strcat(abs_path,path);
	wash_path(abs_path,final_path);
}

void buildin_pwd(uint32_t argc,char** argv UNUSED){
	if(argc!=1){
		printf("pwd: no argument support!|n");
		return ;
	}else{
		if(NULL!=getcwd(final_path,MAX_PATH_LEN)){
			printf("%s\n",final_path);
		}else{
			printf("pwd: get current work directory failed\n");
		}
	}
}

char* buildin_cd(uint32_t argc,char** argv){
	if(argc>2){
		printf("cd: only support 1 argument!\n");
		return NULL;
	}

	if(argc==1){
		final_path[0]='/';
		final_path[1]=0;
	}else{
		make_clear_abs_path(argv[1],final_path);
	}

	if(chdir(final_path) < 0){
		printf("cd: no such direcotry %s\n",final_path);
		return NULL;
	}
	return final_path;
}

void buildin_ls(uint32_t argc, char** argv) {

    char* pathname = NULL;
    struct stat file_stat;
    memset(&file_stat, 0, sizeof(struct stat));
    bool long_info = false;
    uint32_t arg_path_nr = 0;
    uint32_t arg_idx = 1;

    // 解析命令行参数
    while (arg_idx < argc) {
        if (argv[arg_idx][0] == '-') {
            if (!strcmp("-l", argv[arg_idx])) {
                long_info = true;
            } else if (!strcmp("-h", argv[arg_idx])) {
                printf("usage:\n  -l list all information\n  -h for help\n");
                return;
            } else {
                printf("ls: invalid option %s\n", argv[arg_idx]);
                return;
            }
        } else {
            if (arg_path_nr == 0) {
                pathname = argv[arg_idx];
                arg_path_nr = 1;
            } else {
                printf("ls: only support one path!\n");
                return;
            }
        }
        arg_idx++;
    }

    // 确定目标路径
    char final_path[MAX_PATH_LEN] = {0};
    if (pathname == NULL) {
        if (NULL != getcwd(final_path, MAX_PATH_LEN)) {
            pathname = final_path;
        } else {
            printf("ls: getcwd failed\n");
            return;
        }
    } else {
        make_clear_abs_path(pathname, final_path);
        pathname = final_path;
    }

    // 获取目标路径的状态
    if (stat(pathname, &file_stat) < 0) {
        printf("ls: cannot access %s: No such file or directory\n", pathname);
        return;
    }

    // 处理目录逻辑
    if (file_stat.st_filetype == FT_DIRECTORY) {
        int32_t fd = open(pathname,O_RDONLY); 
        if (fd < 0) {
            printf("ls: opendir %s failed\n", pathname);
            return;
        }

        struct dirent dir_e; 
        char sub_pathname[MAX_PATH_LEN] = {0};
        uint32_t pathname_len = strlen(pathname);
        memcpy(sub_pathname, pathname, pathname_len);

        if (sub_pathname[pathname_len - 1] != '/') {
            sub_pathname[pathname_len] = '/';
            pathname_len++;
        }

        rewinddir(fd); 

        if (long_info) {
			
            printf("total: %d\n", (int32_t)file_stat.st_size);
            while (readdir(fd, &dir_e) > 0) {
        
                char ftype = (DT_DIR == dir_e.d_type) ? 'd' : '-';

                sub_pathname[pathname_len] = 0;
                
                strcat(sub_pathname, dir_e.d_name);
                memset(&file_stat, 0, sizeof(struct stat));
                if (stat(sub_pathname, &file_stat) < 0) {
                    printf("ls: cannot access %s\n", dir_e.d_name);
                    close(fd);
                    return;
                }

				// st_size 是 64 位的，为了防止 printf 读 dir_e.d_name 读到 st_size 的高 32 位，我们要将这个数据强转成 32 位的
                if (DT_DIR == dir_e.d_type) {
                    printf("%c %d %d " BLUE "%s" RESET "\n", ftype, dir_e.d_ino, (int32_t)file_stat.st_size, dir_e.d_name);
                } else if (DT_CHR == dir_e.d_type) {
                    printf("%c %d %d " RED "%s" RESET "\n", ftype, dir_e.d_ino,  (int32_t)file_stat.st_size, dir_e.d_name);
                } else if (DT_BLK == dir_e.d_type) {
                    printf("%c %d %d " YELLOW "%s" RESET "\n", ftype, dir_e.d_ino,  (int32_t)file_stat.st_size, dir_e.d_name);
                } else {
                    printf("%c %d %d %s\n", ftype, dir_e.d_ino,  (int32_t)file_stat.st_size, dir_e.d_name);
                }
            }
        } else {
            // 简略模式
            while (readdir(fd, &dir_e) > 0) {
                if (DT_DIR == dir_e.d_type) {
                    printf(BLUE "%s " RESET, dir_e.d_name);
                } else if (DT_CHR == dir_e.d_type) {
                    printf(RED "%s " RESET, dir_e.d_name);
                } else if (DT_BLK == dir_e.d_type) {
                    printf(YELLOW "%s " RESET, dir_e.d_name);
                } else {
                    printf("%s ", dir_e.d_name);
                }
            }
            printf("\n");
        }
        close(fd); 
    } else {
        // 处理普通文件逻辑保持不变
        if (long_info) {
            printf("- %d %d %s\n", file_stat.st_ino,  (int32_t)file_stat.st_size, pathname);
        } else {
            printf("%s\n", pathname);
        }
    }
}

void buildin_ps(uint32_t argc,char** argv UNUSED){
	if(argc!=1){
		printf("ps: no argument support!\n");
		return ;
	}
	ps();
}

void buildin_clear(uint32_t argc,char** argv UNUSED){
	if(argc!=1){
		printf("clear: no argument support!\n");
		return ;
	}
	clear();
}

int32_t buildin_mkdir(uint32_t argc,char** argv){
	int32_t ret = -1;
	if(argc!=2){
		printf("mkdir: only support 1 argument\n");
	}else{
		make_clear_abs_path(argv[1],final_path);
		if(strcmp("/",final_path)){
			if(mkdir(final_path)==0){
				ret = 0;
			}else{
				printf("mkdir: create directory %s failed.\n",argv[1]);
			}
		}
	}
	return ret;
}

int32_t buildin_rmdir(uint32_t argc,char** argv){
	int32_t ret = -1;
	if(argc!=2){
		printf("rmdir: only support 1 argument!\n");
	}else{
		if(!strcmp(".",argv[1])||!strcmp("..",argv[1])){
			printf("cannot remove '.' or '..'!\n");
			return ret;
		}
		make_clear_abs_path(argv[1],final_path);
		if(!strcmp("/",final_path)){
			printf("cannot remove the root!\n");
			return ret;
		}
		if(rmdir(final_path)==0){
			ret = 0;
		}else{
			printf("rmdir: remove %s failed.\n",argv[1]);
		}
	}
	return ret;
}

int32_t buildin_rm(uint32_t argc,char** argv){
	int32_t ret = -1;
	if(argc !=2){
		printf("rm: only support 1 argument!\n");
	}else{
		make_clear_abs_path(argv[1],final_path);
		if(strcmp("/",final_path)){
			if(unlink(final_path)==0){
				ret = 0;
			}else{
				printf("rm: delete %s failed.\n",argv[1]);
			}
		}
	}
	return ret;
}

void buildin_help(uint32_t argc UNUSED,char** argv UNUSED){
printf("buildin commands:\n\
	ls: Show directory or file information\n\
	cd: Change current work directory\n\
	mkdir: Creat a directory\n\
	rmdir: Remove a empty directory\n\
	rm: Remove a regular file\n\
	pwd: Show current work directory\n\
	ps:Show process information\n\
	clear: Clear screen\n\
	readraw: Read the disk without file system\n\
	free_mem: Check memory usage in the system​\n\
	df: Check disk space usage in the system​​\n\
shortcut key:\n\
	ctrl+l: clear screen\n\
	ctrl+u: clear input\n\
	External Command Search:\n\
The shell searches for programs in this order:\n\
    1. Absolute path (if starts with '/')\n\
    2. Current working directory\n\
    3. The '/bin' directory\n\
Pipe Example:\n\
    ls | cat (Redirect output of 'ls' to 'cat')\n");
}

void buildin_readraw(uint32_t argc,char** argv){
	uint32_t arg_idx = 0;
	// readraw -f /file1 -d sda -s 10403 -l 300
	if((argc==2)&&(!strcmp("-h",argv[1]))){
		printf("readraw: usage: readraw -f file_name -d disk_name -s file_size -l lba\n");
		return;
	}
	if(argc!=9){
		printf("readraw: wrong usage! use 'readraw -h' for help!\n");
		return;
	}

	char* filename=NULL;
	char* disk_name=NULL;
	int32_t lba=-1,file_size=-1;
	while(arg_idx<argc){
		if(argv[arg_idx][0]=='-'){
			if(!strcmp("-f",argv[arg_idx])){
				if(arg_idx+1>=argc){
					printf("readraw: argument -f error! use 'readraw -h' for help!\n");
					return;
				}

				make_clear_abs_path(argv[arg_idx+1],final_path);
				filename = final_path;
			}else if(!strcmp("-l",argv[arg_idx])){
				if(arg_idx+1>=argc||(!atoi_dep(argv[arg_idx+1],&lba))){
					printf("readraw: argument -l error! use 'readraw -h' for help!\n");
					return;
				}
			}else if(!strcmp("-s",argv[arg_idx])){
				if(arg_idx+1>=argc||(!atoi_dep(argv[arg_idx+1],&file_size))){
					printf("readraw: argument -s error! use 'readraw -h' for help!\n");
					return;
				}
			}else if(!strcmp("-d",argv[arg_idx])){
				if(arg_idx+1>=argc){
					printf("readraw: argument -d error! use 'readraw -h' for help!\n");
					return;
				}
				disk_name = argv[arg_idx+1];
			}else{
				printf("readraw: unknown argument! use -h for help!\n");
				return;
			}
		}
		arg_idx++;
	}
	// printf("%d %s\n",filename==NULL,filename);
	// printf("%d %s\n",disk_name==NULL,disk_name);
	// printf("%d %d\n",lba<0,lba);
	// printf("%d %d\n",file_size<0,file_size);

	if(filename==NULL||disk_name==NULL||lba<0||file_size<0){
		printf("readraw: missing argument\n");
		return ;
	}
	

	readraw(disk_name,(uint32_t)lba,(const char*)filename,(uint32_t)file_size);
	printf("readraw: success!\n");
}

void buildin_free_mem(uint32_t argc UNUSED,char** argv UNUSED){
	free_mem();
}

void buildin_df(uint32_t argc UNUSED,char** argv UNUSED){
	disk_info();
}

// mount dev/sdb1 mnt/sdb1 sifs
void buildin_mount(uint32_t argc,char** argv){

	if(argc!=4){
		printf("usage: mount <dev_path> <target_dir> <type>\n");
		printf("e.g. mount /dev/sdb1 /mnt/sdb1 sifs\n");
		return;
	}
	
	mount(argv[1],argv[2],argv[3],0,0);
}

void buildin_umount(uint32_t argc,char** argv){

	if(argc!=2){
		printf("usage: umount <target_dir>\n");
		return;
	}
	
	umount(argv[1]);
}

// 简单的文件拷贝，用于跨分区搬运数据
static int copy_file(const char* src, const char* dest) {
    int fd_src = open((char*)src, O_RDONLY);
    if (fd_src < 0) return -1;

    // 创建目标文件，如果已存在则截断
    int fd_dest = open((char*)dest, O_WRONLY | O_CREATE | O_TRUNC);
    if (fd_dest < 0) {
        close(fd_src);
        return -1;
    }

	// 用户栈是自带扩容的，最大扩到8MB，所以随便操作没事
	char* buf = (char*) malloc(4096);
    int bytes;
    while ((bytes = read(fd_src, buf, 4096)) > 0) {
        if (write(fd_dest, buf, bytes) != bytes) break;
    }

    close(fd_src);
    close(fd_dest);
	free(buf);
    return 0;
}

static int move_recursive(const char* src, const char* dest) {
    struct stat st;
	// 检查每一层中要移动的文件是否存在
    if (stat(src, &st) < 0) return -1;

	// 如果 src 和 dest 是同一个文件，直接返回，防止自杀
    if (strcmp(src, dest) == 0) return 0;

    // 尝试原子重命名。
    // 如果 src 和 dest 在同一分区，这一步直接成功，瞬间完成。
    // 因为 rename 只移动目录项，不会删除和拷贝文件
	if (rename(src, dest) == 0) return 0;
    // 如果 rename 返回 -EXDEV (跨设备错误)，进入手动搬迁逻辑
    if (st.st_filetype == FT_DIRECTORY) {
        // 处理目录
        // 在目标分区创建同名目录
        // 如果 mkdir 失败，要区分原因
		if (mkdir(dest) < 0) {
            // 如果是因为目录已存在，且我们要移动的是目录内容，可以允许继续
            // 但标准 mv 跨分区移动时，如果目标目录已存在且非空，通常会报错
            struct stat st_d;
            if (stat(dest, &st_d) < 0 || st_d.st_filetype != FT_DIRECTORY) {
                return -1; 
            }
        }
        // 打开源目录开始遍历
        int fd = open((char*)src, O_RDONLY);
        if (fd < 0){
			printf("mv: fail to open %s\n",src);
			return -1;
		}
        struct dirent de;
		// 用户栈是自带扩容的，最大扩到8MB，所以随便操作没事，借此机会也可以试验下这个扩容的健壮性
        char src_buf[MAX_PATH_LEN];
        char dest_buf[MAX_PATH_LEN];

		int move_failed = 0;
		// 读目录项，然后逐个移动
		// readdir=0时说明读取结束，小于0时，返回的是错误码，返回1时说明成功读取但是后面还有数据
        while (readdir(fd, &de) > 0) {
			// printf("readdir :%s\n",de.d_name);
            // 过滤 "." 和 ".." 防止死循环
            if (!strcmp(de.d_name, ".") || !strcmp(de.d_name, "..")) continue;

            // 拼接子路径
            sprintf(src_buf, "%s/%s", src, de.d_name);
            sprintf(dest_buf, "%s/%s", dest, de.d_name);

			// printf("src: %s, dst: %s\n",src_buf,dest_buf);

            // 递归移动子项
            if (move_recursive(src_buf, dest_buf) < 0) {
				printf("mv: fail to move_recursive!\n");
				move_failed = 1; // 标记子项移动失败
            }
        }
        close(fd);

		// 只有所有子项都成功了，才删自己。否则报错退出，保留现场。
        if (move_failed) {
            printf("mv: sub-items move failed, keeping source directory '%s'\n", src);
            return -1;
        }
        // 整个目录的内容都搬走了，删除旧的空目录
        return rmdir(src);

    } else {
        // 处理普通文件 
        if (copy_file(src, dest) == 0) {
            return unlink(src); // 拷贝成功后删除源文件
        }
    }
    return -1;
}

// 在linux的标准下，若一个目录下同时存在 f1 和 f2
// 我们运行 mv f1 f2，系统的默认的行为是用 f1 将 f2 给覆盖了
void buildin_mv(uint32_t argc, char** argv) {
    bool recursive = false;
    char* src = NULL;
    char* dest = NULL;

    // 简单的参数解析
    for (uint32_t i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            recursive = true;
        } else if (src == NULL) {
            src = argv[i];
        } else if (dest == NULL) {
            dest = argv[i];
        }
    }

    if (src == NULL || dest == NULL) {
        printf("mv: missing file operand\nUsage: mv [-r] SOURCE DEST\n");
        return;
    }

    // 获取源文件状态，进行安全校验
    struct stat st;
    if (stat(src, &st) < 0) {
        printf("mv: cannot stat '%s': No such file or directory\n", src);
        return;
    }

    struct stat st_src, st_dest;
    if (stat(src, &st_src) < 0) { // 校验要移动的源文件是否存在
        printf("mv: cannot stat '%s'\n", src);
        return;
    }

    // 检查目标路径
    char final_dest[MAX_PATH_LEN];
    strcpy(final_dest, dest);

    if (stat(dest, &st_dest) == 0) {
        // 如果目标是一个已存在的目录，则将 src 移动到该目录下
		// 例如执行 mv -r dir .. 由于 .. 肯定是存在的，所以这个命令的语义是 mv -r dir ../dir
		// 如果 mv -r dir dir2 如果dir2不存在，那么这其实就是一个重命名操作
        if (st_dest.st_filetype == FT_DIRECTORY) {
            char* src_basename = strrchr(src, '/');
            src_basename = (src_basename == NULL) ? src : (src_basename + 1);
            
            sprintf(final_dest, "%s/%s", dest, src_basename);
        }
    }

    // 如果是目录且没有 -r 参数，拦截操作，防止误操作，这是模仿cp的逻辑
	// 因为对于目录的移动太过重量级，需要谨慎
    if (FT_DIRECTORY == st.st_filetype  && !recursive) {
        printf("mv: '%s' is a directory (not moved). Use -r to move directories.\n", src);
        return;
    }

    // 使用处理后的路径
    if (move_recursive(src, final_dest) < 0) {
        printf("mv: failed to move '%s' to '%s'\n", src, final_dest);
    }
}