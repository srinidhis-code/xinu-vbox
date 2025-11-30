/* kill.c - kill */

#include <xinu.h>

/*------------------------------------------------------------------------
 *  kill  -  Kill a process and remove it from the system
 *------------------------------------------------------------------------
 */
syscall	kill(
	  pid32		pid		/* ID of process to kill	*/
	)
{
	intmask	mask;			/* Saved interrupt mask		*/
	struct	procent *prptr;		/* Ptr to process's table entry	*/
	int32	i;			/* Index into descriptors	*/

	mask = disable();
	if (isbadpid(pid) || (pid == NULLPROC)
	    || ((prptr = &proctab[pid])->prstate) == PR_FREE) {
		restore(mask);
		return SYSERR;
	}

	if (--prcount <= 1) {		/* Last user process completes	*/
		xdone();
	}

	/* Clean up paging resources for user processes */
	/* CRITICAL: For current process, we must switch to kernel PD to access */
	/* the page directory (which may be above 32MB) and FFS frames */
	/* But we must be careful about when we call send(), because it may trigger resched() */
	if (prptr->prisuser && prptr->prpd != NULL) {
		extern void free_pt_frame_export(char *);
		extern void free_ffs_frame_export(char *);
		extern pd_t *kernel_pd;
		pd_t *pd = prptr->prpd;
		uint32 j;
		bool8 is_current = (prptr->prstate == PR_CURR);
		
		/* Switch to kernel PD for all cleanup to safely access high frames */
		/* This is safe because the stack is in the identity-mapped region */
		uint32 saved_cr3_kill = read_cr3();
		write_cr3((uint32)kernel_pd);
		
		/* Free page tables (directory entries 8+ for virtual heap) */
		/* Virtual heap starts at 0x02000000 (32MB), which is entry 8 */
		/* CRITICAL: Only free user heap page tables (pd_user == 1) */
		/* Do NOT free identity-mapped kernel page tables (pd_user == 0) */
		/* These are shared with the kernel and must not be freed */
		for (j = 8; j < 1024; j++) {
			/* Only process user heap page tables, skip identity-mapped kernel entries */
			if (pd[j].pd_pres && pd[j].pd_user == 1) {
				uint32 pt_frame = pd[j].pd_base;
				/* Convert frame number to virtual address (identity mapped) */
				char *pt_addr = FRAME_TO_PHYS(pt_frame);
				pt_t *pt = (pt_t *)pt_addr;
				uint32 k;
				
				/* Free FFS frames mapped by this page table */
				for (k = 0; k < 1024; k++) {
					if (pt[k].pt_pres) {
						uint32 ffs_frame = pt[k].pt_base;
						/* Convert frame number to virtual address (identity mapped) */
						char *ffs_addr = FRAME_TO_PHYS(ffs_frame);
						free_ffs_frame_export(ffs_addr);
					}
				}
				
				/* Free the page table frame */
				/* ECE-565: Free page directory and page tables */
				free_pt_frame_export(pt_addr);
			}
		}
		
		/* ECE-565: Free page directory */
		/* For current process, keep the PD frame to avoid freeing while still on its stack */
		if (!is_current) {
			free_pt_frame_export((char *)pd);
			/* Not killing current process - restore original CR3 */
			write_cr3(saved_cr3_kill);
			prptr->prpd = NULL;
		} else {
			/* Stay on kernel PD and keep pd frame; mark pointer NULL so it won't be reused */
			prptr->prpd = NULL;
		}
	}

	/* For non-current processes, send message and free resources now */
	/* For current process, defer send() until after state change to avoid */
	/* triggering resched() while we're still cleaning up */
	if (prptr->prstate != PR_CURR) {
		send(prptr->prparent, pid);
		for (i=0; i<3; i++) {
			close(prptr->prdesc[i]);
		}
		freestk(prptr->prstkbase, prptr->prstklen);
	}

	switch (prptr->prstate) {
	case PR_CURR:
		/* For current process, we're already on kernel PD from the cleanup above */
		/* PD was left allocated intentionally to avoid freeing while on this stack */
		/* Close file descriptors */
		for (i=0; i<3; i++) {
			close(prptr->prdesc[i]);
		}
		/* CRITICAL: Set state to PR_FREE before send() and resched() */
		/* This ensures resched() knows this process is no longer eligible */
		prptr->prstate = PR_FREE;
		/* Now send message to parent - this may trigger resched() */
		/* but we're already in a clean state with kernel PD loaded */
		send(prptr->prparent, pid);
		/* CRITICAL: resched() will never return for this process - it switches */
		/* to another process and this process's stack is never used again */
		/* The stack will be freed later when the process table entry is reused */
		resched();

	case PR_SLEEP:
	case PR_RECTIM:
		unsleep(pid);
		prptr->prstate = PR_FREE;
		break;

	case PR_WAIT:
		semtab[prptr->prsem].scount++;
		/* Fall through */

	case PR_READY:
		getitem(pid);		/* Remove from queue */
		/* Fall through */

	default:
		prptr->prstate = PR_FREE;
	}

	restore(mask);
	return OK;
}
