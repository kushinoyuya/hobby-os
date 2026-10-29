/*
 * vga.c - テキスト画面への表示
 *
 * BIOS も標準ライブラリもないので、画面のメモリ(VRAM、0xB8000 番地)に直接書き込む。
 * 1文字は2バイト(下位8ビット = 文字、上位8ビット = 色)。
 */
#include "vga.h"
#include "io.h"

static volatile uint16_t *const vga = (volatile uint16_t *)0xB8000;

static int cursor_row;
static int cursor_col;
static uint8_t text_color = COLOR_NORMAL;

static void put_at(int row, int col, char c, uint8_t color)
{
    vga[row * VGA_WIDTH + col] = (uint16_t)color << 8 | (uint8_t)c;
}

/* 点滅するカーソル(ハードウェアカーソル)を今の位置に動かす。
 * VGA のポート 0x3D4 で「どの設定か」を選び、0x3D5 に値を書く。 */
static void update_hw_cursor(void)
{
    uint16_t pos = cursor_row * VGA_WIDTH + cursor_col;
    outb(0x3D4, 0x0F);              /* カーソル位置の下位8ビット */
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);              /* カーソル位置の上位8ビット */
    outb(0x3D5, pos >> 8);
}

void vga_clear(void)
{
    for (int row = 0; row < VGA_HEIGHT; row++)
        for (int col = 0; col < VGA_WIDTH; col++)
            put_at(row, col, ' ', COLOR_NORMAL);
    cursor_row = 0;
    cursor_col = 0;
    update_hw_cursor();
}

void vga_set_color(uint8_t color)
{
    text_color = color;
}

/* 画面の一番下まで来たら、全体を1行上にずらす(1行目のタイトルは残す) */
static void scroll_if_needed(void)
{
    if (cursor_row < VGA_HEIGHT)
        return;
    for (int i = VGA_WIDTH; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++)
        vga[i] = vga[i + VGA_WIDTH];
    for (int col = 0; col < VGA_WIDTH; col++)
        put_at(VGA_HEIGHT - 1, col, ' ', COLOR_NORMAL);
    cursor_row = VGA_HEIGHT - 1;
}

void vga_putchar(char c)
{
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            put_at(cursor_row, cursor_col, ' ', text_color);
        }
    } else {
        put_at(cursor_row, cursor_col, c, text_color);
        if (++cursor_col == VGA_WIDTH) {
            cursor_col = 0;
            cursor_row++;
        }
    }
    scroll_if_needed();
    update_hw_cursor();
}

void vga_print(const char *s)
{
    while (*s)
        vga_putchar(*s++);
}

void vga_print_hex(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    vga_print("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        vga_putchar(digits[(value >> shift) & 0xF]);
}

void vga_print_dec(uint32_t value)
{
    char buf[11];               /* 32ビットの最大値 4294967295 は10桁 + 終端 */
    int i = sizeof(buf) - 1;
    buf[i] = '\0';
    do {
        buf[--i] = '0' + value % 10;
        value /= 10;
    } while (value > 0);
    vga_print(&buf[i]);
}

void vga_write_at(int row, int col, const char *s, uint8_t color)
{
    while (*s && col < VGA_WIDTH)
        put_at(row, col++, *s++, color);
}
