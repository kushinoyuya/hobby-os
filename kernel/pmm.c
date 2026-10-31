/*
 * pmm.c - 物理メモリの管理(ビットマップ方式)
 *
 * メモリ全体(最大 4GB)を 4KB のページに区切り、1ページを1ビットで表す。
 *   ビットが 1 = 使用中(または使ってはいけない)
 *   ビットが 0 = 空いている
 *
 *   ページ番号:  0   1   2   3   4   5   6   7  …
 *   ビット    :  1   1   1   0   0   1   0   0  …
 *                            └空き┘     └─空き─┘
 *
 * 4GB ÷ 4KB = 1,048,576 ページなので、ビットマップは 128KB になる。
 * 番地 = ページ番号 × 4096 で、ページ番号 = 番地 >> 12。
 */
#include "pmm.h"
#include "memmap.h"
#include "vga.h"

#define MAX_PAGES   (1u << 20)          /* 4GB 分のページ数 */
#define LOW_MEMORY  0x100000            /* 1MB。ここより下は貸し出さない(下の説明) */

static uint32_t bitmap[MAX_PAGES / 32]; /* 1要素 = 32ページ分 */
static uint32_t page_limit;             /* 管理する範囲の終わり(これ以上のページ番号は探さない) */
static uint32_t total_pages;
static uint32_t free_count;

static int  is_used(uint32_t page) { return bitmap[page / 32] >> (page % 32) & 1; }
static void set_used(uint32_t page) { bitmap[page / 32] |=  1u << (page % 32); }
static void set_free(uint32_t page) { bitmap[page / 32] &= ~(1u << (page % 32)); }

void pmm_init(void)
{
    /* まず全部「使用中」にしてから、地図で「使ってよい」とされた所だけ空ける */
    for (uint32_t i = 0; i < MAX_PAGES / 32; i++)
        bitmap[i] = 0xFFFFFFFF;

    const struct memmap *map = memmap_get();
    for (uint32_t i = 0; i < map->count; i++) {
        const struct memmap_entry *e = &map->entries[i];
        if (e->type != MEMMAP_USABLE)
            continue;

        /* 32ビットの OS なので 4GB より上は扱わない */
        uint64_t start = e->base;
        uint64_t end   = e->base + e->length;
        if (start >= 0x100000000ULL)
            continue;
        if (end > 0x100000000ULL)
            end = 0x100000000ULL;

        /* 半端な部分は使わない: 開始はページの境目まで切り上げ、終わりは切り捨て */
        uint32_t first = (uint32_t)((start + PAGE_SIZE - 1) >> 12);
        uint32_t last  = (uint32_t)(end >> 12);     /* last は含まない */

        for (uint32_t p = first; p < last; p++) {
            /*
             * 1MB より下は空いていても貸さない。ここにはカーネル本体、
             * スタック(0x90000 から下へ伸びる)、メモリの地図、BIOS の領域がある。
             */
            if (p < LOW_MEMORY / PAGE_SIZE)
                continue;
            if (is_used(p)) {
                set_free(p);
                total_pages++;
                free_count++;
            }
        }
        if (last > page_limit)
            page_limit = last;
    }
}

uint32_t pmm_alloc(uint32_t count)
{
    if (count == 0)
        return 0;

    /* 空きが count ページ続いている場所を、前から順に探す(ファーストフィット) */
    uint32_t run = 0;
    for (uint32_t p = LOW_MEMORY / PAGE_SIZE; p < page_limit; p++) {
        /* 32ページ全部が使用中の所は、まとめて飛ばす(速くするための工夫) */
        if (p % 32 == 0 && bitmap[p / 32] == 0xFFFFFFFF) {
            run = 0;
            p += 31;
            continue;
        }
        if (is_used(p)) {
            run = 0;
            continue;
        }
        if (++run == count) {
            uint32_t first = p - count + 1;
            for (uint32_t q = first; q <= p; q++)
                set_used(q);
            free_count -= count;
            return first * PAGE_SIZE;
        }
    }
    return 0;   /* 番地 0 は絶対に貸さないので「失敗」の印に使える */
}

void pmm_free(uint32_t addr, uint32_t count)
{
    uint32_t first = addr / PAGE_SIZE;
    if (addr % PAGE_SIZE != 0 || first < LOW_MEMORY / PAGE_SIZE || first + count > page_limit) {
        vga_set_color(COLOR_ERROR);
        vga_print("pmm_free: bad address ");
        vga_print_hex(addr);
        vga_set_color(COLOR_NORMAL);
        vga_print("\n");
        return;
    }
    for (uint32_t p = first; p < first + count; p++) {
        if (!is_used(p)) {
            /* 二重解放: 同じページを2回返そうとした。呼び出し側のバグ */
            vga_set_color(COLOR_ERROR);
            vga_print("pmm_free: page already free ");
            vga_print_hex(p * PAGE_SIZE);
            vga_set_color(COLOR_NORMAL);
            vga_print("\n");
            continue;
        }
        set_free(p);
        free_count++;
    }
}

uint32_t pmm_total_pages(void) { return total_pages; }
uint32_t pmm_free_pages(void)  { return free_count; }
