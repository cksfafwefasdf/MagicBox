target remote :1234
set architecture i386
b *0x7c00
b *0x900
display/8xb $pc
file ../build/kernel/kernel.elf
add-symbol-file /home/cks/musl_install/test/test_getpid 0x8048000
# b early_init
# b make_main_thread
# b after_init
# b filesys_init
# b mount_partition
b init
b sys_mount
