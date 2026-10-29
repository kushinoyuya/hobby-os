/*
 * idt.h - 割り込みの登録と処理
 */
#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* isr.asm の isr_common がスタックに積んだ内容。並び順はスタックの低い番地から */
struct regs {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;   /* pusha が積んだもの */
    uint32_t int_no, err_code;                         /* isrN が積んだもの */
    uint32_t eip, cs, eflags;                          /* CPU が自動で積んだもの */
} __attribute__((packed));

typedef void (*irq_handler_t)(struct regs *r);

void idt_init(void);

/* 装置の割り込み(IRQ 0〜15)が来たときに呼ぶ関数を登録する */
void irq_register(int irq, irq_handler_t handler);

static inline void interrupts_enable(void)  { __asm__ volatile ("sti"); }
static inline void interrupts_disable(void) { __asm__ volatile ("cli"); }

/* 次の割り込みが来るまで CPU を休ませる */
static inline void cpu_halt(void) { __asm__ volatile ("hlt"); }

#endif
