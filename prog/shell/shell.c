#include "shell.h"
#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "stdbool.h"
#include "assert.h"
#include "buildin_cmd.h"
#include "unistd.h"

#define SIG_IGN  ((void (*)(int))1)
#define SIG_DFL  ((void (*)(int))0)
#define SIGINT 2
#define SIGCHLD  17

#define TIOCSPGRP 0x5410
#define TIOCGPGRP 0x540F

static int32_t cmd_parse(char* cmd_str, char** argv, char token);
void readline(char* buf, int32_t count);
void print_prompt(void);

char* argv[MAX_ARG_NR] = {NULL};
int32_t argc = -1;
static char cmd_line[MAX_PATH_LEN] = {0};
char final_path[MAX_PATH_LEN] = {0};

// 信号处理，异步回收后台子进程，防止僵尸进程堆积
void handle_sigchld(int sig) {
    int status;
    int pid;
	// 循环收割。因为信号不排队，万一同时死两个，我们要一次性收完。
    // WNOHANG 保证了如果没有僵尸了，立刻退出函数，不卡死 Shell。
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        printf("\n[Shell Notice] Background child %d cleaned up. Status: %d\n", pid, status);
    }
}

// 如果 pgid_to_set >= 0，说明是在子进程环境下调用，不再内部 fork
// 增加了 is_background 参数，用于判断进程是否是后台进程
static void cmd_execute(uint32_t argc, char** argv, int32_t pgid_to_set, bool is_background) {
    // 优先处理内建命令 (内建命令通常不支持后台运行，或是直接在父进程执行)
    if (!strcmp("ls", argv[0])) { buildin_ls(argc, argv); if(pgid_to_set >= 0) exit(0); return; }
    if (!strcmp("cd", argv[0])) { buildin_cd(argc, argv); if(pgid_to_set >= 0) exit(0); return; }
    if (!strcmp("pwd", argv[0])) { buildin_pwd(argc, argv); if(pgid_to_set >= 0) exit(0); return; }
    if (!strcmp("ps", argv[0])) { buildin_ps(argc, argv); if(pgid_to_set >= 0) exit(0); return; }
    if (!strcmp("clear", argv[0])) { buildin_clear(argc, argv); if(pgid_to_set >= 0) exit(0); return; }
    if (!strcmp("mkdir", argv[0])) { buildin_mkdir(argc, argv); if(pgid_to_set >= 0) exit(0); return; }
    if (!strcmp("rmdir", argv[0])) { buildin_rmdir(argc, argv); if(pgid_to_set >= 0) exit(0); return; }
    if (!strcmp("rm", argv[0])) { buildin_rm(argc, argv); if(pgid_to_set >= 0) exit(0); return; }
    if (!strcmp("help", argv[0])) { buildin_help(argc, argv); if(pgid_to_set >= 0) exit(0); return; }
    if (!strcmp("free_mem", argv[0])) { buildin_free_mem(argc, argv); if(pgid_to_set >= 0) exit(0); return; }
    if (!strcmp("df", argv[0])) { buildin_df(argc, argv); if(pgid_to_set >= 0) exit(0); return; }
    if (!strcmp("mount", argv[0])) { buildin_mount(argc, argv); if(pgid_to_set >= 0) exit(0); return; }

    // 外部程序逻辑
    if (pgid_to_set >= 0) {
        // 子进程环境
        setpgid(0, pgid_to_set); 
        signal(SIGINT, SIG_DFL); // 让子进程能响应 Ctrl+C

        char exec_path[MAX_PATH_LEN];
        struct stat file_stat;
        if (argv[0][0] == '/') {
            strcpy(exec_path, argv[0]);
        } else {
            make_clear_abs_path(argv[0], exec_path);
            if (stat(exec_path, &file_stat) == -1) {
                memset(exec_path, 0, MAX_PATH_LEN);
                strcpy(exec_path, "/bin/");
                strcat(exec_path, argv[0]);
                if (stat(exec_path, &file_stat) == -1) {
                    printf("shell: command not found: %s\n", argv[0]);
                    exit(-1);
                }
            }
        }
        execv(exec_path, argv);
        exit(-1);
    } else {
        // 父进程环境：准备 Fork
        int32_t pid = fork();
        if (pid == 0) {
            cmd_execute(argc, argv, 0, is_background); 
        } else {
            setpgid(pid, pid);
            if (!is_background) {
                // 前台：移交 TTY 权限并阻塞等待
                ioctl(stdin_no, TIOCSPGRP, (uint32_t)&pid);
                waitpid(pid, NULL, 0);
                int32_t shell_pid = getpid();
                ioctl(stdin_no, TIOCSPGRP, (uint32_t)&shell_pid);
            } else {
                // 后台：直接返回，控制权留在 Shell
                printf("[Background] %d\n", pid);
            }
        }
    }
}

int main(void) {
    clear();
    printf("Welcome to MagicBox! Type 'help' for a list of commands.\n\n");
    signal(SIGINT, SIG_IGN); 
    signal(SIGCHLD, handle_sigchld);

    while (1) {
        memset(cmd_line, 0, MAX_PATH_LEN);
        print_prompt();
        readline(cmd_line, MAX_PATH_LEN);
        if (cmd_line[0] == 0) continue;

        // 后台运行符 '&' 识别逻辑 -
        bool is_background = false;
        int32_t len = strlen(cmd_line);
        // 查找最后一个非空字符是否为 '&'
        char* back_ptr = strrchr(cmd_line, '&');
        if (back_ptr != NULL && (*(back_ptr + 1) == '\0' || *(back_ptr + 1) == ' ')) {
            is_background = true;
            *back_ptr = '\0'; // 抹除 '&'，不影响后续解析
        }

        char* pipe_symbol = strchr(cmd_line, '|');
        if (pipe_symbol) {
            int32_t fd[2];
            pipe(fd);
            *pipe_symbol = 0;

            // 生产者
            int32_t pid1 = fork();
            if (pid1 == 0) {
                dup2(fd[1], 1);
                close(fd[0]); close(fd[1]);
                argc = cmd_parse(cmd_line, argv, ' ');
                cmd_execute(argc, argv, 0, is_background); // 传入 0 成为组长
            }
            setpgid(pid1, pid1);

            // 消费者
            int32_t pid2 = fork();
            if (pid2 == 0) {
                dup2(fd[0], 0);
                close(fd[1]); close(fd[0]);
                argc = cmd_parse(pipe_symbol + 1, argv, ' ');
                cmd_execute(argc, argv, pid1, is_background); // 加入 pid1 的组
            }
            setpgid(pid2, pid1);

            close(fd[0]); close(fd[1]);

            if (!is_background) {
                // 前台管道，Shell 移交 TTY 给整个进程组并等待
                ioctl(stdin_no, TIOCSPGRP, (uint32_t)&pid1);
                waitpid(pid1, NULL, 0);
                waitpid(pid2, NULL, 0);
                int32_t shell_pid = getpid();
                ioctl(stdin_no, TIOCSPGRP, (uint32_t)&shell_pid);
            } else {
                // 后台管道，打印组长 PID，Shell 继续
                printf("[Background Pipe Group] %d\n", pid1);
            }
        } else {
            // 无管道普通命令逻辑
            argc = cmd_parse(cmd_line, argv, ' ');
            if (argc == -1) continue;
            cmd_execute(argc, argv, -1, is_background);
        }

        for (int i = 0; i < MAX_ARG_NR; i++) argv[i] = NULL;
    }
    return 0;
}

void print_prompt(void) {
    char current_path[MAX_PATH_LEN] = {0};
    if (getcwd(current_path, MAX_PATH_LEN) != NULL) printf(PROMPT_STR, current_path);
    else printf(PROMPT_STR, "unknown");
}

void readline(char* buf, int32_t count) {
    int32_t bytes_read = read(stdin_no, buf, count);
    if (bytes_read <= 0) return;
    char* newline = strchr(buf, '\n');
    if (newline) *newline = 0;
    if (buf[0] == 12) { clear(); buf[0] = 0; }
}

static int32_t cmd_parse(char* cmd_str, char** argv, char token) {
    int32_t arg_idx = 0;
    while(arg_idx < MAX_ARG_NR) argv[arg_idx++] = NULL;
    char* next = cmd_str;
    int32_t argc = 0;
    while(*next) {
        while(*next == token) next++;
        if(*next == 0) break;
        argv[argc++] = next;
        while(*next && *next != token) next++;
        if(*next) *next++ = 0;
        if(argc >= MAX_ARG_NR) return -1;
    }
    return argc;
}