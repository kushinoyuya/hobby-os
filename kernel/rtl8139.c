/*
 * rtl8139.c - ネットワークカード RTL8139 のドライバ
 *
 * RTL8139 は Realtek 社の古くて単純な 100Mbps のカード。QEMU が真似できるので
 * 教材によく使われる。カードとのやりとりは2種類ある:
 *
 *   1. I/O ポート(レジスタ): 「送って」「受信を始めて」などの命令や状態の確認
 *        io_base + 0x37 (CR) に書く、io_base + 0x3E (ISR) を読む …
 *   2. メモリ(DMA): フレームの中身そのもの
 *        カードが CPU を通さずに、メモリへ直接読み書きする
 *
 * 受信: 8KB の輪(リングバッファ)をカードに渡しておくと、届いたフレームを
 *       順番に書き込んでくれる。1つ1つの前に4バイトの見出しが付く。
 *
 *   rx_buf
 *   ┌────────┬───────────────┬────────┬──────────┬───┬─────
 *   │見出し 4│ フレーム1      │見出し 4│ フレーム2 │   │ …
 *   └────────┴───────────────┴────────┴──────────┴───┴─────
 *   ▲ rx_offset(次にここを読む)                  (4の倍数にそろえる)
 *     見出し = 状態(2バイト) + 長さ(2バイト、末尾の CRC 4バイトを含む)
 *
 *   読み終わったら CAPR に「ここまで読んだ」と書いて、カードに場所を返す。
 *
 * 送信: 送信用の箱(descriptor)が4つあり、順番に使う。
 *       箱 i の番地を TSAD[i] に、長さを TSD[i] に書くと送信が始まる。
 *       終わると TSD[i] の OWN ビットが 1 になる(= また使ってよい)。
 */
#include "rtl8139.h"
#include "io.h"
#include "idt.h"
#include "pmm.h"
#include "string.h"

#define RTL_VENDOR 0x10EC           /* Realtek */
#define RTL_DEVICE 0x8139

/* レジスタ(io_base からの位置) */
#define REG_MAC     0x00            /* MAC アドレス(6バイト) */
#define REG_TSD0    0x10            /* 送信の状態 箱0〜3(4バイトずつ) */
#define REG_TSAD0   0x20            /* 送信する番地 箱0〜3(4バイトずつ) */
#define REG_RBSTART 0x30            /* 受信の輪の番地 */
#define REG_CR      0x37            /* 命令(Command Register) */
#define REG_CAPR    0x38            /* 受信: ドライバがどこまで読んだか */
#define REG_IMR     0x3C            /* どの出来事で割り込むか(Interrupt Mask) */
#define REG_ISR     0x3E            /* 何が起きたか(Interrupt Status) */
#define REG_RCR     0x44            /* 受信の設定(Receive Config) */
#define REG_CONFIG1 0x52

/* CR のビット */
#define CR_BUFE  0x01               /* 受信の輪が空(読むものがない) */
#define CR_TE    0x04               /* 送信を有効に */
#define CR_RE    0x08               /* 受信を有効に */
#define CR_RST   0x10               /* リセット(終わると 0 に戻る) */

/* ISR / IMR のビット */
#define INT_ROK   0x0001            /* 受信できた */
#define INT_RER   0x0002            /* 受信でエラー */
#define INT_TOK   0x0004            /* 送信できた */
#define INT_TER   0x0008            /* 送信でエラー */
#define INT_RXOVW 0x0010            /* 受信の輪があふれた */

/* RCR のビット: どのフレームを受け取るか */
#define RCR_APM   0x0002            /* 宛先が自分の MAC アドレス */
#define RCR_AM    0x0004            /* マルチキャスト */
#define RCR_AB    0x0008            /* ブロードキャスト(宛先 FF:FF:FF:FF:FF:FF) */
#define RCR_WRAP  0x0080            /* 輪の終わりをはみ出して書いてよい(下の説明) */
#define RCR_MXDMA 0x0700            /* 1回の DMA の大きさに制限をつけない */

/* TSD のビット */
#define TSD_OWN   0x2000            /* 1 = カードの仕事が終わった(ドライバが使ってよい) */
#define TSD_TOK   0x8000            /* 送信できた */

/* 受信の状態(見出しの最初の2バイト)のビット */
#define RX_STATUS_ROK 0x0001

/*
 * 受信の輪は 8KB。ただし RCR_WRAP を付けると、輪の終わりをまたぐフレームを
 * 先頭に折り返さず、そのまま後ろへはみ出して書いてくれる(読む側が楽になる)。
 * そのため、はみ出す分(最大フレーム + 見出し)の余白を後ろに付けて借りる。
 */
#define RX_RING_SIZE 8192
#define RX_BUF_BYTES (RX_RING_SIZE + 16 + 1536)
#define TX_SLOTS     4
#define TX_SLOT_SIZE 1536           /* 1つの箱の大きさ(最大フレームより大きく、4の倍数) */

static struct rtl8139_info info;
static int ready;
static uint32_t rx_offset;          /* 受信の輪で、次に読む位置 */
static uint32_t tx_next;            /* 次に使う送信の箱 */

static uint32_t pages_for(uint32_t bytes)
{
    return (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
}

/*
 * 割り込み: 「何が起きたか」を読んで、カードに「見た」と伝えるだけ。
 * フレームを読み出すのは kmain のループ(rtl8139_receive)に任せる。
 * (キーボードと同じく「割り込みの中は短く」)
 */
static void rtl8139_irq(struct regs *r)
{
    (void)r;
    uint16_t status = inw(info.io_base + REG_ISR);
    outw(info.io_base + REG_ISR, status);   /* 1 を書いたビットが消える(= 見た) */
    info.irqs++;
    if (status & (INT_RER | INT_RXOVW))
        info.rx_errors++;
    if (status & INT_TER)
        info.tx_errors++;
}

int rtl8139_init(void)
{
    /* 1. PCI バスからカードを探す */
    if (!pci_find(RTL_VENDOR, RTL_DEVICE, &info.pci))
        return RTL8139_NOT_FOUND;

    /*
     * 2. BAR0 に I/O ポートの窓口の先頭番号が入っている(BIOS が決めてくれた)。
     *    下位2ビットは「I/O ポートかメモリか」の印なので取り除く。
     */
    info.io_base = pci_read32(&info.pci, PCI_BAR0) & ~3u;
    info.irq     = pci_read8(&info.pci, PCI_INTERRUPT_LINE);

    /* 3. I/O ポートでの読み書きと、カードが自分でメモリを読み書きする(DMA)のを許す */
    uint16_t cmd = pci_read16(&info.pci, PCI_COMMAND);
    pci_write16(&info.pci, PCI_COMMAND, cmd | PCI_CMD_IO | PCI_CMD_BUS_MASTER);

    uint16_t io = info.io_base;

    /* 4. 電源を入れて、リセットする */
    outb(io + REG_CONFIG1, 0x00);
    outb(io + REG_CR, CR_RST);
    int timeout = 1000000;
    while (inb(io + REG_CR) & CR_RST) {
        if (--timeout == 0)
            return RTL8139_RESET_TIMEOUT;
    }

    /* 5. MAC アドレス(カードごとに違う6バイトの番号)を読む */
    for (int i = 0; i < 6; i++)
        info.mac[i] = inb(io + REG_MAC + i);

    /*
     * 6. 受信の輪と送信の箱のメモリを、⑤ の pmm_alloc でページ単位で借りる。
     *    カードは番地を直接使ってメモリに書くので、「ひと続きの場所」でないと困る。
     *    kmalloc ではなくページで借りるのはそのため(ページの中はひと続きが保証される)。
     *    ページングをまだ使っていないので、C から見える番地 = カードに渡す番地。
     */
    info.rx_buf = pmm_alloc(pages_for(RX_BUF_BYTES));
    info.tx_buf = pmm_alloc(pages_for(TX_SLOTS * TX_SLOT_SIZE));
    if (!info.rx_buf || !info.tx_buf)
        return RTL8139_NO_MEMORY;
    memset((void *)(uintptr_t)info.rx_buf, 0, RX_BUF_BYTES);
    rx_offset = 0;
    tx_next = 0;

    /* 7. 受信の輪の番地を教える */
    outl(io + REG_RBSTART, info.rx_buf);

    /* 8. 「受信できた」「送信できた」とエラーで割り込むようにする */
    outw(io + REG_IMR, INT_ROK | INT_RER | INT_TOK | INT_TER | INT_RXOVW);

    /* 9. 受信の設定: 自分宛て・ブロードキャスト・マルチキャストを受け取る */
    outl(io + REG_RCR, RCR_APM | RCR_AM | RCR_AB | RCR_WRAP | RCR_MXDMA);

    /* 10. 送信と受信を始める */
    outb(io + REG_CR, CR_RE | CR_TE);

    /* 11. 割り込みを登録する(IRQ が決まっていない場合は、割り込みなしでも受信は読める) */
    if (info.irq < 16)
        irq_register(info.irq, rtl8139_irq);

    ready = 1;
    return RTL8139_OK;
}

const struct rtl8139_info *rtl8139_get_info(void)
{
    return ready ? &info : 0;
}

int rtl8139_send(const void *frame, uint32_t len)
{
    if (!ready || len > ETH_FRAME_MAX)
        return -1;

    uint16_t io = info.io_base;
    uint16_t tsd = REG_TSD0 + tx_next * 4;

    /* この箱の前の送信が終わっているか(OWN = 1)確かめる */
    int timeout = 1000000;
    while (!(inl(io + tsd) & TSD_OWN)) {
        if (--timeout == 0) {
            info.tx_errors++;
            return -1;
        }
    }

    /* 箱にフレームを写す。60バイトより短ければ、後ろを 0 で埋める */
    uint8_t *slot = (uint8_t *)(uintptr_t)(info.tx_buf + tx_next * TX_SLOT_SIZE);
    memcpy(slot, frame, len);
    if (len < ETH_FRAME_MIN) {
        memset(slot + len, 0, ETH_FRAME_MIN - len);
        len = ETH_FRAME_MIN;
    }

    /* 番地を教えてから長さを書くと、送信が始まる(長さを書いたときに OWN が 0 になる) */
    outl(io + REG_TSAD0 + tx_next * 4, (uint32_t)(uintptr_t)slot);
    outl(io + tsd, len);

    tx_next = (tx_next + 1) % TX_SLOTS;
    info.tx_frames++;
    return 0;
}

uint32_t rtl8139_receive(void *buf, uint32_t size)
{
    if (!ready)
        return 0;

    uint16_t io = info.io_base;
    if (inb(io + REG_CR) & CR_BUFE)         /* 輪が空なら、読むものはない */
        return 0;

    /* 見出し: 状態(2バイト)と長さ(2バイト)。番地は4の倍数なので、そのまま読める */
    const uint8_t *p = (const uint8_t *)(uintptr_t)(info.rx_buf + rx_offset);
    uint16_t status = *(const uint16_t *)p;
    uint16_t length = *(const uint16_t *)(p + 2);   /* 末尾の CRC(4バイト)込み */

    if (!(status & RX_STATUS_ROK) || length < 14 + 4 || length > ETH_FRAME_MAX + 4) {
        /*
         * 見出しが壊れている。どこから読めばよいか分からなくなったので、
         * カードが今書いている位置まで飛ばして、溜まっていた分は捨てる。
         */
        info.rx_errors++;
        rx_offset = inw(io + 0x3A) % RX_RING_SIZE;  /* 0x3A = CBR(カードが書いた位置) */
        outw(io + REG_CAPR, (uint16_t)(rx_offset - 16));
        return 0;
    }

    uint32_t frame_len = length - 4;        /* CRC は要らないので外す */
    uint32_t copy = frame_len < size ? frame_len : size;
    memcpy(buf, p + 4, copy);               /* WRAP のおかげで、輪の終わりを気にせず写せる */

    /* 次の見出しへ進む: 見出し4バイト + 長さ、を4の倍数に切り上げる */
    rx_offset = (rx_offset + 4 + length + 3) & ~3u;
    rx_offset %= RX_RING_SIZE;
    /* 「ここまで読んだ」とカードに伝える。昔からの決まりで 16 引いた値を書く */
    outw(io + REG_CAPR, (uint16_t)(rx_offset - 16));

    info.rx_frames++;
    return copy;
}
