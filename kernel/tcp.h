/*
 * tcp.h - TCP(届くかどうか運次第の IP の上に、確実な「つながり」を作る)
 *
 * 待ち受ける側(サーバー)だけを作っている。使い方:
 *
 *   tcp_listen(7);                      ポート 7 で待ち受ける
 *   id = tcp_accept(7);                 つながった相手を1つ受け取る(いなければ -1)
 *   n  = tcp_recv(id, buf, sizeof buf); 届いたデータを読む
 *   tcp_send(id, buf, n);               データを送る
 *   tcp_close(id);                      切断する。id はこれ以降使えない
 *
 * どの関数も待たずにすぐ戻る。メインのループから何度も呼ぶ。
 */
#ifndef TCP_H
#define TCP_H

#include <stdint.h>

#define TCP_MAX_CONNS 8         /* 同時に持てるつながりの数 */
#define TCP_RX_BUF    4096      /* 1つのつながりの受信バッファ。広告するウィンドウの上限 */
#define TCP_TX_BUF    8192      /* 1つのつながりの送信バッファ。相手の ACK が来るまで取っておく */

/* 見出しのフラグ */
#define TCP_FIN 0x01            /* もう送るものはない */
#define TCP_SYN 0x02            /* つながりを始めたい */
#define TCP_RST 0x04            /* このつながりは無効、強制終了 */
#define TCP_PSH 0x08            /* ためずにアプリへ渡して */
#define TCP_ACK 0x10            /* ack の欄が有効 */

enum tcp_state {
    TCP_FREE,           /* 表のこの行は空き */
    TCP_SYN_RECEIVED,   /* SYN を受け取り、SYN+ACK を返した。最後の ACK を待っている */
    TCP_ESTABLISHED,    /* つながっている */
    TCP_FIN_WAIT_1,     /* こちらから FIN を出した(これから出す)。ACK を待っている */
    TCP_FIN_WAIT_2,     /* FIN の ACK は来た。相手の FIN を待っている */
    TCP_CLOSE_WAIT,     /* 相手の FIN を受け取った。アプリが閉じるのを待っている */
    TCP_CLOSING,        /* 同時に FIN を出し合った。こちらの FIN の ACK を待っている */
    TCP_LAST_ACK,       /* 相手の FIN の後でこちらも FIN を出した。最後の ACK を待っている */
    TCP_TIME_WAIT,      /* 終わったが、遅れて届くものに備えて少し残っている */
    TCP_CLOSED,         /* 切れた。アプリが tcp_close を呼ぶのを待っている */
};

/* ---- アプリから使う ---- */

int tcp_listen(uint16_t port);                  /* 成功 0、待ち受けの表が満杯なら -1 */
void tcp_unlisten(uint16_t port);               /* 待ち受けをやめる。まだ受け取っていない相手は切る */
int tcp_accept(uint16_t port);                  /* つながりの番号。誰もいなければ -1 */

/* 読んだバイト数。まだ何も届いていなければ 0、相手が閉じた(切れた)なら -1 */
int tcp_recv(int id, void *buf, uint32_t size);

/* 送信バッファに入れたバイト数(空きが足りなければ len より少ない)。切れていたら -1 */
int tcp_send(int id, const void *data, uint32_t len);

uint32_t tcp_send_space(int id);                /* 送信バッファの空き */
void tcp_close(int id);                         /* 残りのデータを送ってから FIN を出す */

/* ---- ネットワークの層から呼ばれる ---- */

void tcp_input(const uint8_t *src_ip, const uint8_t *segment, uint32_t len);
void tcp_tick(void);                            /* 再送などの時間切れを調べる。何度も呼ぶ */

/* ---- 観察と実験(tcp コマンド) ---- */

struct tcp_stats {
    uint32_t segs_in;           /* 受け取ったセグメント */
    uint32_t segs_out;          /* 送ったセグメント(再送を含む) */
    uint32_t bad;               /* 壊れていて捨てた */
    uint32_t retransmits;       /* 時間切れで再送した回数 */
    uint32_t fast_retransmits;  /* 同じ ACK が3回続いたので再送した回数 */
    uint32_t out_of_order;      /* 順番が飛んでいて捨てた */
    uint32_t rst_sent;          /* 送った RST */
    uint32_t syn_dropped;       /* 表が満杯で断った SYN */
    uint32_t test_dropped;      /* tcp drop でわざと落とした */
};
extern struct tcp_stats tcp_stats;

struct tcp_info {
    enum tcp_state state;
    uint8_t  peer_ip[4];
    uint16_t peer_port;
    uint16_t local_port;
    uint32_t rx_len;            /* アプリがまだ読んでいないバイト数 */
    uint32_t tx_len;            /* まだ ACK されていないバイト数 */
};

void tcp_get_info(int i, struct tcp_info *out);     /* i は 0〜TCP_MAX_CONNS-1 */
const char *tcp_state_name(enum tcp_state state);

/* 流れるセグメントと状態の変化を見たいときに設定する(NULL なら何もしない) */
typedef void (*tcp_seg_log_fn)(const char *dir, const uint8_t *peer_ip, uint16_t peer_port,
                               uint8_t flags, uint32_t seq, uint32_t ack,
                               uint16_t wnd, uint32_t len);
typedef void (*tcp_state_log_fn)(int id, enum tcp_state from, enum tcp_state to);
extern tcp_seg_log_fn tcp_seg_log;
extern tcp_state_log_fn tcp_state_log;

void tcp_set_drop_every(uint32_t n);    /* 送受信とも n 個に1個わざと落とす。0 でやめる */
void tcp_set_next_isn(uint32_t isn);    /* 次のつながりの最初のシーケンス番号を決める */

#endif
