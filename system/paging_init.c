/* paging_init.c - paging initialization and management */

#include <xinu.h>

/* Global variables for paging */
pd_t *kernel_pd = NULL;		/* Kernel page directory (shared by system processes) */
char **ffs_frames = NULL;	/* Array to store FFS frame addresses */
char **pt_frames = NULL;	/* Array to store page table frame addresses */
char **swap_frames = NULL;	/* Array to store swap frame addresses (optional) */
uint32 *ffs_bitmap = NULL;	/* Bitmap for FFS frame allocation */
uint32 *pt_bitmap = NULL;	/* Bitmap for page table frame allocation */
uint32 *swap_bitmap = NULL;	/* Bitmap for swap frame allocation (optional) */
uint32 ffs_free_count = 0;	/* Number of free FFS frames */
uint32 swap_free_count = 0;	/* Number of free swap frames */

/* Frame to physical address conversion macros are in paging.h */

/* Helper function to get a free frame from FFS */
static char *get_ffs_frame_phys(void);
static void free_ffs_frame_phys(char *frame_addr);
static char *get_pt_frame_phys(void);
static void free_pt_frame_phys(char *frame_addr);

/*------------------------------------------------------------------------
 * init_kernel_pd - Initialize kernel page directory
 *------------------------------------------------------------------------
 */
static void init_kernel_pd(void)
{
	uint32 i;
	pd_t *pd;
	pt_t *pt;
	
	/* Allocate a frame for kernel page directory */
	char *pd_addr = get_pt_frame_phys();
	if (pd_addr == NULL) {
		panic("Failed to allocate kernel page directory");
	}
	pd = (pd_t *)pd_addr;
	
	/* Clear the page directory */
	memset(pd, 0, PAGE_SIZE);
	
	/* Map physical memory identity mapped for kernel */
	/* We need to map enough to cover all physical memory */
	/* Map at least 64MB (16 page directory entries) to cover:
	 * - Kernel code/data/bss
	 * - Heap (up to maxheap)
	 * - FFS frames
	 * - Page table frames
	 * - Page directory itself
	 */
	extern void *maxheap;
	uint32 max_phys = (uint32)maxheap + 1;
	/* Map at least 64MB, or more if maxheap is larger */
	uint32 min_map = 64 * 1024 * 1024; /* 64MB minimum */
	if (max_phys > min_map) {
		min_map = max_phys;
	}
	/* Add extra margin for FFS and page table frames */
	min_map += (MAX_FFS_SIZE + MAX_PT_SIZE) * PAGE_SIZE;
	
	uint32 max_pd_entries = (min_map + 0x400000 - 1) / 0x400000; /* 4MB per entry */
	if (max_pd_entries > 1024) max_pd_entries = 1024; /* Limit to 4GB */
	if (max_pd_entries < 16) max_pd_entries = 16; /* At least 64MB */
	
	/* Map physical memory identity mapped for kernel */
	for (i = 0; i < max_pd_entries; i++) {
		/* Allocate a page table for this directory entry */
		char *pt_addr = get_pt_frame_phys();
		if (pt_addr == NULL) {
			panic("Failed to allocate kernel page tables");
		}
		pt = (pt_t *)pt_addr;
		memset(pt, 0, PAGE_SIZE);
		
		/* Map all pages in this page table identity mapped */
		uint32 j;
		for (j = 0; j < 1024; j++) {
			uint32 phys_addr = (i * 1024 + j) * PAGE_SIZE;
			pt[j].pt_pres = 1;
			pt[j].pt_write = 1;
			pt[j].pt_user = 0;	/* Kernel mode */
			pt[j].pt_base = PHYS_TO_FRAME(phys_addr);
		}
		
		/* Set up page directory entry */
		pd[i].pd_pres = 1;
		pd[i].pd_write = 1;
		pd[i].pd_user = 0;
		pd[i].pd_base = PHYS_TO_FRAME((uint32)pt_addr);
	}
	
	kernel_pd = pd;
}

/*------------------------------------------------------------------------
 * get_ffs_frame_phys - Get a free frame from FFS (returns physical address)
 *------------------------------------------------------------------------
 */
static char *get_ffs_frame_phys(void)
{
	intmask mask;
	uint32 i;
	char *frame_addr;
	
	mask = disable();
	
	for (i = 0; i < MAX_FFS_SIZE; i++) {
		if ((ffs_bitmap[i / 32] & (1 << (i % 32))) == 0) {
			ffs_bitmap[i / 32] |= (1 << (i % 32));
			ffs_free_count--;
			frame_addr = ffs_frames[i];
			restore(mask);
			return frame_addr;
		}
	}
	
	restore(mask);
	return NULL;
}

/*------------------------------------------------------------------------
 * free_ffs_frame_phys - Free an FFS frame
 *------------------------------------------------------------------------
 */
static void free_ffs_frame_phys(char *frame_addr)
{
	intmask mask;
	uint32 i;
	
	mask = disable();
	
	for (i = 0; i < MAX_FFS_SIZE; i++) {
		if (ffs_frames[i] == frame_addr) {
			ffs_bitmap[i / 32] &= ~(1 << (i % 32));
			ffs_free_count++;
			restore(mask);
			return;
		}
	}
	
	restore(mask);
}

/*------------------------------------------------------------------------
 * get_pt_frame_phys - Get a free frame for page tables (returns physical address)
 *------------------------------------------------------------------------
 */
static char *get_pt_frame_phys(void)
{
	intmask mask;
	uint32 i;
	char *frame_addr;
	
	mask = disable();
	
	for (i = 0; i < MAX_PT_SIZE; i++) {
		if ((pt_bitmap[i / 32] & (1 << (i % 32))) == 0) {
			pt_bitmap[i / 32] |= (1 << (i % 32));
			frame_addr = pt_frames[i];
			restore(mask);
			return frame_addr;
		}
	}
	
	restore(mask);
	return NULL;
}

/*------------------------------------------------------------------------
 * free_pt_frame_phys - Free a page table frame
 *------------------------------------------------------------------------
 */
static void free_pt_frame_phys(char *frame_addr)
{
	intmask mask;
	uint32 i;
	
	mask = disable();
	
	for (i = 0; i < MAX_PT_SIZE; i++) {
		if (pt_frames[i] == frame_addr) {
			pt_bitmap[i / 32] &= ~(1 << (i % 32));
			restore(mask);
			return;
		}
	}
	
	restore(mask);
}

/*------------------------------------------------------------------------
 * paging_init - Initialize paging system
 *------------------------------------------------------------------------
 */
void paging_init(void)
{
	uint32 i;
	char *frame_addr;
	
	/* Allocate arrays to store frame addresses */
	ffs_frames = (char **)getmem(MAX_FFS_SIZE * sizeof(char *));
	pt_frames = (char **)getmem(MAX_PT_SIZE * sizeof(char *));
	swap_frames = (char **)getmem(MAX_SWAP_SIZE * sizeof(char *));
	
	if (ffs_frames == NULL || pt_frames == NULL || swap_frames == NULL) {
		panic("Failed to allocate frame arrays");
	}
	
	/* Allocate bitmaps */
	ffs_bitmap = (uint32 *)getmem((MAX_FFS_SIZE + 31) / 32 * sizeof(uint32));
	pt_bitmap = (uint32 *)getmem((MAX_PT_SIZE + 31) / 32 * sizeof(uint32));
	swap_bitmap = (uint32 *)getmem((MAX_SWAP_SIZE + 31) / 32 * sizeof(uint32));
	
	if (ffs_bitmap == NULL || pt_bitmap == NULL || swap_bitmap == NULL) {
		panic("Failed to allocate paging bitmaps");
	}
	
	/* Initialize bitmaps - all frames free initially */
	memset(ffs_bitmap, 0, (MAX_FFS_SIZE + 31) / 32 * sizeof(uint32));
	memset(pt_bitmap, 0, (MAX_PT_SIZE + 31) / 32 * sizeof(uint32));
	memset(swap_bitmap, 0, (MAX_SWAP_SIZE + 31) / 32 * sizeof(uint32));
	
	ffs_free_count = MAX_FFS_SIZE;
	swap_free_count = MAX_SWAP_SIZE;
	
	/* Pre-allocate frames for page tables from memory pool */
	for (i = 0; i < MAX_PT_SIZE; i++) {
		frame_addr = (char *)getmem(PAGE_SIZE);
		if (frame_addr == (char *)SYSERR) {
			panic("Failed to allocate page table frames");
		}
		/* Round to page boundary */
		frame_addr = (char *)(((uint32)frame_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
		pt_frames[i] = frame_addr;
		/* Frame is pre-allocated but not yet used, so don't mark in bitmap */
	}
	
	/* Pre-allocate frames for FFS from memory pool */
	for (i = 0; i < MAX_FFS_SIZE; i++) {
		frame_addr = (char *)getmem(PAGE_SIZE);
		if (frame_addr == (char *)SYSERR) {
			panic("Failed to allocate FFS frames");
		}
		frame_addr = (char *)(((uint32)frame_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
		ffs_frames[i] = frame_addr;
		/* Frame is pre-allocated but not yet used */
	}
	
	/* Initialize kernel page directory */
	init_kernel_pd();
	
	/* Load kernel page directory into CR3 */
	/* CR3 needs physical address, but kernel_pd is a virtual address */
	/* Since kernel has identity mapping for first 32MB, virtual = physical */
	write_cr3((unsigned long)kernel_pd);
	
	/* Enable paging */
	enable_paging();
}

/* Export functions for use by other modules */
char *get_ffs_frame_export(void) { return get_ffs_frame_phys(); }
void free_ffs_frame_export(char *frame) { free_ffs_frame_phys(frame); }
char *get_pt_frame_export(void) { return get_pt_frame_phys(); }
void free_pt_frame_export(char *frame) { free_pt_frame_phys(frame); }

