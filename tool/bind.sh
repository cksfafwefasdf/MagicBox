IMG="../disk_env/hd60M.img"
MNT="../disk_env/mnt"

LOOP_DEV=$(sudo losetup -fP --show $IMG)
PART_DEV="${LOOP_DEV}p1" # 对应第一个分区


echo "Using device: $LOOP_DEV, Partition: $PART_DEV"
mkdir $MNT
sudo mount $PART_DEV $MNT