#!/bin/sh
# 该文件用于说明如何在宿主机上为一个磁盘镜像文件初始化上 ext2 文件系统

# 映射镜像（-P 会扫描分区表）
# Linux losetup 命令用于设置循环设备。
# 循环设备可把文件虚拟成区块设备，籍以模拟整个文件系统，
# 让用户得以将其视为硬盘驱动器，光驱或软驱等设备，并挂入当作目录来使用。
sudo losetup -fP ./disk_env/hd20M_share.img

# 查看生成的设备名
# 不出意外应该能看到 loop0p1 这样的名字，p1就是第一个分区，p2是第二个
ls /dev/loop0*

# 只格式化第一个分区 (p1)
# 这一步绝对不要直接写镜像文件名！否则会破坏分区表
# -O ^dir_index: 关闭目录索引（哈希树），改用简单的线性扫描
# -O ^filetype: 目录项里不存储文件类型，让代码更简单
# -O ^sparse_super: 每个块组都放超级块备份
sudo mkfs.ext2 -b 1024 -I 128 -r 0 \
               -O ^dir_index,^sparse_super \
               /dev/loop0p1

# 创建挂载点并挂载第一个分区
mkdir -p ./disk_env/mnt_ext2
sudo mount /dev/loop0p1 ./disk_env/mnt_ext2

# 创建一个测试文本，方便我们之后在内核里直接搜字符串
echo "hello ext2 world" | sudo tee ./disk_env/mnt_ext2/test.txt
sudo mkdir ./disk_env/mnt_ext2/test_dir 
echo "hello dir ext2!" | sudo tee ./disk_env/mnt_ext2/test_dir/test.txt

# 操作完成后，卸载并解除映射
sudo umount ./disk_env/mnt_ext2
sudo losetup -d /dev/loop0