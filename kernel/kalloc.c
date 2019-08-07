// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include <cdefs.h>
#include <defs.h>
#include <e820.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <spinlock.h>
#include <fs.h>
#include <proc.h>
#include "../inc/mmu.h"

int npages = 0;
int pages_in_use;
int pages_in_swap;
int free_pages;
uint64_t cow_ppn;

struct core_map_entry *core_map = NULL;
struct swap_map_entry *swap_map = NULL;

struct core_map_entry *pa2page(uint64_t pa) {
  if (PGNUM(pa) >= npages) {
    panic("pa2page called with invalid pa");
  }
  return &core_map[PGNUM(pa)];
}

uint64_t page2pa(struct core_map_entry *pp) {
  return (pp - core_map) << PT_SHIFT;
}

// --------------------------------------------------------------
// Detect machine's physical memory setup.
// --------------------------------------------------------------

void detect_memory(void) {
  uint32_t i;
  struct e820_entry *e;
  size_t mem = 0, mem_max = -KERNBASE;

  e = e820_map.entries;
  for (i = 0; i != e820_map.nr; ++i, ++e) {
    if (e->addr >= mem_max)
      continue;
    mem = max(mem, (size_t)(e->addr + e->len));
  }

  // Limit memory to 256MB.
  mem = min(mem, mem_max);
  npages = mem / PGSIZE;
  cprintf("E820: physical memory %dMB\n", mem / 1024 / 1024);
}

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file

struct {
  struct spinlock lock;
  int use_lock;
} kmem;

static void setrand(unsigned int);

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void mem_init(void *vstart) {
  void *vend;

  core_map = vstart;
  memset(vstart, 0, PGROUNDUP(npages * sizeof(struct core_map_entry)));
  vstart += PGROUNDUP(npages * sizeof(struct core_map_entry));

  swap_map = vstart;
  memset(vstart, 0, PGROUNDUP(SWAPPAGES * sizeof(struct swap_map_entry)));
  vstart += PGROUNDUP(SWAPPAGES * sizeof(struct swap_map_entry));

  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;

  vend = (void *)P2V((uint64_t)(npages * PGSIZE));
  freerange(vstart, vend);
  free_pages = (vend - vstart) >> PT_SHIFT;
  pages_in_use = 0;
  pages_in_swap = 0;
  kmem.use_lock = 1;
  setrand(1);
}

void freerange(void *vstart, void *vend) {
  char *p;
  p = (char *)PGROUNDUP((uint64_t)vstart);
  for (; p + PGSIZE <= (char *)vend; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(char *v) {
  struct core_map_entry *r;

  if ((uint64_t)v % PGSIZE || v < _end || V2P(v) >= (uint64_t)(npages * PGSIZE))
    panic("kfree");

  if (kmem.use_lock)
    acquire(&kmem.lock);

  r = (struct core_map_entry *)pa2page(V2P(v));

  r->ref--;
  if (kmem.use_lock == 0 || r->ref == 0) {
    pages_in_use--;
    free_pages++;

    // Fill with junk to catch dangling refs.
    memset(v, 2, PGSIZE);

    r->available = 1;
    r->user = 0;
    r->va = 0;
    r->ref = 0;
  }

  if (kmem.use_lock)
    release(&kmem.lock);
}

void
mark_user_mem(uint64_t pa, uint64_t va)
{
  // for user mem, add an mapping to proc_info
  struct core_map_entry *r = pa2page(pa);

  r->user = 1;
  r->va = va;
}

void
mark_kernel_mem(uint64_t pa)
{
  // for user mem, add an mapping to proc_info
  struct core_map_entry *r = pa2page(pa);

  r->user = 0;
  r->va = 0;
}

char* evictpage(int iskalloc) {
  struct core_map_entry* cme;
  char* addr;
  uint swap_idx;

  if (kmem.use_lock ) {
    acquire(&kmem.lock);
  }

  // pick a randome user page to evict
  cme = get_random_user_page();
  while (PGNUM(page2pa(cme)) == cow_ppn || PGNUM(page2pa(cme)) == 0 || cme->available) {
    cme = get_random_user_page();
  }
  assert(cme->ref > 0);
  addr = P2V(page2pa(cme));

  // find a free swap region page
  for (swap_idx = 0; swap_idx < SWAPPAGES; swap_idx++) {
    if (swap_map[swap_idx].used == 0) {
      swap_map[swap_idx].used = 1;
      swap_map[swap_idx].ref = cme->ref;
      swap_map[swap_idx].va = cme->va;
      pages_in_swap++;
      break;
    }
  }

  // if no free swap page
  if (swap_idx == SWAPPAGES) {
    if (kmem.use_lock)
      release(&kmem.lock);
    return 0;
  }

  if (iskalloc) {
    // set up kalloc memory
    cme->ref = 1;
    assert(cme->available = 0);
  } else {
    // set up free page
    cme->available = 1;
    cme->ref = 0;
    pages_in_use--;
    free_pages++;
  }
  cme->user = 0;
  cme->va = 0;

  if (kmem.use_lock)
    release(&kmem.lock);

  // write the data into swap region
  swapwrite(ROOTDEV, swap_idx, addr);

  // update vpage_infos
  markswapped(PGNUM(page2pa(cme)), swap_idx, swap_map[swap_idx].va);

  vspaceinstall(myproc());

  return addr;
}

char *kalloc(void) {
  int i;
  short lockacquired = 0;

  if (kmem.use_lock && !holding(&kmem.lock)) {
    acquire(&kmem.lock);
    lockacquired = 1;
  }

  for (i = 0; i < npages; i++) {
    if (core_map[i].available == 1) {
      core_map[i].available = 0;
      core_map[i].ref = 1;
      core_map[i].user = 0;
      core_map[i].va = 0;
      if (lockacquired && kmem.use_lock)
        release(&kmem.lock);
      pages_in_use++;
      free_pages--;
      return P2V(page2pa(&core_map[i]));
    }
  }

  if (lockacquired && kmem.use_lock)
    release(&kmem.lock);

  return evictpage(1);
}


// Increments the reference count for a given physical page
void increment_cme_ref(uint64_t ppn) {
  if (kmem.use_lock) {
    acquire(&kmem.lock);
  }
  struct core_map_entry *cme = pa2page(ppn << PT_SHIFT);
  assert(!cme->available && cme->ref > 0);
  cme->ref++;
  if (kmem.use_lock) {
    release(&kmem.lock);
  }
}

// Increments the reference count for a given swapped page
void increment_sme_ref(uint swap_idx) {
  if (kmem.use_lock) {
    acquire(&kmem.lock);
  }
  struct swap_map_entry *sme = &swap_map[swap_idx];
  assert(sme->used && sme->ref > 0);
  sme->ref++;
  if (kmem.use_lock) {
    release(&kmem.lock);
  }
}

// Decrement the reference count of a given swapped page
void swapfree(uint swap_idx) {
  if (kmem.use_lock) {
    acquire(&kmem.lock);
  }
  struct swap_map_entry *sme = &swap_map[swap_idx];
  assert(sme->used && sme->ref > 0);
  sme->ref--;

  // free the page
  if (sme->ref == 0) {
    sme->used = 0;
    pages_in_swap--;
  }

  if (kmem.use_lock) {
    release(&kmem.lock);
  }
}

// allocate and copy the page data when ref count is larger than 1
int ppage_copy(uint64_t* ppn_ptr) {
  struct core_map_entry *cme;
  char *data;
  uint64_t ppn = *ppn_ptr;

  if (kmem.use_lock) {
    acquire(&kmem.lock);
  }

  cme = pa2page(ppn << PT_SHIFT);
  assert(cme->ref != 0);
  if (cme->ref > 1) {
    cow_ppn = ppn;
    if (!(data = kalloc())) {
      if (kmem.use_lock) {
        release(&kmem.lock);
      }
      return -1;
    }
    memmove(data, P2V(ppn << PT_SHIFT), PGSIZE);
    cme->ref--;
    *ppn_ptr = PGNUM(V2P(data));
  }

  if (kmem.use_lock) {
    release(&kmem.lock);
  }
  return 0;
}

int swappage_copy(uint swap_idx) {
  struct core_map_entry *cme;
  struct swap_map_entry *swe;
  char *mem;
  uint64_t ppn;

  if (kmem.use_lock) {
    acquire(&kmem.lock);
  }

  // Allocate a new page
  if (!(mem = kalloc())) {
    return -1;
  }

  // Get the new ppn and the core map entry associated with it
  ppn = PGNUM(V2P(mem));
  cme = pa2page(V2P(mem));

  // Update the core map entry with the fields of the swap_map_entry
  swe = &(swap_map[swap_idx]);
  assert(swe->used == 1);
  assert(swe->ref > 0);
  assert(swe->va != 0);

  cme->user = 1;
  cme->ref = swe->ref;
  cme->va = swe->va;

  swe->used = 0;
  swe->ref = 0;
  pages_in_swap--;

  if (kmem.use_lock) {
    release(&kmem.lock);
  }

  // Read the data from the swap entry into the newly allocated page
  swapread(ROOTDEV, swap_idx, mem);

  // If the page is a cow page, update all processes referencing the cow page
  updatecowreferences(ppn, swap_idx, cme->va);

  return 0;
}

void ensure_n_free_pages(uint n) {
  while (free_pages < n) {
    if (!evictpage(0))
      panic("Run out of swap region memory");
  }
}

static unsigned long int next = 1; 

// returns random integer from [0, limit)
static int rand(int limit) { 
  next = next * 1103515245 + 12345; 
  return (unsigned int)(next/65536) % limit; 
} 

// Sets the seed for random.
// Intended to be used before calling rand.
static void setrand(unsigned int seed) { 
  next = seed; 
}

struct core_map_entry * get_random_user_page() {
  int x = 100;
  while(x--) {
    int rand_index = rand(npages);
    if (core_map[rand_index].va != 0) {
      return &core_map[rand_index];
    }
  }
  panic("Tried 100 random indices for random user page, all failed");
}
