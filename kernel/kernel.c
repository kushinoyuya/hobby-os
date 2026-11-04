/*
 * kernel.c - カーネル本体
 *
 * 各部品を初期化してから割り込みを有効にし、あとは
 * 「割り込みを待つ → 起きたことを処理する」をずっと繰り返す。
 * キーボードから1行ずつ読み、簡単なコマンド(help / mem / pci / net / …)を実行する。
 * ネットワークカードにフレームが届いていたら、その中身を表示する。
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
#include "pci.h"
#include "rtl8139.h"

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
    vga_print("pci    list the devices on the PCI bus\n");
    vga_print("net    show the network card (RTL8139)\n");
    vga_print("nettest  send an ARP request to 10.0.2.2 and show the reply\n");
    vga_print("clear  clear the screen\n");
}

/* ---- ネットワーク(⑥) ---- */

/* MAC アドレスを 52:54:00:12:34:56 の形で表示する */
static void print_mac(const uint8_t *mac)
{
    for (int i = 0; i < 6; i++) {
        if (i > 0)
            vga_putchar(':');
        vga_print_hexn(mac[i], 2);
    }
}

static void print_ip(const uint8_t *ip)
{
    for (int i = 0; i < 4; i++) {
        if (i > 0)
            vga_putchar('.');
        vga_print_dec(ip[i]);
    }
}

/* 中身を16バイトずつ16進数で表示する(長いものは最初の64バイトだけ) */
static void hexdump(const uint8_t *data, uint32_t len)
{
    uint32_t shown = len < 64 ? len : 64;
    for (uint32_t i = 0; i < shown; i += 16) {
        vga_print("  ");
        vga_print_hexn(i, 4);
        vga_print(" ");
        for (uint32_t j = i; j < i + 16 && j < shown; j++) {
            vga_putchar(' ');
            vga_print_hexn(data[j], 2);
        }
        vga_putchar('\n');
    }
    if (shown < len)
        vga_print("  ...\n");
}

static uint16_t read_be16(const uint8_t *p)    /* ネットワークの数は上位バイトが先(ビッグエンディアン) */
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

/*
 * Ethernet フレームの見出し(14バイト)を読んで表示する
 *   0-5 : 宛先の MAC アドレス
 *   6-11: 送り主の MAC アドレス
 *   12-13: 中身の種類(0x0800 = IPv4, 0x0806 = ARP, 0x86DD = IPv6)
 */
static void show_frame(const char *dir, const uint8_t *f, uint32_t len)
{
    uint16_t type = read_be16(f + 12);
    vga_set_color(COLOR_OK);
    vga_print(dir);
    vga_set_color(COLOR_NORMAL);
    vga_print(" ");
    vga_print_dec(len);
    vga_print(" bytes  ");
    print_mac(f + 6);
    vga_print(" -> ");
    print_mac(f);
    vga_print("  type ");
    vga_print_hexn(type, 4);
    vga_print(type == 0x0806 ? " (ARP)" : type == 0x0800 ? " (IPv4)" :
              type == 0x86DD ? " (IPv6)" : "");
    vga_putchar('\n');
    hexdump(f, len);
}

static void cmd_pci_visit(const struct pci_device *d, void *ctx)
{
    (void)ctx;
    vga_print("  ");
    vga_print_hexn(d->bus, 2);
    vga_putchar(':');
    vga_print_hexn(d->dev, 2);
    vga_putchar('.');
    vga_print_hexn(d->func, 1);
    vga_print("  ");
    vga_print_hexn(d->vendor, 4);
    vga_putchar(':');
    vga_print_hexn(d->device, 4);
    vga_print("  ");
    vga_print(pci_class_name(d->class_code));
    if (d->vendor == 0x10EC && d->device == 0x8139)
        vga_print("  <- RTL8139");
    vga_putchar('\n');
}

static void cmd_pci(void)
{
    vga_print("PCI devices (bus:dev.func  vendor:device  class):\n");
    int n = pci_scan(cmd_pci_visit, 0);
    vga_print_dec(n);
    vga_print(" devices\n");
}

static void cmd_net(void)
{
    const struct rtl8139_info *n = rtl8139_get_info();
    if (!n) {
        vga_print("no network card\n");
        return;
    }
    vga_print("RTL8139 at PCI ");
    vga_print_hexn(n->pci.bus, 2);
    vga_putchar(':');
    vga_print_hexn(n->pci.dev, 2);
    vga_putchar('.');
    vga_print_hexn(n->pci.func, 1);
    vga_print("\n  MAC address : ");
    print_mac(n->mac);
    vga_print("\n  I/O ports   : ");
    vga_print_hex(n->io_base);
    vga_print(" - ");
    vga_print_hex(n->io_base + 0xFF);       /* RTL8139 のレジスタは 256 バイト分 */
    vga_print("\n  IRQ         : ");
    vga_print_dec(n->irq);
    vga_print("\n  RX ring     : ");
    vga_print_hex(n->rx_buf);
    vga_print("  TX buffers: ");
    vga_print_hex(n->tx_buf);
    vga_print("\n  frames      : ");
    vga_print_dec(n->rx_frames);
    vga_print(" received, ");
    vga_print_dec(n->tx_frames);
    vga_print(" sent, ");
    vga_print_dec(n->irqs);
    vga_print(" interrupts, ");
    vga_print_dec(n->rx_errors + n->tx_errors);
    vga_print(" errors\n");
}

/*
 * 送受信の確認: 「10.0.2.2 の MAC アドレスを知っている人は?」という ARP の問い合わせを
 * 手で組み立てて、全員宛て(ブロードキャスト)に送る。
 * QEMU の仮想ネットワーク(user モード)では 10.0.2.2 がゲートウェイ役なので、返事が来るはず。
 * (ARP のちゃんとした処理は ⑦ で作る。ここでは「送れる・受け取れる」の確認だけ)
 */
static void cmd_nettest(void)
{
    const struct rtl8139_info *n = rtl8139_get_info();
    if (!n) {
        vga_print("no network card\n");
        return;
    }

    static const uint8_t my_ip[4]     = { 10, 0, 2, 15 };  /* QEMU が客に割り当てる番号 */
    static const uint8_t target_ip[4] = { 10, 0, 2, 2 };

    uint8_t f[42];
    memset(f, 0xFF, 6);                 /* 宛先: FF:FF:FF:FF:FF:FF(全員) */
    memcpy(f + 6, n->mac, 6);           /* 送り主: 自分 */
    f[12] = 0x08; f[13] = 0x06;         /* 種類: ARP */
    /* ここから ARP(28バイト) */
    f[14] = 0x00; f[15] = 0x01;         /* ハードウェアの種類: Ethernet */
    f[16] = 0x08; f[17] = 0x00;         /* プロトコルの種類: IPv4 */
    f[18] = 6;                          /* MAC アドレスの長さ */
    f[19] = 4;                          /* IP アドレスの長さ */
    f[20] = 0x00; f[21] = 0x01;         /* 1 = 問い合わせ(request) */
    memcpy(f + 22, n->mac, 6);          /* 問い合わせた人の MAC */
    memcpy(f + 28, my_ip, 4);           /* 問い合わせた人の IP */
    memset(f + 32, 0, 6);               /* 知りたい MAC(まだ分からないので 0) */
    memcpy(f + 38, target_ip, 4);       /* 知りたい人の IP */

    vga_print("ARP request: who has ");
    print_ip(target_ip);
    vga_print("? tell ");
    print_ip(my_ip);
    vga_putchar('\n');
    show_frame("TX", f, sizeof(f));
    if (rtl8139_send(f, sizeof(f)) != 0) {
        vga_set_color(COLOR_ERROR);
        vga_print("send failed\n");
        vga_set_color(COLOR_NORMAL);
        return;
    }

    /* 返事を最大 0.5 秒待つ */
    static uint8_t rx[ETH_FRAME_MAX];
    uint32_t start = timer_ticks();
    while (timer_ticks() - start < TIMER_HZ / 2) {
        uint32_t len = rtl8139_receive(rx, sizeof(rx));
        if (len == 0) {
            cpu_halt();
            continue;
        }
        show_frame("RX", rx, len);
        /* ARP の返事(種類 0x0806、操作 2)なら、答えを読み取って表示する */
        if (len >= 42 && read_be16(rx + 12) == 0x0806 && read_be16(rx + 20) == 2) {
            vga_set_color(COLOR_OK);
            vga_print("ARP reply: ");
            print_ip(rx + 28);
            vga_print(" is at ");
            print_mac(rx + 22);
            vga_set_color(COLOR_NORMAL);
            vga_putchar('\n');
            return;
        }
    }
    vga_print("no ARP reply within 0.5 s\n");
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
    } else if (strcmp(line, "pci") == 0) {
        cmd_pci();
    } else if (strcmp(line, "net") == 0) {
        cmd_net();
    } else if (strcmp(line, "nettest") == 0) {
        cmd_nettest();
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

    vga_print("PCI      : ");
    vga_print_dec(pci_scan(0, 0));
    vga_print(" devices ... ok\n");

    vga_print("Network  : RTL8139 ... ");
    int net = rtl8139_init();       /* ページを借りるので pmm_init の後 */
    if (net == RTL8139_OK) {
        const struct rtl8139_info *n = rtl8139_get_info();
        vga_print("MAC ");
        print_mac(n->mac);
        vga_print(", IRQ ");
        vga_print_dec(n->irq);
        vga_print(" ... ok\n");
    } else {
        static const char *const reasons[] = {
            "", "not found", "no memory", "reset timeout",
        };
        vga_set_color(COLOR_ERROR);
        vga_print(reasons[net]);
        vga_set_color(COLOR_NORMAL);
        vga_print("\n");
    }

    interrupts_enable();       /* ここから割り込みが来るようになる */

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

        /*
         * ネットワークカードにフレームが届いていたら表示する。
         * 割り込みは「届いた」と知らせて CPU を起こすだけで、読み出すのはここ。
         * 打ちかけの行があれば、表示の後に打ち直しておく。
         */
        static uint8_t frame[ETH_FRAME_MAX];
        uint32_t flen;
        while ((flen = rtl8139_receive(frame, sizeof(frame))) > 0) {
            vga_putchar('\n');
            show_frame("RX", frame, flen);
            vga_print("> ");
            for (uint32_t i = 0; i < len; i++)
                vga_putchar(line[i]);
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
