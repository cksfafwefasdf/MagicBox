#ifndef __SHELL_SHELL_H
#define __SHELL_SHELL_H

#include "unistd.h"
#include "color.h"

#define PROMPT_STR GREEN "ccc@magic-box" RESET ":" BLUE "%s" RESET "$ "

extern char final_path[MAX_PATH_LEN];
#endif