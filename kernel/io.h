/*
 * io.h - I/O ポートの読み書き
 *
 * x86 にはメモリとは別に「I/O ポート」という番号付きの窓口があり、
 * PIC・タイマー・キーボードなどの装置とは、ここを通してやりとりする。
 * C には専用の書き方がないので、in / out 命令をインラインアセンブリで呼ぶ。
 *
 * 1度に読み書きする大きさで命令が違う:
 *   inb / outb : 1バイト(b = byte)
 *   inw / outw : 2バイト(w = word)
 *   inl / outl : 4バイト(l = long)
 *
 * "memory" の指定について:
 *   ネットワークカードは、CPU を通さずにメモリへ直接書き込む(DMA)。
 *   「ポートを読む → メモリを読む」の順番をコンパイラに入れ替えられると、
 *   カードが書き込む前の古い中身を読んでしまう。"memory" と書くと
 *   「この命令の前後でメモリの中身が変わるかもしれない」とコンパイラに伝わり、
 *   順番を入れ替えなくなる。
 */
#ifndef IO_H
#define IO_H

#include <stdint.h>

/* ポート port に 1バイト value を書く */
static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

/* ポート port から 1バイト読む */
static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

/* 2バイト版 */
static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

/* 4バイト版 */
static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

/* 古い装置が追いつけるように、ほんの少し待つ(使われていないポート 0x80 に書く) */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif
