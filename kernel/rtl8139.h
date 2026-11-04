/*
 * rtl8139.h - ネットワークカード RTL8139 のドライバ
 */
#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>
#include "pci.h"

#define ETH_FRAME_MAX 1514      /* Ethernet フレームの最大(見出し14 + 中身1500。CRC は含まない) */
#define ETH_FRAME_MIN 60        /* これより短いフレームは 0 で埋めて送る決まり */

/* rtl8139_init の結果 */
enum {
    RTL8139_OK = 0,
    RTL8139_NOT_FOUND,          /* PCI バスに RTL8139 がいない(QEMU の起動オプションを確認) */
    RTL8139_NO_MEMORY,          /* 受信・送信用のページが借りられない */
    RTL8139_RESET_TIMEOUT,      /* リセットが終わらない */
};

struct rtl8139_info {
    struct pci_device pci;      /* PCI バスのどこにいるか */
    uint16_t io_base;           /* I/O ポートの窓口の先頭番号 */
    uint8_t  irq;               /* 使う IRQ の番号(16 以上なら割り込みなし) */
    uint8_t  mac[6];            /* このカードの MAC アドレス */
    uint32_t rx_buf;            /* 受信用の輪(リングバッファ)の番地 */
    uint32_t tx_buf;            /* 送信用の4つの箱の番地 */

    /* 数えておくもの */
    uint32_t irqs;              /* 割り込みが来た回数 */
    uint32_t rx_frames;         /* 受け取ったフレームの数 */
    uint32_t tx_frames;         /* 送ったフレームの数 */
    uint32_t rx_errors;
    uint32_t tx_errors;
};

/* カードを探して初期化する。pmm_init の後に呼ぶ */
int rtl8139_init(void);

/* 初期化できていれば情報を、できていなければ NULL を返す */
const struct rtl8139_info *rtl8139_get_info(void);

/* フレームを1つ送る。成功なら 0、失敗なら -1 */
int rtl8139_send(const void *frame, uint32_t len);

/*
 * 届いたフレームを1つ取り出して buf に写す。
 * 返り値はフレームの長さ(バイト)。何も届いていなければ 0。
 */
uint32_t rtl8139_receive(void *buf, uint32_t size);

#endif
