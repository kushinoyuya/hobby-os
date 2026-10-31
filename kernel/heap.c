/*
 * heap.c - カーネルのヒープ(kmalloc / kfree)
 *
 * pmm はページ(4KB)単位でしか貸せない。でもパケット1つ(〜1.5KB)や
 * 接続の情報(数十バイト)のために 4KB ずつ使うのはもったいない。
 * そこで pmm から借りたページをさらに細かく切り分けて貸す。
 *
 * メモリの中は「見出し + 中身」の区画が番地の順に並んでいて、見出し同士が
 * 前後の区画を指している(双方向リスト)。
 *
 *   ┌────────┬──────────┬────────┬────────┬────────┬────────────────┐
 *   │見出し  │ 中身     │見出し  │ 中身   │見出し  │ 中身           │
 *   │USED 64 │(貸出中) │FREE 32 │(空き) │USED 128│(貸出中)       │
 *   └────────┴──────────┴────────┴────────┴────────┴────────────────┘
 *
 * kmalloc: 前から順に「空きで、十分大きい区画」を探す(ファーストフィット)。
 *          大きすぎたら、後ろの余りを新しい空き区画として切り分ける。
 *          見つからなければ pmm からページを借りてきて足す。
 * kfree  : 区画を空きに戻し、すぐ隣も空きならくっつける(断片化を防ぐ)。
 */
#include "heap.h"
#include "pmm.h"
#include "vga.h"

/* 見出しに書く印。メモリを覗いたときに読める文字になるようにしてある */
#define MAGIC_USED 0x44455355u  /* "USED" */
#define MAGIC_FREE 0x45455246u  /* "FREE" */

#define ALIGN      16           /* 中身の番地と大きさを16の倍数にそろえる */
#define GROW_PAGES 16           /* 足りないとき、最低でもこれだけ(64KB)借り足す */

struct block {
    uint32_t size;              /* 中身の大きさ(バイト、見出しは含まない) */
    uint32_t magic;             /* MAGIC_USED か MAGIC_FREE */
    struct block *prev;         /* 番地が1つ前の区画 */
    struct block *next;         /* 番地が1つ後の区画 */
};                              /* 16バイト。だから中身も16の倍数の番地から始まる */

#define HDR sizeof(struct block)

static struct block *head;      /* 番地が一番小さい区画 */

static uint32_t align_up(uint32_t n) { return (n + ALIGN - 1) & ~(uint32_t)(ALIGN - 1); }

/* b の直後に、すき間なく n が続いているか(別々に借りたページは離れていることがある) */
static int adjacent(struct block *b, struct block *n)
{
    return (uint8_t *)b + HDR + b->size == (uint8_t *)n;
}

/* b と b->next がどちらも空きで隣り合っていれば、1つにまとめる */
static void merge_with_next(struct block *b)
{
    struct block *n = b->next;
    if (!n || b->magic != MAGIC_FREE || n->magic != MAGIC_FREE || !adjacent(b, n))
        return;
    b->size += HDR + n->size;
    b->next = n->next;
    if (n->next)
        n->next->prev = b;
}

/* pmm からページを借りて、空き区画としてリストに(番地の順を守って)入れる */
static int grow(uint32_t need)
{
    uint32_t pages = (need + HDR + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < GROW_PAGES)
        pages = GROW_PAGES;

    uint32_t addr = pmm_alloc(pages);
    if (!addr)
        return 0;

    struct block *b = (struct block *)addr;
    b->size  = pages * PAGE_SIZE - HDR;
    b->magic = MAGIC_FREE;

    /* 入れる場所を探す: b より番地が小さい最後の区画の後ろ */
    struct block *prev = 0;
    struct block *cur = head;
    while (cur && cur < b) {
        prev = cur;
        cur = cur->next;
    }
    b->prev = prev;
    b->next = cur;
    if (cur)
        cur->prev = b;
    if (prev)
        prev->next = b;
    else
        head = b;

    /* 前後の空き区画とくっつけられるならくっつける */
    merge_with_next(b);
    if (prev)
        merge_with_next(prev);
    return 1;
}

void *kmalloc(size_t size)
{
    if (size == 0 || size > 0x40000000)    /* 1GB を超える要求は、計算があふれる前に断る */
        return 0;
    uint32_t need = align_up(size);

    for (int tries = 0; tries < 2; tries++) {
        for (struct block *b = head; b; b = b->next) {
            if (b->magic != MAGIC_FREE || b->size < need)
                continue;

            /* 余りが「見出し + 最小の中身」以上あれば、後ろを新しい空き区画に切り分ける */
            if (b->size >= need + HDR + ALIGN) {
                struct block *rest = (struct block *)((uint8_t *)b + HDR + need);
                rest->size  = b->size - need - HDR;
                rest->magic = MAGIC_FREE;
                rest->prev  = b;
                rest->next  = b->next;
                if (b->next)
                    b->next->prev = rest;
                b->next = rest;
                b->size = need;
            }
            b->magic = MAGIC_USED;
            return (uint8_t *)b + HDR;
        }
        /* 見つからなかった: ページを借り足して、もう1回だけ探す */
        if (!grow(need))
            return 0;
    }
    return 0;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    struct block *b = (struct block *)((uint8_t *)ptr - HDR);
    if (b->magic != MAGIC_USED) {
        /* 二重解放か、kmalloc で借りていない番地。見出しが壊れている可能性もある */
        vga_set_color(COLOR_ERROR);
        vga_print("kfree: bad pointer ");
        vga_print_hex((uint32_t)(uintptr_t)ptr);
        vga_set_color(COLOR_NORMAL);
        vga_print("\n");
        return;
    }

    b->magic = MAGIC_FREE;
    merge_with_next(b);             /* 後ろとくっつける */
    if (b->prev)
        merge_with_next(b->prev);   /* 前とくっつける */
}

void heap_get_stats(struct heap_stats *s)
{
    s->total = s->used = s->blocks = s->free_blocks = 0;
    for (struct block *b = head; b; b = b->next) {
        s->total += b->size;
        s->blocks++;
        if (b->magic == MAGIC_USED)
            s->used += b->size;
        else
            s->free_blocks++;
    }
}
