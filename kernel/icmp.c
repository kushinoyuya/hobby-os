/*
 * icmp.c - ICMP(ping の問いかけと返事)
 *
 * Echo の中身:
 *    0    1    2      4    6    8
 *    ┌────┬────┬──────┬────┬────┬──────────────┐
 *    │種類│0   │検査  │ id │seq │ データ(自由) │
 *    └────┴────┴──────┴────┴────┴──────────────┘
 *    種類: 8 = 問いかけ(Echo Request)、0 = 返事(Echo Reply)
 *
 * 返事のしかたは簡単で、受け取ったものの種類を 0 に変えて、そのまま送り返す。
 * id と seq とデータもそのまま返す決まりなので、ping を送った側は
 * 「どの問いかけへの返事か」を見分けられる。
 */
#include "net.h"
#include "string.h"

#define ICMP_HDR_LEN    8
#define ICMP_MAX_LEN    1480        /* IP の中身の最大(1500 - 見出し20) */
#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO_REQ   8
#define PING_DATA_LEN   32          /* 自分が ping を送るときのデータの大きさ */

/* 最後に届いた返事(icmp_take_reply で受け取る) */
static int reply_valid;
static uint16_t reply_id, reply_seq;
static struct icmp_reply reply;

void icmp_input(const uint8_t *src, uint8_t ttl, const uint8_t *data, uint32_t len)
{
    if (len < ICMP_HDR_LEN || len > ICMP_MAX_LEN || net_checksum(data, len) != 0) {
        net_stats.icmp_dropped++;
        return;
    }

    if (data[0] == ICMP_ECHO_REQ && data[1] == 0) {
        /* 種類を 0 に変えてチェックサムを計算し直し、送り主へそのまま返す */
        static uint8_t out[ICMP_MAX_LEN];
        memcpy(out, data, len);
        out[0] = ICMP_ECHO_REPLY;
        put_be16(out + 2, 0);
        put_be16(out + 2, net_checksum(out, len));
        if (ip_send(src, IP_PROTO_ICMP, out, len) == 0)
            net_stats.icmp_replied++;
    } else if (data[0] == ICMP_ECHO_REPLY && data[1] == 0) {
        reply_id  = get_be16(data + 4);
        reply_seq = get_be16(data + 6);
        memcpy(reply.src, src, 4);
        reply.ttl = ttl;
        reply.len = len;
        reply_valid = 1;
    }
}

int icmp_send_echo(const uint8_t *dst, uint16_t id, uint16_t seq)
{
    uint8_t m[ICMP_HDR_LEN + PING_DATA_LEN];
    m[0] = ICMP_ECHO_REQ;
    m[1] = 0;
    put_be16(m + 2, 0);
    put_be16(m + 4, id);
    put_be16(m + 6, seq);
    for (int i = 0; i < PING_DATA_LEN; i++)
        m[ICMP_HDR_LEN + i] = (uint8_t)('a' + i % 26);
    put_be16(m + 2, net_checksum(m, sizeof(m)));
    return ip_send(dst, IP_PROTO_ICMP, m, sizeof(m));
}

int icmp_take_reply(uint16_t id, uint16_t seq, struct icmp_reply *out)
{
    if (!reply_valid || reply_id != id || reply_seq != seq)
        return 0;
    *out = reply;
    reply_valid = 0;
    return 1;
}
