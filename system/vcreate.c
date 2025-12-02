#include <xinu.h>
#include <paging.h>
#include <stdarg.h>

/*
 * init_proc_vmem - initialize virtual memory for a user process
 * Sets up a single free region covering the entire heap range
 */
static void init_proc_vmem(struct procent *prptr)
{
    struct vmem_region *r;

    r = (struct vmem_region *)getmem(sizeof(struct vmem_region));
    if ((int)r == SYSERR) {
        panic("init_proc_vmem: out of memory\n");
    }

    r->start_addr = VHEAP_START;
    r->size       = VHEAP_END - VHEAP_START + 1;  /* full heap range */
    r->allocated  = FALSE;
    r->next       = NULL;

    prptr->vmem.regions         = r;
    prptr->vmem.total_allocated = 0;
}

/*
 * vcreate - create a "user" process with its own page directory.
 * Behaves like create(), including argument passing, then adds VM setup.
 */
pid32 vcreate(
        void    *funcaddr,
        uint32  ssize,
        pri16   priority,
        char    *name,
        uint32  nargs,
        ...
    )
{
    intmask mask;
    pid32 pid = SYSERR;
    struct procent *prptr;

    mask = disable();

    /* ---------- 1. Forward arguments to create() properly ---------- */

    va_list ap;
    va_start(ap, nargs);

    switch (nargs) {
    case 0:
        pid = create(funcaddr, ssize, priority, name, 0);
        break;

    case 1: {
        uint32 a1 = va_arg(ap, uint32);
        pid = create(funcaddr, ssize, priority, name, 1, a1);
        break;
    }

    case 2: {
        uint32 a1 = va_arg(ap, uint32);
        uint32 a2 = va_arg(ap, uint32);
        pid = create(funcaddr, ssize, priority, name, 2, a1, a2);
        break;
    }

    case 3: {
        uint32 a1 = va_arg(ap, uint32);
        uint32 a2 = va_arg(ap, uint32);
        uint32 a3 = va_arg(ap, uint32);
        pid = create(funcaddr, ssize, priority, name, 3, a1, a2, a3);
        break;
    }

    case 4: {
        uint32 a1 = va_arg(ap, uint32);
        uint32 a2 = va_arg(ap, uint32);
        uint32 a3 = va_arg(ap, uint32);
        uint32 a4 = va_arg(ap, uint32);
        pid = create(funcaddr, ssize, priority, name, 4, a1, a2, a3, a4);
        break;
    }

    case 5: {
        uint32 a1 = va_arg(ap, uint32);
        uint32 a2 = va_arg(ap, uint32);
        uint32 a3 = va_arg(ap, uint32);
        uint32 a4 = va_arg(ap, uint32);
        uint32 a5 = va_arg(ap, uint32);
        pid = create(funcaddr, ssize, priority, name, 5, a1, a2, a3, a4, a5);
        break;
    }

    default:
        /* You can extend this if you need more args */
        va_end(ap);
        restore(mask);
        return SYSERR;
    }

    va_end(ap);

    if (pid == (pid32)SYSERR) {
        restore(mask);
        return SYSERR;
    }

    /* ---------- 2. VM / PD setup for this process ---------- */

    prptr = &proctab[pid];

    /* Allocate a new page directory from PT space */
    unsigned long pd_phys = alloc_frame();   /* 4KB for PD */
    pd_t *pd = (pd_t *)pd_phys;

    memset(pd, 0, PAGE_SIZE);

    /* Copy kernel mappings from system PD */
    int i;
    for (i = 0; i < 1024; i++) {
        pd[i] = sys_page_dir[i];
    }

    prptr->user_process = TRUE;
    prptr->prpdbr       = pd_phys;   /* physical addr for CR3 */

    init_proc_vmem(prptr);

    restore(mask);
    return pid;
}
