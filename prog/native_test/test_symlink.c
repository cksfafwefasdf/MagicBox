#include <errno.h>
#include <syscall.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unitype.h> 

int main() {
    printf("\n--- MagicBox Symlink User-Space Test Start ---\n");

    // 测试 Fast Symlink 创建
    // 逻辑：路径小于 60 字节，内核应存储在 i_block 数组中
    printf("1. Creating Fast Symlink...\n");
    if (symlink("/bin/cat", "/cat_lnk") == 0) {
        printf("[OK] Fast Symlink created: /ls_lnk\n");
    } else {
        printf("[FAIL] Failed to create Fast Symlink\n");
    }

    // 测试 Normal Symlink 创建
    // 逻辑：路径较长，内核应分配独立磁盘块
    printf("2. Creating Normal Symlink...\n");
    char long_path[128];
    memset(long_path, 'a', 100);
    long_path[100] = '\0'; // 100 字节，强制触发 Normal Link
    if (symlink(long_path, "/long_lnk") == 0) {
        printf("[OK] Normal Symlink created: /long_lnk\n");
    }else{
        printf("[FAIL] Failed to normal Symlink\n");
    }

    // 测试 相对路径 + 目录穿透
    printf("3. Testing Relative Link & Path Resolution...\n");
    mkdir("/test_dir");
    // 创建一个指向 test_dir 的链接
    symlink("test_dir", "/dir_lnk");
    
    // 在真实目录里创建一个文件
    int32_t fd = open("/test_dir/magic.txt", O_CREATE | O_RDWR);
    write(fd, "Kernel Magic!", 13);
    close(fd);

    // 尝试通过链接访问：/dir_lnk -> test_dir -> magic.txt
    // 这将验证 search_file 的相对路径解析和 STATE_PARSE_COMPONENT 状态切换
    int32_t link_fd = open("/dir_lnk/magic.txt", O_RDONLY);
    if (link_fd >= 0) {
        char read_buf[32] = {0};
        read(link_fd, read_buf, 13);
        printf("[OK] Content read through link: %s\n", read_buf);
        close(link_fd);
    } else {
        printf("[FAIL] Path resolution through link failed!\n");
    }

    // 测试 绝对路径重启 (Absolute Path Restart)
    // 逻辑：/to_root -> / -> test_dir/magic.txt
    printf("4. Testing Absolute Link Restart...\n");
    symlink("/", "/to_root");
    int32_t root_lnk_fd = open("/to_root/test_dir/magic.txt", O_RDONLY);
    if (root_lnk_fd >= 0) {
        printf("[OK] Absolute path restart success!\n");
        close(root_lnk_fd);
    }

    // 测试 嵌套链接 (Nested Links)
    // 逻辑：/v2 -> /ls_lnk -> /bin/ls
    printf("5. Testing Nested Links (Depth=2)...\n");
    symlink("/ls_lnk", "/v2");
    
    struct stat s_v2, s_target;
    if (stat("/v2", &s_v2) == 0 && stat("/bin/ls", &s_target) == 0) {
        if (s_v2.st_ino == s_target.st_ino) {
            printf("[OK] Nested link matched target Inode: %d\n", s_v2.st_ino);
        }
    }

    // 循环链接防御 (Circular Link Defense)
    printf("6. Testing Circular Link (Loop Protection)...\n");
    symlink("/loop_a", "/loop_b");
    symlink("/loop_b", "/loop_a");
    // 预期：内核 search_file 会因为 symlink_depth > 8 返回 -1
    if (open("/loop_a", O_RDONLY) == -ELOOP) {
        printf("[OK] Circular link blocked by Kernel safely.\n");
    } else {
        printf("[FAIL] Kernel didn't block the circular link!\n");
    }

    printf("--- MagicBox Symlink Test Complete ---\n\n");
}