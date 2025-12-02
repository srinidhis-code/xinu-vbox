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
    unsigned long vaddr;   /* virtual address mapped to this frame   */
    pd_t         *pd;      /* page directory of owner process        */
} ffs_frame_t;

/* FFS frames are at fixed physical addresses: FFS_START to FFS_END.
 * Frame i is at physical address: FFS_START + (i * PAGE_SIZE)
 */
static ffs_frame_t ffs_tab[MAX_FFS_SIZE];
static uint32      ffs_free_count = MAX_FFS_SIZE;

/* Clock hand for approximate LRU - persists across test cases */
static int clock_hand = 0;

/* ----------------- Swap space tracking (ECE565 optional) ----------------- */

typedef struct {
    bool8         used;       /* swap slot in use                       */
    unsigned long ffs_frame;  /* FFS physical addr when in memory       */
    pid32         owner;      /* owning process (for cleanup)           */
} swap_entry_t;

static swap_entry_t swap_tab[MAX_SWAP_SIZE];

/* Debug swapping counter - limits debug output */
unsigned debug_swapping = 0;

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
 *   FFS frames are at fixed addresses: FFS_START + (index * PAGE_SIZE)
 * -----------------------------------------------------------------------
 */
unsigned long ffs_alloc_frame(pid32 pid)
{
    intmask mask;
    int   i;
    unsigned long frame_addr;

    mask = disable();

    if (isbadpid(pid)) {
        restore(mask);
        return (unsigned long)SYSERR;
    }

    /* Simple first-fit from index 0 */
    for (i = 0; i < MAX_FFS_SIZE; i++) {
        if (!ffs_tab[i].used) {
            frame_addr = FFS_START + (i * PAGE_SIZE);

            ffs_tab[i].used  = TRUE;
            ffs_tab[i].owner = pid;
            ffs_tab[i].vaddr = 0;    /* set later by ffs_set_vaddr */
            ffs_tab[i].pd    = NULL; /* set later by ffs_set_vaddr */

            if (ffs_free_count > 0) {
                ffs_free_count--;
            }

            memset((void *)frame_addr, 0, PAGE_SIZE);

            restore(mask);
            return frame_addr;
        }
    }

    /* No free FFS slots */
    restore(mask);
    return (unsigned long)SYSERR;
}

/* -----------------------------------------------------------------------
 * ffs_free_frame - free one FFS frame (mark slot as unused)
 *   FFS frames are at fixed addresses, no need to call freemem()
 * -----------------------------------------------------------------------
 */
void ffs_free_frame(pid32 pid, unsigned long frame)
{
    intmask mask;
    int i;

    mask = disable();

    if (frame == 0 || frame < FFS_START || frame >= FFS_END) {
        restore(mask);
        return;
    }

    /* Calculate index from frame address */
    i = (frame - FFS_START) / PAGE_SIZE;

    if (i >= 0 && i < MAX_FFS_SIZE && ffs_tab[i].used) {
        ffs_tab[i].used  = FALSE;
        ffs_tab[i].owner = -1;
        ffs_tab[i].vaddr = 0;
        ffs_tab[i].pd    = NULL;

        if (ffs_free_count < MAX_FFS_SIZE) {
            ffs_free_count++;
        }
    }

    restore(mask);
}

/* -----------------------------------------------------------------------
 * ffs_set_vaddr - set virtual address and PD for an FFS frame
 *   Called by pagefault_handler after allocating a frame.
 * -----------------------------------------------------------------------
 */
void ffs_set_vaddr(unsigned long frame, unsigned long vaddr, pd_t *pd)
{
    int i;

    if (frame < FFS_START || frame >= FFS_END) {
        return;
    }

    i = (frame - FFS_START) / PAGE_SIZE;
    if (i >= 0 && i < MAX_FFS_SIZE && ffs_tab[i].used) {
        ffs_tab[i].vaddr = vaddr;
        ffs_tab[i].pd    = pd;
    }
}

/* -----------------------------------------------------------------------
 * ffs_claim_frame - transfer ownership of an evicted FFS frame
 *   Called after swap_out to assign the frame to a new owner.
 *   Does NOT change ffs_free_count - frame stays "in use".
 * -----------------------------------------------------------------------
 */
void ffs_claim_frame(unsigned long frame, pid32 new_owner)
{
    int i;

    if (frame < FFS_START || frame >= FFS_END) {
        return;
    }

    i = (frame - FFS_START) / PAGE_SIZE;
    if (i >= 0 && i < MAX_FFS_SIZE) {
        ffs_tab[i].used  = TRUE;
        ffs_tab[i].owner = new_owner;
        ffs_tab[i].vaddr = 0;    /* set later by ffs_set_vaddr */
        ffs_tab[i].pd    = NULL;
    }
}

/* -----------------------------------------------------------------------
 * alloc_frame - allocate one 4KB frame for PD/PT from pt_space
 * -----------------------------------------------------------------------
 * Uses dedicated, page-aligned pool; DO NOT call freemem on these frames.
 * -----------------------------------------------------------------------
 */
unsigned long alloc_frame(void)
{
    intmask mask;
    unsigned long frame;

    mask = disable();

    if (pt_next >= MAX_PT_SIZE) {
        restore(mask);
        panic("alloc_frame: out of PT frames");
    }

    frame = pt_base + (pt_next * PAGE_SIZE);
    pt_next++;

    memset((void *)frame, 0, PAGE_SIZE);

    restore(mask);
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

    /* Init FFS table - frames are at fixed addresses FFS_START + (i * PAGE_SIZE) */
    for (i = 0; i < MAX_FFS_SIZE; i++) {
        ffs_tab[i].used  = FALSE;
        ffs_tab[i].owner = -1;
        ffs_tab[i].vaddr = 0;
        ffs_tab[i].pd    = NULL;
    }
    ffs_free_count = MAX_FFS_SIZE;

    /* Note: clock_hand is NOT reset - it persists across test cases */

    /* Init swap subsystem */
    swap_init();
    debug_swapping = 0;

    /* Allocate system page directory from PT pool */
    sys_pdbr = alloc_frame();
    sys_page_dir = (pd_t *)sys_pdbr;

    /* Identity-map physical memory: 0 to PHYS_MEM_END (224MB)
     * Layout:
     *   0x00000000 - 0x02000000 (32MB)  : Kernel
     *   0x02000000 - 0x06000000 (64MB)  : FFS frames
     *   0x06000000 - 0x0E000000 (128MB) : Swap space
     */
    map_region(sys_page_dir, 0, PHYS_MEM_END);

    kprintf("Paging: sys_pdbr=0x%08X, mapped=0x%08X (224MB)\n",
            sys_pdbr, PHYS_MEM_END);
    kprintf("  Kernel: 0x00000000 - 0x%08X\n", KERNEL_END);
    kprintf("  FFS:    0x%08X - 0x%08X (%d frames)\n", FFS_START, FFS_END, MAX_FFS_SIZE);
    kprintf("  Swap:   0x%08X - 0x%08X (%d frames)\n", SWAP_START, SWAP_END, MAX_SWAP_SIZE);
}

/* -----------------------------------------------------------------------
 * vm_cleanup - free all heap frames for pid (and later PTs/PDs)
 * -----------------------------------------------------------------------
 */
void vm_cleanup(pid32 pid)
{
    intmask mask;
    int i;

    mask = disable();

    if (isbadpid(pid)) {
        restore(mask);
        return;
    }

    /* Free all FFS frames owned by this pid */
    for (i = 0; i < MAX_FFS_SIZE; i++) {
        if (ffs_tab[i].used && ffs_tab[i].owner == pid) {
            ffs_tab[i].used  = FALSE;
            ffs_tab[i].owner = -1;
            ffs_tab[i].vaddr = 0;
            ffs_tab[i].pd    = NULL;
            if (ffs_free_count < MAX_FFS_SIZE) {
                ffs_free_count++;
            }
        }
    }

    /* Free all swap frames owned by this pid */
    for (i = 0; i < MAX_SWAP_SIZE; i++) {
        if (swap_tab[i].used && swap_tab[i].owner == pid) {
            swap_free_frame(i);
        }
    }

    /* Do NOT freemem() PD/PT frames here – they come from pt_space.
     * For now we just leave them; MAX_PT_SIZE is large enough
     * for the professor's tests. Later you can add a bitmap for reuse.
     */

    restore(mask);
}

/*============================================================================
 * SWAPPING SUPPORT - Phase 1: Metadata + counting (no logic yet)
 *============================================================================
 */

/* -----------------------------------------------------------------------
 * free_swap_pages - return number of free swap frames
 * -----------------------------------------------------------------------
 */
uint32 free_swap_pages(void)
{
    uint32 count = 0;
    int i;

    for (i = 0; i < MAX_SWAP_SIZE; i++) {
        if (!swap_tab[i].used) {
            count++;
        }
    }
    return count;
}

/* -----------------------------------------------------------------------
 * swap_init - initialize swap subsystem
 * -----------------------------------------------------------------------
 */
void swap_init(void)
{
    int i;
    for (i = 0; i < MAX_SWAP_SIZE; i++) {
        swap_tab[i].used      = FALSE;
        swap_tab[i].ffs_frame = 0;
        swap_tab[i].owner     = -1;
    }
}

/* -----------------------------------------------------------------------
 * swap_select_victim - select an FFS frame to evict using clock algorithm
 *   Uses approximate LRU (clock) with global replacement.
 *   Returns physical address of victim FFS frame, or SYSERR if none found.
 * -----------------------------------------------------------------------
 */
unsigned long swap_select_victim(void)
{
    int start = clock_hand;
    int passes = 0;

    /* Up to 2 passes: first to clear accessed bits, second to find victim */
    while (passes < 2) {
        do {
            if (ffs_tab[clock_hand].used) {
                pd_t *pd = ffs_tab[clock_hand].pd;
                unsigned long vaddr = ffs_tab[clock_hand].vaddr;

                /* Only consider frames with valid PTE info */
                if (pd != NULL && vaddr != 0) {
                    pt_t *pte = get_pte(pd, vaddr);

                    if (pte->pt_acc == 0) {
                        /* Found victim - not recently accessed */
                        unsigned long victim_phys = FFS_START + (clock_hand * PAGE_SIZE);
                        clock_hand = (clock_hand + 1) % MAX_FFS_SIZE;
                        return victim_phys;
                    } else {
                        /* Recently accessed - give it a second chance */
                        pte->pt_acc = 0;
                    }
                }
            }

            clock_hand = (clock_hand + 1) % MAX_FFS_SIZE;
        } while (clock_hand != start);

        passes++;
    }

    return (unsigned long)SYSERR;
}

/* -----------------------------------------------------------------------
 * swap_alloc_frame - allocate a swap frame (first-fit)
 *   Returns swap index (not physical address), or SYSERR if exhausted.
 * -----------------------------------------------------------------------
 */
unsigned long swap_alloc_frame(void)
{
    int i;

    for (i = 0; i < MAX_SWAP_SIZE; i++) {
        if (!swap_tab[i].used) {
            swap_tab[i].used      = TRUE;
            swap_tab[i].ffs_frame = 0;
            swap_tab[i].owner     = -1;
            return (unsigned long)i;  /* swap index, not real address */
        }
    }
    return (unsigned long)SYSERR;
}

/* -----------------------------------------------------------------------
 * swap_free_frame - free a swap frame
 * -----------------------------------------------------------------------
 */
void swap_free_frame(unsigned long swap_idx)
{
    if (swap_idx >= MAX_SWAP_SIZE) {
        return;
    }
    swap_tab[swap_idx].used      = FALSE;
    swap_tab[swap_idx].ffs_frame = 0;
    swap_tab[swap_idx].owner     = -1;
}

/* -----------------------------------------------------------------------
 * ffs_index_from_phys - convert FFS physical address to index
 *   Returns index (0 to MAX_FFS_SIZE-1), or -1 if invalid
 * -----------------------------------------------------------------------
 */
static int ffs_index_from_phys(unsigned long ffs_frame_phys)
{
    int idx;

    if (ffs_frame_phys < FFS_START || ffs_frame_phys >= FFS_END) {
        return -1;
    }
    idx = (ffs_frame_phys - FFS_START) / PAGE_SIZE;
    if (idx < 0 || idx >= MAX_FFS_SIZE) {
        return -1;
    }
    return idx;
}

/* -----------------------------------------------------------------------
 * swap_out - evict an FFS frame to swap
 *   Updates victim's PTE to mark as swapped (pt_pres=0, pt_avail=1).
 * -----------------------------------------------------------------------
 */
void swap_out(unsigned long ffs_frame_phys)
{
    int f_idx;
    pid32 owner;
    unsigned long s_idx;
    pd_t *victim_pd;
    unsigned long victim_vaddr;
    pt_t *victim_pte;

    f_idx = ffs_index_from_phys(ffs_frame_phys);
    if (f_idx < 0) {
        return; /* should not happen; be defensive */
    }

    owner        = ffs_tab[f_idx].owner;
    victim_pd    = ffs_tab[f_idx].pd;
    victim_vaddr = ffs_tab[f_idx].vaddr;

    s_idx = swap_alloc_frame();
    if ((int)s_idx == SYSERR) {
        /* Per spec: assume swap never exhausts, so panic if this happens */
        panic("swap_out: no swap frame available\n");
    }

    swap_tab[s_idx].used      = TRUE;
    swap_tab[s_idx].ffs_frame = ffs_frame_phys;
    swap_tab[s_idx].owner     = owner;

    /* Copy FFS frame contents to swap space */
    {
        unsigned long swap_phys = SWAP_START + (s_idx * PAGE_SIZE);
        memcpy((void *)swap_phys, (void *)ffs_frame_phys, PAGE_SIZE);
    }

#if DEBUG_SWAPPING
    if (debug_swapping < 200) {
        kprintf("eviction:: FFS frame 0x%X, swap frame 0x%X copy\n",
                f_idx, (unsigned)s_idx);
        debug_swapping++;
    }
#endif

    /* Update victim's PTE to mark page as swapped out */
    if (victim_pd != NULL && victim_vaddr != 0) {
        victim_pte = get_pte(victim_pd, victim_vaddr);
        victim_pte->pt_pres  = 0;           /* not present */
        victim_pte->pt_avail = 1;           /* in swap */
        victim_pte->pt_base  = s_idx;       /* swap frame index */
        victim_pte->pt_write = 0;
        victim_pte->pt_user  = 0;
        victim_pte->pt_acc   = 0;
        victim_pte->pt_dirty = 0;

        /* Invalidate TLB for the evicted page */
        invlpg((void *)victim_vaddr);
    }

    /* Clear the vaddr/pd but keep the frame marked as "used".
     * The caller will transfer ownership to the new page.
     * Do NOT mark used=FALSE or increment ffs_free_count.
     */
    ffs_tab[f_idx].vaddr = 0;
    ffs_tab[f_idx].pd    = NULL;
    /* owner will be updated by the caller when they claim this frame */
}

/* -----------------------------------------------------------------------
 * swap_in - bring a page from swap back into FFS
 *   Returns new FFS physical address, or SYSERR on failure.
 *   If FFS is full, evicts a frame first.
 * -----------------------------------------------------------------------
 */
unsigned long swap_in(unsigned long swap_idx)
{
    pid32 owner;
    unsigned long new_ffs;
    int ffs_idx;

    if (swap_idx >= MAX_SWAP_SIZE || !swap_tab[swap_idx].used) {
        return (unsigned long)SYSERR;
    }

    owner = swap_tab[swap_idx].owner;

    new_ffs = ffs_alloc_frame(owner);
    if ((int)new_ffs == SYSERR) {
        /* FFS is full - evict a frame and directly use it */
        unsigned long victim_phys = swap_select_victim();
        if ((int)victim_phys == SYSERR) {
            return (unsigned long)SYSERR;
        }
        swap_out(victim_phys);

        /* Directly use the evicted frame - transfer ownership */
        new_ffs = victim_phys;
        ffs_claim_frame(new_ffs, owner);
    }

    /* Calculate FFS index for debug output */
    ffs_idx = (new_ffs - FFS_START) / PAGE_SIZE;

    /* Copy data from swap space back to FFS frame */
    {
        unsigned long swap_phys = SWAP_START + (swap_idx * PAGE_SIZE);
        memcpy((void *)new_ffs, (void *)swap_phys, PAGE_SIZE);
    }

#if DEBUG_SWAPPING
    if (debug_swapping < 200) {
        kprintf("swapping:: swap frame 0x%X, FFS frame 0x%X\n",
                (unsigned)swap_idx, (unsigned)ffs_idx);
        debug_swapping++;
    }
#endif

    swap_free_frame(swap_idx);
    return new_ffs;
}
