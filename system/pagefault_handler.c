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

    /* We do not expect page faults in kernel-only processes */
    if (!prptr->user_process) {
        kprintf("Page fault in kernel process %d at 0x%08X\n",
                currpid, (unsigned)fault_addr);
        kill(currpid);
        return;
    }

    /* Check if fault address is in an allocated user heap region */
    if (!vaddr_in_allocated_region(prptr, vpage)) {
        /* Segmentation fault in user process */
        kprintf("P%d:: SEGMENTATION_FAULT\n", currpid);
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

    /* Choose the page directory */
    if (prptr->user_process && prptr->prpdbr != 0) {
        pd = (pd_t *)prptr->prpdbr;
    } else {
        pd = sys_page_dir;
    }

    /* Get/create the PTE for this virtual page */
    pte = get_pte(pd, vpage);

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
    /* pt_avail left for later (e.g., swapping metadata) */

    /* No explicit TLB invalidation needed: this was a not-present entry */
}
