#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);
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

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	debugf("sys_read fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDIN)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
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

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!\n");
	return fork();
}

uint64 sys_exec(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	copyinstr(p->pagetable, name, va, 200);
	debugf("sys_exec %s\n", name);
	return exec(name);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_spawn(uint64 va)
{
	// TODO: your job is to complete the sys call
	//Declare a buffer for the filename, a pointer to the current
	//(parent) process, and a null pointer for the new (child) process.
	char filename[128];
	struct proc *p = curr_proc();
	struct proc *np; 

	// Copy the filename string from the user virtual address (va)
	// into our kernel-side filename buffer using copyinstr.
	if (copyinstr(p->pagetable, filename, va, 128) < 0){
		return -1;
	}

	// Resolve the filename to an internal program ID using 
	// get_id_by_name. Returns -1 if no matching program is found
	int ID = get_id_by_name(filename);

	if (ID<0){
		return -1;
	}

	// Allocate a new proc struct from the pool and set its parent to the current process.
	np = allocproc();

	if (np == 0){
		return -1;
	}
	np->parent = p;

	//Load the program (by ID) into the new process's address
	// space using loader(). This sets up its page table, stack, and entry point.
	if(loader(ID, np) < 0)
		return -1;

	// Add the new process to the scheduler's task pool so it
	//becomes eligible to run. add_task is defined in proc.c.
	add_task(np);

	//Return the new process's PID to the caller.
	return np->pid;
}

uint64 sys_set_priority(long long prio){
	// Reject priorities below 2 (1 would make pass == BIG_STRIDE, same
	//  as the lowest-priority process, and 0 would be a divide-by-zero).
    if (prio < 2){
        return -1;
    }

	//Get the current process and update both its priority and its
	// derived pass value so the scheduler immediately reflects the change.
    struct proc *p = curr_proc();

    p->priority = prio;

    p->pass = BIG_STRIDE / prio;

    return prio;
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
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	case SYS_mmap:
		ret = sys_mmap((void *)args[0], (unsigned long long)args[1], (int)args[2], (int)args[3], (int)args[4]);
		break;
	case SYS_munmap:
		ret = sys_munmap((void *)args[0], (unsigned long long)args[1]);
		break;
	case SYS_setpriority:
		ret = sys_set_priority(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
