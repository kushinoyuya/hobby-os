/*
 * heap.h - カーネルのヒープ(好きな大きさのメモリを借りる・返す)
 */
#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

/* size バイトのメモリを借りる。番地は16バイトの倍数。足りなければ NULL */
void *kmalloc(size_t size);

/* kmalloc で借りたメモリを返す。NULL を渡しても何もしない */
void kfree(void *ptr);

struct heap_stats {
    uint32_t total;         /* ヒープ全体の大きさ(バイト、見出しを除く) */
    uint32_t used;          /* 貸し出し中(バイト) */
    uint32_t blocks;        /* 区画の数 */
    uint32_t free_blocks;   /* そのうち空いている区画の数 */
};
void heap_get_stats(struct heap_stats *s);

#endif
