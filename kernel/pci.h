/*
 * pci.h - PCI バス(装置を探す・設定を読み書きする)
 */
#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* 見つかった装置1つ分の情報 */
struct pci_device {
    uint8_t  bus, dev, func;    /* 場所: バス番号・装置番号・機能番号(00:03.0 のように書く) */
    uint16_t vendor;            /* 作った会社の番号(0x10EC = Realtek など) */
    uint16_t device;            /* 製品の番号(0x8139 = RTL8139 など) */
    uint8_t  class_code;        /* 種類(0x02 = ネットワーク など) */
    uint8_t  subclass;
};

/* 設定空間(装置ごとに 256 バイトある設定の表)の offset 番目を読み書きする */
uint32_t pci_read32(const struct pci_device *d, uint8_t offset);
uint16_t pci_read16(const struct pci_device *d, uint8_t offset);
uint8_t  pci_read8(const struct pci_device *d, uint8_t offset);
void     pci_write16(const struct pci_device *d, uint8_t offset, uint16_t value);

/* 全部の装置について visit(装置, ctx) を呼ぶ。見つかった数を返す */
typedef void (*pci_visit_t)(const struct pci_device *d, void *ctx);
int pci_scan(pci_visit_t visit, void *ctx);

/* vendor:device の装置を探す。見つかれば *out に入れて 1、なければ 0 */
int pci_find(uint16_t vendor, uint16_t device, struct pci_device *out);

/* 種類の番号を名前にする("network" など) */
const char *pci_class_name(uint8_t class_code);

/* 設定空間のよく使う場所 */
#define PCI_VENDOR_ID      0x00
#define PCI_DEVICE_ID      0x02
#define PCI_COMMAND        0x04
#define PCI_CLASS          0x08     /* 0x0B が種類、0x0A がその細かい分類 */
#define PCI_HEADER_TYPE    0x0E
#define PCI_BAR0           0x10     /* 装置の窓口がどこにあるか(Base Address Register) */
#define PCI_INTERRUPT_LINE 0x3C     /* どの IRQ を使うか(BIOS が決めて書いてくれる) */

/* PCI_COMMAND のビット */
#define PCI_CMD_IO         0x0001   /* I/O ポートでの読み書きを許す */
#define PCI_CMD_MEMORY     0x0002   /* メモリ番地での読み書きを許す */
#define PCI_CMD_BUS_MASTER 0x0004   /* 装置が自分からメモリを読み書きする(DMA)のを許す */

#endif
