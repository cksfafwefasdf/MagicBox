#include "shell.h"
#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "stdbool.h"
#include "assert.h"
#include "buildin_cmd.h"
#include "unistd.h"

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

// 我们将字符处理相关的逻辑已经放到tty中了
// 因此此函数的代码可以精简一些了
static void readline(char* buf, int32_t count) {
    assert(buf != NULL && count > 0);
    
    // 内核 TTY 发现没有回车时，会让 Shell 进程在信号量上休眠。
    // 用户按下回车后，TTY 会处理好所有的退格、Ctrl+U，并将最终结果一次性返回。并唤醒shell
	// 因此我们直接调用一次 read即可。
    
	int32_t bytes_read = read(stdin_no, buf, count);
    
	if (bytes_read <= 0) return;

    // TTY 返回的数据最后通常带着 '\n'，我们需要把它去掉，方便后续 cmd_parse
    char* newline = strchr(buf, '\n');
    if (newline) {
        *newline = 0;
    }

    // 处理特殊的 Ctrl+L 转义逻辑
    if (buf[0] == ('l' - 'a' + 1)) {
        clear();
        // print_prompt();
        // 如果 Ctrl+L 后面还带了命令内容，可以递归或者简单处理
        buf[0] = 0; // 暂时清空，让 Shell 重新来过
    }
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
		memset(final_path,0,MAX_PATH_LEN);
		memset(cmd_line,0,MAX_PATH_LEN);
		print_prompt();
		readline(cmd_line,MAX_PATH_LEN);
		if(cmd_line[0]==0){
			continue;
		}

		// 如果只是一个单纯的 Ctrl+L 产生的唤醒
		if (cmd_line[0] == 12) {
			// readline 内部已经处理了 clear() 和 print_prompt()
			// 因此我们不用再执行后面的指令了
			continue; 
		}

		char* pipe_symbol = strchr(cmd_line,'|');
		if (pipe_symbol) {
			int32_t fd[2];
			pipe(fd); // 此调用会分配两个全局 file 和一个 pipe 结构

			int32_t pid1 = fork();
			if (pid1 == 0) { // 子进程 1，生产者 (如 ls)
				dup2(fd[1], 1);  // 将标准输出指向管道写端
				close(fd[0]);    // 子进程不需要读
				close(fd[1]);    // 已经 dup2 了，原来的可以关了
				
				// 解析第一段命令并执行
				*pipe_symbol = 0;
				argc = cmd_parse(cmd_line, argv, ' ');
				cmd_execute(argc, argv); // 这里内部会调 exec，ls 就带着重定向好的 stdout 跑起来了
				exit(0); 
			}

			int32_t pid2 = fork();
			if (pid2 == 0) { // 子进程 2，消费者 (如 cat)
				dup2(fd[0], 0); // 将标准输入指向管道读端
				close(fd[1]); // 子进程不需要写
				close(fd[0]); // 原来的可以关了

				// 解析第二段命令
				char* next_cmd = pipe_symbol + 1;
				argc = cmd_parse(next_cmd, argv, ' ');
				cmd_execute(argc, argv); // cat 带着重定向好的 stdin 跑起来了
				exit(0);
			}

			// 父进程（Shell）的任务：
			close(fd[0]); // 父进程必须关掉这两个端点！
			close(fd[1]); // 否则管道的 reader/writer 计数永远不会清零，cat 会永远阻塞读
			// 参数为 NULL 表示不接受状态返回值 shell 进程等待两个子进程退出后回收，wait会进行回收过程
			wait(NULL);
			wait(NULL);
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