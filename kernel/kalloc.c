// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct kmem {
  struct spinlock lock;
  struct run *freelist;
};

struct kmem kmems[NCPU];  // 每个 CPU 一个 kmem


void
kinit()   // 对每一个CPU初始化
{
  for (int i = 0; i < NCPU; i++) {
    initlock(&kmems[i].lock, "kmem");
  }
  freerange(end, (void*)PHYSTOP);
}


void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // 获取当前 CPU 的 kmem
  int id = cpuid();
  struct kmem *k = &kmems[id];

  acquire(&k->lock);
  r->next = k->freelist;
  k->freelist = r;
  release(&k->lock);
}


// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  int id = cpuid();
  struct kmem *k = &kmems[id];

  acquire(&k->lock);
  r = k->freelist;
  if(r)
    k->freelist = r->next;
  release(&k->lock);

  if (r == 0) {
    // 当前 CPU 没有空闲内存，从其他 CPU 获取
    for (int i = 0; i < NCPU; i++) {
      if (i == id)
        continue;
      k = &kmems[i];
      acquire(&k->lock);
      r = k->freelist;
      if (r) {
        k->freelist = r->next;
        release(&k->lock);
        break;
      }
      release(&k->lock);
    }
  }

  if (r)
    memset((char*)r, 5, PGSIZE); // 填充垃圾数据
  return (void*)r;
}
