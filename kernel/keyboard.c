/*
 * keyboard.c - PS/2 キーボード
 *
 * キーを押す・離すたびに IRQ1 が来る。ポート 0x60 を読むと「スキャンコード」
 * (どのキーが押されたか・離されたかを表す番号)が取れるので、文字に変換する。
 *
 * 割り込みの中では「文字をバッファに入れる」だけにして、表示などの処理は
 * kmain() のループで行う。割り込みの処理は短くするのが鉄則
 * (ネットワークカードの受信も同じ形にする予定)。
 */
#include <stdint.h>
#include "keyboard.h"
#include "idt.h"
#include "io.h"

#define KBD_DATA 0x60

/* スキャンコード(セット1)→ 文字 の対応表(US 配列)。0 は文字にならないキー */
static const char keymap[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0,   ' ',
};

static const char keymap_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0,   ' ',
};

#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_RELEASED 0x80        /* 離したときは、押したときのコード + 0x80 */
#define SC_EXTENDED 0xE0        /* 矢印キーなどの前に付く印 */

/* リングバッファ: 割り込み(書く側)と kmain(読む側)の受け渡し場所 */
#define BUF_SIZE 64
static volatile char buf[BUF_SIZE];
static volatile uint32_t buf_head;     /* 次に書く位置(割り込みだけが進める) */
static volatile uint32_t buf_tail;     /* 次に読む位置(kmain だけが進める) */

static int shift_pressed;
static int extended;

static void buf_put(char c)
{
    uint32_t next = (buf_head + 1) % BUF_SIZE;
    if (next == buf_tail)       /* いっぱいなら捨てる */
        return;
    buf[buf_head] = c;
    buf_head = next;
}

static void keyboard_irq(struct regs *r)
{
    (void)r;
    uint8_t sc = inb(KBD_DATA);

    if (sc == SC_EXTENDED) {    /* 次のコードは矢印キーなど。今は無視する */
        extended = 1;
        return;
    }
    if (extended) {
        extended = 0;
        return;
    }

    if (sc == SC_LSHIFT || sc == SC_RSHIFT) {
        shift_pressed = 1;
        return;
    }
    if (sc == (SC_LSHIFT | SC_RELEASED) || sc == (SC_RSHIFT | SC_RELEASED)) {
        shift_pressed = 0;
        return;
    }
    if (sc & SC_RELEASED)       /* その他のキーを離したときは何もしない */
        return;

    char c = shift_pressed ? keymap_shift[sc] : keymap[sc];
    if (c)
        buf_put(c);
}

void keyboard_init(void)
{
    irq_register(1, keyboard_irq);
}

int keyboard_getchar(void)
{
    if (buf_tail == buf_head)
        return -1;
    char c = buf[buf_tail];
    buf_tail = (buf_tail + 1) % BUF_SIZE;
    return (unsigned char)c;
}
