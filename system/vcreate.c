/* vcreate.c - vcreate */

#include <xinu.h>

extern char *get_pt_frame_export(void);

/* Virtual heap base address - 32MB (entry 8 in page directory) */
#define VHEAP_BASE 0x02000000

/*------------------------------------------------------------------------
 * vcreate - Create a user process with virtual heap
 *------------------------------------------------------------------------
 */
pid32 vcreate(
	void *funcaddr,
	uint32 ssize,
	pri16 priority,
	char *name,
	uint32 nargs,
	...
)
{
	intmask mask;
	pid32 pid;
	struct procent *prptr;
	pd_t *pd;
	uint32 i;
	
	mask = disable();
	
	/* Get a new process ID */
	pid = newpid();
	if (pid == SYSERR) {
		restore(mask);
		return SYSERR;
	}
	
	/* Allocate stack using getstk (shared heap) */
	if (ssize < MINSTK)
		ssize = MINSTK;
	ssize = (uint32) roundmb(ssize);
	
	char *saddr = (char *)getstk(ssize);
	if (saddr == (char *)SYSERR) {
		restore(mask);
		return SYSERR;
	}
	
	prcount++;
	prptr = &proctab[pid];
	
	/* Initialize process table entry */
	prptr->prstate = PR_SUSP;
	prptr->prprio = priority;
	prptr->prstkbase = saddr;
	prptr->prstklen = ssize;
	prptr->prname[PNMLEN-1] = NULLCH;
	int32 k;
	for (k = 0; k < PNMLEN-1 && (prptr->prname[k] = name[k]) != NULLCH; k++)
		;
	prptr->prsem = -1;
	prptr->prparent = (pid32)getpid();
	prptr->prhasmsg = FALSE;
	prptr->prdesc[0] = CONSOLE;
	prptr->prdesc[1] = CONSOLE;
	prptr->prdesc[2] = CONSOLE;
	
	/* Initialize paging fields */
	prptr->prisuser = TRUE;
	prptr->prvheap = (char *)VHEAP_BASE;
	prptr->prvheapnext = (char *)VHEAP_BASE;
	/* Initialize with XINU_PAGES (8192) pre-allocated virtual pages */
	prptr->prvpages = XINU_PAGES;
	prptr->prffsframes = 0;
	
	/* Initialize virtual heap block tracking for this process */
	extern uint32 vheap_block_count[NPROC];
	vheap_block_count[pid] = 0;
	
	/* Allocate a page directory for this process */
	char *pd_addr = get_pt_frame_export();
	if (pd_addr == NULL) {
		freestk(saddr, ssize);
		prcount--;
		prptr->prstate = PR_FREE;
		restore(mask);
		return SYSERR;
	}
	
	pd = (pd_t *)pd_addr;
	
	/* CRITICAL: Switch to kernel PD if page directory is above 32MB before accessing it */
	/* Page directories are allocated from PT frame pool, which may be above 32MB */
	extern pd_t *kernel_pd;
	uint32 saved_cr3_pd = read_cr3();
	bool8 switched_cr3_pd = FALSE;
	
	if ((uint32)pd >= 0x02000000) {
		write_cr3((uint32)kernel_pd);
		switched_cr3_pd = TRUE;
	}
	
	/* Clear the page directory - now accessible via kernel PD if above 32MB */
	memset(pd, 0, PAGE_SIZE);
	prptr->prpd = pd;
	
	/* Copy kernel mappings for first 32MB (entries 0-7) */
	/* This maps the kernel code, data, and shared heap */
	/* Virtual heap starts at entry 8 (0x02000000 = 32MB) */
	/* We only copy entries 0-7 because entries 8+ are for the virtual heap */
	/* The page fault handler switches to kernel PD to access FFS/PT frames above 32MB */
	/* so the user PD doesn't need identity-mapped entries beyond entry 7 */
	extern pd_t *kernel_pd;
	
	/* Copy kernel PDEs 0-7 (first 32MB) */
	for (i = 0; i < 8; i++) {
		if (kernel_pd[i].pd_pres) {
			/* Copy the page directory entry */
			pd[i] = kernel_pd[i];
			/* Note: We're sharing page tables with kernel for identity-mapped region */
		}
	}
	
	/* CRITICAL: Clear entries 8+ for the virtual heap */
	/* This ensures virtual heap accesses trigger page faults for lazy allocation */
	/* The page fault handler will allocate new page tables for the virtual heap */
	/* The page fault handler switches to kernel PD to access FFS/PT frames, */
	/* so it doesn't need identity-mapped entries in the user PD */
	for (i = 8; i < 1024; i++) {
		pd[i].pd_pres = 0;
		pd[i].pd_write = 0;
		pd[i].pd_user = 0;
		pd[i].pd_base = 0;
	}
	
	/* Switch back to original CR3 if we switched for page directory access */
	if (switched_cr3_pd) {
		write_cr3(saved_cr3_pd);
	}
	
	/* Initialize stack - match create.c exactly */
	/* Use uint32 pointer for proper 4-byte decrements */
	uint32 *saddr_uint = (uint32 *)saddr;
	uint32 *a;
	uint32 *pushsp;
	uint32 savsp;
	
	*saddr_uint = STACKMAGIC;
	savsp = (uint32)saddr_uint;

	/* Push arguments */
	a = (uint32 *)(&nargs + 1);	/* Start of args		*/
	a += nargs -1;			/* Last argument		*/
	for ( ; nargs > 0 ; nargs--)	/* Machine dependent; copy args	*/
		*--saddr_uint = *a--;	/* onto created process's stack	*/
	*--saddr_uint = (long)INITRET;	/* Push on return address	*/

	/* The following entries on the stack must match what ctxsw	*/
	/*   expects a saved process state to contain: ret address,	*/
	/*   ebp, interrupt mask, flags, registers, and an old SP	*/

	*--saddr_uint = (long)funcaddr;	/* Make the stack look like it's*/
					/*   half-way through a call to	*/
					/*   ctxsw that "returns" to the*/
					/*   new process		*/
	*--saddr_uint = savsp;		/* This will be register ebp	*/
					/*   for process exit		*/
	savsp = (uint32) saddr_uint;		/* Start of frame for ctxsw	*/
	*--saddr_uint = 0x00000200;		/* New process runs with	*/
					/*   interrupts enabled		*/

	/* Basically, the following emulates an x86 "pushal" instruction*/

	*--saddr_uint = 0;			/* %eax */
	*--saddr_uint = 0;			/* %ecx */
	*--saddr_uint = 0;			/* %edx */
	*--saddr_uint = 0;			/* %ebx */
	*--saddr_uint = 0;			/* %esp; value filled in below	*/
	pushsp = saddr_uint;			/* Remember this location	*/
	*--saddr_uint = savsp;		/* %ebp (while finishing ctxsw)	*/
	*--saddr_uint = 0;			/* %esi */
	*--saddr_uint = 0;			/* %edi */
	*pushsp = (unsigned long) (prptr->prstkptr = (char *)saddr_uint);
	
	restore(mask);
	return pid;
}

