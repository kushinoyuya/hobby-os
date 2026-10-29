/*
 * timer.h - PIT(タイマー)
 */
#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#define TIMER_HZ 100            /* 1秒に100回(10ミリ秒ごと)割り込む */

void timer_init(void);
uint32_t timer_ticks(void);     /* 起動してから何回割り込みが来たか */

#endif
