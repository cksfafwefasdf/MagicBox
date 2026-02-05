#ifndef __SHELL_BUILDIN_CMD_H
#define __SHELL_BUILDIN_CMD_H

#include "unistd.h"
extern void make_clear_abs_path(char* path,char* final_path);
extern int32_t buildin_rm(uint32_t argc,char** argv);
extern int32_t buildin_rmdir(uint32_t argc,char** argv);
extern int32_t buildin_mkdir(uint32_t argc,char** argv);
extern void buildin_clear(uint32_t argc,char** argv UNUSED);
extern void buildin_ps(uint32_t argc,char** argv UNUSED);
extern void buildin_ls(uint32_t argc,char** argv);
extern char* buildin_cd(uint32_t argc,char** argv);
extern void buildin_pwd(uint32_t argc,char** argv UNUSED);
extern void buildin_readraw(uint32_t argc,char** argv);
extern void buildin_help(uint32_t argc UNUSED,char** argv UNUSED);
extern void buildin_free_mem(uint32_t argc UNUSED,char** argv UNUSED);
extern void buildin_df(uint32_t argc UNUSED,char** argv UNUSED);
extern void buildin_mount(uint32_t argc,char** argv);
extern char* path_parse(char* pathname,char* name_store);

#endif