#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(TimeVal *val, int _tz) // TODO: implement sys_gettimeofday in pagetable. (VA to PA)
{
	if (!val){
		return -1;
	}

	//create the time result here in kernel since val is a user VA and we cannot write to it
	/* The code in `ch3` will leads to memory bugs*/
	TimeVal valTemp;
    uint64 cycle = get_cycle();
    valTemp.sec = cycle / CPU_FREQ;
    valTemp.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;

	//copys from kernel to page table 
    if (copyout(curr_proc()->pagetable, (uint64)val, (char *)&valTemp, sizeof(TimeVal)) < 0) {
        return -1;
    }

	return 0;
}

// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
/*
* LAB1: you may need to define sys_task_info here
*/
uint64 sys_task_info( TaskInfo *task_info) {
    struct proc *p = curr_proc();

    if (task_info == 0) return -1; 

    // Populate a temporary TaskInfo struct in kernel space
    TaskInfo temp;

    temp.status = Running;

    // Add the syscall counts to our task info struct
    for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
        temp.syscall_times[i] = p->syscall_times[i];
    }

	temp.time = (int)((get_cycle() - p->start_time) / (CPU_FREQ / 1000));
    // calculate then set the time for task info
	//copy out to send data from kernel to va
    if (copyout(p->pagetable, (uint64)task_info, (char *)&temp, sizeof(TaskInfo)) < 0) {
        return -1;
    }

    return 0;
}

int sys_mmap(void* start, unsigned long long len, int port, int flag, int fd){
	// Cast the void* start address to a uint64 for pointer arithmetic
	uint64 virtAdrStart = (uint64) start;

	// Reject if start address is not page-aligned
	if (virtAdrStart % PAGE_SIZE != 0){
		return -1;
	}

	// Reject if any bits above the lower 3 (R/W/X) are set in port which makes them invalid permission flags
	if ((port & ~0x7)!=0){
		return -1;
	}

	// Reject if all permission bits are zero to make sure its accessible
	if((port & 0x7) == 0){
		return -1;
	}

	// Reject if the requested length exceeds 1 GiB
	if (len > (1UL << 30)){
		return -1;
	}

	// Round len up to the nearest page boundary
	uint64 size = PGROUNDUP(len);

	// Calculate the exclusive end address of the virtual range to be mapped
	uint64 virtAdrEnd = virtAdrStart + size;

	// Get a pointer to the current process's struct
	struct proc *p = curr_proc(); 

	// Walk every page in the requested range and reject if any are already mapped
	for (uint64 va = virtAdrStart; va < virtAdrEnd; va+= PAGE_SIZE){
		if (walkaddr(p->pagetable, va)!=0){return -1;}
	}

	// Start with PTE_U 
	int permBits = PTE_U;

	// Bit 0 of port the read permission
	if (port & 0x1){
		permBits = permBits | PTE_R;
	}

	// Bit 1 of port the write permission
	if (port & 0x2){
		permBits = permBits | PTE_W;
	}

	// Bit 2 of port the execute permission
	if (port & 0x4){
		permBits = permBits | PTE_X;
	}

	// Allocate and map one physical page for each virtual page in the range
	for (uint64 va = virtAdrStart; va < virtAdrEnd; va+= PAGE_SIZE){
		// Allocate a fresh physical page from the kernel's free memory pool
		void* physAdr = kalloc();

		// If kalloc returns NULL, the system is out of memory
		if (!physAdr){return -1;}

		// Zero out the page so no leftover kernel data is leaked to userspace
		memset(physAdr, 0, PAGE_SIZE);

		// Install the virtual physical mapping in the process's page table with the assembled permission bits
		if (mappages(p->pagetable, va, PAGE_SIZE, (uint64)physAdr, permBits) < 0){return -1;}
	}
	return 0;
}


int sys_munmap(void* start, unsigned long long len){
	// Cast the void* start address to a uint64 for pointer arithmetic
	uint64 virtAdrStart = (uint64) start;

	// Nothing to unmap no need to unmap
	if (len == 0){return 0;}

	// Reject if start address is not page-aligned
	if (virtAdrStart %PAGE_SIZE != 0){
		return -1;
	}

	// Round len up to the nearest page boundary to cover the full range
	uint64 size = PGROUNDUP(len);

	// Calculate the exclusive end address of the virtual range to be unmapped
	uint64 virtAdrEnd = virtAdrStart + size;

	// Get a pointer to the current process's struct
	struct proc *p = curr_proc();
	
	// Walk every page in the range and reject if any are NOT mapped 
	for (uint64 va = virtAdrStart; va < virtAdrEnd; va+= PAGE_SIZE){
		if (walkaddr(p->pagetable, va) ==0){return -1;}
	}

	// Calculate number of pages to unmap
	uint64 npages = size/PAGE_SIZE;

	// Remove the mappings from the page table and free the underlying physical pages
	// (the '1' argument is the do_free flag — tells uvmunmap to call kfree on each page)
	uvmunmap(p->pagetable, virtAdrStart, npages, 1);
	return 0;
}

extern char trap_page[];

void syscall()
{
	struct proc *p = curr_proc();
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	/*
	* LAB1: you may need to update syscall counter for task info here
	*/
	//update syscall counter for task info here
	if ( id >= 0 && id < MAX_SYSCALL_NUM ) {
    	p->syscall_times[id]++;
	}
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday((TimeVal *)args[0], args[1]);
		break;
	/*
	* LAB1: you may need to add SYS_taskinfo case here
	*/
	//add SYS_taskinfo case here
	case SYS_task_info:
        ret = sys_task_info((TaskInfo *)args[0]);
        break;
	case SYS_mmap:
		ret = sys_mmap((void *)args[0], (unsigned long long)args[1], (int)args[2], (int)args[3], (int)args[4]);
		break;
	case SYS_munmap:
		ret = sys_munmap((void *)args[0], (unsigned long long)args[1]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
