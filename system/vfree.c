#include <xinu.h>
#include <paging.h>

static uint32 round_page_up(uint32 n)
{
    if (n == 0) return 0;
    return (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static uint32 round_page_down(uint32 n)
{
    return n & ~(PAGE_SIZE - 1);
}

/* Check that every page in [ptr, ptr+nbytes) lies inside some allocated region */
static bool8 validate_vfree(struct procent *prptr, char *ptr, uint32 nbytes)
{
    uint32 start = (uint32)ptr;
    uint32 end   = start + nbytes;

    start = round_page_down(start);
    end   = round_page_up(end);

    uint32 va;
    for (va = start; va < end; va += PAGE_SIZE) {

        bool8 ok = FALSE;
        struct vmem_region *r;

        for (r = prptr->vmem.regions; r != NULL; r = r->next) {
            if (r->allocated &&
                va >= r->start_addr &&
                va <  (r->start_addr + r->size)) {
                ok = TRUE;
                break;
            }
        }

        if (!ok) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Coalesce adjacent free regions that are contiguous in address space */
static void coalesce_free_regions(struct procent *prptr)
{
    struct vmem_region *r = prptr->vmem.regions;

    while (r != NULL && r->next != NULL) {
        struct vmem_region *n = r->next;

        if (!r->allocated && !n->allocated &&
            (r->start_addr + r->size) == n->start_addr) {

            /* Merge n into r */
            r->size += n->size;
            r->next  = n->next;

            /* Free the node structure itself */
            freemem((char *)n, sizeof(struct vmem_region));
        } else {
            r = r->next;
        }
    }
}

syscall vfree(char *ptr, uint32 nbytes)
{
    struct procent *prptr = &proctab[currpid];
    uint32 start, end, size;
    uint32 freed_pages;
    pd_t *pd;
    uint32 va;

    if (ptr == NULL || nbytes == 0) {
        return SYSERR;
    }

    /* Validate that this is a user process */
    if (!prptr->user_process) {
        return SYSERR;
    }

    start = round_page_down((uint32)ptr);
    end   = round_page_up((uint32)ptr + nbytes);
    size  = end - start;

    /* Validate that the region is fully allocated */
    if (!validate_vfree(prptr, ptr, nbytes)) {
        return SYSERR;
    }

    /* Number of pages we are freeing */
    freed_pages = size / PAGE_SIZE;

    /* Choose the correct page directory */
    if (prptr->user_process && prptr->prpdbr != 0) {
        pd = (pd_t *)prptr->prpdbr;
    } else {
        pd = sys_page_dir;
    }

    /* Free the physical frames backing this range (if any) */
    for (va = start; va < end; va += PAGE_SIZE) {
        pt_t *pte = get_pte(pd, va);
        if (pte->pt_pres) {
            unsigned long phys = (unsigned long)(pte->pt_base << 12);

            /* Return frame to FFS */
            ffs_free_frame(currpid, phys);

            /* Clear PTE */
            pte->pt_pres  = 0;
            pte->pt_write = 0;
            pte->pt_user  = 0;
            pte->pt_acc   = 0;
            pte->pt_dirty = 0;

            /* Invalidate this TLB entry - MUST be inside loop for each page */
            invlpg((void *)va);
        }
    }

    /* Mark all vmem regions fully inside [start, end) as free */
    struct vmem_region *r;
    for (r = prptr->vmem.regions; r != NULL; r = r->next) {
        uint32 r_start = r->start_addr;
        uint32 r_end   = r->start_addr + r->size;

        if (r->allocated &&
            r_start >= start &&
            r_end   <= end) {
            r->allocated = FALSE;
        }
    }

    /* Update virtual pages accounting: only allocated pages count */
    prptr->vmem.total_allocated -= freed_pages;

    /* Coalesce adjacent free regions to recreate bigger holes */
    coalesce_free_regions(prptr);

    return OK;
}
