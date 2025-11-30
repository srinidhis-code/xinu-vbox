/* pagefault_handler.c - page fault handler */

#include <xinu.h>

/* External declarations for virtual heap tracking */
extern struct vheap_block vheap_blocks[NPROC][MAX_VHEAP_BLOCKS];
extern uint32 vheap_block_count[NPROC];

/*------------------------------------------------------------------------
 * pagefault_handler - Handle page faults (interrupt 14)
 *------------------------------------------------------------------------
 */
void pagefault_handler(void)
{
	uint32 fault_addr;
	pid32 pid;
	struct procent *prptr;
	pd_t *pd;
	pt_t *pt;
	virt_addr_t *va;
	uint32 pd_idx, pt_idx;
	char *ffs_frame_addr;
	uint32 ffs_frame_num;
	
	/* Get fault address from CR2 */
	fault_addr = read_cr2();
	
	/* Note: Error code is popped by pagefault_handler_disp.S */
	
	/* CRITICAL: currpid should be correct because the page fault occurs in the context */
	/* of the process whose page directory is loaded in CR3. However, we need to ensure */
	/* that currpid is valid and that the process table entry is valid. */
	pid = currpid;
	
	/* Safety check: ensure pid is valid */
	if (isbadpid(pid) || pid >= NPROC) {
		/* Invalid pid - this is a serious error */
		kprintf("Page fault handler: invalid currpid %d\n", pid);
		panic("Invalid currpid in page fault handler");
	}
	
	prptr = &proctab[pid];
	
	/* Check if this is a user process */
	if (!prptr->prisuser) {
		/* System process - page faults should not occur on kernel memory */
		/* If they do, it means the kernel page directory is not mapping
		 * the required memory. This is a serious error. */
		extern pd_t *kernel_pd;
		if (kernel_pd == NULL) {
			panic("Kernel page directory not initialized");
		}
		
		/* Check if this is a valid kernel address that should be mapped */
		/* Kernel memory is typically below 0xC0000000 */
		if (fault_addr < 0xC0000000) {
			/* This is kernel memory - it should be mapped */
			/* This indicates a bug in kernel PD initialization */
			kprintf("System process page fault on kernel memory at 0x%x\n", fault_addr);
			panic("Kernel page directory mapping error");
		} else {
			/* Accessing user space from system process - error */
			kprintf("P%d:: SEGMENTATION_FAULT (system process accessing user space at 0x%x)\n", pid, fault_addr);
			/* Don't kill null process - it's protected */
			if (pid != NULLPROC) {
				kill(pid);
			}
		}
		return;
	}
	
	/* User process - check if fault address is in virtual heap range */
	/* Virtual heap starts at 0x02000000 (32MB) */
	if (fault_addr < (uint32)prptr->prvheap) {
		/* Access outside virtual heap - segmentation fault */
		kprintf("P%d:: SEGMENTATION_FAULT\n", pid);
		kill(pid);
		return;
	}
	
	/* Parse virtual address */
	va = (virt_addr_t *)&fault_addr;
	pd_idx = va->pd_offset;
	pt_idx = va->pt_offset;
	
	/* CRITICAL: Get the page directory pointer from the process table */
	/* The page directory is stored as a virtual address, but we need to access it */
	/* through kernel identity mapping (first 32MB). Since user process page directories */
	/* are allocated from the PT frame pool (which is in first 32MB), they are identity mapped. */
	pd = prptr->prpd;
	if (pd == NULL) {
		kprintf("P%d:: SEGMENTATION_FAULT (no page directory)\n", pid);
		kill(pid);
		return;
	}
	
	/* Safety check: ensure pd_idx is valid */
	if (pd_idx >= 1024) {
		kprintf("P%d:: SEGMENTATION_FAULT (invalid pd_idx %d)\n", pid, pd_idx);
		kill(pid);
		return;
	}
	
	/* Save original CR3; we may switch to kernel PD to touch high frames */
	extern pd_t *kernel_pd;
	uint32 saved_cr3 = read_cr3();
	bool8 switched_to_kernel = FALSE;
	if ((uint32)pd >= 0x02000000) {
		write_cr3((uint32)kernel_pd);
		switched_to_kernel = TRUE;
	}
	
	/* Check if page directory entry exists */
	/* Access pd[pd_idx] - now accessible via kernel PD if pd is above 32MB */
	if (!pd[pd_idx].pd_pres) {
		/* Need to allocate a page table */
		char *pt_addr = get_pt_frame_export();
		if (pt_addr == NULL) {
			kprintf("P%d:: SEGMENTATION_FAULT\n", pid);
			kill(pid);
			goto out_restore;
		}
		
		pt = (pt_t *)pt_addr;
		
		/* Ensure we are on kernel PD if the PT frame is above 32MB */
		if (!switched_to_kernel && (uint32)pt >= 0x02000000) {
			write_cr3((uint32)kernel_pd);
			switched_to_kernel = TRUE;
		}
		
		/* Clear the page table */
		memset(pt, 0, PAGE_SIZE);
		
		/* Convert virtual address to physical frame number for page directory entry */
		uint32 pt_frame_num = PHYS_TO_FRAME((uint32)pt_addr);
		
		pd[pd_idx].pd_pres = 1;
		pd[pd_idx].pd_write = 1;
		pd[pd_idx].pd_user = 1;
		pd[pd_idx].pd_base = pt_frame_num;
		
		/* Memory barrier to ensure PDE is written before we use the page table */
		asm volatile("" ::: "memory");
		
		/* Force a read to ensure the PDE is committed */
		volatile uint32 dummy_pde = *(volatile uint32 *)&pd[pd_idx];
		(void)dummy_pde;
	} else {
		/* PDE is present - could be identity-mapped (kernel) or user heap page table */
		/* Check if this is an identity-mapped entry (kernel page table) */
		/* Identity-mapped entries have pd_user = 0, user heap entries have pd_user = 1 */
		if (pd[pd_idx].pd_user == 0) {
			/* This is an identity-mapped entry - we need to allocate a new page table for virtual heap */
			/* The identity-mapped entry points to a kernel page table that maps physical addresses 1:1 */
			/* We need a separate page table for the virtual heap that maps virtual addresses to FFS frames */
			char *pt_addr = get_pt_frame_export();
			if (pt_addr == NULL) {
				kprintf("P%d:: SEGMENTATION_FAULT\n", pid);
				kill(pid);
				goto out_restore;
			}
			
			pt = (pt_t *)pt_addr;
			
			/* Ensure we are on kernel PD if the PT frame is above 32MB */
			if (!switched_to_kernel && (uint32)pt >= 0x02000000) {
				write_cr3((uint32)kernel_pd);
				switched_to_kernel = TRUE;
			}
			
			/* Clear the page table */
			memset(pt, 0, PAGE_SIZE);
			
			/* Convert virtual address to physical frame number for page directory entry */
			uint32 pt_frame_num = PHYS_TO_FRAME((uint32)pt_addr);
			
			pd[pd_idx].pd_pres = 1;
			pd[pd_idx].pd_write = 1;
			pd[pd_idx].pd_user = 1;  /* Mark as user heap page table */
			pd[pd_idx].pd_base = pt_frame_num;
			
			/* Memory barrier to ensure PDE is written */
			asm volatile("" ::: "memory");
			
			/* Force a read to ensure the PDE is committed */
			volatile uint32 dummy_pde = *(volatile uint32 *)&pd[pd_idx];
			(void)dummy_pde;
			
			/* Don't switch back from PT CR3 here - we'll switch back after we're done with PD access */
			/* If we switched for both PT and PD, we'll switch back once at the end */
		} else {
			/* This is already a user heap page table - use it */
			uint32 pt_frame_num = pd[pd_idx].pd_base;
			
			/* Safety check: ensure page table frame number is valid (non-zero) */
			if (pt_frame_num == 0) {
				kprintf("P%d:: SEGMENTATION_FAULT (invalid page table frame 0)\n", pid);
				kill(pid);
				goto out_restore;
			}
			
			char *pt_phys = FRAME_TO_PHYS(pt_frame_num);
			if (!switched_to_kernel && (uint32)pt_phys >= 0x02000000) {
				write_cr3((uint32)kernel_pd);
				switched_to_kernel = TRUE;
			}
			pt = (pt_t *)pt_phys;
		}
	}
	
	/* Don't switch back from PD CR3 yet - we might need kernel PD for PT access too */
	/* We'll switch back at the end after all operations are complete */
	
	/* Safety check: ensure page table pointer is valid */
	if (pt == NULL) {
		kprintf("P%d:: SEGMENTATION_FAULT (null page table)\n", pid);
		kill(pid);
		goto out_restore;
	}
	
	/* Safety check: ensure pt_idx is valid */
	if (pt_idx >= 1024) {
		kprintf("P%d:: SEGMENTATION_FAULT (invalid pt_idx %d)\n", pid, pt_idx);
		kill(pid);
		goto out_restore;
	}
	
	/* Check if page table entry exists and is allocated */
	if (!pt[pt_idx].pt_pres) {
		/* Check if this page was allocated via vmalloc */
		bool8 is_allocated = FALSE;
		uint32 j;
		
		/* Safety check: ensure pid is valid and within bounds */
		if (isbadpid(pid) || pid >= NPROC) {
			kprintf("P%d:: SEGMENTATION_FAULT (invalid pid)\n", pid);
			kill(pid);
			goto out_restore;
		}
		
		/* Safety check: ensure block count is within bounds */
		if (vheap_block_count[pid] > MAX_VHEAP_BLOCKS) {
			kprintf("P%d:: SEGMENTATION_FAULT (invalid block count)\n", pid);
			kill(pid);
			goto out_restore;
		}
		
		for (j = 0; j < vheap_block_count[pid]; j++) {
			if (vheap_blocks[pid][j].allocated) {
				char *block_start = vheap_blocks[pid][j].start;
				char *block_end = block_start + vheap_blocks[pid][j].npages * PAGE_SIZE;
				
				if (fault_addr >= (uint32)block_start && fault_addr < (uint32)block_end) {
					is_allocated = TRUE;
					break;
				}
			}
		}
		
		if (!is_allocated) {
			/* Page not allocated - segmentation fault */
			kprintf("P%d:: SEGMENTATION_FAULT\n", pid);
			kill(pid);
			goto out_restore;
		}
		
		/* Allocate an FFS frame */
		ffs_frame_addr = get_ffs_frame_export();
		if (ffs_frame_addr == NULL) {
			/* No free FFS frames - could implement swapping here */
			kprintf("P%d:: SEGMENTATION_FAULT\n", pid);
			kill(pid);
			goto out_restore;
		}
		
		/* Map the page */
		/* Convert virtual address to physical frame number for page table entry */
		ffs_frame_num = PHYS_TO_FRAME((uint32)ffs_frame_addr);
		
		/* Clear the page; switch to kernel PD if FFS frame is above 32MB */
		if (!switched_to_kernel && (uint32)ffs_frame_addr >= 0x02000000) {
			write_cr3((uint32)kernel_pd);
			switched_to_kernel = TRUE;
		}
		memset(ffs_frame_addr, 0, PAGE_SIZE);
		
		/* Now set up the page table entry */
		/* Set all fields to ensure the entry is correct */
		/* Match the pattern used in paging_init.c */
		pt[pt_idx].pt_pres = 1;
		pt[pt_idx].pt_write = 1;
		pt[pt_idx].pt_user = 1;
		pt[pt_idx].pt_pwt = 0;
		pt[pt_idx].pt_pcd = 0;
		pt[pt_idx].pt_acc = 0;
		pt[pt_idx].pt_dirty = 0;
		pt[pt_idx].pt_mbz = 0;
		pt[pt_idx].pt_global = 0;
		pt[pt_idx].pt_avail = 0;
		pt[pt_idx].pt_base = ffs_frame_num;
		
		/* Memory barrier to ensure page table entry is fully written */
		asm volatile("" ::: "memory");
		
		/* Force a read to ensure the write is committed */
		volatile uint32 dummy = *(volatile uint32 *)&pt[pt_idx];
		(void)dummy;  /* Suppress unused variable warning */
		
		/* Update process statistics */
		prptr->prffsframes++;
		
		/* Critical: Ensure all writes to the page table entry are visible */
		/* before invalidating the TLB. The CPU must see the complete PTE. */
		asm volatile("" ::: "memory");
		
		/* Invalidate TLB entry for the faulting address */
		/* This ensures the CPU sees the new page table entry */
		/* Use volatile to ensure the instruction is not optimized away */
		/* The invlpg instruction invalidates a single TLB entry */
		asm volatile("invlpg (%0)" : : "r" (fault_addr) : "memory");
		
		/* Additional memory barrier after TLB invalidation */
		/* This ensures the TLB invalidation is complete before we return */
		asm volatile("" ::: "memory");
		
	} else {
		/* Page is present but fault occurred - might be protection fault */
		/* For now, treat as segmentation fault */
		kprintf("P%d:: SEGMENTATION_FAULT\n", pid);
		kill(pid);
		goto out_restore;
	}
	
out_restore:
	if (switched_to_kernel) {
		write_cr3(saved_cr3);
	}
}
