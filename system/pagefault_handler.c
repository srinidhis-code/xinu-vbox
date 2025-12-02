#include <xinu.h>
#include <paging.h>

/* Check if vaddr lies inside some allocated virtual heap region */
static bool8 vaddr_in_allocated_region(struct procent *prptr,
                                       unsigned long vaddr)
{
    struct vmem_region *r;

    for (r = prptr->vmem.regions; r != NULL; r = r->next) {
        if (r->allocated &&
            vaddr >= r->start_addr &&
            vaddr <  (r->start_addr + r->size)) {
            return TRUE;
        }
    }
    return FALSE;
}

/*-------------------------------------------------------------------------
 * pagefault_handler  -  High-level C handler for page faults (ISR 14)
 *
 *  - If fault is on a valid vmalloc'ed page: allocate an FFS frame and map it
 *  - Otherwise: treat as fatal (segfault or OOM) and kill(currpid)
 *-------------------------------------------------------------------------
 */
void pagefault_handler(void)
{
    unsigned long fault_addr;
    unsigned long vpage;
    struct procent *prptr;
    pd_t *pd;
    pt_t *pte;
    unsigned long frame;

    /* CR2 holds the faulting linear (virtual) address */
    fault_addr = read_cr2();
    vpage      = fault_addr & 0xFFFFF000;   /* page-align */

    prptr = &proctab[currpid];

    /* Kernel processes should not page fault on user heap addresses */
    if (!prptr->user_process) {
        kprintf("Page fault in kernel process %d at 0x%08X\n",
                currpid, (unsigned)fault_addr);
        panic("Kernel page fault");
        return;
    }

    /* Check if fault address is in an allocated user heap region */
    if (!vaddr_in_allocated_region(prptr, vpage)) {
        /* Segmentation fault in user process */
        kprintf("P%d:: SEGMENTATION_FAULT at 0x%08X\n", currpid, (unsigned)fault_addr);
        kill(currpid);
        return;
    }

    /* Valid heap page: perform lazy allocation using FFS */
    frame = ffs_alloc_frame(currpid);
    if ((int)frame == SYSERR || frame == 0) {
        kprintf("P%d:: OUT_OF_MEMORY (addr=0x%08X)\n",
                currpid, (unsigned)fault_addr);
        kill(currpid);
        return;
    }

    /* ffs_alloc_frame() already zeroes the frame */

    /* Use the process's page directory (set during vcreate) */
    pd = (pd_t *)prptr->prpdbr;
    if (pd == NULL || prptr->prpdbr == 0) {
        /* Fallback to system PD (shouldn't happen for user processes) */
        pd = sys_page_dir;
    }

    /* Get/create the PTE for this virtual page in the process's PD */
    pte = get_pte(pd, vpage);

    /* Map the FFS frame to this virtual page */
    pte->pt_base   = frame >> 12;  /* physical frame number */
    pte->pt_pres   = 1;            /* present */
    pte->pt_write  = 1;            /* writable */
    pte->pt_user   = 1;            /* user accessible */
    pte->pt_pwt    = 0;
    pte->pt_pcd    = 0;
    pte->pt_acc    = 0;
    pte->pt_dirty  = 0;
    pte->pt_mbz    = 0;
    pte->pt_global = 0;

    /* Invalidate the TLB entry for this virtual address.
     * Even though the page was not present before, some CPUs
     * may cache "not present" entries. Safe to always invalidate.
     */
    invlpg((void *)vpage);

    /* Return to retry the faulting instruction - page is now mapped */
}
