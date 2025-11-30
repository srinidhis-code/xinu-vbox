/* vfree.c - vfree */

#include <xinu.h>

/* vheap_blocks and vheap_block_count are defined in vmalloc.c */
extern struct vheap_block vheap_blocks[NPROC][MAX_VHEAP_BLOCKS];
extern uint32 vheap_block_count[NPROC];

/*------------------------------------------------------------------------
 * vfree - Free virtual heap space
 *------------------------------------------------------------------------
 */
syscall vfree(char *ptr, uint32 nbytes)
{
	intmask mask;
	pid32 pid;
	struct procent *prptr;
	uint32 npages;
	uint32 i;
	bool8 found = FALSE;
	
	mask = disable();
	
	pid = currpid;
	prptr = &proctab[pid];
	
	/* Check if this is a user process */
	if (!prptr->prisuser) {
		restore(mask);
		return SYSERR;
	}
	
	/* Round up to page size */
	npages = (nbytes + PAGE_SIZE - 1) / PAGE_SIZE;
	
	/* Find the allocation block - ECE-565: must handle incorrect vfree */
	for (i = 0; i < vheap_block_count[pid]; i++) {
		if (vheap_blocks[pid][i].start == ptr && 
		    vheap_blocks[pid][i].npages == npages &&
		    vheap_blocks[pid][i].allocated) {
			found = TRUE;
			break;
		}
	}
	
	/* ECE-565: Check for incorrect vfree - return error if not found */
	if (!found) {
		restore(mask);
		return SYSERR;
	}
	
	/* Mark block as freed */
	vheap_blocks[pid][i].allocated = FALSE;
	
	/* Update process statistics */
	prptr->prvpages -= npages;
	
	/* Unmap and free any FFS frames that back this virtual range */
	/* CRITICAL: Page directory may be above 32MB, so we need to access it via kernel PD */
	pd_t *pd = prptr->prpd;
	if (pd != NULL) {
		/* Switch to kernel PD if needed and restore once at the end */
		extern pd_t *kernel_pd;
		uint32 saved_cr3_pd_vfree = read_cr3();
		bool8 switched_cr3_pd_vfree = FALSE;
		if ((uint32)pd >= 0x02000000) {
			write_cr3((uint32)kernel_pd);
			switched_cr3_pd_vfree = TRUE;
		}
		
		uint32 vaddr = (uint32)ptr;
		uint32 vend = vaddr + npages * PAGE_SIZE;
		virt_addr_t *va;
		uint32 pd_idx, pt_idx;
		pt_t *pt;
		uint32 pt_frame;
		char *pt_phys;
		uint32 ffs_frame;
		char *ffs_addr;
		extern pd_t *kernel_pd;
		uint32 saved_cr3_pt_vfree = 0;
		uint32 kernel_cr3_pt_vfree = (uint32)kernel_pd;
		bool8 switched_cr3_pt_vfree = FALSE;
		
		for (; vaddr < vend; vaddr += PAGE_SIZE) {
			/* Parse virtual address */
			va = (virt_addr_t *)&vaddr;
			pd_idx = va->pd_offset;
			pt_idx = va->pt_offset;
			
			/* Safety check: ensure indices are valid */
			if (pd_idx >= 1024 || pt_idx >= 1024) {
				continue;
			}
			
			/* CRITICAL: When we access pd[pd_idx], we're accessing through the current CR3 */
			/* For user processes, entries 0-7 are copied from kernel PD, which maps the first 32MB */
			/* So accessing pd (which is in first 32MB) should work */
			/* If PDE not present, no mappings here */
			if (!pd[pd_idx].pd_pres) {
				continue;
			}
			
			/* Get the PT from the PDE */
			pt_frame = pd[pd_idx].pd_base;
			
			/* Safety check: ensure page table frame number is valid (non-zero) */
			if (pt_frame == 0) {
				continue;
			}
			
			pt_phys = FRAME_TO_PHYS(pt_frame);
			pt = (pt_t *)pt_phys;
			
			/* If page table is above 32MB, ensure we are on kernel PD */
			if ((uint32)pt >= 0x02000000 && !switched_cr3_pd_vfree) {
				saved_cr3_pt_vfree = read_cr3();
				write_cr3(kernel_cr3_pt_vfree);
				switched_cr3_pt_vfree = TRUE;
			}
			
			/* If PTE present, free the backing FFS frame */
			if (pt[pt_idx].pt_pres) {
				ffs_frame = pt[pt_idx].pt_base;
				
				/* Safety check: ensure FFS frame number is valid */
				if (ffs_frame == 0) {
					/* If we switched CR3 for page table, switch back */
					if (switched_cr3_pt_vfree) {
						write_cr3(saved_cr3_pt_vfree);
					}
					continue;
				}
				
				ffs_addr = FRAME_TO_PHYS(ffs_frame);
				
				/* Ensure we are on kernel PD for high FFS frames */
				if (!switched_cr3_pt_vfree && !switched_cr3_pd_vfree && (uint32)ffs_addr >= 0x02000000) {
					saved_cr3_pt_vfree = read_cr3();
					write_cr3(kernel_cr3_pt_vfree);
					switched_cr3_pt_vfree = TRUE;
				}
				
				/* Return FFS frame to the pool */
				extern void free_ffs_frame_export(char *);
				free_ffs_frame_export(ffs_addr);
				
				/* Clear the PTE - set all fields to 0 */
				pt[pt_idx].pt_pres = 0;
				pt[pt_idx].pt_write = 0;
				pt[pt_idx].pt_user = 0;
				pt[pt_idx].pt_pwt = 0;
				pt[pt_idx].pt_pcd = 0;
				pt[pt_idx].pt_acc = 0;
				pt[pt_idx].pt_dirty = 0;
				pt[pt_idx].pt_mbz = 0;
				pt[pt_idx].pt_global = 0;
				pt[pt_idx].pt_avail = 0;
				pt[pt_idx].pt_base = 0;
				
				/* Memory barrier to ensure PTE is cleared before TLB invalidation */
				asm volatile("" ::: "memory");
				
				/* Update process stats */
				if (prptr->prffsframes > 0) {
					prptr->prffsframes--;
				}
				
				/* Invalidate this TLB entry */
				asm volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
				
				/* Additional memory barrier after TLB invalidation */
				asm volatile("" ::: "memory");
				
				/* If we switched CR3 to access page table, switch back */
				if (switched_cr3_pt_vfree) {
					write_cr3(saved_cr3_pt_vfree);
				}
			} else {
				/* If we switched CR3 to access page table but PTE not present, switch back */
				if (switched_cr3_pt_vfree) {
					write_cr3(saved_cr3_pt_vfree);
				}
			}
		}
		
		/* Switch back to original CR3 if we switched for PD access */
		if (switched_cr3_pd_vfree) {
			write_cr3(saved_cr3_pd_vfree);
		}
	}
	
	restore(mask);
	return OK;
}
