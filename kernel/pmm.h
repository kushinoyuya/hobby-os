/*
 * pmm.h - 物理メモリの管理(Physical Memory Manager)
 *
 * メモリを 4KB の「ページ」に区切り、ページ単位で貸し出す・返してもらう。
 */
#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PAGE_SIZE 4096

/* E820 の地図を読み、使ってよいページを数える */
void pmm_init(void);

/* 連続した count ページを借りる。返り値は先頭の番地。足りなければ 0 */
uint32_t pmm_alloc(uint32_t count);

/* pmm_alloc で借りた count ページを返す */
void pmm_free(uint32_t addr, uint32_t count);

uint32_t pmm_total_pages(void);     /* 管理しているページの数 */
uint32_t pmm_free_pages(void);      /* そのうち空いているページの数 */

#endif
