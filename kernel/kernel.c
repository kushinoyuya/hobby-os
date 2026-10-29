/*
 * kernel.c - C で書いたカーネル本体
 *
 * BIOS も標準ライブラリ(printf など)もないので、
 * 画面のメモリ(VRAM、0xB8000 番地)に直接書き込んで文字を表示する。
 */
#include <stdint.h>

/* テキスト画面: 80文字 x 25行。1文字は2バイト(下位8ビット = 文字、上位8ビット = 色) */
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
static volatile uint16_t *const vga = (volatile uint16_t *)0xB8000;

/* 色: 上位4ビットが背景、下位4ビットが文字 */
enum {
    COLOR_NORMAL  = 0x07,   /* 黒地に灰色 */
    COLOR_TITLE   = 0x1F,   /* 青地に白 */
    COLOR_OK      = 0x0A,   /* 黒地に明るい緑 */
};

static int cursor_row;
static int cursor_col;
static uint8_t text_color = COLOR_NORMAL;

static void put_at(int row, int col, char c, uint8_t color)
{
    vga[row * VGA_WIDTH + col] = (uint16_t)color << 8 | (uint8_t)c;
}

static void clear_screen(void)
{
    for (int row = 0; row < VGA_HEIGHT; row++)
        for (int col = 0; col < VGA_WIDTH; col++)
            put_at(row, col, ' ', COLOR_NORMAL);
    cursor_row = 0;
    cursor_col = 0;
}

/* 画面の一番下まで来たら、全体を1行上にずらす */
static void scroll_if_needed(void)
{
    if (cursor_row < VGA_HEIGHT)
        return;
    for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++)
        vga[i] = vga[i + VGA_WIDTH];
    for (int col = 0; col < VGA_WIDTH; col++)
        put_at(VGA_HEIGHT - 1, col, ' ', COLOR_NORMAL);
    cursor_row = VGA_HEIGHT - 1;
}

static void put_char(char c)
{
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else {
        put_at(cursor_row, cursor_col, c, text_color);
        if (++cursor_col == VGA_WIDTH) {
            cursor_col = 0;
            cursor_row++;
        }
    }
    scroll_if_needed();
}

static void print(const char *s)
{
    while (*s)
        put_char(*s++);
}

/* 32ビットの数を 0x0000ABCD の形で表示する */
static void print_hex(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    print("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        put_char(digits[(value >> shift) & 0xF]);
}

/* 初期値のないグローバル変数。entry.asm が .bss を0で埋めるので 0 のはず */
static uint32_t boot_count;

void kmain(void)
{
    clear_screen();

    text_color = COLOR_TITLE;
    print(" hobby-os                                                                       ");
    text_color = COLOR_NORMAL;

    print("\nHello from C!\n\n");

    print("kmain()      is at ");
    print_hex((uint32_t)(uintptr_t)kmain);
    print("   (machine code, from 0x8000)\n");

    print("VRAM         is at ");
    print_hex((uint32_t)(uintptr_t)vga);
    print("   (text screen memory)\n");

    print("boot_count   = ");
    print_hex(boot_count);
    print("   (.bss is zero-cleared)\n\n");

    text_color = COLOR_OK;
    print("32-bit protected mode + C kernel: OK\n");
    text_color = COLOR_NORMAL;
}
