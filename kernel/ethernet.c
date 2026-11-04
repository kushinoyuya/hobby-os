/*
 * ethernet.c - Ethernet 層
 *
 * フレームの見出し(14バイト)を読んで、中身の種類に応じて上の層に渡す。
 *
 *    0        6        12    14
 *    ┌────────┬────────┬─────┬───────────────┐
 *    │宛先 MAC │送り主MAC│種類 │ 中身(ARP や IP) │
 *    └────────┴────────┴─────┴───────────────┘
 */
#include "net.h"
#include "rtl8139.h"
#include "string.h"

const uint8_t eth_broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void eth_input(const uint8_t *frame, uint32_t len)
{
    if (len < ETH_HDR_LEN)
        return;

    const uint8_t *payload = frame + ETH_HDR_LEN;
    uint32_t payload_len = len - ETH_HDR_LEN;

    switch (get_be16(frame + 12)) {
    case ETH_TYPE_ARP:
        arp_input(payload, payload_len);
        break;
    case ETH_TYPE_IPV4:
        ip_input(payload, payload_len);
        break;
    default:
        break;      /* IPv6 など、扱わない種類は捨てる */
    }
}

int eth_send(const uint8_t *dst_mac, uint16_t type, const void *payload, uint32_t len)
{
    const struct rtl8139_info *nic = rtl8139_get_info();
    if (!nic || len > ETH_FRAME_MAX - ETH_HDR_LEN)
        return -1;

    static uint8_t frame[ETH_FRAME_MAX];
    memcpy(frame, dst_mac, 6);
    memcpy(frame + 6, nic->mac, 6);
    put_be16(frame + 12, type);
    memcpy(frame + ETH_HDR_LEN, payload, len);

    if (net_log)
        net_log("TX", frame, ETH_HDR_LEN + len);
    return rtl8139_send(frame, ETH_HDR_LEN + len);
}
