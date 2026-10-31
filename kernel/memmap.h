/*
 * memmap.h - BIOS から受け取ったメモリの地図(E820)
 *
 * stage2.asm がプロテクトモードに入る前に BIOS(int 0x15, eax=0xE820)に聞いて、
 * 結果を 0x1000 番地に置いておいてくれる。カーネルはそれを読むだけ。
 *
 *   0x1000: 項目の数(uint32_t)
 *   0x1004: 項目 0, 項目 1, …(1項目24バイト)
 */
#ifndef MEMMAP_H
#define MEMMAP_H

#include <stdint.h>

#define MEMMAP_ADDR 0x1000      /* stage2.asm の MMAP_COUNT と同じ番地 */

/* 項目の種類 */
enum {
    MEMMAP_USABLE   = 1,        /* 自由に使ってよい */
    MEMMAP_RESERVED = 2,        /* BIOS や装置が使っている。触ってはいけない */
    MEMMAP_ACPI     = 3,        /* ACPI の表。読んだ後なら使ってよい */
    MEMMAP_NVS      = 4,        /* ACPI が使い続ける。触ってはいけない */
    MEMMAP_BAD      = 5,        /* 壊れている */
};

/* BIOS が返す1項目(24バイト)。番地と大きさは64ビットで返ってくる */
struct memmap_entry {
    uint64_t base;              /* 開始番地 */
    uint64_t length;            /* 大きさ(バイト) */
    uint32_t type;              /* 種類(上の enum) */
    uint32_t acpi_attr;         /* 拡張属性(今は使わない) */
} __attribute__((packed));

struct memmap {
    uint32_t count;
    struct memmap_entry entries[];
} __attribute__((packed));

static inline const struct memmap *memmap_get(void)
{
    return (const struct memmap *)MEMMAP_ADDR;
}

#endif
