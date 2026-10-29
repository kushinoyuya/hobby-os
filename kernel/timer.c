/*
 * timer.c - PIT(Programmable Interval Timer、8253/8254)
 *
 * PIT は 1,193,182 Hz で数を数えている部品。「N 回数えるごとに IRQ0 を出す」と設定すると、
 * 一定の間隔で割り込みが来るようになる。OS の「時計」の役目。
 * (TCP の再送やタイムアウトも、この時計を使って作る予定)
 */
#include "timer.h"
#include "idt.h"
#include "io.h"

#define PIT_FREQ     1193182
#define PIT_CHANNEL0 0x40       /* チャンネル0(IRQ0 につながっている)のデータポート */
#define PIT_CMD      0x43       /* 命令ポート */

static volatile uint32_t ticks;  /* 割り込みの中で書き換わるので volatile */

static void timer_irq(struct regs *r)
{
    (void)r;
    ticks++;
}

void timer_init(void)
{
    uint16_t divisor = PIT_FREQ / TIMER_HZ;     /* 11931 回数えるごとに割り込む */

    outb(PIT_CMD, 0x36);        /* チャンネル0 / 下位→上位の順に書く / モード3(方形波) */
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, divisor >> 8);

    irq_register(0, timer_irq);
}

uint32_t timer_ticks(void)
{
    return ticks;
}
