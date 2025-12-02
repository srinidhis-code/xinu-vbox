# Xinu Virtual Memory Implementation Documentation

## Table of Contents
1. [Overview](#overview)
2. [Memory Layout](#memory-layout)
3. [Data Structures](#data-structures)
4. [Files Modified/Created](#files-modifiedcreated)
5. [Function Implementations](#function-implementations)
6. [Page Fault Handling](#page-fault-handling)
7. [Process Creation and Context Switching](#process-creation-and-context-switching)
8. [API Reference](#api-reference)

---

## Overview

This document describes the implementation of a virtual memory system for Xinu OS. The implementation provides:

- **Demand paging** with lazy allocation
- **Per-process page directories** for memory isolation
- **Virtual memory allocation** via `vmalloc()` and `vfree()`
- **First-fit allocation** strategy for virtual address space
- **FFS (First-Fit Space)** frame management for physical memory
- **CR3 switching** on context switch for process isolation

---

## Memory Layout

### Physical Memory Layout (224MB Total)

```
┌─────────────────────────────────────────────────────────────────┐
│  Address Range                    │  Region       │  Size       │
├─────────────────────────────────────────────────────────────────┤
│  0x00000000 - 0x02000000          │  Kernel       │  32MB       │
│  (code, data, BSS, heap)          │               │             │
├─────────────────────────────────────────────────────────────────┤
│  0x02000000 - 0x06000000          │  FFS Frames   │  64MB       │
│  (16,384 frames × 4KB)            │               │  (16K frames)│
├─────────────────────────────────────────────────────────────────┤
│  0x06000000 - 0x0E000000          │  Swap Space   │  128MB      │
│  (32,768 frames × 4KB)            │  (reserved)   │  (32K frames)│
└─────────────────────────────────────────────────────────────────┘
```

### Virtual Address Space (Per-Process User Heap)

```
┌─────────────────────────────────────────────────────────────────┐
│  Virtual Address Range            │  Region       │  Size       │
├─────────────────────────────────────────────────────────────────┤
│  0x10000000 - 0x1FFFFFFF          │  User Heap    │  256MB      │
│  (VHEAP_START - VHEAP_END)        │  (vmalloc)    │             │
└─────────────────────────────────────────────────────────────────┘
```

### Key Macros (paging.h)

```c
#define XINU_PAGES      8192        /* Default Xinu pages (32MB)         */
#define PAGE_SIZE       4096        /* 4KB per page                      */
#define MAX_FFS_SIZE    16*1024     /* 16K FFS frames                    */
#define MAX_SWAP_SIZE   32*1024     /* 32K swap frames (reserved)        */
#define MAX_PT_SIZE     1024        /* 1K frames for page tables         */

#define KERNEL_END      0x02000000u /* 32MB                              */
#define FFS_START       0x02000000u /* 32MB                              */
#define FFS_END         0x06000000u /* 96MB                              */
#define SWAP_START      0x06000000u /* 96MB                              */
#define SWAP_END        0x0E000000u /* 224MB                             */
#define PHYS_MEM_END    0x0E000000u /* Total mapped: 224MB               */

#define VHEAP_START     0x10000000u /* User virtual heap start (256MB)   */
#define VHEAP_END       0x1FFFFFFFu /* User virtual heap end (512MB-1)   */
```

---

## Data Structures

### Page Directory Entry (pd_t)

```c
typedef struct {
    unsigned int pd_pres  : 1;    /* page table present?              */
    unsigned int pd_write : 1;    /* page is writable?                */
    unsigned int pd_user  : 1;    /* user-level protection?           */
    unsigned int pd_pwt   : 1;    /* write-through caching for PT?    */
    unsigned int pd_pcd   : 1;    /* cache disable for this PT?       */
    unsigned int pd_acc   : 1;    /* page table was accessed?         */
    unsigned int pd_mbz   : 1;    /* must be zero                     */
    unsigned int pd_fmb   : 1;    /* four MB pages?                   */
    unsigned int pd_global: 1;    /* global (ignored)                 */
    unsigned int pd_avail : 3;    /* for programmer's use             */
    unsigned int pd_base  : 20;   /* page table base address >> 12    */
} pd_t;
```

### Page Table Entry (pt_t)

```c
typedef struct {
    unsigned int pt_pres  : 1;    /* page is present?                 */
    unsigned int pt_write : 1;    /* page is writable?                */
    unsigned int pt_user  : 1;    /* user-level protection?           */
    unsigned int pt_pwt   : 1;    /* write-through for this page?     */
    unsigned int pt_pcd   : 1;    /* cache disable for this page?     */
    unsigned int pt_acc   : 1;    /* page was accessed?               */
    unsigned int pt_dirty : 1;    /* page was written?                */
    unsigned int pt_mbz   : 1;    /* must be zero                     */
    unsigned int pt_global: 1;    /* should be zero in 586            */
    unsigned int pt_avail : 3;    /* for programmer's use             */
    unsigned int pt_base  : 20;   /* physical frame number            */
} pt_t;
```

### Virtual Address Structure (virt_addr_t)

```c
typedef struct {
    unsigned int pg_offset : 12;  /* page offset (0-4095)             */
    unsigned int pt_offset : 10;  /* page table index (0-1023)        */
    unsigned int pd_offset : 10;  /* page directory index (0-1023)    */
} virt_addr_t;
```

### Virtual Memory Region (vmem_region)

```c
struct vmem_region {
    uint32               start_addr;   /* starting virtual address    */
    uint32               size;         /* size in bytes               */
    bool8                allocated;    /* TRUE if allocated           */
    struct vmem_region  *next;         /* next region in list         */
};
```

### Process Virtual Memory State (proc_vmem)

```c
struct proc_vmem {
    struct vmem_region *regions;        /* head of region list        */
    uint32              total_allocated;/* total pages allocated      */
};
```

### FFS Frame Entry (internal to paging.c)

```c
typedef struct {
    bool8         used;     /* TRUE if this FFS slot is in use   */
    pid32         owner;    /* pid that owns this frame          */
} ffs_frame_t;
```

---

## Files Modified/Created

### New Files

| File | Description |
|------|-------------|
| `system/paging.c` | Core paging functions: init, frame allocation, PTE management |
| `system/vmalloc.c` | Virtual memory allocation (first-fit) |
| `system/vfree.c` | Virtual memory deallocation with coalescing |
| `system/vcreate.c` | User process creation with private page directory |
| `system/pagefault_handler.c` | Page fault ISR handler (lazy allocation) |
| `system/pagefault_handler_disp.S` | Assembly dispatcher for page faults |
| `include/paging.h` | Paging structures, macros, and prototypes |

### Modified Files

| File | Changes |
|------|---------|
| `system/resched.c` | Added CR3 switching on context switch |
| `system/kill.c` | Added `vm_cleanup()` call to free FFS frames |
| `system/control_reg.c` | Added `invlpg()` function for TLB invalidation |
| `include/process.h` | Added `vmem_region`, `proc_vmem`, `user_process`, `prpdbr` fields |
| `system/i386.c` | Added page fault vector (14) registration |
| `include/xinu.h` | Added `#include <paging.h>` |

---

## Function Implementations

### paging.c - Core Paging Functions

#### `init_paging()`

Initializes the paging system:

```c
void init_paging(void)
{
    int i;

    /* Init PT/PD pool - static array for page tables */
    pt_base = (unsigned long)pt_space;
    pt_next = 0;

    /* Init FFS table - all frames initially free */
    for (i = 0; i < MAX_FFS_SIZE; i++) {
        ffs_tab[i].used  = FALSE;
        ffs_tab[i].owner = -1;
    }
    ffs_free_count = MAX_FFS_SIZE;

    /* Allocate system page directory */
    sys_pdbr = alloc_frame();
    sys_page_dir = (pd_t *)sys_pdbr;

    /* Identity-map 0 to PHYS_MEM_END (224MB) */
    map_region(sys_page_dir, 0, PHYS_MEM_END);
}
```

**Called from:** `system/initialize.c` during boot.

#### `alloc_frame()`

Allocates a 4KB frame from the PT/PD pool for page directories and page tables:

```c
unsigned long alloc_frame(void)
{
    intmask mask = disable();

    if (pt_next >= MAX_PT_SIZE) {
        panic("alloc_frame: out of PT frames");
    }

    unsigned long frame = pt_base + (pt_next * PAGE_SIZE);
    pt_next++;
    memset((void *)frame, 0, PAGE_SIZE);

    restore(mask);
    return frame;
}
```

#### `get_pte()`

Returns a pointer to the PTE for a given virtual address, creating the page table if necessary:

```c
pt_t* get_pte(pd_t *pd, unsigned long vaddr)
{
    virt_addr_t *va = (virt_addr_t *)&vaddr;
    pd_t *pde = &pd[va->pd_offset];

    /* Create page table if not present */
    if (!pde->pd_pres) {
        unsigned long pt_phys = alloc_frame();
        pde->pd_base  = pt_phys >> 12;
        pde->pd_pres  = 1;
        pde->pd_write = 1;
        pde->pd_user  = 0;
    }

    pt_t *pt = (pt_t *)((pde->pd_base) << 12);
    return &pt[va->pt_offset];
}
```

#### `map_region()`

Identity-maps a physical address range:

```c
void map_region(pd_t *pd, unsigned long start, unsigned long end)
{
    for (unsigned long addr = start; addr < end; addr += PAGE_SIZE) {
        pt_t *pte = get_pte(pd, addr);
        pte->pt_base  = addr >> 12;
        pte->pt_pres  = 1;
        pte->pt_write = 1;
        pte->pt_user  = 0;   /* kernel-only */
    }
}
```

#### `ffs_alloc_frame()`

Allocates an FFS frame for user process heap:

```c
unsigned long ffs_alloc_frame(pid32 pid)
{
    intmask mask = disable();

    for (int i = 0; i < MAX_FFS_SIZE; i++) {
        if (!ffs_tab[i].used) {
            unsigned long frame_addr = FFS_START + (i * PAGE_SIZE);
            ffs_tab[i].used  = TRUE;
            ffs_tab[i].owner = pid;
            ffs_free_count--;
            memset((void *)frame_addr, 0, PAGE_SIZE);
            restore(mask);
            return frame_addr;
        }
    }

    restore(mask);
    return (unsigned long)SYSERR;
}
```

#### `ffs_free_frame()`

Frees an FFS frame:

```c
void ffs_free_frame(pid32 pid, unsigned long frame)
{
    intmask mask = disable();

    if (frame < FFS_START || frame >= FFS_END) {
        restore(mask);
        return;
    }

    int i = (frame - FFS_START) / PAGE_SIZE;
    if (ffs_tab[i].used) {
        ffs_tab[i].used  = FALSE;
        ffs_tab[i].owner = -1;
        ffs_free_count++;
    }

    restore(mask);
}
```

#### `vm_cleanup()`

Frees all FFS frames owned by a process (called from `kill()`):

```c
void vm_cleanup(pid32 pid)
{
    intmask mask = disable();

    for (int i = 0; i < MAX_FFS_SIZE; i++) {
        if (ffs_tab[i].used && ffs_tab[i].owner == pid) {
            ffs_tab[i].used  = FALSE;
            ffs_tab[i].owner = -1;
            ffs_free_count++;
        }
    }

    restore(mask);
}
```

---

### vmalloc.c - Virtual Memory Allocation

#### `vmalloc()`

Allocates virtual memory using first-fit algorithm:

```c
char* vmalloc(uint32 nbytes)
{
    intmask mask = disable();
    struct procent *prptr = &proctab[currpid];

    if (nbytes == 0) {
        restore(mask);
        return (char*)SYSERR;
    }

    uint32 size = round_page(nbytes);

    /* First-fit search */
    for (struct vmem_region *r = prptr->vmem.regions; r != NULL; r = r->next) {
        if (!r->allocated && r->size >= size) {
            uint32 alloc_addr = r->start_addr;

            if (r->size == size) {
                /* Perfect fit */
                r->allocated = TRUE;
            } else {
                /* Split region */
                struct vmem_region *newr = getmem(sizeof(struct vmem_region));
                newr->start_addr = r->start_addr + size;
                newr->size       = r->size - size;
                newr->allocated  = FALSE;
                newr->next       = r->next;

                r->size      = size;
                r->allocated = TRUE;
                r->next      = newr;
            }

            prptr->vmem.total_allocated += (size / PAGE_SIZE);
            restore(mask);
            return (char*)alloc_addr;
        }
    }

    restore(mask);
    return (char*)SYSERR;
}
```

**Key Features:**
- Rounds size up to page boundary
- First-fit allocation from region list
- Splits larger regions when needed
- Tracks total allocated pages

---

### vfree.c - Virtual Memory Deallocation

#### `vfree()`

Frees virtual memory and releases physical frames:

```c
syscall vfree(char *ptr, uint32 nbytes)
{
    intmask mask = disable();
    struct procent *prptr = &proctab[currpid];

    uint32 start = round_page_down((uint32)ptr);
    uint32 end   = round_page_up((uint32)ptr + nbytes);

    /* Validate region is allocated */
    if (!validate_vfree(prptr, ptr, nbytes)) {
        restore(mask);
        return SYSERR;
    }

    pd_t *pd = (pd_t *)prptr->prpdbr;

    /* Free physical frames */
    for (uint32 va = start; va < end; va += PAGE_SIZE) {
        pt_t *pte = get_pte(pd, va);
        if (pte->pt_pres) {
            unsigned long phys = (unsigned long)(pte->pt_base << 12);
            ffs_free_frame(currpid, phys);
            pte->pt_pres = 0;
            invlpg((void *)va);  /* Invalidate TLB entry */
        }
    }

    /* Mark regions as free */
    for (struct vmem_region *r = prptr->vmem.regions; r != NULL; r = r->next) {
        if (r->allocated && r->start_addr >= start && 
            (r->start_addr + r->size) <= end) {
            r->allocated = FALSE;
        }
    }

    prptr->vmem.total_allocated -= (end - start) / PAGE_SIZE;

    /* Coalesce adjacent free regions */
    coalesce_free_regions(prptr);

    restore(mask);
    return OK;
}
```

**Key Features:**
- Validates the region before freeing
- Frees physical FFS frames
- Clears PTEs and invalidates TLB
- Coalesces adjacent free regions

---

## Page Fault Handling

### pagefault_handler_disp.S - Assembly Dispatcher

```asm
.text
.globl pagefault_handler_disp

pagefault_handler_disp:
    pushal                  # Save all general purpose registers
    cli                     # Disable interrupts

    call    pagefault_handler   # Call C handler

    sti                     # Re-enable interrupts
    popal                   # Restore registers
    add     $4, %esp        # Skip error code pushed by CPU
    iret                    # Return from interrupt
```

### pagefault_handler.c - C Handler

```c
void pagefault_handler(void)
{
    unsigned long fault_addr = read_cr2();  /* Faulting address */
    unsigned long vpage = fault_addr & 0xFFFFF000;

    struct procent *prptr = &proctab[currpid];

    /* Kernel processes should not fault */
    if (!prptr->user_process) {
        panic("Kernel page fault");
    }

    /* Check if address is in allocated region */
    if (!vaddr_in_allocated_region(prptr, vpage)) {
        kprintf("P%d:: SEGMENTATION_FAULT\n", currpid);
        kill(currpid);
        return;
    }

    /* Lazy allocation: allocate FFS frame */
    unsigned long frame = ffs_alloc_frame(currpid);
    if ((int)frame == SYSERR) {
        kprintf("P%d:: OUT_OF_MEMORY\n", currpid);
        kill(currpid);
        return;
    }

    /* Map frame to virtual page */
    pd_t *pd = (pd_t *)prptr->prpdbr;
    pt_t *pte = get_pte(pd, vpage);

    pte->pt_base  = frame >> 12;
    pte->pt_pres  = 1;
    pte->pt_write = 1;
    pte->pt_user  = 1;

    invlpg((void *)vpage);
}
```

**Page Fault Flow:**

```
1. User process accesses unmapped virtual address
       ↓
2. CPU generates page fault (INT 14)
       ↓
3. pagefault_handler_disp saves registers, calls C handler
       ↓
4. pagefault_handler():
   a. Read CR2 for faulting address
   b. Check if address is in allocated vmalloc region
   c. If not: SEGMENTATION_FAULT → kill process
   d. If yes: allocate FFS frame (lazy allocation)
   e. Map frame in process's page table
   f. Invalidate TLB entry
       ↓
5. Return from interrupt, CPU retries instruction
       ↓
6. Page now mapped, access succeeds
```

---

## Process Creation and Context Switching

### vcreate.c - User Process Creation

```c
pid32 vcreate(void *funcaddr, uint32 ssize, pri16 priority, 
              char *name, uint32 nargs, ...)
{
    intmask mask = disable();

    /* Create process using standard create() */
    pid32 pid = create(funcaddr, ssize, priority, name, nargs, ...);

    struct procent *prptr = &proctab[pid];

    /* Allocate new page directory */
    unsigned long pd_phys = alloc_frame();
    pd_t *pd = (pd_t *)pd_phys;
    memset(pd, 0, PAGE_SIZE);

    /* Copy kernel mappings from system PD */
    for (int i = 0; i < 1024; i++) {
        pd[i] = sys_page_dir[i];
    }

    /* Mark as user process */
    prptr->user_process = TRUE;
    prptr->prpdbr = pd_phys;

    /* Initialize virtual memory regions */
    init_proc_vmem(prptr);  /* Creates single free region VHEAP_START to VHEAP_END */

    restore(mask);
    return pid;
}
```

### resched.c - CR3 Switching

```c
void resched(void)
{
    /* ... standard scheduling logic ... */

    currpid = dequeue(readylist);
    struct procent *ptnew = &proctab[currpid];

    /* Switch page directory (CR3) */
    if (ptnew->user_process && ptnew->prpdbr != 0) {
        write_cr3(ptnew->prpdbr);  /* User process PD */
    } else {
        write_cr3(sys_pdbr);       /* Kernel PD */
    }

    preempt = QUANTUM;
    ctxsw(&ptold->prstkptr, &ptnew->prstkptr);
}
```

---

## API Reference

### Virtual Memory Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `vmalloc` | `char* vmalloc(uint32 nbytes)` | Allocate virtual memory |
| `vfree` | `syscall vfree(char *ptr, uint32 nbytes)` | Free virtual memory |
| `vcreate` | `pid32 vcreate(void *func, uint32 ssize, pri16 pri, char *name, uint32 nargs, ...)` | Create user process |

### Paging Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `init_paging` | `void init_paging(void)` | Initialize paging system |
| `alloc_frame` | `unsigned long alloc_frame(void)` | Allocate PT/PD frame |
| `get_pte` | `pt_t* get_pte(pd_t *pd, unsigned long vaddr)` | Get/create PTE |
| `map_region` | `void map_region(pd_t *pd, unsigned long start, unsigned long end)` | Identity-map region |
| `ffs_alloc_frame` | `unsigned long ffs_alloc_frame(pid32 pid)` | Allocate FFS frame |
| `ffs_free_frame` | `void ffs_free_frame(pid32 pid, unsigned long frame)` | Free FFS frame |
| `vm_cleanup` | `void vm_cleanup(pid32 pid)` | Free all frames for process |

### Debug/Test Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `free_ffs_pages` | `uint32 free_ffs_pages(void)` | Count free FFS frames |
| `used_ffs_frames` | `uint32 used_ffs_frames(pid32 pid)` | Count FFS frames owned by pid |
| `allocated_virtual_pages` | `uint32 allocated_virtual_pages(pid32 pid)` | Count allocated virtual pages |

### Control Register Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `read_cr0/2/3/4` | `unsigned long read_crX(void)` | Read control register |
| `write_cr0/3/4` | `void write_crX(unsigned long n)` | Write control register |
| `enable_paging` | `void enable_paging(void)` | Enable paging (set CR0.PG) |
| `invlpg` | `void invlpg(void *addr)` | Invalidate TLB entry |

---

## Test Cases

The implementation passes all 8 standard test cases:

| Test | Description | Result |
|------|-------------|--------|
| TEST1 | Single process, partial FFS | PASS |
| TEST2 | Single process, exhaust FFS | PASS |
| TEST3 | 2 sequential processes | PASS |
| TEST4 | 4 concurrent processes | PASS |
| TEST5 | Allocate more than use | PASS |
| TEST6 | 4 concurrent, large alloc | PASS |
| TEST7 | Exceed virtual address space | PASS |
| TEST8 | Exhaust PT area | PASS |

---

## Summary

This virtual memory implementation provides:

1. **Memory Isolation** - Each user process has its own page directory
2. **Demand Paging** - Physical frames allocated only on first access
3. **Efficient Allocation** - First-fit algorithm for virtual addresses
4. **Proper Cleanup** - FFS frames freed when process terminates
5. **TLB Management** - Invalidation on vfree and context switch via CR3

The design separates concerns:
- **PT/PD frames** from static pool (`pt_space`)
- **FFS frames** from dedicated region (32MB-96MB)
- **Virtual heap** in separate address space (256MB-512MB)

