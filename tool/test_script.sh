sudo losetup -Pf --show disk_env/hd80M.img
# 只读查看问题
sudo e2fsck -n -v /dev/loop0p1
sudo losetup -d /dev/loop0

hexdump -C -s 1024 -n 128 /dev/loop0p1

sudo e2fsck -n -v /dev/loop0p1