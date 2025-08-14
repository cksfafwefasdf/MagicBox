#include "print.h"
#include "init.h"
#include "debug.h"
#include "memory.h"
#include "thread.h"
#include "interrupt.h"
#include "console.h"
#include "ioqueue.h"
#include "keyboard.h"
#include "process.h"
#include "syscall.h"
#include "stdio.h"
#include "fs.h"
#include "string.h"
#include "stdio-kernel.h"
#include "dir.h"
#include "main.h"
#include "timer.h"
#include "shell.h"
#include "assert.h"
#include "ide.h"
#include "exec.h"
// void k_thread_a(void*);
// void k_thread_b(void*);
// void u_prog_a(void);
// void u_prog_b(void);
void build_env(void);
void delete_dir_test(void);
void cwd_test(void);
void stat_test(void);
void load_prog(const char* disk_name,uint32_t lba,const char* filename,uint32_t file_size);

int main(void) {
   put_str("enter kernel\n");
   init_all();

   // load_prog("/prog_no_arg",300,10324);
   // load_prog("/prog_arg",400,10548);
   // load_prog("sda",500,"/cat",10568);
   // load_prog("sda",600,"/prog_pipe",10552);

   mtime_sleep(3000);
   cls_screen();
   printk(PROMPT_STR,"/");
   

   // build_env();
   // delete_dir_test();
   // cwd_test();
   // stat_test();

   // printf("%d\n",sys_unlink("/dir1/subdir1/file2"));
   // printf("%d\n",sys_rmdir("/dir1/subdir1/"));
   // printf("%d\n",sys_rmdir("/dir1"));

   // int fd = sys_open("/file6",O_CREATE|O_RDWR);
   // char str[] = "hello,world!";
   // printf("%d\n",sys_write(fd,str,12));
   // printf("%d",sys_mkdir("/dir1/"));

   // printf("%d\n",sys_mkdir("/dir1/subdir1"));
   // printf("%d\n",sys_mkdir("/dir1"));
   // int fd2 = sys_open("/file2",O_CREATE|O_RDWR);
   // char str2[] = "hello,world!";
   // printf("%d\n",sys_write(fd2,str2,12));   
   while(1);
   return 0;
}

void load_prog(const char* disk_name,uint32_t lba,const char* filename,uint32_t file_size){
   uint32_t sec_cnt = DIV_ROUND_UP(file_size,512);
   struct disk* disk;
   if(!strcmp("sda",disk_name)){
      disk = &channels[0].devices[0];
   }else if(!strcmp("sdb",disk_name)){
      disk = &channels[0].devices[1];
   }else{
      printf("unknown disk name!\n");
   }
   
   void* prog_buf =  sys_malloc(file_size);
   ide_read(disk,lba,prog_buf,sec_cnt);
   int32_t fd = sys_open(filename,O_CREATE|O_RDWR);

   if(fd!=-1){
      if(sys_write(fd,prog_buf,file_size)==-1){
         printk("file write error!\n");
         while(1);
      }
   }
   sys_free(prog_buf);
   sys_close(fd);
}

void init(void){
   uint32_t ret_pid = fork();
   if(ret_pid){ // parent proc
      int status;
      int child_pid;
      // init will recycle the zombie proc
      while(1){   
         child_pid = wait(&status);
         printf("I'm init, my pid is 1,I receive a child, it's pid is %d, status is %d\n",child_pid,status);
      }
      // printf("I am father, my pid is %d, child pid is %d\n",getpid(),ret_pid);
   }else{
      my_shell();
      // printf("I am child, my pid is %d, ret pid is %d\n",getpid(),ret_pid);
   }
   panic("init: should not be here!\n");
}

void stat_test(){
   struct stat obj_stat;
   sys_stat("/",&obj_stat);
   printf("/'s info\n\ti_no: %d\n\tsize: %d\n\tfiletype: %s\n",\
   obj_stat.st_ino,obj_stat.st_size,obj_stat.st_filetype==2?"directory":"regular");
   sys_stat("/dir1",&obj_stat);
   printf("/dir1's info\n\ti_no: %d\n\tsize: %d\n\tfiletype: %s\n",\
   obj_stat.st_ino,obj_stat.st_size,obj_stat.st_filetype==2?"directory":"regular");
}

void cwd_test(){
   char cwd_buf[32] = {0};
   sys_getcwd(cwd_buf,32);
   printf("cwd: %s\n",cwd_buf);
   sys_chdir("/diraaa");
   sys_chdir("/dir1");
   printf("change cwd now\n");
   sys_getcwd(cwd_buf,32);
   printf("cwd: %s\n",cwd_buf);
}

void build_env(){
      int fd = sys_open("/file1",O_CREATE|O_RDWR);
      char str[] = "hello,world!\n";
      printf("%d\n",sys_write(fd,str,13));
      printf("%d\n",sys_mkdir("/dir1"));
      printf("%d\n",sys_mkdir("/dir1/subdir1"));
      fd = sys_open("/dir1/subdir1/file2",O_CREATE|O_RDWR);
      printf("%d\n",sys_write(fd,str,13));
}

void delete_dir_test(){
   printf("/dir1 content before delete /dir1/subdir1:\n");
   struct dir* dir = sys_opendir("/dir1");
   char* type = NULL;
   struct dir_entry* dir_e = NULL;
   while((dir_e=sys_readdir(dir))){
      if(dir_e->f_type==FT_REGULAR){
         type = "regular";
      }else{
         type = "directory";
      }
      printf("\t%s\t%s\n",type,dir_e->filename);
   }

   printf("try to delete nonempty directory /dir1/subdir1\n");

   if(sys_rmdir("/dir1/subdir1")==-1){
      printf("sys_rmdir: /dir1/subdir1 delete fail!\n");
   }

   printf("try to delete /dir1/subdir1/file2\n");
   if(sys_rmdir("/dir1/subdir1/file2")==-1){
      printf("sys_rmdir: /dir1/subdir1/file2 delete fail!\n");
   }
   if(sys_unlink("/dir1/subdir1/file2")==0){
      printf("sys_unlink: /dir1/subdir1/file2 delete done \n");
   }

   printf("try to delete directory /dir1/subdir1 again\n");
   if(sys_rmdir("/dir1/subdir1")==0){
      printf("/dir1/subdir1 delete done!\n");
   }
   
   printf("/dir1 content after delete /dir1/subdir1:\n");

   sys_rewinddir(dir);
   while((dir_e=sys_readdir(dir))){
      if(dir_e->f_type==FT_REGULAR){
         type = "regular";
      }else{
         type = "directory";
      }
      printf("\t%s\t%s\n",type,dir_e->filename);
   }
}


// void k_thread_a(void* arg) {     
//    void* addr1 = sys_malloc(256);
//    void* addr2 = sys_malloc(255);
//    void* addr3 = sys_malloc(254);
//    console_put_str(" thread_a malloc addr:0x");
//    console_put_int_HAX((int)addr1);
//    console_put_char(',');
//    console_put_int_HAX((int)addr2);
//    console_put_char(',');
//    console_put_int_HAX((int)addr3);
//    console_put_char('\n');

//    uint32_t cpu_delay = 10000000;
//    while(cpu_delay-->0);
//    sys_free(addr1);
//    sys_free(addr2);
//    sys_free(addr3);
//    while(1);
// }

// void k_thread_b(void* arg) {     
//    void* addr1 = sys_malloc(256);
//    void* addr2 = sys_malloc(255);
//    void* addr3 = sys_malloc(254);
//    console_put_str(" thread_b malloc addr:0x");
//    console_put_int_HAX((int)addr1);
//    console_put_char(',');
//    console_put_int_HAX((int)addr2);
//    console_put_char(',');
//    console_put_int_HAX((int)addr3);
//    console_put_char('\n');

//    int cpu_delay = 10000000;
//    while(cpu_delay-->0);
//    sys_free(addr1);
//    sys_free(addr2);
//    sys_free(addr3);
//    while(1);
// }
// void u_prog_a(void) {
//    void* addr1 = malloc(256);
//    void* addr2 = malloc(255);
//    void* addr3 = malloc(254);
//    printf(" proc_a malloc addr:0x%x,0x%x,0x%x\n",(int)addr1,(int)addr2,(int)addr3);

//    int cpu_delay = 100000;
//    while(cpu_delay-->0);
//    free(addr1);
//    free(addr2);
//    free(addr3);
//    while(1);
// }

// void u_prog_b(void) {
//    void* addr1 = malloc(256);
//    void* addr2 = malloc(255);
//    void* addr3 = malloc(254);
//    printf(" proc_b malloc addr:0x%x,0x%x,0x%x\n",(int)addr1,(int)addr2,(int)addr3);

//    int cpu_delay = 100000;
//    while(cpu_delay-->0);
//    free(addr1);
//    free(addr2);
//    free(addr3);
//    while(1);
// }
