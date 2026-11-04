/*
 * pci.c - PCI バス
 *
 * ネットワークカードやディスクなどの装置は PCI バスにつながっている。
 * キーボード(ポート 0x60)や PIC(0x20)のように「昔から決まった番号」がないので、
 * まず「どこに何がつながっているか」を1つずつ聞いて回る必要がある。
 *
 * 聞き方: 2つの I/O ポートを使う
 *   0xCF8 (CONFIG_ADDRESS) に「どの装置の、設定の何バイト目か」を書く
 *   0xCFC (CONFIG_DATA)    を読むと、その4バイトが返ってくる
 *
 * 0xCF8 に書く値(32ビット):
 *   31     30-24  23-16   15-11    10-8     7-0
 *   [有効] [ 0 ]  [バス]  [装置]   [機能]   [設定の何バイト目か(4の倍数)]
 *
 * いない場所を聞くと vendor が 0xFFFF で返ってくる。
 */
#include "pci.h"
#include "io.h"

#define CONFIG_ADDRESS 0xCF8
#define CONFIG_DATA    0xCFC

static void select(const struct pci_device *d, uint8_t offset)
{
    uint32_t addr = 0x80000000u
                  | (uint32_t)d->bus  << 16
                  | (uint32_t)d->dev  << 11
                  | (uint32_t)d->func << 8
                  | (offset & 0xFC);        /* 4バイト単位でしか選べない */
    outl(CONFIG_ADDRESS, addr);
}

uint32_t pci_read32(const struct pci_device *d, uint8_t offset)
{
    select(d, offset);
    return inl(CONFIG_DATA);
}

/* 2バイト・1バイトは、4バイト読んでから必要な部分を取り出す */
uint16_t pci_read16(const struct pci_device *d, uint8_t offset)
{
    return pci_read32(d, offset) >> ((offset & 2) * 8);
}

uint8_t pci_read8(const struct pci_device *d, uint8_t offset)
{
    return pci_read32(d, offset) >> ((offset & 3) * 8);
}

/* 2バイト書く: 4バイト読んで、その部分だけ入れ替えて書き戻す */
void pci_write16(const struct pci_device *d, uint8_t offset, uint16_t value)
{
    uint32_t shift = (offset & 2) * 8;
    uint32_t v = pci_read32(d, offset);
    v = (v & ~(0xFFFFu << shift)) | (uint32_t)value << shift;
    select(d, offset);
    outl(CONFIG_DATA, v);
}

int pci_scan(pci_visit_t visit, void *ctx)
{
    int found = 0;
    struct pci_device d;

    /* バス 0〜255 × 装置 0〜31 × 機能 0〜7 を総当たりで聞いて回る */
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                d.bus = bus;
                d.dev = dev;
                d.func = func;
                d.vendor = pci_read16(&d, PCI_VENDOR_ID);
                if (d.vendor == 0xFFFF) {           /* ここには何もいない */
                    if (func == 0)
                        break;                      /* 機能0がなければ、その装置ごといない */
                    continue;
                }
                d.device     = pci_read16(&d, PCI_DEVICE_ID);
                d.class_code = pci_read8(&d, PCI_CLASS + 3);
                d.subclass   = pci_read8(&d, PCI_CLASS + 2);
                found++;
                if (visit)
                    visit(&d, ctx);

                /* ヘッダの種類の最上位ビットが 0 なら、機能は 0 だけ(1〜7 は聞かなくてよい) */
                if (func == 0 && !(pci_read8(&d, PCI_HEADER_TYPE) & 0x80))
                    break;
            }
        }
    }
    return found;
}

struct find_ctx {
    uint16_t vendor, device;
    int found;
    struct pci_device *out;
};

static void find_visit(const struct pci_device *d, void *ctx)
{
    struct find_ctx *f = ctx;
    if (!f->found && d->vendor == f->vendor && d->device == f->device) {
        *f->out = *d;
        f->found = 1;
    }
}

int pci_find(uint16_t vendor, uint16_t device, struct pci_device *out)
{
    struct find_ctx f = { vendor, device, 0, out };
    pci_scan(find_visit, &f);
    return f.found;
}

const char *pci_class_name(uint8_t class_code)
{
    static const char *const names[] = {
        "old", "storage", "network", "display", "multimedia",
        "memory", "bridge", "serial", "system", "input",
    };
    if (class_code < sizeof(names) / sizeof(names[0]))
        return names[class_code];
    return "other";
}
