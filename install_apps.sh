#!/bin/sh

# 配置路径
ROOT_DIR=$(pwd)
BUILD_DIR="./build/prog"
KERNEL_BUILD="./build/kernel"   
LIB_DIR="./lib"
DD_OUT="./disk_env/hd60M.img"
TAR_NAME="$BUILD_DIR/initrd.tar"
LBA_SEEK=1000

# 确保目录存在且清空旧包
mkdir -p "$BUILD_DIR"
rm -f "$TAR_NAME"

# 编译选项
CFLAGS="-Wall -c -fno-builtin -m32 -fno-stack-protector -I $LIB_DIR -I $LIB_DIR/common -I $LIB_DIR/user"
BASE_OBJS="$KERNEL_BUILD/string.o $KERNEL_BUILD/syscall.o $KERNEL_BUILD/stdio.o $KERNEL_BUILD/assert.o"

# 编译 CRT 入口
echo "Compiling start.s..."
nasm ./prog/start.s -f elf -o "$BUILD_DIR/start.o"

# 定义目标程序
# 格式: "程序名,源文件"
TARGETS="cat,prog/prog/cat.c echo,prog/prog/echo.c prog_pipe,prog/prog/prog_pipe.c shell,prog/shell/shell.c"

# 循环编译
BIN_LIST="" # 用于记录编译成功的二进制文件名
for item in $TARGETS; do
    OLD_IFS=$IFS; IFS=","; set -- $item; BIN=$1; SRC=$2; IFS=$OLD_IFS
    
    echo "---------------------------------------"
    echo "Compiling [$BIN] from $SRC"

    if [ "$BIN" = "shell" ]; then
        gcc $CFLAGS -o "$BUILD_DIR/buildin_cmd.o" "prog/shell/buildin_cmd.c"
        gcc $CFLAGS -o "$BUILD_DIR/$BIN.o" "$SRC"
        EXTRA_OBJS="$BUILD_DIR/buildin_cmd.o"
    else
        gcc $CFLAGS -o "$BUILD_DIR/$BIN.o" "$SRC"
        EXTRA_OBJS=""
    fi

    ld -m elf_i386 "$BUILD_DIR/$BIN.o" $EXTRA_OBJS $BASE_OBJS "$BUILD_DIR/start.o" -o "$BUILD_DIR/$BIN"
    
    if [ -f "$BUILD_DIR/$BIN" ]; then
        BIN_LIST="$BIN_LIST $BIN"
    else
        echo "ERROR: Build failed for $BIN"
        exit 1
    fi
done

# 打包为 Tar
echo "---------------------------------------"
echo "Creating directory structure and tar archive..."

# 在 build 目录下创建一个临时 bin 目录
mkdir -p "$BUILD_DIR/bin"

# 将编译好的二进制文件移动或链接到 bin 目录下
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