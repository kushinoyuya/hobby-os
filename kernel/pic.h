/*
 * pic.h - PIC(割り込みコントローラ、8259A)
 */
#ifndef PIC_H
#define PIC_H

#include <stdint.h>

void pic_init(void);            /* IRQ 0〜15 を割り込み番号 32〜47 にずらし、全部止める */
void pic_unmask(int irq);       /* この IRQ を通すようにする */
void pic_send_eoi(int irq);     /* 割り込みの処理が終わったことを伝える */

#endif
