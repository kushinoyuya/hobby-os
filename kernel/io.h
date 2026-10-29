/*
 * io.h - I/O ポートの読み書き
 *
 * x86 にはメモリとは別に「I/O ポート」という番号付きの窓口があり、
 * PIC・タイマー・キーボードなどの装置とは、ここを通してやりとりする。
 * C には専用の書き方がないので、in / out 命令をインラインアセンブリで呼ぶ。
 */
#ifndef IO_H
#define IO_H

#include <stdint.h>

/* ポート port に 1バイト value を書く */
static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* ポート port から 1バイト読む */
static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* 古い装置が追いつけるように、ほんの少し待つ(使われていないポート 0x80 に書く) */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif
