#include "shell.h"
#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "stdbool.h"
#include "assert.h"
#include "buildin_cmd.h"
#include "fs_types.h"

#define CMD_LEN 128
#define MAX_ARG_NR 16
#define CMD_NUM 64

static int32_t cmd_parse(char* cmd_str,char** argv, char token);

char* argv[MAX_ARG_NR] = {NULL};

int32_t argc = -1;


static char cmd_line[MAX_PATH_LEN] = {0};
char final_path[MAX_PATH_LEN] = {0};


void print_prompt(void){
	// 动态获取当前路径，不依赖缓存变量
    char current_path[MAX_PATH_LEN] = {0};
    if (getcwd(current_path, MAX_PATH_LEN) != NULL) {
        printf(PROMPT_STR, current_path);
    } else {
        printf(PROMPT_STR, "unknown");
    }
}

static void readline(char* buf,int32_t count){
	assert(buf!=NULL&&count>0);
	char* pos = buf;
	while(read(stdin_no,pos,1)!=-1&&(pos - buf) < count){
		switch (*pos){
			case '\n':
			case '\r':
				*pos = 0;
				putchar('\n');
				return;
			case '\b':
				if(buf[0]!='\b'){
					--pos;
					putchar('\b');
				}
				break;
			case 'l'-'a': //Ctrl+l
				*pos = 0;
				clear();
				print_prompt();
				printf("%s",buf);
				break;
			case 'u'-'a': // Ctrl+u
				while(buf!=pos){
					putchar('\b');
					*(pos--) = 0;
				}
				break;
			default:
				putchar(*pos);
				pos++;
		}
	}
	printf("readline: can\t find enter_key in the cmd_line. max num of char is 128\n");
}

static void cmd_execute(uint32_t argc,char** argv){
	if(!strcmp("ls",argv[0])){
		buildin_ls(argc,argv);
	}else if(!strcmp("cd",argv[0])){
		buildin_cd(argc,argv);
	}else if(!strcmp("pwd",argv[0])){
		buildin_pwd(argc,argv);
	}else if(!strcmp("ps",argv[0])){
		buildin_ps(argc,argv);
	}else if(!strcmp("clear",argv[0])){
		buildin_clear(argc,argv);
	}else if(!strcmp("mkdir",argv[0])){
		buildin_mkdir(argc,argv);
	}else if(!strcmp("rmdir",argv[0])){
		buildin_rmdir(argc,argv);
	}else if(!strcmp("rm",argv[0])){
		buildin_rm(argc,argv);
	}else if(!strcmp("readraw",argv[0])){
		buildin_readraw(argc,argv);
	}else if(!strcmp("help",argv[0])){
		buildin_help(argc,argv);
	}else if(!strcmp("free_mem",argv[0])){
		buildin_free_mem(argc,argv);
	}else if(!strcmp("df",argv[0])){
		buildin_df(argc,argv);
	}else if(!strcmp("mount",argv[0])){
		buildin_mount(argc,argv);
	} else {
        int32_t pid = fork();
        if (pid) { 
            // 父进程
            int32_t status;
            int32_t child_pid = wait(&status);
            if (child_pid == -1) panic("shell: no child\n");
            // printf("child_pid %d, it's status: %d\n", child_pid, status);
        } else {
            // 子进程，搜索程序并执行
			// 先到当前路径下找，要找不到再到bin目录下找，要再找不到再报错
            char exec_path[MAX_PATH_LEN];
            struct stat file_stat;

            // 如果输入的是绝对路径，直接处理
            if (argv[0][0] == '/') {
                strcpy(exec_path, argv[0]);
            } else {
                // 尝试在当前目录下查找
                make_clear_abs_path(argv[0], exec_path);
                memset(&file_stat, 0, sizeof(struct stat));
                if (stat(exec_path, &file_stat) == -1) {
                    
                    // 当前目录没找到，尝试在 /bin 目录下查找
                    memset(exec_path, 0, MAX_PATH_LEN);
                    strcpy(exec_path, "/bin/");
                    strcat(exec_path, argv[0]);
                    
                    memset(&file_stat, 0, sizeof(struct stat));
                    if (stat(exec_path, &file_stat) == -1) {
                        printf("shell: command not found: %s\n", argv[0]);
                        exit(-1);
                    }
                }
            }

            // 执行程序
            execv(exec_path, argv);
            // 如果 execv 返回，说明出错了
            printf("shell: execv failed for %s\n", exec_path);
            exit(-1);
        }
    }
}

int main(void){
	clear();
	printf("Welcome to MagicBox! Type 'help' for a list of commands.\n\n");
	while(1){
		print_prompt();
		memset(final_path,0,MAX_PATH_LEN);
		memset(cmd_line,0,MAX_PATH_LEN);
		readline(cmd_line,MAX_PATH_LEN);
		if(cmd_line[0]==0){
			continue;
		}

		char* pipe_symbol = strchr(cmd_line,'|');
		if(pipe_symbol){
			// ls -l|cat
			int32_t fd[2] = {-1};
			pipe(fd);
			fd_redirect(1,fd[1]);
			char* each_cmd = cmd_line;
			pipe_symbol = strchr(each_cmd,'|');
			*pipe_symbol = 0;

			argc = -1;
			argc = cmd_parse(each_cmd,argv,' ');
			cmd_execute(argc,argv);
			
			each_cmd = pipe_symbol + 1;

			fd_redirect(0,fd[0]);
			while((pipe_symbol=strchr(each_cmd,'|'))){
				*pipe_symbol = 0;
				argc = -1;
				argc = cmd_parse(each_cmd,argv,' ');
				cmd_execute(argc,argv);
				each_cmd = pipe_symbol + 1;
			}
		
			
			fd_redirect(1,1);
			argc = -1;
			argc = cmd_parse(each_cmd,argv,' ');
			
			cmd_execute(argc,argv);

			fd_redirect(0,0);

			close(fd[0]);
			close(fd[1]);
		}else{
			argc = -1;
			argc = cmd_parse(cmd_line,argv,' ');
			if(argc == -1){
				printf("num of argument exceed %d\n",MAX_ARG_NR);
				continue;
			}
			cmd_execute(argc,argv);
		}
		
		int32_t arg_idx = 0;
		while (arg_idx<MAX_ARG_NR){
			argv[arg_idx] = NULL;
			arg_idx++;
		}
	}
	panic("shell: should not be here!\n");
}

static int32_t cmd_parse(char* cmd_str,char** argv, char token){
	assert(cmd_str!=NULL);
	int32_t arg_idx = 0;
	while(arg_idx<MAX_ARG_NR){
		argv[arg_idx] = NULL;
		arg_idx++;
	}
	char* next = cmd_str;
	int32_t argc = 0;
	while(*next){
		while(*next==token){
			next++;
		}
		if(*next==0){
			break;
		}
		argv[argc] = next;

		while(*next&&*next!=token){
			next++;
		}

		if(*next){
			*next++ = 0;
		}

		if(argc>MAX_ARG_NR){
			return -1;
		}

		argc++;
	}
	return argc;	
}