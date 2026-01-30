#include "tar.h"
#include "stdio.h"
#include "string.h"
#include "global.h"
#include "syscall.h"
#include "stdint.h"


// 将八进制字符串转换为十进制整数
// str 指向八进制字符串的指针 (如 "00000001234")
// size 字段的最大长度 (tar 标准中 size 字段通常为 12)
static uint32_t oct2bin(char* str, int size) {
    uint32_t n = 0;
    char* ptr = str;

    // 过滤掉开头的空格或零 (虽然有些 tar 实现会补零，有些会补空格)
    while (size > 0 && (*ptr == ' ' || *ptr == '0')) {
        ptr++;
        size--;
    }

    // 开始转换
    while (size > 0 && *ptr >= '0' && *ptr <= '7') {
        n = n * 8 + (*ptr - '0');
        ptr++;
        size--;
    }
    
    return n;
}

// 将指定 LBA 开始的 tar 包内容解压到文件系统
// base_lba 是 tar 包在裸磁盘上的起始扇区号
void untar_all(uint32_t base_lba) {
    uint32_t current_lba = base_lba;
    
    // 直接在栈上分配 512 字节用作缓冲
    struct tar_header hdr; 
    char full_path[MAX_PATH_LEN]; // 用于拼接绝对路径

    printf("untar: start unarchiving from LBA %d\n", base_lba);
	
    while (1) {
        // 读取 Header 扇区
        memset(&hdr, 0, SECTOR_SIZE);
		// 将磁盘扇区读取到内存缓冲区中
        read_sectors("sda", current_lba, &hdr, 1);
        // 终止条件，tar 标准规定结束符是连续两个全 0 扇区
        // 为了简单，判断文件名首字节是否为空即可
        if (hdr.name[0] == '\0') {
            break;
        }

        // 校验魔数以防读错扇区
        if (memcmp(hdr.magic, "ustar", 5) != 0) {
            printf("untar: invalid tar magic at LBA %d, stopping.\n", current_lba);
            break;
        }

        // 解析文件大小（八进制转十进制）
        uint32_t filesize = oct2bin(hdr.size, 12);

		printf(" File found: %s, octal_size_str: %s, converted_size: %d\n", hdr.name, hdr.size, filesize);

        // 路径处理：确保以 '/' 开头
        memset(full_path, 0, MAX_PATH_LEN);
        if (hdr.name[0] != '/') {
            full_path[0] = '/';
            strcat(full_path, hdr.name);
        } else {
            strcpy(full_path, hdr.name);
        }

        // 根据类型标志处理
        if (hdr.typeflag == '0' || hdr.typeflag == '\0') {
            // 普通文件，调用 readraw 读取
            printf("  extracting %s (%d bytes)... ", full_path, filesize);
            
            // 数据就在 Header 后的下一个扇区
            readraw("sda", current_lba + 1, full_path, filesize);
            
            printf("done\n");
        } else if (hdr.typeflag == '5') {
			// 去掉末尾斜杠
			uint32_t len = strlen(full_path);
			if (len > 1 && full_path[len - 1] == '/') {
				full_path[len - 1] = '\0';
			}

			// 尝试创建文件夹
			// 如果返回 -1，可能是目录已存在，正常，也可能是父目录不存在，此时报错
			if (mkdir(full_path) == 0) {
				printf("  Directory created: %s\n", full_path);
			} else {
				// 先不做处理，因为如果父目录真的不存在，
				// 后面的文件解压自然会通过 sys_open 的报错体现出来
			}
		}

        // 下一个 Header 的 LBA = 当前 Header(1个扇区) + 数据所占扇区数
        // 即使文件只有 1 字节，它在 tar 中也会占用 1 个完整的 512B 扇区数据块
        current_lba += (1 + DIV_ROUND_UP(filesize, SECTOR_SIZE));
    }

    printf("untar: all files extracted successfully.\n");
}