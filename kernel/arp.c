/*
 * arp.c - ARP(Address Resolution Protocol)
 *
 * IP アドレスしか分からない相手の MAC アドレスを調べる。
 *   問い合わせ: 「10.0.2.2 の人は MAC を教えて」を全員宛て(ブロードキャスト)に送る
 *   返事      : 本人だけが「それは私、MAC は 52:55:…」と問い合わせた人に答える
 *
 * ARP の中身(28バイト、Ethernet + IPv4 の場合):
 *    0   2   4 5 6   8       14    18      24    28
 *    ┌───┬───┬─┬─┬───┬───────┬─────┬───────┬─────┐
 *    │HW │PR │6│4│操作│送り主  │送り主│調べる  │調べる│
 *    │種類│種類│ │ │   │MAC    │IP   │MAC    │IP   │
 *    └───┴───┴─┴─┴───┴───────┴─────┴───────┴─────┘
 *    操作: 1 = 問い合わせ、2 = 返事
 *
 * 毎回聞くと無駄なので、分かった組み合わせを表に覚えておく。
 */
#include "net.h"
#include "rtl8139.h"
#include "string.h"

#define ARP_LEN      28
#define ARP_HW_ETH   1
#define ARP_OP_REQ   1
#define ARP_OP_REPLY 2

static struct arp_entry table[ARP_TABLE_SIZE];
static int next_slot;       /* 表が満杯のとき、次に上書きする場所(古い順におおよそ入れ替わる) */

static struct arp_entry *find(const uint8_t *ip)
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (table[i].used && memcmp(table[i].ip, ip, 4) == 0)
            return &table[i];
    }
    return 0;
}

static void remember(const uint8_t *ip, const uint8_t *mac)
{
    struct arp_entry *e = find(ip);
    if (!e) {
        e = &table[next_slot];
        next_slot = (next_slot + 1) % ARP_TABLE_SIZE;
        e->used = 1;
        memcpy(e->ip, ip, 4);
    }
    memcpy(e->mac, mac, 6);
}

const uint8_t *arp_lookup(const uint8_t *ip)
{
    const struct arp_entry *e = find(ip);
    return e ? e->mac : 0;
}

const struct arp_entry *arp_entry_at(int i)
{
    return &table[i];
}

/* ARP の中身を組み立てて送る */
static int arp_send(uint16_t op, const uint8_t *dst_mac,
                    const uint8_t *target_mac, const uint8_t *target_ip)
{
    const struct rtl8139_info *nic = rtl8139_get_info();
    if (!nic)
        return -1;

    uint8_t a[ARP_LEN];
    put_be16(a + 0, ARP_HW_ETH);
    put_be16(a + 2, ETH_TYPE_IPV4);
    a[4] = 6;                           /* MAC アドレスの長さ */
    a[5] = 4;                           /* IP アドレスの長さ */
    put_be16(a + 6, op);
    memcpy(a + 8, nic->mac, 6);
    memcpy(a + 14, net_my_ip, 4);
    memcpy(a + 18, target_mac, 6);
    memcpy(a + 24, target_ip, 4);
    return eth_send(dst_mac, ETH_TYPE_ARP, a, ARP_LEN);
}

void arp_request(const uint8_t *ip)
{
    static const uint8_t unknown[6] = { 0 };
    arp_send(ARP_OP_REQ, eth_broadcast, unknown, ip);
}

void arp_input(const uint8_t *data, uint32_t len)
{
    /* Ethernet + IPv4 の ARP だけを扱う */
    if (len < ARP_LEN || get_be16(data) != ARP_HW_ETH ||
        get_be16(data + 2) != ETH_TYPE_IPV4 || data[4] != 6 || data[5] != 4)
        return;

    uint16_t op = get_be16(data + 6);
    const uint8_t *sender_mac = data + 8;
    const uint8_t *sender_ip  = data + 14;
    const uint8_t *target_ip  = data + 24;
    int for_me = memcmp(target_ip, net_my_ip, 4) == 0;
    static const uint8_t no_ip[4] = { 0 };

    /*
     * 自分宛ての問い合わせや返事なら、送り主の組み合わせを覚える(相手も自分と話す気がある)。
     * 自分に関係ない通信は、すでに表にある相手の更新だけに使う。
     * 送り主の IP が 0.0.0.0 のものは「まだ IP がない人の確認」なので覚えない。
     */
    int learned = 0;
    if (memcmp(sender_ip, no_ip, 4) != 0 && (for_me || find(sender_ip))) {
        remember(sender_ip, sender_mac);
        learned = 1;
    }

    if (op == ARP_OP_REQ && for_me) {
        arp_send(ARP_OP_REPLY, sender_mac, sender_mac, sender_ip);
        net_stats.arp_replied++;
    }

    /* 待たせていたパケットは、問い合わせへの返事を先に出してから送る */
    if (learned)
        ip_on_arp_learned(sender_ip);
}
