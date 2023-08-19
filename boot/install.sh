nasm -I include -o bin/mbr.bin mbr/mbr.s
nasm -I include -o bin/loader.bin loader/loader.s

sudo dd if=~/gitrepo/MagicBox/boot/bin/mbr.bin of=/opt/bochs/hd60M.img count=1 bs=512 conv=notrunc 
sudo dd if=~/gitrepo/MagicBox/boot/bin/loader.bin seek=2 of=/opt/bochs/hd60M.img count=2 bs=512 conv=notrunc 

echo "install successfully"
