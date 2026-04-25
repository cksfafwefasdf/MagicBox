#include <stdio.h>
#include <string.h>
#include <unitype.h>
#include <stdint.h>
#include <ioctl.h>
#include <ext2_sb.h>
#include <ext2_inode.h>
#include <ext2_fs.h>
#include <syscall.h>
#include <sifs_sb.h>
#include <sifs_inode.h>
#include <sifs_fs.h>

#define CAT_BUF_SIZE 512

// mkfs.ext2 使用
#define EXT2_INODES_PER_GROUP 1024
#define EXT2_BLOCKS_PER_GROUP 8192
#define EXT2_INODE_SIZE 128
#define EXT2_ROOT_INO 2
#define DEFAULT_EXT2_BLOCK_SIZE 1024

// 根据内核定义以便于确保这些常量一致(sifs 使用)
#define SECTOR_SIZE 512
#define BITS_PER_SECTOR (SECTOR_SIZE * 8)
#define DIV_ROUND_UP(X, STEP) (((X) + (STEP) - 1) / (STEP))
#define MAX_FILES_PER_PART 4096

// hd 使用
#define LINE_SIZE 16
#define SCREEN_LINES 16  // 每显示 16 行暂停一次

// 定义命令处理函数的函数指针类型
typedef int (*cmd_func_t)(int argc, char** argv);

int do_mount(int argc, char** argv) {
    if (argc != 4) {
        printf("usage: mount <dev_path> <target_dir> <type>\n");
        return 1;
    }
    mount(argv[1], argv[2], argv[3], 0, 0);
    return 0;
}

int do_fm(int argc, char** argv) {
	free_mem();
    return 0;
}

int do_umount(int argc, char** argv) {
    if (argc != 2) {
        printf("usage: umount <target_dir>\n");
        return 1;
    }
    umount(argv[1]);
    return 0;
}

int do_ps(int argc, char** argv) {
    ps();
    return 0;
}

int do_df(int argc, char** argv) {
    disk_info();
    return 0;
}

int do_cat(int argc,char** argv){
	if(argc>2){
		printf("cat: only support 1 argument.\neg: cat filename\n");
		return -2;
	}

	if(argc==1){
		char buf[CAT_BUF_SIZE] = {0};
		read(0,buf,CAT_BUF_SIZE);
		printf("%s",buf);
		return 0;
	}

	int buf_size = CAT_BUF_SIZE;
	char *path = argv[1];
	
	void *buf = malloc(buf_size);
	
	if(buf==NULL){
		printf("cat: malloc memory failed!\n");
		return -1;
	} 
	
	
	int fd = open(path,O_RDONLY);
	
	if(fd < 0){
		printf("cat:open: open %s failed\n",argv[1]);
        free(buf);
		return -1;
	}
	
	int read_bytes = 0;
	while(1){
		read_bytes = read(fd,buf,buf_size);
		if(read_bytes<=0){
			break;
		}
		write(1,buf,read_bytes);
	}
	
	free(buf);
	close(fd);
	return 0;
}

int do_echo(int argc, char** argv) {
    if ((argc == 2 && !strcmp(argv[1],"-h"))||argc < 2) {
        printf("usage: echo [string] [-f filename]\n");
        return -1;
    }

    int fd = 1;
    int final_argc = argc;

    if (argc >= 4 && strcmp(argv[argc - 2], "-f") == 0) {
        char* path = argv[argc - 1];

        // 第一次尝试：直接打开 (O_WRONLY)
        fd = open(path, O_WRONLY); 

        if (fd < 0) {
            // 第二次尝试：创建 (O_WRONLY | O_CREATE)
            fd = open(path, O_WRONLY | O_CREATE);
        }

        if (fd < 0) {
            printf("echo: open/create %s failed\n", path);
            return -1;
        }
        final_argc = argc - 2;
    }

    for (int i = 1; i < final_argc; i++) {
        write(fd, argv[i], strlen(argv[i]));
        if (i < final_argc - 1) {
            write(fd, " ", 1);
        }
    }

    write(fd, "\n", 1);

    if (fd != 1) {
        close(fd);
    }

    return 0;
}

int do_hd(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: hd <file_or_dev> [offset] [length]\n");
        return -1;
    }

    char* path = argv[1];
    int32_t offset = 0;
    int32_t length = 256;

    int32_t line_count = 0;

    if (argc > 2) {
        if (!atoi_dep(argv[2], &offset)) {
            printf("hd: invalid offset %s\n", argv[2]);
            return -1;
        }
    }
    if (argc > 3) {
        if (!atoi_dep(argv[3], &length)) {
            printf("hd: invalid length %s\n", argv[3]);
            return -1;
        }
    }

    int32_t fd = open(path, O_RDONLY);

    if (fd < 0) {
        printf("hd: open %s failed!\n", path);
        return -1;
    }

    if (lseek(fd, offset, SEEK_SET) == -1) {
        printf("hd: lseek to %d failed!\n", offset);
        close(fd);
        return -1;
    }

    uint8_t* buf = malloc(length);
    if (!buf) {
        printf("hd: malloc failed\n");
        close(fd);
        return -1;
    }

    int32_t read_bytes = read(fd, buf, length);
    if (read_bytes <= 0) {
        printf("hd: read failed or EOF\n");
    } else {
        printf("Dumping %s: %d bytes from offset %d\n", path, read_bytes, offset);
        
        for (int32_t i = 0; i < read_bytes; i++) {
            if (i % LINE_SIZE == 0) {
                // 分页逻辑
                if (line_count != 0 && line_count % SCREEN_LINES == 0) {
                    printf("\n-- press any key to continue --");
                    char pause_buf[2];
                    read(0, pause_buf, 1); // 从标准输入（键盘）读一个字符，起到阻塞作用
                }

                char addr_buf[16];
                itoa(offset + i, addr_buf, 16);
                printf("\n%s: ", addr_buf);
                line_count++;
            }

            char hex_buf[16];
            itoa(buf[i], hex_buf, 16);
            // 手动补齐两位对齐，如果 itoa 不支持自动补 0
            if (buf[i] < 0x10) printf("0"); 
            printf("%s ", hex_buf);

            if ((i + 1) % 8 == 0 && (i + 1) % LINE_SIZE != 0) {
                printf(" ");
            }

            if ((i + 1) % LINE_SIZE == 0 || (i + 1) == read_bytes) {
                // 最后一行补空格对齐 ASCII 预览
                if ((i + 1) == read_bytes && (read_bytes % LINE_SIZE != 0)) {
                    int spaces = LINE_SIZE - (read_bytes % LINE_SIZE);
                    for (int s = 0; s < spaces; s++) printf("   ");
                    if (spaces >= 8) printf(" ");
                }
                printf(" |");
                int start = (i / LINE_SIZE) * LINE_SIZE;
                for (int j = start; j <= i; j++) {
                    if (buf[j] >= 32 && buf[j] <= 126) printf("%c", buf[j]);
                    else printf(".");
                }
                printf("|");
            }
        }
        printf("\n");
    }

    free(buf);
    close(fd);
    return 0;
}

int do_mkfs_ext2(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: mkfs.ext2 <device_path>\n");
        return -1;
    }

    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { printf("fail to open!\n"); return -1; }

    uint32_t sec_cnt = 0;
    // 获取块设备大小
    if (ioctl(fd, BLKGETSIZE, (uint32_t)&sec_cnt) < 0) {
        // 如果是普通文件而不是设备，用 lseek 算大小
        sec_cnt = lseek(fd, 0, SEEK_END) / 512;
    }

    uint32_t block_count = (sec_cnt * 512) / DEFAULT_EXT2_BLOCK_SIZE;

    // 计算动态组数
    uint32_t group_count = (block_count + EXT2_BLOCKS_PER_GROUP - 1) / EXT2_BLOCKS_PER_GROUP;
    uint32_t gdt_size_bytes = group_count * sizeof(struct ext2_group_desc);
    uint32_t gdt_blocks = (gdt_size_bytes + DEFAULT_EXT2_BLOCK_SIZE - 1) / DEFAULT_EXT2_BLOCK_SIZE;

    // 初始化超级块
    struct ext2_super_block sb = {0};
    sb.s_inodes_count = group_count * EXT2_INODES_PER_GROUP;
    sb.s_blocks_count = block_count;
    sb.s_r_blocks_count = block_count / 20;
    sb.s_free_inodes_count = sb.s_inodes_count - 11; 
    sb.s_free_blocks_count = block_count; 
    sb.s_first_data_block = 1; 
    sb.s_log_block_size = 0;   // 1KB
    sb.s_blocks_per_group = EXT2_BLOCKS_PER_GROUP;
    sb.s_inodes_per_group = EXT2_INODES_PER_GROUP;
    sb.s_magic = EXT2_MAGIC_NUMBER;
    sb.s_state = 1;
    sb.s_rev_level = 1;
    sb.s_inode_size = EXT2_INODE_SIZE;
    sb.s_first_ino = 11;
    sb.s_feature_incompat = 0x02;
    sb.s_log_frag_size = 0;
    sb.s_errors = 1; // EXT2_ERRORS_CONTINUE (发现错误继续运行，这是标准值)
    sb.s_creator_os = 0; // Linux
    sb.s_max_mnt_count = 20; // 最大挂载次数
    sb.s_checkinterval = 0; // 不强制定期检查
    sb.s_wtime = 1710900000; // 随便写一个最近的 Unix 时间戳,防止报错
    sb.s_frags_per_group = sb.s_blocks_per_group; // 这一句一定要写，不然 e2fsck 会报错！

    // 构造组描述符表 (GDT)
    struct ext2_group_desc* gdt = calloc(group_count, sizeof(struct ext2_group_desc));
    uint32_t itable_blocks = (EXT2_INODES_PER_GROUP * EXT2_INODE_SIZE) / DEFAULT_EXT2_BLOCK_SIZE;
    uint32_t total_used_blocks = 0;
    for (uint32_t i = 0; i < group_count; i++) {
        uint32_t group_base = i * EXT2_BLOCKS_PER_GROUP + sb.s_first_data_block;
        uint32_t offset = 1 + gdt_blocks;
        gdt[i].bg_block_bitmap = group_base + offset;
        gdt[i].bg_inode_bitmap = group_base + offset + 1;
        gdt[i].bg_inode_table  = group_base + offset + 2;
        
        uint32_t used_meta = offset + 2 + itable_blocks;
        if (i == 0) used_meta++; // 根目录数据块

        gdt[i].bg_free_blocks_count = (i == group_count - 1) ? 
            (block_count - (i * EXT2_BLOCKS_PER_GROUP + sb.s_first_data_block) - used_meta) : 
            (EXT2_BLOCKS_PER_GROUP - used_meta);
        gdt[i].bg_free_inodes_count = (i == 0) ? (EXT2_INODES_PER_GROUP - 11) : EXT2_INODES_PER_GROUP;
        gdt[i].bg_used_dirs_count = (i == 0) ? 1 : 0;

        total_used_blocks += used_meta;
    }

    sb.s_free_blocks_count = block_count - total_used_blocks;

    // 写入元数据
    lseek(fd, DEFAULT_EXT2_BLOCK_SIZE, SEEK_SET);
    write(fd, &sb, sizeof(sb));
    lseek(fd, 2 * DEFAULT_EXT2_BLOCK_SIZE, SEEK_SET);
    write(fd, gdt, gdt_size_bytes);

    // 循环初始化块组
    uint8_t* buf = calloc(1, DEFAULT_EXT2_BLOCK_SIZE);
    for (uint32_t i = 0; i < group_count; i++) {
        // 块位图
        memset(buf, 0, DEFAULT_EXT2_BLOCK_SIZE);
        uint32_t meta_in_group = (1 + gdt_blocks + 2 + itable_blocks);
        for (uint32_t b = 0; b < meta_in_group; b++) buf[b/8] |= (1 << (b%8));
        if (i == 0) buf[meta_in_group/8] |= (1 << (meta_in_group%8));
        
        if (i == group_count - 1) {
            uint32_t last_group_blocks = block_count - (i * EXT2_BLOCKS_PER_GROUP + sb.s_first_data_block);
            for (uint32_t b = last_group_blocks; b < 8192; b++) buf[b/8] |= (1 << (b%8));
        }
        lseek(fd, (int32_t)gdt[i].bg_block_bitmap * DEFAULT_EXT2_BLOCK_SIZE, SEEK_SET);
        write(fd, buf, DEFAULT_EXT2_BLOCK_SIZE);

        // Inode 位图
        memset(buf, 0, DEFAULT_EXT2_BLOCK_SIZE);
        if (i == 0) {
            for (int n = 0; n < 11; n++) buf[n/8] |= (1 << (n%8));
        }
        lseek(fd, (int32_t)gdt[i].bg_inode_bitmap * DEFAULT_EXT2_BLOCK_SIZE, SEEK_SET);
        write(fd, buf, DEFAULT_EXT2_BLOCK_SIZE);

        // 清空 Inode Table
        memset(buf, 0, DEFAULT_EXT2_BLOCK_SIZE);
        lseek(fd, (int32_t)gdt[i].bg_inode_table * DEFAULT_EXT2_BLOCK_SIZE, SEEK_SET);
        for (uint32_t t = 0; t < itable_blocks; t++) write(fd, buf, DEFAULT_EXT2_BLOCK_SIZE);
    }

    // 根目录初始化
    uint32_t root_data_block = gdt[0].bg_inode_table + itable_blocks; 
    struct ext2_inode root_inode = {0};
    root_inode.i_mode = 0x41ED; // drwxr-xr-x
    root_inode.i_size = DEFAULT_EXT2_BLOCK_SIZE;
    root_inode.i_links_count = 2;
    root_inode.i_blocks = 2; // 1KB block = 2 sectors
    root_inode.i_block[0] = root_data_block;
    
    lseek(fd, (int32_t)gdt[0].bg_inode_table * DEFAULT_EXT2_BLOCK_SIZE + (EXT2_ROOT_INO - 1) * EXT2_INODE_SIZE, SEEK_SET);
    write(fd, &root_inode, EXT2_INODE_SIZE);

    memset(buf, 0, DEFAULT_EXT2_BLOCK_SIZE);
    struct ext2_dir_entry* de = (struct ext2_dir_entry*)buf;
    // "." 条目
    de->i_no = EXT2_ROOT_INO;
    de->rec_len = 12;
    de->name_len = 1;
    de->file_type = 2;
    strcpy(de->name, ".");
    // ".." 条目
    de = (void*)de + de->rec_len;
    de->i_no = EXT2_ROOT_INO;
    de->rec_len = DEFAULT_EXT2_BLOCK_SIZE - 12; // 覆盖剩余空间
    de->name_len = 2;
    de->file_type = 2;
    strcpy(de->name, "..");

    lseek(fd, (int32_t)root_data_block * DEFAULT_EXT2_BLOCK_SIZE, SEEK_SET);
    write(fd, buf, DEFAULT_EXT2_BLOCK_SIZE);

    
    free(gdt); free(buf); close(fd);
    printf("mkfs.ext2 done!\n");
    return 0;
}

int do_mkfs_sifs(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: mkfs.sifs <device_path> (e.g. /dev/sdb1)\n");
        return -1;
    }

    // 打开设备
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        printf("Error: Could not open device\n");
        return -1;
    }

    // 获取分区扇区数
    uint32_t sec_cnt = 0;
    if (ioctl(fd, BLKGETSIZE, (uint32_t)&sec_cnt) < 0) {
        printf("Error: ioctl BLKGETSIZE failed\n");
        close(fd);
        return -1;
    }

    // 计算布局 (与内核 sifs_format 逻辑严格对齐)
    uint32_t boot_sector_sects = 1;
    uint32_t super_block_sects = 1;
    uint32_t inode_bitmap_sects = DIV_ROUND_UP(MAX_FILES_PER_PART, BITS_PER_SECTOR);
    uint32_t inode_table_sects = DIV_ROUND_UP(((sizeof(struct sifs_inode) * MAX_FILES_PER_PART)), SECTOR_SIZE);
    
    uint32_t used_sects = boot_sector_sects + super_block_sects + inode_bitmap_sects + inode_table_sects;
    uint32_t free_sects = sec_cnt - used_sects;

    // 计算 block bitmap 所需空间
    uint32_t block_bitmap_sects = DIV_ROUND_UP(free_sects, BITS_PER_SECTOR);
    uint32_t block_bitmap_bit_len = free_sects - block_bitmap_sects;
    block_bitmap_sects = DIV_ROUND_UP(block_bitmap_bit_len, BITS_PER_SECTOR);

    // 构造超级块
    struct sifs_super_block sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = SIFS_FS_MAGIC_NUMBER;
    sb.sec_cnt = sec_cnt;
    sb.inode_cnt = MAX_FILES_PER_PART;
    
    // 相对偏移设置
    sb.block_bitmap_lba = 2; // OBR(0) + SB(1)
    sb.block_bitmap_sects = block_bitmap_sects;
    sb.inode_bitmap_lba = sb.block_bitmap_lba + sb.block_bitmap_sects;
    sb.inode_bitmap_sects = inode_bitmap_sects;
    sb.inode_table_lba = sb.inode_bitmap_lba + sb.inode_bitmap_sects;
    sb.inode_table_sects = inode_table_sects;
    sb.data_start_lba = sb.inode_table_lba + sb.inode_table_sects;
    
    sb.root_inode_no = 0;
    sb.dir_entry_size = sizeof(struct sifs_dir_entry);

    // 准备内存缓冲区 (取最大元数据块的大小)
    uint32_t max_sects = sb.inode_table_sects > sb.block_bitmap_sects ? sb.inode_table_sects : sb.block_bitmap_sects;
    uint8_t* buf = (uint8_t*)malloc(max_sects * SECTOR_SIZE);
    if (!buf) { printf("Error: Out of memory\n"); return -1; }

    // 写入超级块
    lseek(fd, 1 * SECTOR_SIZE, SEEK_SET);
    write(fd, &sb, SECTOR_SIZE);

    // 初始化并写入 Block Bitmap
    memset(buf, 0, max_sects * SECTOR_SIZE);
    buf[0] |= 0x01; // 第0块预留给根目录数据
    // 处理末尾 padding (将超出的位设为1，表示不可用)
    uint32_t last_byte = block_bitmap_bit_len / 8;
    uint8_t last_bit = block_bitmap_bit_len % 8;
    for (uint32_t b = last_byte + 1; b < sb.block_bitmap_sects * SECTOR_SIZE; b++) buf[b] = 0xff;
    for (uint8_t bit = last_bit; bit < 8; bit++) buf[last_byte] |= (1 << bit);
    
    lseek(fd, sb.block_bitmap_lba * SECTOR_SIZE, SEEK_SET);
    write(fd, buf, sb.block_bitmap_sects * SECTOR_SIZE);

    // 初始化并写入 Inode Bitmap
    memset(buf, 0, max_sects * SECTOR_SIZE);
    buf[0] |= 0x01; // 0号号Inode预留给根目录
    lseek(fd, sb.inode_bitmap_lba * SECTOR_SIZE, SEEK_SET);
    write(fd, buf, sb.inode_bitmap_sects * SECTOR_SIZE);

    // 初始化并写入 Inode Table
    memset(buf, 0, max_sects * SECTOR_SIZE);
    struct sifs_inode* root_i = (struct sifs_inode*)buf;
    root_i->i_size = sb.dir_entry_size * 2; // "." 和 ".."
    root_i->i_type = FT_DIRECTORY;
    root_i->sii.i_sectors[0] = sb.data_start_lba;
    
    lseek(fd, sb.inode_table_lba * SECTOR_SIZE, SEEK_SET);
    write(fd, buf, sb.inode_table_sects * SECTOR_SIZE);

    // 写入根目录数据块
    memset(buf, 0, max_sects * SECTOR_SIZE);
    struct sifs_dir_entry* de = (struct sifs_dir_entry*)buf;
    // "." 目录
    memcpy(de[0].filename, ".", 1);
    de[0].i_no = 0;
    de[0].f_type = FT_DIRECTORY;
    // ".." 目录
    memcpy(de[1].filename, "..", 2);
    de[1].i_no = 0;
    de[1].f_type = FT_DIRECTORY;

    lseek(fd, sb.data_start_lba * SECTOR_SIZE, SEEK_SET);
    write(fd, buf, SECTOR_SIZE); // 根目录占1个扇区

    // 收尾
    free(buf);
    close(fd);
    printf("SIFS successfully formatted on %s\n", argv[1]);
    return 0;
}

static char* get_applet_name(char* path) {
    char* name = strrchr(path, '/');
    if (name) {
        return name + 1; // 跳过 '/'
    }
    return path;
}

int do_sync(int argc,char** argv){
    sync();
    return 0;
} 

int main(int argc, char** argv) {
    if (argc < 1) return 1;

    // 首先尝试从 argv[0] 识别（支持符号链接）
    char* applet_name = get_applet_name(argv[0]);
    
    // 定义子命令的参数起点
    int sub_argc = argc;
    char** sub_argv = argv;

    // 如果 argv[0] 是 mbbox 本身，则尝试从 argv[1] 识别（支持直接调用）
    if (strcmp(applet_name, "mbbox") == 0) {
        if (argc < 2) {
            printf("MBBox - Usage: mbbox <command> [args]\n");
            printf("Functions: mount, umount, ps, df, cat, echo, hd, mkfs.ext2, mkfs.sifs\n");
            return 1;
        }
        applet_name = argv[1];
        sub_argc = argc - 1;
        sub_argv = &argv[1];
    }

    // 统一分发处理
    if (strcmp(applet_name, "mount") == 0)  return do_mount(sub_argc, sub_argv);
    if (strcmp(applet_name, "umount") == 0) return do_umount(sub_argc, sub_argv);
    if (strcmp(applet_name, "ps") == 0)     return do_ps(sub_argc, sub_argv);
    if (strcmp(applet_name, "df") == 0)     return do_df(sub_argc, sub_argv);
    if (strcmp(applet_name, "cat") == 0)     return do_cat(sub_argc, sub_argv);
    if (strcmp(applet_name, "echo") == 0)     return do_echo(sub_argc, sub_argv);
    if (strcmp(applet_name, "hd") == 0)     return do_hd(sub_argc, sub_argv);
    if (strcmp(applet_name, "fm") == 0)     return do_fm(sub_argc, sub_argv);
    if (strcmp(applet_name, "mkfs.ext2") == 0)     return do_mkfs_ext2(sub_argc, sub_argv);
    if (strcmp(applet_name, "mkfs.sifs") == 0)     return do_mkfs_sifs(sub_argc, sub_argv);
    if (strcmp(applet_name, "sync") == 0)     return do_sync(sub_argc, sub_argv);

    printf("mbbox: applet not found: %s\n", applet_name);
    return 1;
}