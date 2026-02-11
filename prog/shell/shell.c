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

#define SIG_IGN  ((void (*)(int))1)
#define SIG_DFL  ((void (*)(int))0)
#define SIGINT 2
#define SIGCHLD  17

#define TIOCSPGRP 0x5410
#define TIOCGPGRP 0x540F

static int32_t cmd_parse(char* cmd_str, char** argv, char token);
char* argv[MAX_ARG_NR] = {NULL};
int32_t argc = -1;
static char cmd_line[MAX_PATH_LEN] = {0};
char final_path[MAX_PATH_LEN] = {0};

// 信号处理函数：每当有孩子死了，内核会“踢”父进程来执行这个
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
static void cmd_execute(uint32_t argc, char** argv, int32_t pgid_to_set) {
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
        // 已在子进程中 (用于管道或普通外部命令)
        setpgid(0, pgid_to_set); 
        signal(SIGINT, SIG_DFL); // 恢复信号，让程序能被杀死

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
        // 普通非内建外部命令，Fork 出去执行
        int32_t pid = fork();
        if (pid == 0) {
            cmd_execute(argc, argv, 0); // 递归调用，进入上面的子进程分支
        } else {
            // 父进程
            setpgid(pid, pid);
            ioctl(stdin_no, TIOCSPGRP, &pid); // 设为前台
            wait(NULL);
            int32_t shell_pid = getpid();
            ioctl(stdin_no, TIOCSPGRP, &shell_pid); // 回收前台
        }
    }
}

int main(void) {
    clear();
    printf("Welcome to MagicBox! Type 'help' for a list of commands.\n\n");
    signal(SIGINT, SIG_IGN); // Shell 忽略 Ctrl+C
    // 默认情况下，收到SIGCHLD信号是什么都不会做的
    // 现在我们注册一个函数，用来防止shell没有主动调用wait时，无法回收exit的子进程
    signal(SIGCHLD, handle_sigchld);

    while (1) {
        memset(cmd_line, 0, MAX_PATH_LEN);
        memset(final_path,0,MAX_PATH_LEN);
        print_prompt();
        readline(cmd_line, MAX_PATH_LEN);
        if (cmd_line[0] == 0) continue;

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
                cmd_execute(argc, argv, 0); // 传入 0 成为组长
            }
            setpgid(pid1, pid1);
            ioctl(stdin_no, TIOCSPGRP, &pid1); // 管道组设为前台

            // 消费者
            int32_t pid2 = fork();
            if (pid2 == 0) {
                dup2(fd[0], 0);
                close(fd[1]); close(fd[0]);
                argc = cmd_parse(pipe_symbol + 1, argv, ' ');
                cmd_execute(argc, argv, pid1); // 加入 pid1 的组
            }
            setpgid(pid2, pid1);

            close(fd[0]); close(fd[1]);
            waitpid(pid1, NULL, 0);
            waitpid(pid2, NULL, 0);

            int32_t shell_pid = getpid();
            ioctl(stdin_no, TIOCSPGRP, &shell_pid); // 拿回控制权
        } else {
            // 无管道普通命令逻辑
            argc = cmd_parse(cmd_line, argv, ' ');
            if (argc == -1) continue;
            // 传 -1 告诉 cmd_execute，由其决定是否 fork
            // 若是内建命令则不fork，若不是则fork
            cmd_execute(argc, argv, -1);
        }

        // 清理 argv 缓冲区
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