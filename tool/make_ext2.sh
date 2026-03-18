#!/bin/sh
# 该文件用于说明如何在宿主机上为一个磁盘镜像文件初始化上 ext2 文件系统

IMG="../disk_env/hd20M_share.img"
# IMG="../disk_env/hd60M.img"
# IMG="../disk_env/hd80M.img"
MNT="../disk_env/mnt_ext2"

# 映射镜像并获取设备名（--show 会返回分配的 loop 名，如 /dev/loop0）
# 映射镜像（-P 会扫描分区表）
# Linux losetup 命令用于设置循环设备。
# 循环设备可把文件虚拟成区块设备，籍以模拟整个文件系统，
# 让用户得以将其视为硬盘驱动器，光驱或软驱等设备，并挂入当作目录来使用。
LOOP_DEV=$(sudo losetup -fP --show $IMG)
PART_DEV="${LOOP_DEV}p1" # 对应第一个分区

echo "Using device: $LOOP_DEV, Partition: $PART_DEV"

# 只格式化第一个分区 (p1)
# 这一步绝对不要直接写镜像文件名！否则会破坏分区表
# -O ^dir_index: 关闭目录索引（哈希树），改用简单的线性扫描
# -O filetype: 目录项里存储文件类型
# -O ^sparse_super: 每个块组都放超级块备份
# -O ^resize_inode: 关闭动态inode大小
sudo mkfs.ext2 -b 1024 -I 128 -r 1 \
               -O ^dir_index,^sparse_super,filetype,^resize_inode\
               /dev/loop0p1

# 创建挂载点并挂载第一个分区
sudo mkdir -p $MNT
sudo mount /dev/loop0p1 $MNT

# 创建一个测试文本，方便我们之后在内核里直接搜字符串
echo "hello ext2 world" | sudo tee $MNT/test.txt
sudo mkdir $MNT/test_dir 
echo "hello dir ext2!" | sudo tee $MNT/test_dir/test.txt

# 操作完成后，卸载并解除映射
sudo sync
sudo umount $MNT
sudo losetup -d $LOOP_DEV

echo "Done! Ext2 Image initialized."