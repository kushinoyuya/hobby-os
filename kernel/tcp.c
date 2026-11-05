/*
 * tcp.c - TCP(Transmission Control Protocol)
 *
 * 見出し(ふつうは20バイト):
 *    0     2     4          8          12    13    14    16    18    20
 *    ┌─────┬─────┬──────────┬──────────┬─────┬─────┬─────┬─────┬─────┐
 *    │送り主│宛先 │シーケンス │ACK 番号  │見出し│フラグ│ウィン│検査 │緊急 │
 *    │ポート│ポート│番号 seq  │ack       │の長さ│     │ドウ  │     │     │
 *    └─────┴─────┴──────────┴──────────┴─────┴─────┴─────┴─────┴─────┘
 *
 * IP は「届かない・順番が変わる・重複する」ことがある。TCP はデータの1バイトごとに
 * 番号(seq)を振り、「次はこの番号が欲しい」(ack)と返し合うことで、
 * 順番どおりに、抜けなく届ける。SYN と FIN も番号を1つ使う。
 *
 * 送信バッファの中身と番号の関係:
 *
 *      snd_una          snd_nxt
 *         │  送ったが      │  まだ送って
 *         │  ACK がまだ    │  いない
 *         ▼                ▼
 *         ├────────────────┼───────────┤
 *         │◀──────── tx_len ──────────▶│
 *
 * ACK が来たら snd_una を進めて、その分のバッファを空ける。
 * 時間内に ACK が来なければ、snd_una から snd_nxt までを送り直す(再送)。
 *
 * 作っていないもの: 自分からつなぎに行く(SYN_SENT)、順番の飛んだデータの保管、
 * 輻輳制御、RTT の測定、ウィンドウスケールなどの拡張。docs/08-tcp.md の補足を参照。
 */
#include <stddef.h>
#include "tcp.h"
#include "net.h"
#include "string.h"
#include "timer.h"

#define TCP_HDR_LEN      20
#define TCP_MSS_OPT_LEN  4
#define TCP_MSS          1460           /* 1回に運べるデータ。1500 - IP 20 - TCP 20 */
#define TCP_DEFAULT_MSS  536            /* 相手が MSS を教えてくれないときの決まりの値 */
#define TCP_MAX_LISTENERS 4

/* 時間はタイマーの回数で数える(1回 = 10ミリ秒) */
#define RTO_INITIAL      (TIMER_HZ / 2) /* 最初の再送まで 0.5 秒 */
#define RTO_MAX          (16 * TIMER_HZ)
#define MAX_RETRIES      6              /* データは 6 回送り直してもだめならあきらめる(約48秒) */
#define MAX_SYN_RETRIES  4              /* SYN+ACK は 4 回まで(約16秒) */
#define TIME_WAIT_TICKS  (4 * TIMER_HZ) /* 本物は 60 秒。表が小さいので短くしている */
#define FIN_WAIT_2_TICKS (30 * TIMER_HZ)/* 相手がいつまでも FIN を出さないときの見切り */
#define DUP_ACK_LIMIT    3              /* 同じ ACK がこの回数続いたら抜けたと判断する */

/*
 * シーケンス番号は 32 ビットで、4GB 送ると 0 に戻る。だから大小を普通に比べられない。
 * 引き算の結果を符号付きで見ると、差が 2GB 以内なら正しく前後が分かる。
 *   例: a = 0x00000010, b = 0xFFFFFFF0 → a - b = 0x20(正)→ a のほうが「後」
 */
#define SEQ_LT(a, b) ((int32_t)((a) - (b)) < 0)
#define SEQ_LE(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b) ((int32_t)((a) - (b)) > 0)

/* a の時刻になったか。タイマーの値が一周しても正しく比べられるよう引き算で見る */
#define TIME_REACHED(now, a) ((int32_t)((now) - (a)) >= 0)

/* アプリとの関係 */
enum {
    APP_PENDING,        /* まだ tcp_accept で受け取られていない */
    APP_OWNED,          /* アプリが使っている */
    APP_RELEASED,       /* アプリは tcp_close した。残りの片付けは TCP だけでやる */
};

struct tcp_conn {
    enum tcp_state state;
    int      app;
    uint8_t  peer_ip[4];
    uint16_t peer_port;
    uint16_t local_port;

    /* 送る側の番号 */
    uint32_t snd_una;       /* ACK をもらっていない最初の番号 */
    uint32_t snd_nxt;       /* 次に送る番号 */
    uint32_t snd_wnd;       /* 相手の受信バッファの空き(ウィンドウ) */
    uint32_t snd_wl1;       /* snd_wnd を更新したときの seq(古い知らせで上書きしないため) */
    uint32_t snd_wl2;       /* snd_wnd を更新したときの ack */
    uint32_t mss;           /* 相手が受け取れる1回の大きさ */

    /* 受け取る側の番号 */
    uint32_t rcv_nxt;       /* 次に欲しい番号。ack としてこれを返す */
    uint32_t rcv_edge;      /* 相手に伝えたウィンドウの右端 */

    /* FIN */
    int fin_queued;         /* アプリが閉じたので、データの後に FIN を出す */
    int fin_sent;           /* FIN を出した(番号を1つ使った) */
    int fin_received;       /* 相手の FIN を受け取った */

    /* 再送タイマー */
    int      rto_armed;
    uint32_t rto_deadline;
    uint32_t rto;           /* 今の待ち時間。再送するたびに2倍にする */
    uint32_t retries;
    uint32_t dup_acks;      /* 同じ ACK が続いた回数 */

    uint32_t close_deadline;    /* TIME_WAIT と FIN_WAIT_2 の終わる時刻 */

    /* バッファは輪として使う。start から len バイトが中身 */
    uint32_t rx_start, rx_len;
    uint32_t tx_start, tx_len;
    uint8_t  rx_buf[TCP_RX_BUF];
    uint8_t  tx_buf[TCP_TX_BUF];
};

/* 受け取ったセグメントを読み解いたもの */
struct segment {
    const uint8_t *src_ip;
    uint16_t src_port, dst_port;
    uint32_t seq, ack;
    uint8_t  flags;
    uint16_t wnd;
    const uint8_t *options;
    uint32_t options_len;
    const uint8_t *data;
    uint32_t data_len;
};

static struct tcp_conn conns[TCP_MAX_CONNS];
static uint16_t listeners[TCP_MAX_LISTENERS];   /* 0 は空き */

struct tcp_stats tcp_stats;
tcp_seg_log_fn tcp_seg_log;
tcp_state_log_fn tcp_state_log;

static uint32_t drop_every, rx_drop_count, tx_drop_count;
static int next_isn_set;
static uint32_t next_isn;
static uint32_t isn_counter;

/* 送るセグメントを作る場所: 見出し + MSS オプション + データ */
static uint8_t seg_buf[TCP_HDR_LEN + TCP_MSS_OPT_LEN + TCP_MSS];

/* ---- 小さな部品 ---- */

static inline uint32_t get_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static inline uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

const char *tcp_state_name(enum tcp_state state)
{
    static const char *const names[] = {
        "FREE", "SYN_RECEIVED", "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2",
        "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT", "CLOSED",
    };
    return names[state];
}

static void set_state(struct tcp_conn *c, enum tcp_state state)
{
    if (tcp_state_log)
        tcp_state_log((int)(c - conns), c->state, state);
    c->state = state;
}

/* 切れて、アプリも持っていないなら、表の行を空ける */
static void free_if_done(struct tcp_conn *c)
{
    if (c->state == TCP_CLOSED && c->app != APP_OWNED)
        set_state(c, TCP_FREE);
}

/* つながりを終わらせる。読み残しと送り残しは捨てる */
static void enter_closed(struct tcp_conn *c)
{
    c->rto_armed = 0;
    c->rx_len = 0;
    c->tx_len = 0;
    set_state(c, TCP_CLOSED);
    free_if_done(c);
}

static void enter_time_wait(struct tcp_conn *c)
{
    c->rto_armed = 0;
    c->close_deadline = timer_ticks() + TIME_WAIT_TICKS;
    set_state(c, TCP_TIME_WAIT);
}

static int is_listening(uint16_t port)
{
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (listeners[i] == port)
            return 1;
    }
    return 0;
}

static struct tcp_conn *find_conn(const struct segment *s)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &conns[i];
        if (c->state != TCP_FREE && c->state != TCP_CLOSED &&
            c->local_port == s->dst_port && c->peer_port == s->src_port &&
            memcmp(c->peer_ip, s->src_ip, 4) == 0)
            return c;
    }
    return 0;
}

/* アプリから渡された番号を確かめる */
static struct tcp_conn *get_conn(int id)
{
    if (id < 0 || id >= TCP_MAX_CONNS)
        return 0;
    struct tcp_conn *c = &conns[id];
    return c->state != TCP_FREE && c->app == APP_OWNED ? c : 0;
}

/* ---- 送信 ---- */

/*
 * TCP のチェックサムは、見出しとデータに加えて IP アドレスなどの「疑似ヘッダー」も足して計算する。
 * 宛先の違うパケットに紛れ込んでも気づけるようにするため。
 */
static uint16_t tcp_checksum(const uint8_t *src, const uint8_t *dst,
                             const void *segment, uint32_t len)
{
    uint8_t pseudo[12];
    memcpy(pseudo, src, 4);
    memcpy(pseudo + 4, dst, 4);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_TCP;
    put_be16(pseudo + 10, (uint16_t)len);

    uint32_t sum = net_sum(0, pseudo, sizeof(pseudo));
    sum = net_sum(sum, segment, len);
    return net_sum_finish(sum);
}

/* seg_buf の見出しを埋めて送る。オプションとデータは呼ぶ側が先に入れておく */
static void emit(const uint8_t *dst_ip, uint16_t src_port, uint16_t dst_port,
                 uint32_t seq, uint32_t ack, uint8_t flags, uint16_t wnd,
                 uint32_t options_len, uint32_t data_len)
{
    uint32_t hdr_len = TCP_HDR_LEN + options_len;
    uint32_t total = hdr_len + data_len;

    put_be16(seg_buf, src_port);
    put_be16(seg_buf + 2, dst_port);
    put_be32(seg_buf + 4, seq);
    put_be32(seg_buf + 8, ack);
    seg_buf[12] = (uint8_t)(hdr_len / 4 << 4);
    seg_buf[13] = flags;
    put_be16(seg_buf + 14, wnd);
    put_be16(seg_buf + 16, 0);          /* 計算の間は 0 にしておく */
    put_be16(seg_buf + 18, 0);
    put_be16(seg_buf + 16, tcp_checksum(net_my_ip, dst_ip, seg_buf, total));

    int drop = drop_every && ++tx_drop_count % drop_every == 0;
    if (tcp_seg_log)
        tcp_seg_log(drop ? "TX!" : "TX", dst_ip, dst_port, flags, seq, ack, wnd, data_len);
    if (drop) {
        tcp_stats.test_dropped++;
        return;
    }

    tcp_stats.segs_out++;
    ip_send(dst_ip, IP_PROTO_TCP, seg_buf, total);  /* 失敗しても再送が補う */
}

/* このつながりのセグメントを送る。送信バッファの off バイト目から len バイトをデータにする */
static void send_segment(struct tcp_conn *c, uint32_t seq, uint8_t flags,
                         uint32_t off, uint32_t len)
{
    uint32_t options_len = 0;
    if (flags & TCP_SYN) {              /* 「1回に 1460 バイトまで受け取れます」と伝える */
        seg_buf[TCP_HDR_LEN] = 2;       /* オプションの種類: MSS */
        seg_buf[TCP_HDR_LEN + 1] = TCP_MSS_OPT_LEN;
        put_be16(seg_buf + TCP_HDR_LEN + 2, TCP_MSS);
        options_len = TCP_MSS_OPT_LEN;
    }

    /* 輪の終わりをまたぐときは2回に分けて写す */
    uint8_t *out = seg_buf + TCP_HDR_LEN + options_len;
    uint32_t pos = (c->tx_start + off) % TCP_TX_BUF;
    uint32_t first = min_u32(len, TCP_TX_BUF - pos);
    memcpy(out, c->tx_buf + pos, first);
    memcpy(out + first, c->tx_buf, len - first);

    uint32_t wnd = TCP_RX_BUF - c->rx_len;
    c->rcv_edge = c->rcv_nxt + wnd;
    emit(c->peer_ip, c->local_port, c->peer_port, seq, c->rcv_nxt, flags,
         (uint16_t)wnd, options_len, len);
}

static void send_ack(struct tcp_conn *c)
{
    send_segment(c, c->snd_nxt, TCP_ACK, 0, 0);
}

static void send_syn_ack(struct tcp_conn *c)
{
    send_segment(c, c->snd_una, TCP_SYN | TCP_ACK, 0, 0);
}

/* snd_una から off バイト先のデータを len バイト送る */
static void send_data(struct tcp_conn *c, uint32_t off, uint32_t len)
{
    uint8_t flags = TCP_ACK;
    if (off + len == c->tx_len)         /* いま持っているデータの最後 */
        flags |= TCP_PSH;
    send_segment(c, c->snd_una + off, flags, off, len);
}

static void send_fin(struct tcp_conn *c)
{
    send_segment(c, c->snd_una + c->tx_len, TCP_FIN | TCP_ACK, 0, 0);
}

/* 送ったが ACK のないデータのバイト数(FIN の番号は引く) */
static uint32_t data_in_flight(const struct tcp_conn *c)
{
    return c->snd_nxt - c->snd_una - (c->fin_sent ? 1u : 0u);
}

/* 相手のウィンドウが許す分だけ、まだ送っていないデータを送る */
static void tcp_output(struct tcp_conn *c)
{
    if (c->state == TCP_SYN_RECEIVED || c->state == TCP_CLOSED || c->state == TCP_FREE)
        return;

    if (!c->fin_sent) {
        uint32_t flight = data_in_flight(c);
        uint32_t unsent = c->tx_len - flight;
        uint32_t room = c->snd_wnd > flight ? c->snd_wnd - flight : 0;

        while (unsent > 0 && room > 0) {
            uint32_t n = min_u32(min_u32(unsent, room), c->mss);
            send_data(c, c->snd_nxt - c->snd_una, n);
            c->snd_nxt += n;
            unsent -= n;
            room -= n;
        }

        /* データを送り切ったら FIN を出す。FIN は番号を1つ使う */
        if (unsent == 0 && c->fin_queued) {
            send_fin(c);
            c->fin_sent = 1;
            c->snd_nxt++;
        }
    }

    /* ACK を待つものがあるか、ウィンドウが 0 で送れないデータがあるなら時計を動かす */
    if ((c->snd_nxt != c->snd_una || c->tx_len > 0) && !c->rto_armed) {
        c->rto_armed = 1;
        c->rto_deadline = timer_ticks() + c->rto;
    }
}

/* ACK をもらっていないものを全部送り直す */
static void resend_all(struct tcp_conn *c)
{
    uint32_t flight = data_in_flight(c);
    for (uint32_t off = 0; off < flight; ) {
        uint32_t n = min_u32(flight - off, c->mss);
        send_data(c, off, n);
        off += n;
    }
    if (c->fin_sent)
        send_fin(c);
}

/* つながりのない相手へ「そんなつながりはない」と伝える。RST に RST は返さない */
static void send_reset(const struct segment *s)
{
    if (s->flags & TCP_RST)
        return;

    uint32_t seq = 0, ack = 0;
    uint8_t flags = TCP_RST;
    if (s->flags & TCP_ACK) {
        seq = s->ack;                   /* 相手が期待している番号で出さないと無視される */
    } else {
        flags |= TCP_ACK;
        ack = s->seq + s->data_len;
        if (s->flags & TCP_SYN)
            ack++;
        if (s->flags & TCP_FIN)
            ack++;
    }
    tcp_stats.rst_sent++;
    emit(s->src_ip, s->dst_port, s->src_port, seq, ack, flags, 0, 0, 0);
}

/* こちらの都合でつながりを切る */
static void abort_conn(struct tcp_conn *c)
{
    tcp_stats.rst_sent++;
    send_segment(c, c->snd_nxt, TCP_RST | TCP_ACK, 0, 0);
    enter_closed(c);
}

/* ---- 受信 ---- */

static uint32_t parse_mss(const struct segment *s)
{
    const uint8_t *p = s->options;
    uint32_t left = s->options_len;

    while (left > 0) {
        uint8_t kind = p[0];
        if (kind == 0)                  /* オプションの終わり */
            break;
        if (kind == 1) {                /* 何もしない(位置合わせ用) */
            p++;
            left--;
            continue;
        }
        /* 長さの欄を信じる前に確かめる。0 だと永久に進まない */
        if (left < 2 || p[1] < 2 || p[1] > left)
            break;
        if (kind == 2 && p[1] == TCP_MSS_OPT_LEN) {
            uint32_t mss = get_be16(p + 2);
            if (mss > 0)
                return min_u32(mss, TCP_MSS);
        }
        left -= p[1];
        p += p[1];
    }
    return TCP_DEFAULT_MSS;
}

static uint32_t choose_isn(void)
{
    if (next_isn_set) {
        next_isn_set = 0;
        return next_isn;
    }
    /*
     * 古いつながりの迷子と混ざらないよう、毎回違う番号から始める。
     * 本物の OS は推測されないようハッシュで作る。これは推測できるので、インターネットには出せない。
     */
    return timer_ticks() * 2500 + (isn_counter++) * 64000;
}

/* 空いている行を探す。なければ TIME_WAIT の行をつぶす(もう終わった相手なので害が少ない) */
static struct tcp_conn *alloc_conn(void)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (conns[i].state == TCP_FREE)
            return &conns[i];
    }
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (conns[i].state == TCP_TIME_WAIT) {
            enter_closed(&conns[i]);
            if (conns[i].state == TCP_FREE)
                return &conns[i];
        }
    }
    return 0;
}

/* つながりが見つからなかったセグメント: 新しいつながりの SYN か、迷子か */
static void input_no_conn(const struct segment *s)
{
    int is_new_syn = (s->flags & (TCP_SYN | TCP_ACK | TCP_RST | TCP_FIN)) == TCP_SYN;
    if (!is_new_syn || !is_listening(s->dst_port) || s->src_port == 0) {
        send_reset(s);
        return;
    }

    struct tcp_conn *c = alloc_conn();
    if (!c) {
        /* 断る代わりに黙って捨てる。相手が SYN を送り直す頃には空いているかもしれない */
        tcp_stats.syn_dropped++;
        return;
    }

    /* 受信と送信のバッファ以外を 0 にする */
    memset(c, 0, offsetof(struct tcp_conn, rx_buf));
    memcpy(c->peer_ip, s->src_ip, 4);
    c->peer_port = s->src_port;
    c->local_port = s->dst_port;
    c->app = APP_PENDING;
    c->mss = parse_mss(s);
    c->rto = RTO_INITIAL;

    c->rcv_nxt = s->seq + 1;            /* 相手の SYN が番号を1つ使った */
    c->snd_una = choose_isn();
    c->snd_nxt = c->snd_una + 1;        /* こちらの SYN も番号を1つ使う */
    c->snd_wnd = s->wnd;
    c->snd_wl1 = s->seq;
    c->snd_wl2 = 0;

    /* c->state は memset で TCP_FREE になっているので、ここで初めて行が使われる */
    set_state(c, TCP_SYN_RECEIVED);
    send_syn_ack(c);
    c->rto_armed = 1;
    c->rto_deadline = timer_ticks() + c->rto;
}

/* ACK を処理する。続きを処理してよければ 1、このセグメントで終わりなら 0 を返す */
static int input_ack(struct tcp_conn *c, const struct segment *s, uint32_t data_len, int fin)
{
    if (c->state == TCP_SYN_RECEIVED) {
        if (s->ack != c->snd_nxt) {     /* 出した SYN+ACK への答えになっていない */
            send_reset(s);
            return 0;
        }
        c->snd_una = s->ack;
        c->snd_wnd = s->wnd;
        c->snd_wl1 = s->seq;
        c->snd_wl2 = s->ack;
        c->rto_armed = 0;
        c->rto = RTO_INITIAL;
        c->retries = 0;
        set_state(c, TCP_ESTABLISHED);  /* 3ウェイハンドシェイクの完了 */
        return 1;
    }

    if (SEQ_GT(s->ack, c->snd_nxt)) {   /* まだ送っていない番号の ACK はおかしい */
        send_ack(c);
        return 0;
    }

    int is_dup = s->ack == c->snd_una && data_len == 0 && !fin &&
                 s->wnd == c->snd_wnd && c->snd_nxt != c->snd_una;

    if (SEQ_GT(s->ack, c->snd_una)) {
        /* 新しい ACK: 届いた分のデータをバッファから消す */
        uint32_t acked = s->ack - c->snd_una;
        uint32_t data_acked = min_u32(acked, c->tx_len);
        c->tx_start = (c->tx_start + data_acked) % TCP_TX_BUF;
        c->tx_len -= data_acked;
        c->snd_una = s->ack;
        c->rto_armed = 0;               /* tcp_output でまた動かす */
        c->rto = RTO_INITIAL;
        c->retries = 0;
        c->dup_acks = 0;
    } else if (is_dup) {
        /* 同じ ACK が続く = 相手は後ろのデータを受け取っているのに、先頭が抜けている */
        if (++c->dup_acks == DUP_ACK_LIMIT) {
            tcp_stats.fast_retransmits++;
            uint32_t flight = data_in_flight(c);
            if (flight > 0)
                send_data(c, 0, min_u32(flight, c->mss));
            else
                send_fin(c);
        }
    }

    /* ウィンドウを覚える。順番が入れ替わった古い知らせでは上書きしない */
    uint32_t old_wnd = c->snd_wnd;
    if (SEQ_LT(c->snd_wl1, s->seq) ||
        (c->snd_wl1 == s->seq && SEQ_LE(c->snd_wl2, s->ack))) {
        c->snd_wnd = s->wnd;
        c->snd_wl1 = s->seq;
        c->snd_wl2 = s->ack;
    }

    /*
     * ウィンドウが開いたのに未確認のものが残っている = 受け取ってもらえなかったプローブ。
     * 再送の待ち時間は延びきっているかもしれないので、待たずにすぐ送り直す。
     */
    if (old_wnd == 0 && c->snd_wnd > 0 && c->snd_nxt != c->snd_una) {
        resend_all(c);
        c->rto = RTO_INITIAL;
        c->retries = 0;
        c->rto_deadline = timer_ticks() + c->rto;
    }

    /*
     * ウィンドウ 0 のままの ACK は「生きているが、まだ受け取れない」という返事。
     * 再送の失敗に数えない(アプリが閉じた後は待ち続けない)。
     */
    if (c->snd_wnd == 0 && s->ack == c->snd_una && c->app != APP_RELEASED)
        c->retries = 0;

    /* 自分の FIN が届いた */
    if (c->fin_sent && s->ack == c->snd_nxt) {
        switch (c->state) {
        case TCP_FIN_WAIT_1:
            c->close_deadline = timer_ticks() + FIN_WAIT_2_TICKS;
            set_state(c, TCP_FIN_WAIT_2);
            break;
        case TCP_CLOSING:
            enter_time_wait(c);
            break;
        case TCP_LAST_ACK:
            enter_closed(c);
            return 0;
        default:
            break;
        }
    }
    return 1;
}

/* データを受信バッファに入れる。入れた(または捨ててよいと判断した)バイト数を返す */
static uint32_t store_data(struct tcp_conn *c, const uint8_t *data, uint32_t len)
{
    if (c->app == APP_RELEASED)         /* もう読む人がいない。受け取ったことにして捨てる */
        return len;

    uint32_t n = min_u32(len, TCP_RX_BUF - c->rx_len);
    uint32_t pos = (c->rx_start + c->rx_len) % TCP_RX_BUF;
    uint32_t first = min_u32(n, TCP_RX_BUF - pos);
    memcpy(c->rx_buf + pos, data, first);
    memcpy(c->rx_buf, data + first, n - first);
    c->rx_len += n;
    return n;
}

/* 相手の FIN を受け取った */
static void input_fin(struct tcp_conn *c)
{
    c->rcv_nxt++;                       /* FIN も番号を1つ使う */
    c->fin_received = 1;

    switch (c->state) {
    case TCP_ESTABLISHED:
        set_state(c, TCP_CLOSE_WAIT);
        break;
    case TCP_FIN_WAIT_1:                /* 自分の FIN にまだ ACK がないのに、相手の FIN が来た */
        set_state(c, TCP_CLOSING);
        break;
    case TCP_FIN_WAIT_2:
        enter_time_wait(c);
        break;
    default:
        break;
    }
}

/* 受け取れる範囲にある番号か */
static int in_window(const struct tcp_conn *c, uint32_t seq)
{
    uint32_t wnd = TCP_RX_BUF - c->rx_len;
    return SEQ_LE(c->rcv_nxt, seq) && SEQ_LT(seq, c->rcv_nxt + wnd);
}

/* つながりのあるセグメント */
static void input_conn(struct tcp_conn *c, const struct segment *s)
{
    uint8_t f = s->flags;

    /* SYN+ACK が相手に届かず、SYN が送り直されてきた */
    if (c->state == TCP_SYN_RECEIVED && (f & TCP_SYN) && !(f & (TCP_ACK | TCP_RST)) &&
        s->seq == c->rcv_nxt - 1) {
        send_syn_ack(c);
        return;
    }

    /*
     * RST は番号がぴったりのときだけ信じる(他人が偽の RST で切るのを難しくする)。
     * 範囲内だがずれていれば ACK を返して、本物なら相手に正しい番号で送り直してもらう。
     * TIME_WAIT は迷子の RST で早く消えないよう無視する。
     */
    if (f & TCP_RST) {
        if (c->state == TCP_TIME_WAIT)
            return;
        if (s->seq == c->rcv_nxt)
            enter_closed(c);
        else if (in_window(c, s->seq))
            send_ack(c);
        return;
    }

    /* つながっているのに来た SYN。相手が再起動したのかもしれないので ACK で様子を見る */
    if (f & TCP_SYN) {
        send_ack(c);
        return;
    }

    if (!(f & TCP_ACK))                 /* SYN 以外は必ず ACK が付いている */
        return;

    /* ---- 番号を確かめる ---- */
    uint32_t seq = s->seq;
    const uint8_t *data = s->data;
    uint32_t data_len = s->data_len;
    int fin = (f & TCP_FIN) != 0;

    if (SEQ_LT(seq, c->rcv_nxt)) {
        uint32_t old = c->rcv_nxt - seq;
        if (old >= data_len + (uint32_t)fin) {
            /* 全部受け取り済み。ACK が相手に届かなかったのだろう。もう一度返す */
            if (c->state == TCP_TIME_WAIT)
                c->close_deadline = timer_ticks() + TIME_WAIT_TICKS;
            send_ack(c);
            return;
        }
        /* 前半だけ受け取り済み。その分を切り落とす */
        data += old;
        data_len -= old;
        seq = c->rcv_nxt;
    } else if (seq != c->rcv_nxt) {
        /*
         * 間が抜けている。保管はせず捨てて「欲しいのはまだこの番号」と返す。
         * 相手がこの ACK を3回見ると、抜けた分を送り直してくれる。
         */
        if (data_len > 0)
            tcp_stats.out_of_order++;
        send_ack(c);
        return;
    }

    /* ---- ACK ---- */
    if (!input_ack(c, s, data_len, fin))
        return;

    /* ---- データ ---- */
    uint32_t accepted = 0;
    if (data_len > 0) {
        accepted = store_data(c, data, data_len);
        c->rcv_nxt += accepted;
    }

    /* ---- FIN。入り切らなかったデータがあれば、FIN も次に回す ---- */
    if (fin && accepted == data_len)
        input_fin(c);

    if (data_len > 0 || fin)
        send_ack(c);

    tcp_output(c);                      /* ウィンドウが開いたかもしれない */
}

void tcp_input(const uint8_t *src_ip, const uint8_t *segment, uint32_t len)
{
    /* 見出しの長さを信じる前に確かめる */
    if (len < TCP_HDR_LEN) {
        tcp_stats.bad++;
        return;
    }
    uint32_t hdr_len = (uint32_t)(segment[12] >> 4) * 4;
    if (hdr_len < TCP_HDR_LEN || hdr_len > len) {
        tcp_stats.bad++;
        return;
    }
    if (tcp_checksum(src_ip, net_my_ip, segment, len) != 0) {
        tcp_stats.bad++;
        return;
    }

    struct segment s;
    s.src_ip = src_ip;
    s.src_port = get_be16(segment);
    s.dst_port = get_be16(segment + 2);
    s.seq = get_be32(segment + 4);
    s.ack = get_be32(segment + 8);
    s.flags = segment[13];
    s.wnd = get_be16(segment + 14);
    s.options = segment + TCP_HDR_LEN;
    s.options_len = hdr_len - TCP_HDR_LEN;
    s.data = segment + hdr_len;
    s.data_len = len - hdr_len;

    int drop = drop_every && ++rx_drop_count % drop_every == 0;
    if (tcp_seg_log)
        tcp_seg_log(drop ? "RX!" : "RX", s.src_ip, s.src_port, s.flags, s.seq, s.ack, s.wnd,
                    s.data_len);
    if (drop) {
        tcp_stats.test_dropped++;
        return;
    }

    tcp_stats.segs_in++;

    struct tcp_conn *c = find_conn(&s);

    /* TIME_WAIT の相手が、前より先の番号で新しくつなぎ直してきたら受け付ける */
    if (c && c->state == TCP_TIME_WAIT &&
        (s.flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN &&
        SEQ_GT(s.seq, c->rcv_nxt) && is_listening(s.dst_port)) {
        enter_closed(c);
        c = 0;
    }

    if (c)
        input_conn(c, &s);
    else
        input_no_conn(&s);
}

/* ---- 時間切れ ---- */

void tcp_tick(void)
{
    uint32_t now = timer_ticks();

    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &conns[i];

        if ((c->state == TCP_TIME_WAIT || c->state == TCP_FIN_WAIT_2) &&
            TIME_REACHED(now, c->close_deadline)) {
            enter_closed(c);
            continue;
        }

        if (!c->rto_armed || !TIME_REACHED(now, c->rto_deadline))
            continue;

        uint32_t limit = c->state == TCP_SYN_RECEIVED ? MAX_SYN_RETRIES : MAX_RETRIES;
        if (c->retries >= limit) {      /* 相手がいなくなったと判断する */
            abort_conn(c);
            continue;
        }
        c->retries++;
        tcp_stats.retransmits++;

        if (c->state == TCP_SYN_RECEIVED) {
            send_syn_ack(c);
        } else if (c->snd_nxt == c->snd_una) {
            /*
             * 相手のウィンドウが 0 で何も送れない。空いたという知らせが失われると
             * 永久に止まるので、1バイトだけ送って様子を見る(ウィンドウプローブ)。
             */
            send_data(c, 0, 1);
            c->snd_nxt++;
        } else {
            resend_all(c);
        }

        c->rto = min_u32(c->rto * 2, RTO_MAX);  /* 混んでいるのかもしれないので待ち時間を延ばす */
        c->rto_deadline = now + c->rto;
    }
}

/* ---- アプリから使う ---- */

int tcp_listen(uint16_t port)
{
    if (port == 0)
        return -1;
    if (is_listening(port))
        return 0;
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (listeners[i] == 0) {
            listeners[i] = port;
            return 0;
        }
    }
    return -1;
}

void tcp_unlisten(uint16_t port)
{
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (listeners[i] == port)
            listeners[i] = 0;
    }
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &conns[i];
        if (c->state != TCP_FREE && c->state != TCP_CLOSED &&
            c->app == APP_PENDING && c->local_port == port)
            abort_conn(c);
    }
}

int tcp_accept(uint16_t port)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &conns[i];
        if ((c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) &&
            c->app == APP_PENDING && c->local_port == port) {
            c->app = APP_OWNED;
            return i;
        }
    }
    return -1;
}

int tcp_recv(int id, void *buf, uint32_t size)
{
    struct tcp_conn *c = get_conn(id);
    if (!c)
        return -1;

    if (c->rx_len == 0)
        return c->fin_received || c->state == TCP_CLOSED ? -1 : 0;

    uint32_t n = min_u32(size, c->rx_len);
    uint32_t first = min_u32(n, TCP_RX_BUF - c->rx_start);
    memcpy(buf, c->rx_buf + c->rx_start, first);
    memcpy((uint8_t *)buf + first, c->rx_buf, n - first);
    c->rx_start = (c->rx_start + n) % TCP_RX_BUF;
    c->rx_len -= n;

    /*
     * バッファが空いたことを相手に知らせる。1バイトごとに知らせると無駄なので、
     * 1セグメント分以上広がったときだけ(相手はこれを待っているかもしれない)。
     */
    if (!c->fin_received && c->state != TCP_CLOSED) {
        uint32_t edge = c->rcv_nxt + (TCP_RX_BUF - c->rx_len);
        if (edge - c->rcv_edge >= min_u32(c->mss, TCP_RX_BUF / 2))
            send_ack(c);
    }
    return (int)n;
}

uint32_t tcp_send_space(int id)
{
    struct tcp_conn *c = get_conn(id);
    return c ? TCP_TX_BUF - c->tx_len : 0;
}

int tcp_send(int id, const void *data, uint32_t len)
{
    struct tcp_conn *c = get_conn(id);
    if (!c || (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT))
        return -1;

    uint32_t n = min_u32(len, TCP_TX_BUF - c->tx_len);
    uint32_t pos = (c->tx_start + c->tx_len) % TCP_TX_BUF;
    uint32_t first = min_u32(n, TCP_TX_BUF - pos);
    memcpy(c->tx_buf + pos, data, first);
    memcpy(c->tx_buf, (const uint8_t *)data + first, n - first);
    c->tx_len += n;

    tcp_output(c);
    return (int)n;
}

void tcp_close(int id)
{
    struct tcp_conn *c = get_conn(id);
    if (!c)
        return;

    c->app = APP_RELEASED;
    c->rx_len = 0;                      /* 読み残しは捨てる */

    switch (c->state) {
    case TCP_ESTABLISHED:
        c->fin_queued = 1;
        set_state(c, TCP_FIN_WAIT_1);
        break;
    case TCP_CLOSE_WAIT:
        c->fin_queued = 1;
        set_state(c, TCP_LAST_ACK);
        break;
    default:
        break;
    }
    free_if_done(c);
    tcp_output(c);
}

/* ---- 観察と実験 ---- */

void tcp_get_info(int i, struct tcp_info *out)
{
    const struct tcp_conn *c = &conns[i];
    out->state = c->state;
    memcpy(out->peer_ip, c->peer_ip, 4);
    out->peer_port = c->peer_port;
    out->local_port = c->local_port;
    out->rx_len = c->rx_len;
    out->tx_len = c->tx_len;
}

void tcp_set_drop_every(uint32_t n)
{
    drop_every = n;
    rx_drop_count = 0;
    tx_drop_count = 0;
}

void tcp_set_next_isn(uint32_t isn)
{
    next_isn = isn;
    next_isn_set = 1;
}
