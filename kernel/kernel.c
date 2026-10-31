/*
 * kernel.c - カーネル本体
 *
 * 各部品を初期化してから割り込みを有効にし、あとは
 * 「割り込みを待つ → 起きたことを処理する」をずっと繰り返す。
 * キーボードから1行ずつ読み、簡単なコマンド(help / mem / clear)を実行する。
 */
#include <stdint.h>
#include "vga.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "keyboard.h"
#include "memmap.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"

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

static void draw_title(void)
{
    vga_set_color(COLOR_TITLE);
    vga_print(" hobby-os                                                                       ");
    vga_set_color(COLOR_NORMAL);
    vga_print("\n");
}

/* 64ビットの値を 0x00000000_00000000 の形で表示する(E820 の番地は64ビットのため) */
static void print_hex64(uint64_t value)
{
    vga_print_hex((uint32_t)(value >> 32));
    vga_putchar('_');
    static const char digits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4)
        vga_putchar(digits[(uint32_t)value >> shift & 0xF]);
}

/*
 * kmalloc / kfree が正しく動くかを、起動時に確かめる。
 *   1. 3つ借りる → 番地が16の倍数で、重なっていないか
 *   2. 真ん中を返して、それより小さい大きさを借りる → 同じ場所が再利用されるか
 *   3. 全部返す → 区画がくっついて、空き区画1つに戻るか
 */
static int heap_self_test(void)
{
    uint8_t *a = kmalloc(100);
    uint8_t *b = kmalloc(2000);
    uint8_t *c = kmalloc(30);
    if (!a || !b || !c)
        return 0;
    if (((uint32_t)(uintptr_t)a | (uint32_t)(uintptr_t)b | (uint32_t)(uintptr_t)c) % 16 != 0)
        return 0;
    if (!(a + 100 <= b && b + 2000 <= c))
        return 0;

    memset(b, 0xAB, 2000);          /* 書き込んでも、隣の区画の見出しを壊さないこと */
    kfree(b);
    uint8_t *d = kmalloc(1000);
    if (d != b)
        return 0;

    kfree(a);
    kfree(c);
    kfree(d);

    struct heap_stats s;
    heap_get_stats(&s);
    return s.used == 0 && s.free_blocks == 1 && s.blocks == 1;
}

static void cmd_help(void)
{
    vga_print("help   show this list\n");
    vga_print("mem    show the memory map, pages and heap\n");
    vga_print("clear  clear the screen\n");
}

static void cmd_mem(void)
{
    static const char *const type_names[] = {
        "?", "usable", "reserved", "ACPI", "ACPI NVS", "bad",
    };

    const struct memmap *map = memmap_get();
    vga_print("Memory map from BIOS (E820):\n");
    for (uint32_t i = 0; i < map->count; i++) {
        const struct memmap_entry *e = &map->entries[i];
        vga_print("  ");
        print_hex64(e->base);
        vga_print(" - ");
        print_hex64(e->base + e->length - 1);
        vga_print("  ");
        if (e->type == MEMMAP_USABLE)
            vga_set_color(COLOR_OK);
        vga_print(e->type <= MEMMAP_BAD ? type_names[e->type] : "?");
        vga_set_color(COLOR_NORMAL);
        vga_print("\n");
    }

    vga_print("Pages : ");
    vga_print_dec(pmm_free_pages());
    vga_print(" free / ");
    vga_print_dec(pmm_total_pages());
    vga_print(" total (4 KB each, ");
    vga_print_dec(pmm_free_pages() / 256);
    vga_print(" MB free)\n");

    struct heap_stats s;
    heap_get_stats(&s);
    vga_print("Heap  : ");
    vga_print_dec(s.used);
    vga_print(" used / ");
    vga_print_dec(s.total);
    vga_print(" bytes, ");
    vga_print_dec(s.blocks);
    vga_print(" blocks (");
    vga_print_dec(s.free_blocks);
    vga_print(" free)\n");
}

static void run_command(const char *line)
{
    if (line[0] == '\0')
        return;
    if (strcmp(line, "help") == 0) {
        cmd_help();
    } else if (strcmp(line, "mem") == 0) {
        cmd_mem();
    } else if (strcmp(line, "clear") == 0) {
        vga_clear();
        draw_title();
    } else {
        vga_print("unknown command: ");
        vga_print(line);
        vga_print(" (type 'help')\n");
    }
}

void kmain(void)
{
    vga_clear();
    draw_title();

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

    vga_print("Memory   : ");
    pmm_init();
    vga_print_dec(memmap_get()->count);
    vga_print(" regions from BIOS, ");
    vga_print_dec(pmm_total_pages() / 256);
    vga_print(" MB usable ... ");
    if (pmm_total_pages() > 0) {
        vga_print("ok\n");
    } else {
        vga_set_color(COLOR_ERROR);
        vga_print("no usable memory\n");
        vga_set_color(COLOR_NORMAL);
    }

    vga_print("Heap     : kmalloc/kfree self test ... ");
    if (heap_self_test()) {
        vga_print("ok\n");
    } else {
        vga_set_color(COLOR_ERROR);
        vga_print("FAILED\n");
        vga_set_color(COLOR_NORMAL);
    }

    interrupts_enable();        /* ここから割り込みが来るようになる */

    vga_set_color(COLOR_OK);
    vga_print("\nReady. Type 'help' for commands.\n\n");
    vga_set_color(COLOR_NORMAL);
    vga_print("> ");

    char line[64];              /* 打ち込み中の1行 */
    uint32_t len = 0;
    uint32_t last_sec = (uint32_t)-1;
    for (;;) {
        /* キーボードの割り込みがバッファに入れた文字を、1行分ためる */
        int c;
        while ((c = keyboard_getchar()) >= 0) {
            if (c == '\n') {
                vga_putchar('\n');
                line[len] = '\0';
                run_command(line);
                len = 0;
                vga_print("> ");
            } else if (c == '\b') {
                if (len > 0) {          /* プロンプトの "> " までは消さない */
                    len--;
                    vga_putchar('\b');
                }
            } else if (len < sizeof(line) - 1) {
                line[len++] = (char)c;
                vga_putchar((char)c);
            }
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
