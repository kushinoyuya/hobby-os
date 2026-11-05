/*
 * ip.c - IP(Internet Protocol、バージョン4)
 *
 * 見出し(ふつうは20バイト):
 *    0      1     2       4     6        8    9     10    12    16    20
 *    ┌──────┬─────┬───────┬─────┬────────┬────┬─────┬─────┬─────┬─────┐
 *    │版+長さ│種別 │全体の │番号 │フラグ  │TTL │中身 │検査 │送り主│宛先 │
 *    │0x45  │     │長さ   │     │+断片位置│    │種類 │     │ IP  │ IP  │
 *    └──────┴─────┴───────┴─────┴────────┴────┴─────┴─────┴─────┴─────┘
 *
 * 送るときの判断: 宛先が同じネットワーク(10.0.2.x)ならその人へ直接、
 * 違うネットワークならゲートウェイへ渡す。どちらも ARP で MAC アドレスを調べる。
 */
#include "net.h"
#include "rtl8139.h"
#include "string.h"
#include "tcp.h"

#define IP_HDR_LEN   20
#define IP_MAX_LEN   (ETH_FRAME_MAX - ETH_HDR_LEN)  /* 1500 バイト */
#define IP_FLAG_DF   0x4000     /* 「途中で分割しないで」の印 */
#define IP_FLAG_MF   0x2000     /* 「この後に続きの断片がある」の印 */
#define IP_OFFSET    0x1FFF     /* 断片の位置 */
#define IP_TTL       64         /* ルーターを通るたびに減り、0 で捨てられる。迷子の防止 */

/* ARP の返事を待っているパケット(1つだけ。新しいものが来たら古いものは捨てる) */
static uint8_t pending[IP_MAX_LEN];
static uint32_t pending_len;
static uint8_t pending_hop[4];

/*
 * インターネットチェックサム:
 * 2バイトずつ足し、あふれた分を下に足し戻して、最後に全ビットを反転する。
 * 受け取った側がチェックサムの欄も含めて計算すると、壊れていなければ 0 になる。
 */
uint32_t net_sum(uint32_t sum, const void *data, uint32_t len)
{
    const uint8_t *p = data;

    while (len > 1) {
        sum += get_be16(p);
        p += 2;
        len -= 2;
    }
    if (len == 1)
        sum += (uint32_t)p[0] << 8;     /* 奇数のときは、最後に 0 を補って考える */
    return sum;
}

uint16_t net_sum_finish(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t net_checksum(const void *data, uint32_t len)
{
    return net_sum_finish(net_sum(0, data, len));
}

void ip_input(const uint8_t *packet, uint32_t len)
{
    if (len < IP_HDR_LEN || packet[0] >> 4 != 4) {      /* IPv4 でなければ扱わない */
        net_stats.ip_dropped++;
        return;
    }

    /* 見出しの長さ(4バイト単位)と全体の長さが、届いた大きさと矛盾しないか */
    uint32_t hdr_len = (packet[0] & 0x0F) * 4u;
    uint32_t total   = get_be16(packet + 2);
    if (hdr_len < IP_HDR_LEN || total < hdr_len || total > len) {
        net_stats.ip_dropped++;
        return;
    }

    if (net_checksum(packet, hdr_len) != 0) {           /* 途中で壊れた */
        net_stats.ip_dropped++;
        return;
    }

    if (memcmp(packet + 16, net_my_ip, 4) != 0)         /* 自分宛てでない */
        return;

    /* 分割されたパケットを組み立て直す処理は作っていないので捨てる */
    if (get_be16(packet + 6) & (IP_FLAG_MF | IP_OFFSET)) {
        net_stats.ip_dropped++;
        return;
    }

    net_stats.ip_received++;

    /*
     * Ethernet は最小60バイトまで 0 で埋められるので、len ではなく total を信じる。
     * 見出しの後ろが中身。
     */
    const uint8_t *payload = packet + hdr_len;
    uint32_t payload_len = total - hdr_len;

    if (packet[9] == IP_PROTO_ICMP)
        icmp_input(packet + 12, packet[8], payload, payload_len);
    else if (packet[9] == IP_PROTO_TCP)
        tcp_input(packet + 12, payload, payload_len);
}

/* dst へ直接届くか判断して、次に渡す相手(ネクストホップ)を決める */
static const uint8_t *next_hop(const uint8_t *dst)
{
    for (int i = 0; i < 4; i++) {
        if ((dst[i] & net_netmask[i]) != (net_my_ip[i] & net_netmask[i]))
            return net_gateway;
    }
    return dst;
}

int ip_send(const uint8_t *dst, uint8_t protocol, const void *payload, uint32_t len)
{
    static uint16_t next_id;
    static uint8_t packet[IP_MAX_LEN];

    if (len > IP_MAX_LEN - IP_HDR_LEN)
        return -1;

    packet[0] = 0x45;                   /* バージョン 4、見出しは 5 × 4 = 20 バイト */
    packet[1] = 0;
    put_be16(packet + 2, (uint16_t)(IP_HDR_LEN + len));
    put_be16(packet + 4, next_id++);
    put_be16(packet + 6, IP_FLAG_DF);
    packet[8] = IP_TTL;
    packet[9] = protocol;
    put_be16(packet + 10, 0);           /* 計算の間は 0 にしておく */
    memcpy(packet + 12, net_my_ip, 4);
    memcpy(packet + 16, dst, 4);
    put_be16(packet + 10, net_checksum(packet, IP_HDR_LEN));
    memcpy(packet + IP_HDR_LEN, payload, len);

    const uint8_t *hop = next_hop(dst);
    const uint8_t *mac = arp_lookup(hop);
    if (mac)
        return eth_send(mac, ETH_TYPE_IPV4, packet, IP_HDR_LEN + len);

    /* MAC アドレスが分からないので、ARP で聞いて、返事が来るまでパケットを取っておく */
    memcpy(pending, packet, IP_HDR_LEN + len);
    pending_len = IP_HDR_LEN + len;
    memcpy(pending_hop, hop, 4);
    arp_request(hop);
    return 0;
}

void ip_on_arp_learned(const uint8_t *ip)
{
    if (pending_len == 0 || memcmp(pending_hop, ip, 4) != 0)
        return;

    const uint8_t *mac = arp_lookup(ip);
    if (mac)
        eth_send(mac, ETH_TYPE_IPV4, pending, pending_len);
    pending_len = 0;
}
