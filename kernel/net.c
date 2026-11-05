/*
 * net.c - ネットワーク全体の入口と、このマシンの設定
 */
#include "net.h"
#include "rtl8139.h"
#include "tcp.h"

const uint8_t net_my_ip[4]   = { 10, 0, 2, 15 };
const uint8_t net_gateway[4] = { 10, 0, 2, 2 };
const uint8_t net_netmask[4] = { 255, 255, 255, 0 };

struct net_stats net_stats;
net_log_fn net_log;

/* 大量に届いてもキーボードなどの処理が止まらないよう、1回に処理する数を制限する */
#define POLL_MAX_FRAMES 32

int net_poll(void)
{
    static uint8_t frame[ETH_FRAME_MAX];
    int count = 0;
    uint32_t len;

    while (count < POLL_MAX_FRAMES && (len = rtl8139_receive(frame, sizeof(frame))) > 0) {
        if (net_log)
            net_log("RX", frame, len);
        eth_input(frame, len);
        count++;
    }
    tcp_tick();         /* 何も届かなくても、再送の時間切れは調べる */
    return count;
}
