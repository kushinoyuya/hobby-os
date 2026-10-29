/*
 * vga.h - テキスト画面(80文字 x 25行)への表示
 */
#ifndef VGA_H
#define VGA_H

#include <stdint.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

/* 色: 上位4ビットが背景、下位4ビットが文字 */
enum {
    COLOR_NORMAL = 0x07,    /* 黒地に灰色 */
    COLOR_TITLE  = 0x1F,    /* 青地に白 */
    COLOR_OK     = 0x0A,    /* 黒地に明るい緑 */
    COLOR_ERROR  = 0x4F,    /* 赤地に白 */
};

void vga_clear(void);
void vga_set_color(uint8_t color);
void vga_putchar(char c);               /* '\n' は改行、'\b' は1文字消す */
void vga_print(const char *s);
void vga_print_hex(uint32_t value);     /* 0x0000ABCD の形 */
void vga_print_dec(uint32_t value);     /* 10進数 */

/* カーソルを動かさずに、決まった位置に文字列を書く(ステータス表示用) */
void vga_write_at(int row, int col, const char *s, uint8_t color);

#endif
