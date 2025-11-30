/* paging_debug.c - debugging functions for paging */

#include <xinu.h>

extern uint32 ffs_free_count;
extern uint32 swap_free_count;

/*------------------------------------------------------------------------
 * free_ffs_pages - Return number of free FFS frames
 *------------------------------------------------------------------------
 */
uint32 free_ffs_pages(void)
{
	return ffs_free_count;
}

/*------------------------------------------------------------------------
 * free_swap_pages - Return number of free swap frames (optional)
 *------------------------------------------------------------------------
 */
uint32 free_swap_pages(void)
{
	return swap_free_count;
}

/*------------------------------------------------------------------------
 * allocated_virtual_pages - Return number of virtual pages allocated by process
 *------------------------------------------------------------------------
 */
uint32 allocated_virtual_pages(pid32 pid)
{
	if (isbadpid(pid)) {
		return 0;
	}
	
	return proctab[pid].prvpages;
}

/*------------------------------------------------------------------------
 * used_ffs_frames - Return number of FFS frames in use by process
 *------------------------------------------------------------------------
 */
uint32 used_ffs_frames(pid32 pid)
{
	if (isbadpid(pid)) {
		return 0;
	}
	
	return proctab[pid].prffsframes;
}

/*------------------------------------------------------------------------
 * dump_frame_info - Dump frame allocator information (for debugging)
 *------------------------------------------------------------------------
 */
void dump_frame_info(void)
{
	extern char **ffs_frames;
	extern char **pt_frames;
	extern uint32 ffs_free_count;
	extern uint32 *ffs_bitmap;
	extern uint32 *pt_bitmap;
	uint32 i;
	uint32 ffs_used = 0;
	uint32 pt_used = 0;
	
	sync_printf("\n=== Frame Allocator Information ===\n");
	
	/* FFS Frame Information */
	sync_printf("\nFFS Frames:\n");
	sync_printf("  Total frames: %d (MAX_FFS_SIZE)\n", MAX_FFS_SIZE);
	sync_printf("  Free frames: %d\n", ffs_free_count);
	
	if (ffs_frames != NULL && ffs_bitmap != NULL) {
		/* Count used frames */
		for (i = 0; i < MAX_FFS_SIZE; i++) {
			if (ffs_bitmap[i / 32] & (1 << (i % 32))) {
				ffs_used++;
			}
		}
		sync_printf("  Used frames: %d\n", ffs_used);
		
		/* Show first few frame addresses */
		sync_printf("  First 5 frame addresses:\n");
		for (i = 0; i < 5 && i < MAX_FFS_SIZE; i++) {
			sync_printf("    FFS[%d] = 0x%08X (physical)\n", i, (uint32)ffs_frames[i]);
		}
		if (MAX_FFS_SIZE > 5) {
			sync_printf("    ... (showing first 5 of %d)\n", MAX_FFS_SIZE);
		}
	}
	
	/* PT Frame Information */
	sync_printf("\nPT Frames:\n");
	sync_printf("  Total frames: %d (MAX_PT_SIZE)\n", MAX_PT_SIZE);
	
	if (pt_frames != NULL && pt_bitmap != NULL) {
		/* Count used frames */
		for (i = 0; i < MAX_PT_SIZE; i++) {
			if (pt_bitmap[i / 32] & (1 << (i % 32))) {
				pt_used++;
			}
		}
		sync_printf("  Used frames: %d\n", pt_used);
		sync_printf("  Free frames: %d\n", MAX_PT_SIZE - pt_used);
		
		/* Show first few frame addresses */
		sync_printf("  First 5 frame addresses:\n");
		for (i = 0; i < 5 && i < MAX_PT_SIZE; i++) {
			sync_printf("    PT[%d] = 0x%08X (physical)\n", i, (uint32)pt_frames[i]);
		}
		if (MAX_PT_SIZE > 5) {
			sync_printf("    ... (showing first 5 of %d)\n", MAX_PT_SIZE);
		}
	}
	
	sync_printf("\nIdentity Mapping:\n");
	sync_printf("  All frames are identity-mapped (physical = virtual for kernel)\n");
	sync_printf("  Kernel page directory maps first 32MB+ identity-mapped\n");
	sync_printf("  Frame addresses shown above are physical addresses\n");
	sync_printf("  Kernel can access them using the same address (identity mapping)\n");
	
	sync_printf("\n=== End Frame Allocator Information ===\n\n");
}


