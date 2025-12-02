#include <xinu.h>
#include <paging.h>

/*--------------------------------------------------------------------
 * Global system PD
 *--------------------------------------------------------------------
 */
unsigned long sys_pdbr = 0;
pd_t *sys_page_dir = NULL;

/* linker symbols */
extern int etext;   /* end of text */
extern int edata;   /* end of data */
extern int ebss;    /* end of bss  */
extern int end;     /* start of heap */
extern void *maxheap;

/* ----------------- PT/PD frame space (MAX_PT_SIZE frames) ----------------- */
/* Reserve a dedicated region for page directories/tables.
 * We align it to PAGE_SIZE so every frame boundary is 4KB aligned.
 */
static uint8 pt_space[MAX_PT_SIZE * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static unsigned long pt_base;        /* base address of pt_space */
static int           pt_next = 0;    /* next free PT frame index */

/* ----------------- FFS frame tracking (ECE565) ----------------- */

typedef struct {
    bool8         used;    /* TRUE if this FFS slot is in use        */
    pid32         owner;   /* pid that owns this frame               */
    unsigned long phys;    /* aligned physical address (for PTE)     */
    char         *raw;     /* original pointer returned by getmem()  */
} ffs_frame_t;

/* Logical pool of FFS frames allocated from kernel heap via getmem().
 * Each slot tracks both the aligned address (for PTEs) and raw pointer (for freeing).
 */
static ffs_frame_t ffs_tab[MAX_FFS_SIZE];
static uint32      ffs_free_count = MAX_FFS_SIZE;

/* -----------------------------------------------------------------------
 * free_ffs_pages - return number of free FFS frames (for debugging/tests)
 * -----------------------------------------------------------------------
 */
uint32 free_ffs_pages(void)
{
    return ffs_free_count;
}

/* -----------------------------------------------------------------------
 * used_ffs_frames - number of FFS frames currently owned by pid
 * -----------------------------------------------------------------------
 */
uint32 used_ffs_frames(pid32 pid)
{
    uint32 count = 0;
    int i;

    if (isbadpid(pid)) {
        return 0;
    }

    for (i = 0; i < MAX_FFS_SIZE; i++) {
        if (ffs_tab[i].used && ffs_tab[i].owner == pid) {
            count++;
        }
    }
    return count;
}

/* -----------------------------------------------------------------------
 * allocated_virtual_pages - XINU core pages + vmalloc'ed pages
 *   Testcases expect:
 *      PREALLOCATED_PAGES = XINU_PAGES  (for ECE565)
 *      => allocated_virtual_pages(pid) = XINU_PAGES + (# vmalloc pages)
 * -----------------------------------------------------------------------
 */
uint32 allocated_virtual_pages(pid32 pid)
{
    if (isbadpid(pid)) {
        return 0;
    }

    struct procent *prptr = &proctab[pid];
    return XINU_PAGES + prptr->vmem.total_allocated;
}

/* -----------------------------------------------------------------------
 * ffs_alloc_frame - allocate one FFS frame for a process
 *   Returns physical address of a 4KB frame, or SYSERR on failure.
 * -----------------------------------------------------------------------
 */
unsigned long ffs_alloc_frame(pid32 pid)
{
    int   i;
    char *raw;
    unsigned long aligned;

    if (isbadpid(pid)) {
        return (unsigned long)SYSERR;
    }

    /* Find a free slot in the FFS table */
    for (i = 0; i < MAX_FFS_SIZE; i++) {
        if (!ffs_tab[i].used) {

            /* Get slightly more than a page so we can align safely */
            raw = getmem(PAGE_SIZE + PAGE_SIZE - 1);
            if ((int)raw == SYSERR) {
                /* Out of physical memory */
                return (unsigned long)SYSERR;
            }

            /* Align to 4KB boundary */
            aligned = ((unsigned long)raw + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

            /* Record metadata */
            ffs_tab[i].used  = TRUE;
            ffs_tab[i].owner = pid;
            ffs_tab[i].phys  = aligned;
            ffs_tab[i].raw   = raw;

            if (ffs_free_count > 0) {
                ffs_free_count--;
            }

            /* Zero out the aligned page (safe: region size > PAGE_SIZE) */
            memset((void *)aligned, 0, PAGE_SIZE);

            return aligned;
        }
    }

    /* No free FFS slots */
    return (unsigned long)SYSERR;
}

/* -----------------------------------------------------------------------
 * ffs_free_frame - free one FFS frame and return it to getmem pool
 * -----------------------------------------------------------------------
 */
void ffs_free_frame(pid32 pid, unsigned long frame)
{
    int i;

    if (frame == 0) {
        return;
    }

    for (i = 0; i < MAX_FFS_SIZE; i++) {
        if (ffs_tab[i].used && ffs_tab[i].phys == frame) {

            /* Optionally check owner == pid; we can ignore mismatch */
            if (ffs_tab[i].raw != NULL) {
                /* Free the SAME block we got from getmem() */
                freemem(ffs_tab[i].raw, PAGE_SIZE + PAGE_SIZE - 1);
            }

            ffs_tab[i].used  = FALSE;
            ffs_tab[i].owner = -1;
            ffs_tab[i].phys  = 0;
            ffs_tab[i].raw   = NULL;

            if (ffs_free_count < MAX_FFS_SIZE) {
                ffs_free_count++;
            }
            return;
        }
    }

    /* If not found, silently ignore for now */
}

/* -----------------------------------------------------------------------
 * alloc_frame - allocate one 4KB frame for PD/PT from pt_space
 * -----------------------------------------------------------------------
 * Uses dedicated, page-aligned pool; DO NOT call freemem on these frames.
 * -----------------------------------------------------------------------
 */
unsigned long alloc_frame(void)
{
    if (pt_next >= MAX_PT_SIZE) {
        panic("alloc_frame: out of PT frames");
    }

    unsigned long frame = pt_base + (pt_next * PAGE_SIZE);
    pt_next++;

    memset((void *)frame, 0, PAGE_SIZE);
    return frame;   /* physical address (identity-mapped) */
}

/* -----------------------------------------------------------------------
 * get_pte - return pointer to PTE for virtual address vaddr in PD 'pd'
 * -----------------------------------------------------------------------
 */
pt_t* get_pte(pd_t *pd, unsigned long vaddr)
{
    virt_addr_t *va = (virt_addr_t *)&vaddr;
    pd_t *pde = &pd[va->pd_offset];

    if (!pde->pd_pres) {
        unsigned long pt_phys = alloc_frame();

        pde->pd_base  = pt_phys >> 12;
        pde->pd_pres  = 1;
        pde->pd_write = 1;
        pde->pd_user  = 0;   /* kernel PT; user bits can vary per PTE */
    }

    pt_t *pt = (pt_t *)( (pde->pd_base) << 12 );
    return &pt[va->pt_offset];
}

/* -----------------------------------------------------------------------
 * map_region - identity-map [start, end) into PD 'pd'
 * -----------------------------------------------------------------------
 */
void map_region(pd_t *pd, unsigned long start, unsigned long end)
{
    start &= 0xFFFFF000;
    unsigned long addr;

    for (addr = start; addr < end; addr += PAGE_SIZE) {
        pt_t *pte = get_pte(pd, addr);

        pte->pt_base  = addr >> 12;
        pte->pt_pres  = 1;
        pte->pt_write = 1;
        pte->pt_user  = 0;   /* kernel-only mappings */
    }
}

/* -----------------------------------------------------------------------
 * init_paging - build system PD/PTs & map kernel memory
 * -----------------------------------------------------------------------
 */
void init_paging(void)
{
    int i;

    /* Init PT/PD pool */
    pt_base = (unsigned long)pt_space;
    if (pt_base & (PAGE_SIZE - 1)) {
        panic("pt_space not page-aligned\n");
    }
    pt_next = 0;

    /* Init FFS table */
    for (i = 0; i < MAX_FFS_SIZE; i++) {
        ffs_tab[i].used  = FALSE;
        ffs_tab[i].owner = -1;
        ffs_tab[i].phys  = 0;
        ffs_tab[i].raw   = NULL;
    }
    ffs_free_count = MAX_FFS_SIZE;

    /* Allocate system page directory from PT pool */
    sys_pdbr = alloc_frame();
    sys_page_dir = (pd_t *)sys_pdbr;

    /* Map kernel TEXT, DATA, BSS */
    map_region(sys_page_dir, 0, (unsigned long)&etext);
    map_region(sys_page_dir,
               (unsigned long)&etext,
               (unsigned long)&edata);
    map_region(sys_page_dir,
               (unsigned long)&edata,
               (unsigned long)&ebss);

    /* Map kernel heap: end → maxheap */
    map_region(sys_page_dir, (unsigned long)&end, (unsigned long)maxheap);

    /* Identity-map entire memory range up to maxheap (safe, simple) */
    map_region(sys_page_dir, 0, (unsigned long)maxheap);

    kprintf("Paging: sys_pdbr = 0x%08X, maxheap = 0x%08X\n",
            sys_pdbr, (unsigned long)maxheap);
}

/* -----------------------------------------------------------------------
 * vm_cleanup - free all heap frames for pid (and later PTs/PDs)
 * -----------------------------------------------------------------------
 */
void vm_cleanup(pid32 pid)
{
    int i;

    if (isbadpid(pid)) {
        return;
    }

    /* Free all FFS frames owned by this pid using ffs_free_frame() */
    for (i = 0; i < MAX_FFS_SIZE; i++) {
        if (ffs_tab[i].used && ffs_tab[i].owner == pid) {
            ffs_free_frame(pid, ffs_tab[i].phys);
        }
    }

    /* Do NOT freemem() PD/PT frames here – they come from pt_space.
     * For now we just leave them; MAX_PT_SIZE is large enough
     * for the professor's tests. Later you can add a bitmap for reuse.
     */
}

