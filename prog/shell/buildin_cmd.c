#include "string.h"
#include "syscall.h"
#include "buildin_cmd.h"
#include "stdio.h"
#include "shell.h"
#include "assert.h"
#include "unistd.h"

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

	if(chdir(final_path)==-1){
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
    if (stat(pathname, &file_stat) == -1) {
        printf("ls: cannot access %s: No such file or directory\n", pathname);
        return;
    }

    // 处理目录逻辑
    if (file_stat.st_filetype == FT_DIRECTORY) {
        int32_t fd = opendir(pathname); // 现在返回的是 fd
        if (fd == -1) {
            printf("ls: opendir %s failed\n", pathname);
            return;
        }

        struct dir_entry dir_e; // 用户态缓冲区
        char sub_pathname[MAX_PATH_LEN] = {0};
        uint32_t pathname_len = strlen(pathname);
        memcpy(sub_pathname, pathname, pathname_len);

        // 确保路径以 / 结尾，方便拼接子文件名
        if (sub_pathname[pathname_len - 1] != '/') {
            sub_pathname[pathname_len] = '/';
            pathname_len++;
        }

        rewinddir(fd); // 传入 fd 重置偏移量

        if (long_info) {
            printf("total: %d\n", file_stat.st_size);
            // readdir 传入 fd 和缓冲区地址
            while (readdir(fd, &dir_e) > 0) {
                char ftype = (dir_e.f_type == FT_DIRECTORY) ? 'd' : '-';

                // 拼接子文件完整路径以获取 stat 信息
                sub_pathname[pathname_len] = 0;
                strcat(sub_pathname, dir_e.filename);
                
                memset(&file_stat, 0, sizeof(struct stat));
                if (stat(sub_pathname, &file_stat) == -1) {
                    printf("ls: cannot access %s\n", dir_e.filename);
                    closedir(fd);
                    return;
                }
				if(FT_DIRECTORY==dir_e.f_type){
					printf("%c %d %d " BLUE "%s" RESET "\n", ftype, dir_e.i_no, file_stat.st_size, dir_e.filename);
				}else{
					printf("%c %d %d %s\n", ftype, dir_e.i_no, file_stat.st_size, dir_e.filename);
				}

            }
        } else {
            // 简略模式：只打印文件名
            while (readdir(fd, &dir_e) > 0) {
				if(FT_DIRECTORY == dir_e.f_type){
					printf(BLUE "%s " RESET, dir_e.filename);
				}else{
					printf("%s ", dir_e.filename);
				}
            }
            printf("\n");
        }
        closedir(fd); // 关闭 fd
    } else {
		// 处理普通文件逻辑
        if (long_info) {
            printf("- %d %d %s\n", file_stat.st_ino, file_stat.st_size, pathname);
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
		make_clear_abs_path(argv[1],final_path);
		if(strcmp("/",final_path)){
			if(rmdir(final_path)==0){
				ret = 0;
			}else{
				printf("rmdir: remove %s failed.\n",argv[1]);
			}
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
				if(arg_idx+1>=argc||(!atoi(argv[arg_idx+1],&lba))){
					printf("readraw: argument -l error! use 'readraw -h' for help!\n");
					return;
				}
			}else if(!strcmp("-s",argv[arg_idx])){
				if(arg_idx+1>=argc||(!atoi(argv[arg_idx+1],&file_size))){
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

void buildin_mount(uint32_t argc,char** argv){
	
	if(argc<2){
		printf("mount: missing argument <partition name>!\n");
		return;
	}
	if(argc>2){
		printf("mount: only support one argument <partition name>!\n");
		return;
	}

	if(strcmp(argv[1],"-h")==0){
		printf("mount: usage: mount <partition name>\n");
		return;
	}
	
	mount(argv[1]);
}