#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "stat.h"
#include "file.h"

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
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
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_spawn(uint64 va)
{
    struct proc *p = curr_proc();
    char name[MAX_STR_LEN];
    copyinstr(p->pagetable, name, va, MAX_STR_LEN);

    struct inode *ip = namei(name);
    if (ip == 0)
        return -1;

    struct proc *np = allocproc();
    if (np == 0) {
        iput(ip);
        return -1;
    }
    np->parent = p;

    // Inherit open file descriptors from parent.
    for (int i = 0; i < FD_BUFFER_SIZE; i++) {
        if (p->files[i]) {
            p->files[i]->ref++;
            np->files[i] = p->files[i];
        }
    }

    bin_loader(ip, np);
    iput(ip);

    char *argv[2];
    argv[0] = name;
    argv[1] = NULL;
    np->trapframe->a0 = push_argv(np, argv);

    add_task(np);
    return np->pid;
}

uint64 sys_set_priority(long long prio)
{
	// TODO: your job is to complete the sys call
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


uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

// sys_fstat — syscall handler for fstat(2).
// Looks up the open file referenced by `fd` in the current process's file table
// and copies its metadata into the user-space Stat struct at address `stat`.
// Returns 0 on success, -1 on any error.
int sys_fstat(int fd, uint64 stat)
{
    // Reject out-of-range descriptors before indexing the file table array.
    if (fd < 0 || fd >= FD_BUFFER_SIZE)
        return -1;

    struct proc *p = curr_proc();
    struct file *f = p->files[fd]; // Grab the file pointer from the process's
                                   //   open-file table at index fd

    // A NULL entry means the descriptor is not currently open.
    if (f == NULL)
        return -1;

    // Delegate to filestat(), which validates that the file is inode-backed,
    // populates a Stat struct, and copies it to user space.
    // copyout address validity is checked inside filestat, so no extra check needed here.
    return filestat(f, stat);
}

// sys_linkat — syscall handler for linkat(2) / link(2).
// Creates a new hard link at `newpath` pointing to the same inode as `oldpath`,
// effectively giving the file a second name in the filesystem.
// olddirfd, newdirfd, and flags are POSIX-compatibility parameters that this
// simplified single-root filesystem does not use; they are intentionally ignored.
// Returns 0 on success, -1 on error.
int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd,
               uint64 newpath, uint64 flags)
{
    // olddirfd / newdirfd / flags are compatibility-only — ignore them.
    struct proc *p = curr_proc();
    char old[MAXPATH], new[MAXPATH];

    // Copy both path strings from user space into kernel buffers.
    // copyinstr null-terminates and enforces the MAXPATH length limit.
    copyinstr(p->pagetable, old, oldpath, MAXPATH);
    copyinstr(p->pagetable, new, newpath, MAXPATH);

    // Linking a path to itself would create a duplicate directory entry for
    // the same name, which is nonsensical and potentially dangerous.
    if (strncmp(old, new, MAXPATH) == 0)
        return -1;

    // Resolve the source path to its inode; fail if it doesn't exist.
    struct inode *ip = namei(old);
    if (ip == 0)
        return -1;

    ivalid(ip); // Populate the in-memory inode cache from disk before
                //   reading or modifying any of its fields

    // All files live under the single root directory in this filesystem.
    struct inode *dp = root_dir();

    // Write a new directory entry (new → ip->inum) into the root directory.
    // If the name is already taken or the directory is full, dirlink returns -1.
    if (dirlink(dp, new, ip->inum) < 0) {
        iput(dp); // Release the directory inode reference before returning
        iput(ip); // Release the file inode reference before returning
        return -1;
    }

    // The new directory entry is now on disk, so the inode has one more
    // hard link. Increment the counter and persist it with iupdate.
    ip->nlink++;
    iupdate(ip); // Write the updated nlink field back to the on-disk dinode

    iput(dp); // Drop the root directory reference acquired above
    iput(ip); // Drop the file inode reference; file still exists (nlink >= 1)
    return 0;
}

// sys_unlinkat — syscall handler for unlinkat(2) / unlink(2).
// Removes the directory entry for `name`, decrementing the inode's hard-link
// count. When nlink reaches 0 and no file descriptors remain open, the inode
// and its data blocks are freed automatically by iput.
// dirfd and flags are POSIX-compatibility parameters ignored by this
// simplified single-root filesystem.
// Returns 0 on success, -1 on error.
int sys_unlinkat(int dirfd, uint64 name, uint64 flags)
{
    // dirfd / flags are compatibility-only — ignore them.
    struct proc *p = curr_proc();
    char path[MAXPATH];

    // Copy the target path from user space into a kernel buffer.
    copyinstr(p->pagetable, path, name, MAXPATH);

    // Grab the root directory inode; all lookups and entry removals happen here.
    struct inode *dp = root_dir();
    ivalid(dp); // Ensure the directory's in-memory cache is valid before use

    // Resolve the path to its inode *before* removing the directory entry.
    // We need the inode pointer so we can decrement nlink after the unlink.
    struct inode *ip = namei(path);
    if (ip == 0) {
        iput(dp); // Path does not exist; clean up and bail
        return -1;
    }
    ivalid(ip); // Populate the in-memory inode cache before reading nlink

    // Zero-out the directory entry in the root directory, freeing the name slot.
    // dirunlink also calls iput(ip) internally, but we hold a *separate* reference
    // obtained by our namei call above, so the inode is not freed yet.
    if (dirunlink(dp, path) < 0) {
        iput(dp);
        iput(ip);
        return -1;
    }

    // The directory entry is gone, so reduce the hard-link count and
    // write the change to disk immediately.
    ip->nlink--;
    iupdate(ip); // Persist the decremented nlink to the on-disk dinode

    iput(dp); // Drop the root directory reference

    // Drop our reference to the file's inode. If nlink == 0 AND this is the
    // last open reference (ref == 1), iput will call itrunc + iupdate to free
    // the data blocks and mark the inode slot as available on disk.
    iput(ip);
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
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
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
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
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
