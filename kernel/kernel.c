/*
 * kernel.c - カーネル本体
 *
 * 各部品を初期化してから割り込みを有効にし、あとは
 * 「割り込みを待つ → 起きたことを処理する」をずっと繰り返す。
 */
#include <stdint.h>
#include "vga.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "keyboard.h"

/* 1行目(タイトルバー)の右側に、起動してからの時間を表示する */
static void show_uptime(uint32_t ticks)
{
    uint32_t sec = ticks / TIMER_HZ;
    char text[] = "uptime 00:00:00 ";
    text[7]  = '0' + (sec / 3600) % 24 / 10;
    text[8]  = '0' + (sec / 3600) % 24 % 10;
    text[10] = '0' + (sec / 60) % 60 / 10;
    text[11] = '0' + (sec / 60) % 60 % 10;
    text[13] = '0' + sec % 60 / 10;
    text[14] = '0' + sec % 60 % 10;
    vga_write_at(0, VGA_WIDTH - (int)sizeof(text) + 1, text, COLOR_TITLE);
}

void kmain(void)
{
    vga_clear();
    vga_set_color(COLOR_TITLE);
    vga_print(" hobby-os                                                                       ");
    vga_set_color(COLOR_NORMAL);
    vga_print("\n");

    vga_print("PIC      : remap IRQ 0-15 to interrupt 32-47 ... ");
    pic_init();
    vga_print("ok\n");

    vga_print("IDT      : register interrupt handlers 0-47 ... ");
    idt_init();
    vga_print("ok\n");

    vga_print("Timer    : IRQ0, ");
    vga_print_dec(TIMER_HZ);
    vga_print(" Hz ... ");
    timer_init();
    vga_print("ok\n");

    vga_print("Keyboard : IRQ1 ... ");
    keyboard_init();
    vga_print("ok\n");

    interrupts_enable();        /* ここから割り込みが来るようになる */

    vga_set_color(COLOR_OK);
    vga_print("\nInterrupts enabled. Type something!\n\n");
    vga_set_color(COLOR_NORMAL);
    vga_print("> ");

    uint32_t last_sec = (uint32_t)-1;
    for (;;) {
        /* キーボードの割り込みがバッファに入れた文字を表示する */
        int c;
        while ((c = keyboard_getchar()) >= 0) {
            vga_putchar((char)c);
            if (c == '\n')
                vga_print("> ");
        }

        /* 1秒ごとに時間の表示を更新する */
        uint32_t ticks = timer_ticks();
        if (ticks / TIMER_HZ != last_sec) {
            last_sec = ticks / TIMER_HZ;
            show_uptime(ticks);
        }

        cpu_halt();             /* 次の割り込み(最低でも10ミリ秒後のタイマー)まで休む */
    }
}
