/*
 * net.h - ネットワークの各層(Ethernet / ARP / IP / ICMP)
 *
 * 受信は下の層から上へ、送信は上の層から下へ、見出しを外したり付けたりしながら進む。
 *
 *   ICMP (icmp.c)   ping の問いかけと返事
 *   TCP  (tcp.c)    確実な「つながり」(使い方は tcp.h)
 *     ↕
 *   IP   (ip.c)     どの IP アドレスの相手へ届けるか
 *     ↕  ↖
 *     ↕   ARP (arp.c)  IP アドレス → MAC アドレスを調べる
 *     ↕  ↙
 *   Ethernet (ethernet.c)  同じネットワークの中の、どの MAC アドレスへ渡すか
 *     ↕
 *   RTL8139 (rtl8139.c)    カードのドライバ
 *
 * IP アドレスと MAC アドレスは、ネットワークを流れる順番のままバイト列で持つ。
 * 整数にしないのでエンディアンを気にしなくて済む。
 */
#ifndef NET_H
#define NET_H

#include <stdint.h>

/* ---- このマシンの設定(net.c)。QEMU の user モードが割り当てる値と同じ ---- */
extern const uint8_t net_my_ip[4];      /* 10.0.2.15 */
extern const uint8_t net_gateway[4];    /* 10.0.2.2 */
extern const uint8_t net_netmask[4];    /* 255.255.255.0 */

/* 数えておくもの(net コマンドで表示する) */
struct net_stats {
    uint32_t arp_replied;       /* 答えた ARP の問い合わせ */
    uint32_t ip_received;       /* 自分宛てとして受け取った IP パケット */
    uint32_t ip_dropped;        /* 壊れているなどの理由で捨てた IP パケット */
    uint32_t icmp_replied;      /* 答えた ping */
    uint32_t icmp_dropped;      /* 壊れているなどの理由で捨てた ICMP */
};
extern struct net_stats net_stats;

/* ---- ネットワーク全体(net.c) ---- */

/* 届いているフレームを処理する。処理した数を返す。メインのループから呼び続ける */
int net_poll(void);

/* 送受信するフレームを見たいときに設定する(NULL なら何もしない)。dir は "RX" か "TX" */
typedef void (*net_log_fn)(const char *dir, const uint8_t *frame, uint32_t len);
extern net_log_fn net_log;

/* ネットワークの数は上位バイトが先(ビッグエンディアン) */
static inline uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static inline void put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

/* ---- Ethernet(ethernet.c) ---- */
#define ETH_HDR_LEN    14
#define ETH_TYPE_IPV4  0x0800
#define ETH_TYPE_ARP   0x0806

extern const uint8_t eth_broadcast[6];  /* FF:FF:FF:FF:FF:FF(全員宛て) */

void eth_input(const uint8_t *frame, uint32_t len);
int  eth_send(const uint8_t *dst_mac, uint16_t type, const void *payload, uint32_t len);

/* ---- ARP(arp.c) ---- */
#define ARP_TABLE_SIZE 8

struct arp_entry {
    int     used;
    uint8_t ip[4];
    uint8_t mac[6];
};

void arp_input(const uint8_t *data, uint32_t len);
void arp_request(const uint8_t *ip);    /* 「この IP の人は誰?」と全員に聞く */

/* 覚えていれば MAC アドレスを、知らなければ NULL を返す */
const uint8_t *arp_lookup(const uint8_t *ip);

/* i 番目の表の項目(arp コマンドで表示する)。i は 0〜ARP_TABLE_SIZE-1 */
const struct arp_entry *arp_entry_at(int i);

/* ---- IP(ip.c) ---- */
#define IP_PROTO_ICMP 1

#define IP_PROTO_TCP  6

void ip_input(const uint8_t *packet, uint32_t len);

/* payload を IP パケットにして dst へ送る。相手の MAC が未知なら ARP の返事を待ってから送る */
int ip_send(const uint8_t *dst, uint8_t protocol, const void *payload, uint32_t len);

/* ARP が ip の MAC アドレスを覚えたときに呼ばれる。待たせていたパケットを送る */
void ip_on_arp_learned(const uint8_t *ip);

/* インターネットチェックサム(IP と ICMP と TCP で共通の、壊れていないかの検査の計算) */
uint16_t net_checksum(const void *data, uint32_t len);

/* 離れた場所にあるデータをまとめて計算するときの部品(TCP の疑似ヘッダーなど) */
uint32_t net_sum(uint32_t sum, const void *data, uint32_t len);
uint16_t net_sum_finish(uint32_t sum);

/* ---- ICMP(icmp.c) ---- */
struct icmp_reply {
    uint8_t  src[4];    /* 答えてくれた相手 */
    uint8_t  ttl;       /* 届いたパケットの TTL */
    uint32_t len;       /* ICMP の大きさ(見出しを含む) */
};

void icmp_input(const uint8_t *src, uint8_t ttl, const uint8_t *data, uint32_t len);

/* ping の問いかけ(Echo Request)を送る */
int icmp_send_echo(const uint8_t *dst, uint16_t id, uint16_t seq);

/* id, seq の問いかけへの返事が届いていれば *out に入れて 1 を、まだなら 0 を返す */
int icmp_take_reply(uint16_t id, uint16_t seq, struct icmp_reply *out);

#endif
