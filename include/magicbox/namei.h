#ifndef __INCLUDE_MAGICBOX_NAMEI_H
#define __INCLUDE_MAGICBOX_NAMEI_H

#include <unitype.h>
#include <fs_types.h>
#include <stdint.h>

struct path_search_record{
	char searched_path[MAX_PATH_LEN];
	struct inode* parent_inode; // 父目录的inode
	enum file_types file_type;
    uint32_t i_dev; // 所在设备号
	int error_code; // 用于记录具体的错误原因
};

extern void make_abs_pathname(const char* pathname, char* abs_path);
extern int32_t search_file(char* pathname, struct path_search_record* searched_record, bool follow_link);
extern char* _path_parse(char* pathname,char* name_store);
extern int32_t path_depth_cnt(char* pathname);

#endif