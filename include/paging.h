/* paging.h */

#ifndef _PAGING_H_
#define _PAGING_H_

/* Macros */
#define XINU_PAGES      8192    /* number of pages used by default by Xinu   */
#define PAGE_SIZE       4096    /* bytes per page                            */
#define MAX_FFS_SIZE    16*1024 /* size of FFS space  (in frames)            */
#define MAX_SWAP_SIZE   32*1024 /* size of swap space (in frames)            */
#define MAX_PT_SIZE     1024    /* space used for page tables (in frames)    */

/* ECE565: user heap virtual address range */
#define VHEAP_START     0x02000000u   /* 32MB */
#define VHEAP_END       0x07FFFFFFu   /* 128MB-1, 64MB window */

/* Structure for a page directory entry */

typedef struct {

  unsigned int pd_pres  : 1;        /* page table present?          */
  unsigned int pd_write : 1;        /* page is writable?            */
  unsigned int pd_user  : 1;        /* is use level protection?     */
  unsigned int pd_pwt   : 1;        /* write through cachine for pt?*/
  unsigned int pd_pcd   : 1;        /* cache disable for this pt?   */
  unsigned int pd_acc   : 1;        /* page table was accessed?     */
  unsigned int pd_mbz   : 1;        /* must be zero                 */
  unsigned int pd_fmb   : 1;        /* four MB pages?               */
  unsigned int pd_global: 1;        /* global (ignored)             */
  unsigned int pd_avail : 3;        /* for programmer's use         */
  unsigned int pd_base  : 20;       /* location of page table?      */
} pd_t;

/* Structure for a page table entry */

typedef struct {

  unsigned int pt_pres  : 1;        /* page is present?             */
  unsigned int pt_write : 1;        /* page is writable?            */
  unsigned int pt_user  : 1;        /* is use level protection?     */
  unsigned int pt_pwt   : 1;        /* write through for this page? */
  unsigned int pt_pcd   : 1;        /* cache disable for this page? */
  unsigned int pt_acc   : 1;        /* page was accessed?           */
  unsigned int pt_dirty : 1;        /* page was written?            */
  unsigned int pt_mbz   : 1;        /* must be zero                 */
  unsigned int pt_global: 1;        /* should be zero in 586        */
  unsigned int pt_avail : 3;        /* for programmer's use         */
  unsigned int pt_base  : 20;       /* location of page?            */
} pt_t;

/* Structure for a virtual address */

typedef struct{
  unsigned int pg_offset : 12;      /* page offset                  */
  unsigned int pt_offset : 10;      /* page table offset            */
  unsigned int pd_offset : 10;      /* page directory offset        */
} virt_addr_t;

/* Structure for a physical address */

typedef struct{
  unsigned int fm_offset : 12;      /* frame offset                 */
  unsigned int fm_num    : 20;      /* frame number                 */
} phy_addr_t;


/* Functions to manipulate control registers and enable paging (see control_reg.c) */

unsigned long read_cr0(void);
unsigned long read_cr2(void);
unsigned long read_cr3(void);
unsigned long read_cr4(void);

void write_cr0(unsigned long n);
void write_cr3(unsigned long n);
void write_cr4(unsigned long n);

void enable_paging(void);
void invlpg(void *addr);

extern unsigned long sys_pdbr;
extern pd_t *sys_page_dir;

void init_paging(void);

unsigned long alloc_frame(void);
pt_t* get_pte(pd_t *pd, unsigned long vaddr);
void map_region(pd_t *pd, unsigned long start, unsigned long end);

/* FFS frame allocation and management */
unsigned long ffs_alloc_frame(pid32 pid);
void         ffs_free_frame(pid32 pid, unsigned long frame);

/* VM debug functions */
uint32 free_ffs_pages(void);
uint32 used_ffs_frames(pid32 pid);
uint32 allocated_virtual_pages(pid32 pid);

/* Clean up virtual memory resources for a process */
void vm_cleanup(pid32 pid);

#endif /* _PAGING_H_ */
