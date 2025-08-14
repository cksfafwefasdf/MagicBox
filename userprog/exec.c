#include "stdint.h"
#include "stdbool.h"
#include "global.h"
#include "fs.h"
#include "memory.h"
#include "string.h"
#include "thread.h"
#include "exec.h"
#include "stdio.h"
#include "syscall.h"

extern void intr_exit;

static bool segment_load(int32_t fd,uint32_t offset,uint32_t filesz,uint32_t vaddr){
	
	uint32_t vaddr_first_page = vaddr & 0xfffff000;

	uint32_t size_in_first_page = PG_SIZE - (vaddr& 0x00000fff);
	uint32_t occupy_pages = 0;

	if(filesz>size_in_first_page){
		uint32_t left_size = filesz - size_in_first_page;
		occupy_pages = DIV_ROUND_UP(left_size,PG_SIZE)+1;
	}else{
		occupy_pages = 1;
	}

	uint32_t page_idx = 0;
	uint32_t vaddr_page = vaddr_first_page;
	
	while(page_idx<occupy_pages){
		uint32_t* pde = pde_ptr(vaddr_page);
		uint32_t* pte = pte_ptr(vaddr_page);
		if(!(*pde&0x00000001)|| !(*pte & 0x00000001)){
			if(mapping_v2p(PF_USER,vaddr_page)==NULL){
				return false;
			}
		}
		vaddr_page+=PG_SIZE;
		page_idx++;
	}

	sys_lseek(fd,offset,SEEK_SET);
	sys_read(fd,(void*)vaddr,filesz);
	
	return true;
}

// static int32_t load_new(const char* pathname){
// 	int32_t ret = -1;
// 	struct Elf32_Ehdr elf_header;
// 	memset(&elf_header,0,sizeof(struct Elf32_Ehdr));

// 	int32_t fd = sys_open(pathname,O_RDONLY);
// 	if(fd == -1){
// 		return -1;
// 	}
	
// 	if(sys_read(fd,&elf_header,sizeof(struct Elf32_Ehdr))!= sizeof(struct Elf32_Ehdr)){
// 		ret = -1;
// 		goto done;
// 	}

// 	if(memcmp(elf_header.e_ident,"\177ELF\1\1\1",7)\
// 	|| elf_header.e_type!= 2\
// 	|| elf_header.e_machine!=3\
// 	|| elf_header.e_version!=1\
// 	|| elf_header.e_phnum>1024\
// 	|| elf_header.e_phentsize!=sizeof(struct Elf32_Phdr)){
// 		return -1;
// 		goto done;
// 	}

// 	Elf32_Off prog_header_offset = elf_header.e_phoff;
// 	Elf32_Half prog_header_size = elf_header.e_phentsize;
// 	uint32_t load_segment_nr = 0;
// 	struct Elf32_Phdr prog_headers[MAX_PT_LOADER_SEGMENT];
// 	memset(prog_headers,0,MAX_PT_LOADER_SEGMENT*sizeof(struct Elf32_Phdr));
// 	uint32_t prog_idx = 0;
// 	struct Elf32_Phdr tmp;
// 	while(prog_idx<elf_header.e_phnum){
// 		memset(&tmp,0,sizeof(struct Elf32_Phdr));
// 		sys_lseek(fd,prog_header_offset,SEEK_SET);
// 		if(sys_read(fd,&tmp,prog_header_size)!=prog_header_size){
// 			ret = -1;
// 			goto done;
// 		}
// 		if(tmp.p_type==PT_LOAD){
// 			memcpy(&prog_headers[load_segment_nr],&tmp,sizeof(struct Elf32_Phdr));
// 			load_segment_nr++;
// 		}
// 		prog_header_offset+=elf_header.e_phentsize;
// 		prog_idx++;
// 	}
// 	int i=0;
// 	for(;i<MAX_PT_LOADER_SEGMENT;i++){
// 		// printf("prog_idx,p_type,prog_headers:%x,%x,%x\n",i,prog_headers[i].p_type,prog_headers);
// 		if(prog_headers[i].p_type==PT_LOAD){
// 			if(!segment_load(fd,prog_headers[i].p_offset,prog_headers[i].p_filesz,prog_headers[i].p_vaddr)){
// 				ret = -1;
// 				goto done;
// 			}
// 		}
// 	}
// 	ret = elf_header.e_entry;
// done:
// 	sys_close(fd);
// 	return ret;
// }

static int32_t load(const char* pathname){
	int32_t ret = -1;
	struct Elf32_Ehdr elf_header;
	struct Elf32_Phdr prog_header;
	memset(&elf_header,0,sizeof(struct Elf32_Ehdr));

	int32_t fd = sys_open(pathname,O_RDONLY);
	if(fd == -1){
		return -1;
	}
	
	
	if(sys_read(fd,&elf_header,sizeof(struct Elf32_Ehdr))!= sizeof(struct Elf32_Ehdr)){
		ret = -1;
		goto done;
	}

	if(memcmp(elf_header.e_ident,"\177ELF\1\1\1",7)\
	|| elf_header.e_type!= 2\
	|| elf_header.e_machine!=3\
	|| elf_header.e_version!=1\
	|| elf_header.e_phnum>1024\
	|| elf_header.e_phentsize!=sizeof(struct Elf32_Phdr)){
		return -1;
		goto done;
	}

	Elf32_Off prog_header_offset = elf_header.e_phoff;
	Elf32_Half prog_header_size = elf_header.e_phentsize;

	uint32_t prog_idx = 0;
	
	while(prog_idx<elf_header.e_phnum){
		
		memset(&prog_header,0,prog_header_size);
		
		sys_lseek(fd,prog_header_offset,SEEK_SET);
		// printf("1\n1\n1\n");
		if(sys_read(fd,&prog_header,prog_header_size)!=prog_header_size){
			ret = -1;
			goto done;
		}
		// printf("prog_header.p_type:%x\n",prog_header.p_type);
		if(PT_LOAD == prog_header.p_type){
			if(!segment_load(fd,prog_header.p_offset,prog_header.p_filesz,prog_header.p_vaddr)){
				ret = -1;
				goto done;
			}
		}

		prog_header_offset+=elf_header.e_phentsize;
		prog_idx++;
	}

	ret = elf_header.e_entry;

done:
	sys_close(fd);
	return ret;
}

int32_t sys_execv(const char* path,const char* argv[]){
	
	uint32_t argc = 0;
	while(argv[argc]){
		argc++;
	}
	
	int32_t entry_point = load(path);
	

	if(entry_point==-1){
		return -1;
	}

	struct task_struct* cur = get_running_task_struct();
	memcpy(cur->name,path,TASK_NAME_LEN);
	cur->name[TASK_NAME_LEN-1] = 0;

	struct intr_stack* intr_0_stack = (struct intr_stack*) ((uint32_t)cur+PG_SIZE-sizeof(struct intr_stack));

	intr_0_stack->ebx = (int32_t)argv;
	intr_0_stack->ecx = argc;
	intr_0_stack->eip = (void*) entry_point;
	intr_0_stack->esp = (void*) 0xc0000000;
	
	asm volatile ("movl %0,%%esp; jmp intr_exit"::"g"(intr_0_stack):"memory");
	return 0;
}