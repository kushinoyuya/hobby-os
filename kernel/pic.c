/*
 * pic.c - PIC(Programmable Interrupt Controller、8259A)
 *
 * 装置(タイマー、キーボード、ネットワークカード …)からの割り込みの信号は、
 * いったん PIC に集まり、PIC が CPU に「N 番の割り込み」として伝える。
 * PIC は2つあり、マスターが IRQ 0〜7、スレーブが IRQ 8〜15 を受け持つ。
 * スレーブはマスターの IRQ 2 につながっている(カスケード接続)。
 *
 *   タイマー  ─ IRQ0 ┐
 *   キーボード─ IRQ1 ┤
 *                IRQ2 ┼── マスター PIC ──▶ CPU
 *                 …  ┘     ▲
 *   (NIC など)─ IRQ8〜15 ── スレーブ PIC
 */
#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20  /* マスターの命令ポート */
#define PIC1_DATA 0x21  /* マスターのデータポート */
#define PIC2_CMD  0xA0  /* スレーブの命令ポート */
#define PIC2_DATA 0xA1  /* スレーブのデータポート */

#define PIC_EOI   0x20  /* End Of Interrupt */

#define IRQ_BASE  32    /* IRQ 0 を割り込み番号 32 にする */

void pic_init(void)
{
    /*
     * 起動直後の PIC は IRQ 0〜7 を割り込み番号 8〜15 として CPU に伝える。
     * これは CPU の例外(8 = ダブルフォルト など)と重なってしまうので、
     * 初期化命令(ICW1〜4)を送り直して 32〜47 にずらす。
     */
    outb(PIC1_CMD, 0x11);   io_wait();  /* ICW1: 初期化開始(この後 ICW4 まで送る) */
    outb(PIC2_CMD, 0x11);   io_wait();
    outb(PIC1_DATA, IRQ_BASE);     io_wait();  /* ICW2: マスターは 32 番から */
    outb(PIC2_DATA, IRQ_BASE + 8); io_wait();  /* ICW2: スレーブは 40 番から */
    outb(PIC1_DATA, 0x04);  io_wait();  /* ICW3: マスターの IRQ2 にスレーブがいる */
    outb(PIC2_DATA, 0x02);  io_wait();  /* ICW3: スレーブは IRQ2 につながっている */
    outb(PIC1_DATA, 0x01);  io_wait();  /* ICW4: 8086 モード */
    outb(PIC2_DATA, 0x01);  io_wait();

    /* マスク: 1 のビットの IRQ は止める。まず全部止め、使うものだけ pic_unmask で開ける */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_unmask(int irq)
{
    if (irq >= 8) {
        outb(PIC2_DATA, inb(PIC2_DATA) & ~(1 << (irq - 8)));
        irq = 2;            /* スレーブの割り込みはマスターの IRQ2 を通るので、そこも開ける */
    }
    outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << irq));
}

void pic_send_eoi(int irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}
