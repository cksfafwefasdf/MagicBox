#ifndef __SHELL_SHELL_H
#define __SHELL_SHELL_H

#include "../fs/fs.h"

#define PROMPT_STR "ccc@magic-box:%s$ "

extern char final_path[MAX_PATH_LEN];

extern void my_shell(void);
extern void print_prompt(void);
#endif