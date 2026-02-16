#!/bin/bash
set -e

# 配置路径
# 定义镜像存放的子目录
TARGET_DIR="./disk_env"

# 检查并创建目录
if [ ! -d "$TARGET_DIR" ]; then
    echo "Directory $TARGET_DIR does not exist. Creating it..."
    mkdir -p "$TARGET_DIR"
else
    echo "Directory $TARGET_DIR already exists."
fi

# 定义镜像的完整路径变量
HD60_PATH="$TARGET_DIR/hd60M.img"
HD80_PATH="$TARGET_DIR/hd80M.img"

# 创建 hd60M.img
echo "Creating $HD60_PATH..."
qemu-img create -f raw "$HD60_PATH" 60M

# 分区 hd60M.img (sda)
echo "Partitioning $HD60_PATH..."
(
  # n: 新建分区, p: 主分区, 1: 分区号
  # 40960 是 20MB 对应的扇区偏移 (20*1024*1024/512)
  # 前 20MB 留作内核和预装程序的预留区，后40MB用来分区后装文件系统
  echo n; echo p; echo 1; echo 40960; echo 122879
  echo w
) | fdisk "$HD60_PATH"

# 创建 hd80M.img
echo "Creating $HD80_PATH..."
# 163296 sectors * 512 bytes = 83607552 bytes
qemu-img create -f raw "$HD80_PATH" 83607552

# 分区 hd80M.img
echo "Partitioning $HD80_PATH..."
# fdisk 现在操作的是变量定义的路径
(
  echo n; echo p; echo 1; echo 2048; echo 33263
  echo n; echo e; echo 4; echo 33264; echo 163295
  echo n; echo 35312; echo 51407
  echo n; echo 53456; echo 76607
  echo n; echo 78656; echo 91727
  echo n; echo 93776; echo 121967
  echo n; echo 124016; echo 163295
  echo t; echo 5; echo 66
  echo t; echo 6; echo 66
  echo t; echo 7; echo 66
  echo t; echo 8; echo 66
  echo t; echo 9; echo 66
  echo w
) | fdisk "$HD80_PATH"

echo "---------------------------------------"
echo "Done! Verified with fdisk -l:"
fdisk -l "$HD80_PATH"
fdisk -l "$HD60_PATH"