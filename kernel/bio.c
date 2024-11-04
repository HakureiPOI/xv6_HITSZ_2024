// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 13

// Buffer cache with multiple locks for different buckets.
struct {
  struct spinlock overall_lock;   // Global lock for cross-bucket operations
  struct spinlock lock[NBUCKETS]; // Each hash bucket has its own lock
  struct buf buf[NBUF];
  struct buf hashbucket[NBUCKETS]; // Each hash bucket is a linked list of buffers
} bcache;

char bcache_name[NBUCKETS][24];

// Hash function to map (dev, blockno) to a specific bucket.
uint hash(uint n) {
  return n % NBUCKETS;
}

// Initialize buffer cache with multiple buckets.
void binit(void) {
  struct buf *b;

  initlock(&bcache.overall_lock, "bcache");

  // Initialize hash bucket locks
  for (int i = 0; i < NBUCKETS; i++) {
    snprintf(bcache_name[i], sizeof(bcache_name[i]), "bcache%d", i);
    initlock(&bcache.lock[i], bcache_name[i]);
    
    // Initialize doubly linked list for each bucket
    bcache.hashbucket[i].prev = &bcache.hashbucket[i];
    bcache.hashbucket[i].next = &bcache.hashbucket[i];
  }

  // Initialize buffer cache and link buffers to hash buckets
  for (int i = 0; i < NBUF; i++) {
    uint h = hash(i);
    b = &bcache.buf[i];
    b->next = bcache.hashbucket[h].next;
    b->prev = &bcache.hashbucket[h];
    initsleeplock(&b->lock, "buffer");
    bcache.hashbucket[h].next->prev = b;
    bcache.hashbucket[h].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf* bget(uint dev, uint blockno) {
  struct buf *b;

  uint key = hash(blockno);
  acquire(&bcache.lock[key]);

  // Check if the block is already cached in the current hash bucket
  for (b = bcache.hashbucket[key].next; b != &bcache.hashbucket[key]; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.lock[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // If not cached, look for an unused buffer in the current hash bucket
  for (b = bcache.hashbucket[key].prev; b != &bcache.hashbucket[key]; b = b->prev) {
    if (b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // No free buffer in the current bucket, release the lock and acquire the global lock
  release(&bcache.lock[key]);
  acquire(&bcache.overall_lock);

  // Search other buckets for an unused buffer
  for (int i = 0; i < NBUCKETS; i++) {
    if (i == key) continue;
    acquire(&bcache.lock[i]);
    
    for (b = bcache.hashbucket[i].prev; b != &bcache.hashbucket[i]; b = b->prev) {
      if (b->refcnt == 0) {
        // Found a free buffer, move it to the current bucket
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;

        // Remove buffer from current bucket
        b->next->prev = b->prev;
        b->prev->next = b->next;

        // Add buffer to the target bucket
        acquire(&bcache.lock[key]);
        b->next = &bcache.hashbucket[key];
        b->prev = bcache.hashbucket[key].prev;
        bcache.hashbucket[key].prev->next = b;
        bcache.hashbucket[key].prev = b;
        release(&bcache.lock[key]);

        release(&bcache.lock[i]);
        release(&bcache.overall_lock);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bcache.lock[i]);
  }

  release(&bcache.overall_lock);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf* bread(uint dev, uint blockno) {
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk. Must be locked.
void bwrite(struct buf *b) {
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b) {
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint key = hash(b->blockno);
  acquire(&bcache.lock[key]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // No one is waiting for it, move buffer to the head of the bucket list
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.hashbucket[key].next;
    b->prev = &bcache.hashbucket[key];
    bcache.hashbucket[key].next->prev = b;
    bcache.hashbucket[key].next = b;
  }
  release(&bcache.lock[key]);
}

void bpin(struct buf *b) {
  uint idx = hash(b->blockno);
  acquire(&bcache.lock[idx]);
  b->refcnt++;
  release(&bcache.lock[idx]);
}

void bunpin(struct buf *b) {
  uint idx = hash(b->blockno);
  acquire(&bcache.lock[idx]);
  b->refcnt--;
  release(&bcache.lock[idx]);
}
