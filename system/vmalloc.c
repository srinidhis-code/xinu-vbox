/* vmalloc.c - vmalloc */

#include <xinu.h>

/* Virtual heap allocation tracking - structure defined in paging.h */
struct vheap_block vheap_blocks[NPROC][MAX_VHEAP_BLOCKS];
uint32 vheap_block_count[NPROC];

/*------------------------------------------------------------------------
 * vmalloc - Allocate virtual heap space
 *------------------------------------------------------------------------
 */
char *vmalloc(uint32 nbytes)
{
	intmask mask;
	pid32 pid;
	struct procent *prptr;
	uint32 npages;
	char *start_addr;
	uint32 i;
	
	mask = disable();
	
	pid = currpid;
	prptr = &proctab[pid];
	
	/* Check if this is a user process */
	if (!prptr->prisuser) {
		restore(mask);
		return (char *)SYSERR;
	}
	
	/* Round up to page size */
	npages = (nbytes + PAGE_SIZE - 1) / PAGE_SIZE;
	
	/* First-fit allocation for ECE-565 */
	/* Start from the beginning of virtual heap and find first free region */
	start_addr = prptr->prvheap;
	bool8 found = FALSE;
	char *heap_end = prptr->prvheap + 0x20000000; /* 512MB limit */
	
	/* Try candidate positions starting from heap start */
	while (!found && (uint32)start_addr + npages * PAGE_SIZE <= (uint32)heap_end) {
		char *candidate_end = start_addr + npages * PAGE_SIZE;
		bool8 overlaps = FALSE;
		
		/* Check if this candidate overlaps with any allocated block */
		for (i = 0; i < vheap_block_count[pid]; i++) {
			if (vheap_blocks[pid][i].allocated) {
				char *block_start = vheap_blocks[pid][i].start;
				char *block_end = block_start + vheap_blocks[pid][i].npages * PAGE_SIZE;
				
				/* Check for overlap: candidate overlaps if it's not completely before or after */
				if (!(candidate_end <= block_start || start_addr >= block_end)) {
					overlaps = TRUE;
					/* Move candidate to after this block */
					start_addr = block_end;
					break;
				}
			}
		}
		
		if (!overlaps) {
			found = TRUE;
			break;
		}
	}
	
	/* Check if we found a suitable location */
	if (!found) {
		restore(mask);
		return (char *)SYSERR;
	}
	
	/* Find a free block entry to reuse */
	/* This prevents overflow by reusing freed slots */
	for (i = 0; i < vheap_block_count[pid]; i++) {
		if (!vheap_blocks[pid][i].allocated) {
			break;  /* Found a free slot to reuse */
		}
	}
	
	/* If no free slot in the used portion, add new */
	if (i == vheap_block_count[pid]) {
		if (vheap_block_count[pid] >= MAX_VHEAP_BLOCKS) {
			restore(mask);
			return (char *)SYSERR;
		}
		vheap_block_count[pid]++;
	}
	
	/* Record allocation in the slot (either reused or new) */
	vheap_blocks[pid][i].start = start_addr;
	vheap_blocks[pid][i].npages = npages;
	vheap_blocks[pid][i].allocated = TRUE;
	
	/* Update process statistics */
	prptr->prvpages += npages;
	
	restore(mask);
	return start_addr;
}

