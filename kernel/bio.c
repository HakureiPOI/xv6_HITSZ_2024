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

// 使用多个锁管理不同哈希桶的磁盘缓存
struct {
  struct spinlock overall_lock;   // 跨桶操作的全局锁
  struct spinlock lock[NBUCKETS]; // 每个哈希桶都有一个锁
  struct buf buf[NBUF];
  struct buf hashbucket[NBUCKETS]; // 每个哈希桶是一个缓存块的链表
} bcache;

char bcache_name[NBUCKETS][24];

// 哈希函数，将 (dev, blockno) 映射到特定的桶
uint hash(uint n) {
  return n % NBUCKETS;
}

// 哈希函数，将 (dev, blockno) 映射到特定的桶
void binit(void) {
  struct buf *b;

  initlock(&bcache.overall_lock, "bcache");

  // 初始化每个哈希桶的锁
  for (int i = 0; i < NBUCKETS; i++) {
    snprintf(bcache_name[i], sizeof(bcache_name[i]), "bcache%d", i);
    initlock(&bcache.lock[i], bcache_name[i]);
    
    // 初始化每个桶的双向链表
    bcache.hashbucket[i].prev = &bcache.hashbucket[i];
    bcache.hashbucket[i].next = &bcache.hashbucket[i];
  }

  // 初始化缓冲区缓存并将缓冲区链接到哈希桶
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

// 在缓冲区缓存中查找设备 dev 的磁盘块 blockno
// 如果没有找到，则分配一个缓冲区
// 无论哪种情况，返回加锁的缓冲区
static struct buf* bget(uint dev, uint blockno) {
  struct buf *b;

  uint key = hash(blockno);
  acquire(&bcache.lock[key]);

  // 检查当前哈希桶中是否已经缓存了该磁盘块
  for (b = bcache.hashbucket[key].next; b != &bcache.hashbucket[key]; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.lock[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 如果没有缓存，查找当前哈希桶中的空闲缓冲区
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

  // 当前桶中没有空闲缓冲区，释放锁并获取全局锁
  release(&bcache.lock[key]);
  acquire(&bcache.overall_lock);

  // 在其他哈希桶中查找空闲缓冲区
  for (int i = 0; i < NBUCKETS; i++) {
    if (i == key) continue;
    acquire(&bcache.lock[i]);
    
    for (b = bcache.hashbucket[i].prev; b != &bcache.hashbucket[i]; b = b->prev) {
      if (b->refcnt == 0) {
        // 找到一个空闲缓冲区，将其移动到当前桶
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;

        // 从当前桶中删除该缓冲区
        b->next->prev = b->prev;
        b->prev->next = b->next;

        // 将缓冲区添加到目标桶
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

// 返回一个包含指定块内容的已加锁缓冲区
struct buf* bread(uint dev, uint blockno) {
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// 将缓冲区的内容写入磁盘。必须已加锁
void bwrite(struct buf *b) {
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// 释放一个已加锁的缓冲区
// 移动到最近使用链表的头部
void brelse(struct buf *b) {
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint key = hash(b->blockno);
  acquire(&bcache.lock[key]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // 没有其他进程在引用它，将缓冲区移动到桶的链表头部
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
