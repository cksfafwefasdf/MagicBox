#!/bin/sh

# 使用方法：使用 sh install_apps.sh 只打包核心程序 
# 使用方法：使用 sh install_apps.sh test 同时打包核心程序和测试程序 

# 配置路径
ROOT_DIR=$(pwd)
BUILD_DIR="./build/prog"
KERNEL_BUILD="./build/kernel"   
LIB_DIR="./include"
DD_OUT="./disk_env/hd60M.img"
TAR_NAME="$BUILD_DIR/initrd.tar"
USER_LIBC="$KERNEL_BUILD/libmagicbox.a"
LBA_SEEK=1000

# 确保目录存在且清空旧包
mkdir -p "$BUILD_DIR"
rm -f "$TAR_NAME"

# 编译选项
CFLAGS="-Wall -c -fno-builtin -m32 -fno-stack-protector -I $LIB_DIR -I $LIB_DIR/uapi -I $LIB_DIR/sys -I $LIB_DIR/linux"
BASE_OBJS="$KERNEL_BUILD/string.o $KERNEL_BUILD/syscall.o $KERNEL_BUILD/stdio.o $KERNEL_BUILD/assert.o"

# 编译 CRT 入口
echo "Compiling start.s..."
nasm ./prog/start.s -f elf -o "$BUILD_DIR/start.o"

# 定义目标程序
# 格式: "程序名,源文件"
# 定义核心程序 (mbbox, mbsh)
CORE_TARGETS="mbbox,prog/prog/mbbox.c mbsh,prog/shell/shell.c"

# 定义测试程序 (test_*)
TEST_TARGETS="test_sig,prog/native_test/test_sig.c test_fifo,prog/native_test/test_fifo.c \
test_malloc,prog/native_test/test_malloc.c test_kmalloc,prog/native_test/test_kmalloc.c \
test_mmap,prog/native_test/test_mmap.c test_mmap_file,prog/native_test/test_mmap_file.c \
test_symlink,prog/native_test/test_symlink.c test_rawtty,prog/native_test/test_raw_tty.c \
test_timer,prog/native_test/test_timer.c test_truncate,prog/native_test/test_truncate.c \
test_buffer,prog/native_test/test_ide_buffer.c"

# 根据参数决定最终编译列表
# $1 表示脚本收到的第一个参数
if [ "$1" = "test" ]; then
    echo "Mode: Full Build (Core + Tests)"
    TARGETS="$CORE_TARGETS $TEST_TARGETS"
else
    echo "Mode: Minimal Build (Core only)"
    echo "Hint: Run './script.sh test' to include test programs."
    TARGETS="$CORE_TARGETS"
fi

# 循环编译
BIN_LIST="" # 用于记录编译成功的二进制文件名
for item in $TARGETS; do
    OLD_IFS=$IFS; IFS=","; set -- $item; BIN=$1; SRC=$2; IFS=$OLD_IFS
    
    echo "---------------------------------------"
    echo "Compiling [$BIN] from $SRC"

    if [ "$BIN" = "mbsh" ]; then
        gcc $CFLAGS -o "$BUILD_DIR/buildin_cmd.o" "prog/shell/buildin_cmd.c"
        gcc $CFLAGS -o "$BUILD_DIR/$BIN.o" "$SRC"
        EXTRA_OBJS="$BUILD_DIR/buildin_cmd.o"
    else
        gcc $CFLAGS -o "$BUILD_DIR/$BIN.o" "$SRC"
        EXTRA_OBJS=""
    fi

    # 要注意链接顺序！！！start.o 最前，库文件 $USER_LIBC 最后
    # 当 ld 读到 cat.o 时，它会发现有一些符号（如 printf 或 write）是未定义的。
    # 随后当它读到后面的 $USER_LIBC (libmagicbox.a) 时，它会从库中提取这些符号的定义。
    # 如果反过来写（库在前，.o 在后），ld 在读到库时还没看到未定义的符号，
    # 它就会认为库里啥也不需要，直接跳过，最后报 undefined reference。
    ld -m elf_i386 "$BUILD_DIR/start.o" "$BUILD_DIR/$BIN.o" $EXTRA_OBJS "$USER_LIBC" -o "$BUILD_DIR/$BIN"
    
    if [ -f "$BUILD_DIR/$BIN" ]; then
        BIN_LIST="$BIN_LIST $BIN"
    else
        echo "ERROR: Build failed for $BIN"
        exit 1
    fi
done

# 清理中间生成的 .o 文件
rm -f "$BUILD_DIR"/*.o

# 打包为 Tar
echo "---------------------------------------"
echo "Creating directory structure and tar archive..."

# 先删除旧的 bin 目录，确保它是干净的
rm -rf "$BUILD_DIR/bin"
mkdir -p "$BUILD_DIR/bin"

# 将编译好的二进制文件移动到 bin 目录下
for BIN in $BIN_LIST; do
    mv "$BUILD_DIR/$BIN" "$BUILD_DIR/bin/"
done

# 进入 build 目录打包 bin 文件夹
cd "$BUILD_DIR" || exit
# 打包 bin 目录本身，这样 tar 包里就会包含目录项和正确路径
tar -cf "initrd.tar" bin/
cd "$ROOT_DIR" || exit

# 写入磁盘
if [ -f "$TAR_NAME" ]; then
    SIZE=$(ls -l "$TAR_NAME" | awk '{print $5}')
    SEC_CNT=$(((SIZE + 511) / 512))
    
    dd if="$TAR_NAME" of="$DD_OUT" bs=512 count=$SEC_CNT seek=$LBA_SEEK conv=notrunc
    echo "---------------------------------------"
    echo "SUCCESS: $TAR_NAME ($SIZE bytes) written to $DD_OUT at LBA $LBA_SEEK."
    echo "Files in tar: $BIN_LIST"
else
    echo "ERROR: Tar file not found."
fi
