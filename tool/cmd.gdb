target remote :1234
b *0x7c00
b *0x900
display/8xb $pc
file ../build/kernel/kernel.bin
# b early_init
# b make_main_thread
# b after_init
# b filesys_init
# b mount_partition
b init

