#include <xinu.h>
#include <paging.h>

/* Round up to multiple of PAGE_SIZE */
static uint32 round_page(uint32 n)
{
    if (n == 0) return 0;
    return (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

char* vmalloc(uint32 nbytes)
{
    struct procent *prptr = &proctab[currpid];
    struct vmem_region *r, *newr;
    uint32 size;
    uint32 alloc_addr;

    if (nbytes == 0) {
        return (char*)SYSERR;
    }

    size = round_page(nbytes);

    /* First-fit search from lowest address region */
    for (r = prptr->vmem.regions; r != NULL; r = r->next) {

        if (!r->allocated && r->size >= size) {

            alloc_addr = r->start_addr;

            if (r->size == size) {
                /* Perfect fit */
                r->allocated = TRUE;
                prptr->vmem.total_allocated += (size / PAGE_SIZE);

            } else {
                /* Split region: [alloc] + [remaining free] */
                newr = (struct vmem_region *)getmem(sizeof(struct vmem_region));
                if ((int)newr == SYSERR) {
                    return (char*)SYSERR;
                }

                newr->start_addr = r->start_addr + size;
                newr->size       = r->size - size;
                newr->allocated  = FALSE;
                newr->next       = r->next;

                r->start_addr = alloc_addr;
                r->size       = size;
                r->allocated  = TRUE;
                r->next       = newr;
                prptr->vmem.total_allocated += (size / PAGE_SIZE);
            }

            return (char*)alloc_addr;
        }
    }

    /* No suitable free region found */
    return (char*)SYSERR;
}
