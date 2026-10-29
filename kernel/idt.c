/*
 * idt.c - IDT(割り込みディスクリプタテーブル)の設定と、割り込みの振り分け
 *
 * IDT は「割り込み番号 N が来たら、どの番地に飛ぶか」を CPU に教える表。
 * 1行8バイトで最大256行。ここでは 0〜47 番に isr.asm の入り口を登録する。
 */
#include "idt.h"
#include "pic.h"
#include "vga.h"

struct idt_entry {
    uint16_t offset_low;    /* 飛び先の番地(下位16ビット) */
    uint16_t selector;      /* 飛び先のコードセグメント(GDT の 0x08) */
    uint8_t  zero;          /* 常に0 */
    uint8_t  type_attr;     /* 種類と属性 */
    uint16_t offset_high;   /* 飛び先の番地(上位16ビット) */
} __attribute__((packed));

struct idt_ptr {            /* lidt 命令に渡す「IDT の大きさと場所」 */
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

#define KERNEL_CODE_SEG 0x08
#define IDT_INTERRUPT_GATE 0x8E /* 存在する / 特権レベル0 / 32ビット割り込みゲート */

static struct idt_entry idt[256];
static irq_handler_t irq_handlers[16];

extern uint32_t isr_stub_table[48];     /* isr.asm で定義 */

static void set_gate(int n, uint32_t handler)
{
    idt[n].offset_low  = handler & 0xFFFF;
    idt[n].selector    = KERNEL_CODE_SEG;
    idt[n].zero        = 0;
    idt[n].type_attr   = IDT_INTERRUPT_GATE;
    idt[n].offset_high = handler >> 16;
}

void idt_init(void)
{
    for (int n = 0; n < 48; n++)
        set_gate(n, isr_stub_table[n]);

    struct idt_ptr ptr = { sizeof(idt) - 1, (uint32_t)(uintptr_t)idt };
    __asm__ volatile ("lidt %0" : : "m"(ptr));
}

void irq_register(int irq, irq_handler_t handler)
{
    irq_handlers[irq] = handler;
    pic_unmask(irq);        /* PIC でこの IRQ を通すようにする */
}

static const char *const exception_names[32] = {
    "Divide by zero", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS", "Segment not present",
    "Stack-segment fault", "General protection fault", "Page fault", "Reserved",
    "x87 floating-point", "Alignment check", "Machine check", "SIMD floating-point",
    "Virtualization", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "VMM communication", "Security", "Reserved",
};

/* CPU の例外: 回復のしようがないので、内容を表示して止まる */
static void handle_exception(struct regs *r)
{
    vga_set_color(COLOR_ERROR);
    vga_print("\n EXCEPTION: ");
    vga_print(exception_names[r->int_no]);
    vga_print(" (#");
    vga_print_dec(r->int_no);
    vga_print(") at EIP=");
    vga_print_hex(r->eip);
    vga_print(" error=");
    vga_print_hex(r->err_code);
    vga_print(" ");
    for (;;) {
        interrupts_disable();
        cpu_halt();
    }
}

/* isr.asm の isr_common から、すべての割り込みで呼ばれる */
void isr_handler(struct regs *r)
{
    if (r->int_no < 32) {
        handle_exception(r);
        return;
    }

    int irq = r->int_no - 32;
    if (irq_handlers[irq])
        irq_handlers[irq](r);
    pic_send_eoi(irq);      /* 「処理が終わった」と PIC に伝える。忘れると次が来ない */
}
