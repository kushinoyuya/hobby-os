/*
 * http.c - Web サーバー(HTTP/1.1)
 *
 * HTTP は「テキストの手紙」をやり取りする決まり。TCP のつながりの上で、
 *
 *   ブラウザ → GET /hello.txt HTTP/1.1      リクエスト行(何がほしいか)
 *              Host: localhost:8080          ヘッダー(名前: 値)を何行か
 *              (空行)                        ここで見出しが終わる
 *
 *   OS       → HTTP/1.1 200 OK               ステータス行(結果)
 *              Content-Length: 25            ヘッダー
 *              (空行)
 *              Hello from hobby-os! ...      本文(Content-Length バイト)
 *
 * 1つのつながりで何回もやり取りできる(keep-alive)。本文の長さが先に分かっているので、
 * 受け取る側は「どこまでが1つの応答か」が分かる。
 *
 * つながりごとの状態:
 *   READING : リクエストの見出し(空行まで)がそろうのを待つ
 *   SENDING : 応答を TCP の送信バッファに少しずつ入れる
 *   → 送り終えたら、keep-alive なら READING に戻り、そうでなければ閉じる
 *
 * 作っていないもの: リクエストの本文(POST など)、chunked、Range、圧縮、HTTPS。
 */
#include <stdint.h>
#include "http.h"
#include "tcp.h"
#include "timer.h"
#include "string.h"

#define REQ_MAX        2048     /* リクエストの見出しの上限。超えたら 431 */
#define HEAD_MAX       512      /* 応答の見出しの上限 */
#define PAGE_MAX       4096     /* その場で作る本文(トップページやエラー)の上限 */
#define METHOD_MAX     8
#define PATH_MAX       256      /* これより長いパスは 414 */
#define HOST_MAX       128

#define HEADER_TICKS   (10 * TIMER_HZ)  /* 最初の1バイトから見出しを送り終えるまでの持ち時間 */
#define IDLE_TICKS     (5 * TIMER_HZ)   /* keep-alive で次のリクエストを待つ時間 */
#define SEND_TICKS     (30 * TIMER_HZ)  /* 応答を少しも受け取ってもらえない状態の我慢の限界 */
#define MAX_REQUESTS   100              /* 1つのつながりで受けるリクエストの数 */

#define BIG_LINES      1024             /* /big は 64 バイトの行を 1024 行(64KB) */
#define BIG_LINE_LEN   64

#define TIME_REACHED(now, a) ((int32_t)((now) - (a)) >= 0)

enum phase { READING, SENDING };

/* 本文の出どころ */
enum body_kind {
    BODY_MEMORY,        /* body が指すメモリ(固定の文字列か、page で作ったもの) */
    BODY_BIG,           /* /big の行をその場で作る */
};

struct http_conn {
    int      used;
    enum phase phase;
    uint32_t deadline;          /* この時刻までに進まなければ時間切れ */
    int      idle;              /* keep-alive で次のリクエストを待っている(まだ1バイトも来ていない) */
    uint32_t served;            /* このつながりで応答した数 */
    int      keep_alive;        /* 今の応答を送り終えた後も続けるか */

    char     req[REQ_MAX];      /* 届いたリクエスト(見出しがそろうまでためる) */
    uint32_t req_len;

    /* 応答 = 見出し(head) + 本文。sent は見出しの先頭から数えた送信済みのバイト数 */
    char     head[HEAD_MAX];
    uint32_t head_len;
    enum body_kind body_kind;
    const char *body;
    uint32_t body_len;          /* HEAD の応答では 0(見出しの Content-Length は本来の長さ) */
    uint32_t sent;

    char     page[PAGE_MAX];    /* その場で作る本文の置き場 */
};

/* 読み解いたリクエスト */
struct request {
    char method[METHOD_MAX];
    char path[PATH_MAX];        /* ? より前の部分 */
    char host[HOST_MAX];
    int  has_host;
    int  minor;                 /* HTTP/1.1 なら 1、HTTP/1.0 なら 0 */
    int  conn_close;            /* Connection: close */
    int  conn_keep_alive;       /* Connection: keep-alive */
    int  has_body;              /* Content-Length が 0 でない / Transfer-Encoding がある */
};

/* TCP のつながりの番号(0〜TCP_MAX_CONNS-1)をそのまま添え字に使う */
static struct http_conn conns[TCP_MAX_CONNS];
static int running;

struct http_stats http_stats;
http_log_fn http_log;

/* ---- 小さな部品 ---- */

static uint32_t str_len(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* 大文字と小文字を区別せずに比べる(ヘッダーの名前は区別しない決まり) */
static int equal_nocase(const char *a, uint32_t a_len, const char *b)
{
    uint32_t b_len = str_len(b);
    if (a_len != b_len)
        return 0;
    for (uint32_t i = 0; i < a_len; i++) {
        if (lower(a[i]) != lower(b[i]))
            return 0;
    }
    return 1;
}

static int equal(const char *a, uint32_t a_len, const char *b)
{
    return a_len == str_len(b) && memcmp(a, b, a_len) == 0;
}

/* 長さ付きの文字列を、終わりに 0 を付けて写す。入りきらなければ 0 を返す */
static int copy_str(char *dst, uint32_t cap, const char *src, uint32_t len)
{
    if (len >= cap)
        return 0;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 1;
}

/* 文字を書き足していく入れ物。あふれた分は黙って捨てる */
struct out {
    char *p;
    uint32_t len, cap;
};

static void put_bytes(struct out *o, const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n && o->len < o->cap; i++)
        o->p[o->len++] = s[i];
}

static void put_str(struct out *o, const char *s)
{
    put_bytes(o, s, str_len(s));
}

static void put_dec(struct out *o, uint32_t v)
{
    char digits[10];
    int n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n > 0)
        put_bytes(o, &digits[--n], 1);
}

/*
 * HTML の中に、相手から来た文字列をそのまま入れてはいけない。
 * Host に <script> と書かれていたら、ページの中でそのプログラムが動いてしまう(XSS)。
 * 意味を持つ文字を &lt; などの書き方に置き換えて、ただの文字として表示させる。
 */
static void put_html(struct out *o, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '<':  put_str(o, "&lt;");   break;
        case '>':  put_str(o, "&gt;");   break;
        case '&':  put_str(o, "&amp;");  break;
        case '"':  put_str(o, "&quot;"); break;
        case '\'': put_str(o, "&#39;");  break;
        default:   put_bytes(o, s, 1);   break;
        }
    }
}

static void put_ip(struct out *o, const uint8_t *ip)
{
    for (int i = 0; i < 4; i++) {
        if (i > 0)
            put_str(o, ".");
        put_dec(o, ip[i]);
    }
}

/* ---- リクエストを読み解く ---- */

/*
 * 見出しの終わり(空行の次の位置)を探す。まだ届いていなければ 0。
 * 行の区切りは本来 CRLF(\r\n)だが、LF だけの行も受け付ける。
 */
static uint32_t find_head_end(const char *b, uint32_t len)
{
    uint32_t line_start = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (b[i] != '\n')
            continue;
        uint32_t line_len = i - line_start;
        if (line_len > 0 && b[i - 1] == '\r')
            line_len--;
        if (line_len == 0)
            return i + 1;
        line_start = i + 1;
    }
    return 0;
}

/* pos から1行を取り出し、pos を次の行へ進める。行末の \r\n は含めない */
static void next_line(const char *b, uint32_t end, uint32_t *pos,
                      const char **line, uint32_t *line_len)
{
    uint32_t start = *pos, i = start;
    while (i < end && b[i] != '\n')
        i++;
    *line = b + start;
    *line_len = i - start;
    if (*line_len > 0 && b[i - 1] == '\r')
        (*line_len)--;
    *pos = i < end ? i + 1 : end;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

/* Connection: の値(例 "keep-alive, Upgrade")の中に token があるか */
static int has_token(const char *v, uint32_t len, const char *token)
{
    uint32_t i = 0;
    while (i < len) {
        while (i < len && (is_space(v[i]) || v[i] == ','))
            i++;
        uint32_t start = i;
        while (i < len && v[i] != ',')
            i++;
        uint32_t end = i;
        while (end > start && is_space(v[end - 1]))
            end--;
        if (equal_nocase(v + start, end - start, token))
            return 1;
    }
    return 0;
}

/* 1行のヘッダー「名前: 値」を読む。形がおかしければ 400 を返す */
static int parse_header(struct request *r, const char *line, uint32_t len)
{
    /* 行頭の空白は、昔の「前の行の続き」の書き方。今は禁止されている */
    if (len == 0 || is_space(line[0]))
        return 400;

    uint32_t colon = 0;
    while (colon < len && line[colon] != ':')
        colon++;
    if (colon == 0 || colon == len)
        return 400;
    for (uint32_t i = 0; i < colon; i++) {
        if (is_space(line[i]))              /* "Host : x" のような名前の後の空白も禁止 */
            return 400;
    }

    const char *v = line + colon + 1;
    uint32_t v_len = len - colon - 1;
    while (v_len > 0 && is_space(v[0])) {
        v++;
        v_len--;
    }
    while (v_len > 0 && is_space(v[v_len - 1]))
        v_len--;

    if (equal_nocase(line, colon, "Host")) {
        if (r->has_host)                    /* Host が2つあると、どちらを信じるか決められない */
            return 400;
        for (uint32_t i = 0; i < v_len; i++) {
            if ((uint8_t)v[i] <= ' ' || (uint8_t)v[i] >= 0x7F)
                return 400;
        }
        if (!copy_str(r->host, sizeof(r->host), v, v_len))
            return 400;
        r->has_host = 1;
    } else if (equal_nocase(line, colon, "Connection")) {
        if (has_token(v, v_len, "close"))
            r->conn_close = 1;
        if (has_token(v, v_len, "keep-alive"))
            r->conn_keep_alive = 1;
    } else if (equal_nocase(line, colon, "Content-Length")) {
        if (!equal(v, v_len, "0"))
            r->has_body = 1;
    } else if (equal_nocase(line, colon, "Transfer-Encoding")) {
        r->has_body = 1;
    }
    /* ほかのヘッダー(User-Agent、Accept など)は読み飛ばす */
    return 0;
}

/*
 * 見出し b[0..end) を読み解く。問題なければ 0、だめならエラーのステータス番号を返す。
 * エラーでも、読めたところまでは r に入っている(表示に使う)。
 */
static int parse_request(struct request *r, const char *b, uint32_t end)
{
    memset(r, 0, sizeof(*r));

    uint32_t pos = 0;
    const char *line;
    uint32_t len;
    next_line(b, end, &pos, &line, &len);

    /* リクエスト行: メソッド SP ターゲット SP バージョン(空白はちょうど1つずつ) */
    uint32_t sp1 = 0;
    while (sp1 < len && line[sp1] != ' ')
        sp1++;
    uint32_t sp2 = sp1 + 1;
    while (sp2 < len && line[sp2] != ' ')
        sp2++;
    if (sp1 == 0 || sp2 >= len || sp2 == sp1 + 1)
        return 400;

    const char *method = line, *target = line + sp1 + 1, *version = line + sp2 + 1;
    uint32_t method_len = sp1, target_len = sp2 - sp1 - 1, version_len = len - sp2 - 1;

    for (uint32_t i = 0; i < method_len; i++) {
        if (method[i] < 'A' || method[i] > 'Z')
            return 400;
    }
    if (!copy_str(r->method, sizeof(r->method), method, method_len))
        return 501;                         /* こんなに長いメソッドは知らない */

    /* ? から後(クエリ)は今は使わないので外す */
    uint32_t path_len = 0;
    while (path_len < target_len && target[path_len] != '?')
        path_len++;
    if (!copy_str(r->path, sizeof(r->path), target, path_len))
        return 414;
    if (r->path[0] != '/')                  /* "http://host/path" の形には対応していない */
        return 400;

    if (equal(version, version_len, "HTTP/1.1"))
        r->minor = 1;
    else if (equal(version, version_len, "HTTP/1.0"))
        r->minor = 0;
    else if (version_len >= 5 && memcmp(version, "HTTP/", 5) == 0)
        return 505;                         /* HTTP/2.0 など、知らない版 */
    else
        return 400;

    /* ヘッダーを空行まで1行ずつ */
    for (;;) {
        next_line(b, end, &pos, &line, &len);
        if (len == 0)
            break;
        int status = parse_header(r, line, len);
        if (status)
            return status;
    }

    /*
     * HTTP/1.1 では Host が必須。1つの IP アドレスで複数のサイトを動かすとき、
     * どのサイト宛てかを知る手がかりがこれしかないため(⑩ のバーチャルホストで使う)。
     */
    if (r->minor == 1 && !r->has_host)
        return 400;
    return 0;
}

/* ---- 応答を作る ---- */

static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

/* 見出しを作る。content_len は本来の本文の長さ(HEAD でもそのまま書く) */
static void build_head(struct http_conn *h, int status, const char *type, uint32_t content_len)
{
    struct out o = { h->head, 0, sizeof(h->head) };
    put_str(&o, "HTTP/1.1 ");
    put_dec(&o, (uint32_t)status);
    put_str(&o, " ");
    put_str(&o, reason(status));
    put_str(&o, "\r\nServer: hobby-os\r\nContent-Type: ");
    put_str(&o, type);
    put_str(&o, "\r\nContent-Length: ");
    put_dec(&o, content_len);
    if (status == 405)
        put_str(&o, "\r\nAllow: GET, HEAD");
    put_str(&o, h->keep_alive ? "\r\nConnection: keep-alive" : "\r\nConnection: close");
    put_str(&o, "\r\n\r\n");
    h->head_len = o.len;
}

static void respond(struct http_conn *h, const struct request *r, int status,
                    const char *type, enum body_kind kind, const char *body, uint32_t body_len)
{
    build_head(h, status, type, body_len);
    h->body_kind = kind;
    h->body = body;
    h->body_len = strcmp(r->method, "HEAD") == 0 ? 0 : body_len;   /* HEAD は見出しだけ返す */
    h->sent = 0;
    h->phase = SENDING;
}

static void respond_error(struct http_conn *h, const struct request *r, int status)
{
    struct out o = { h->page, 0, sizeof(h->page) };
    put_str(&o, "<!doctype html>\n<title>");
    put_dec(&o, (uint32_t)status);
    put_str(&o, "</title>\n<h1>");
    put_dec(&o, (uint32_t)status);
    put_str(&o, " ");
    put_str(&o, reason(status));
    put_str(&o, "</h1>\n<p>hobby-os</p>\n");
    respond(h, r, status, "text/html; charset=utf-8", BODY_MEMORY, h->page, o.len);
}

static const char hello_txt[] = "Hello from hobby-os!\n";

/* トップページ: 届いたリクエストの中身と、このつながりの様子を見せる */
static void page_index(struct http_conn *h, int id, const struct request *r)
{
    struct tcp_info t;
    tcp_get_info(id, &t);

    struct out o = { h->page, 0, sizeof(h->page) };
    put_str(&o,
        "<!doctype html>\n"
        "<html lang=\"ja\"><head><meta charset=\"utf-8\"><title>hobby-os</title>\n"
        "<style>body{font-family:sans-serif;margin:2em}th{text-align:left;padding-right:1em}</style>\n"
        "</head><body>\n"
        "<h1>hobby-os の Web サーバー</h1>\n"
        "<p>このページは自作OSが返しています。</p>\n<table>\n"
        "<tr><th>Host ヘッダー</th><td>");
    put_html(&o, r->has_host ? r->host : "(なし)");
    put_str(&o, "</td></tr>\n<tr><th>あなたの IP とポート</th><td>");
    put_ip(&o, t.peer_ip);
    put_str(&o, ":");
    put_dec(&o, t.peer_port);
    put_str(&o, "</td></tr>\n<tr><th>TCP のつながりの番号</th><td>");
    put_dec(&o, (uint32_t)id);
    put_str(&o, "</td></tr>\n<tr><th>このつながりで何回目のリクエストか</th><td>");
    put_dec(&o, h->served + 1);
    put_str(&o, "</td></tr>\n<tr><th>応答したリクエストの合計</th><td>");
    put_dec(&o, http_stats.requests + 1);
    put_str(&o, "</td></tr>\n<tr><th>起動してからの時間</th><td>");
    put_dec(&o, timer_ticks() / TIMER_HZ);
    put_str(&o, " 秒</td></tr>\n</table>\n"
        "<p><a href=\"/hello.txt\">/hello.txt</a> ・ "
        "<a href=\"/big\">/big</a>(64KB) ・ "
        "<a href=\"/nothing\">/nothing</a>(404)</p>\n"
        "</body></html>\n");
    respond(h, r, 200, "text/html; charset=utf-8", BODY_MEMORY, h->page, o.len);
}

/* 見出しがそろったリクエストを1つ処理して、応答を用意する */
static void handle_request(struct http_conn *h, int id, uint32_t end)
{
    struct request r;
    int status = parse_request(&r, h->req, end);

    /* 読み終えた分を詰める(後ろに次のリクエストが続いていることがある) */
    h->req_len -= end;
    for (uint32_t i = 0; i < h->req_len; i++)
        h->req[i] = h->req[end + i];

    /* 1. 何を返すか決める */
    enum { PAGE_ERROR, PAGE_INDEX, PAGE_HELLO, PAGE_BIG } page = PAGE_ERROR;
    if (status == 0) {
        if (strcmp(r.method, "GET") != 0 && strcmp(r.method, "HEAD") != 0) {
            /* 知っているメソッドなら 405(ここでは使えない)、知らなければ 501(作っていない) */
            static const char *const known[] = {
                "POST", "PUT", "DELETE", "PATCH", "OPTIONS", "TRACE", "CONNECT",
            };
            status = 501;
            for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
                if (strcmp(r.method, known[i]) == 0)
                    status = 405;
            }
        } else if (strcmp(r.path, "/") == 0) {
            page = PAGE_INDEX;
        } else if (strcmp(r.path, "/hello.txt") == 0) {
            page = PAGE_HELLO;
        } else if (strcmp(r.path, "/big") == 0) {
            page = PAGE_BIG;
        } else {
            status = 404;
        }
        if (page != PAGE_ERROR)
            status = 200;
    }

    /*
     * 2. 応答の後もつながりを続けるか決める(見出しの Connection: に書くので、作る前に)
     *    - HTTP/1.1 は「続ける」が標準、HTTP/1.0 は「閉じる」が標準
     *    - 本文付きのリクエストは本文を読まないので、どこから次のリクエストか分からない。閉じる
     *    - 形の壊れたリクエストの後は、続きも信用できない。閉じる(404 と 405 は形は正しい)
     */
    int well_formed = status == 200 || status == 404 || status == 405;
    h->keep_alive = well_formed &&
                    (r.minor == 1 ? !r.conn_close : r.conn_keep_alive) &&
                    !r.has_body && h->served + 1 < MAX_REQUESTS;

    /* 3. 応答を作る */
    switch (page) {
    case PAGE_INDEX:
        page_index(h, id, &r);
        break;
    case PAGE_HELLO:
        respond(h, &r, 200, "text/plain; charset=utf-8", BODY_MEMORY,
                hello_txt, sizeof(hello_txt) - 1);
        break;
    case PAGE_BIG:
        respond(h, &r, 200, "text/plain; charset=utf-8", BODY_BIG,
                0, BIG_LINES * BIG_LINE_LEN);
        break;
    default:
        respond_error(h, &r, status);
        break;
    }

    http_stats.requests++;
    if (h->served > 0)
        http_stats.keepalive_reused++;
    if (status < 300)
        http_stats.status_2xx++;
    else if (status < 500)
        http_stats.status_4xx++;
    else
        http_stats.status_5xx++;

    if (http_log) {
        struct tcp_info t;
        tcp_get_info(id, &t);
        http_log(t.peer_ip, t.peer_port, r.method, r.path, r.has_host ? r.host : "",
                 status, h->body_len);
    }
}

/* ---- 応答を送る ---- */

/* /big の pos バイト目からを n バイト作る。1行は "line 000123 " + 英字 51 個 + 改行 = 64 バイト */
static void fill_big(char *dst, uint32_t pos, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++, pos++) {
        uint32_t line = pos / BIG_LINE_LEN, col = pos % BIG_LINE_LEN;
        char c;
        if (col < 5) {
            c = "line "[col];
        } else if (col < 11) {
            uint32_t v = line;
            for (uint32_t k = col; k < 10; k++)     /* 6 桁の (col-5) 桁目 */
                v /= 10;
            c = (char)('0' + v % 10);
        } else if (col == 11) {
            c = ' ';
        } else if (col == BIG_LINE_LEN - 1) {
            c = '\n';
        } else {
            c = (char)('a' + line % 26);
        }
        dst[i] = c;
    }
}

/*
 * 応答の続きを、TCP の送信バッファに入るだけ入れる。
 * 見出しと本文をつなげて 1460 バイト(1セグメント分)ずつ渡す。小分けにすると小さなセグメントが増えるため。
 * 全部入れたら 1、まだ残りがあれば 0、つながりが切れていたら -1。
 */
static int push_response(struct http_conn *h, int id)
{
    static char chunk[1460];
    uint32_t total = h->head_len + h->body_len;

    while (h->sent < total) {
        uint32_t space = tcp_send_space(id);
        if (space == 0)
            return 0;
        uint32_t n = total - h->sent;
        if (n > space)
            n = space;
        if (n > sizeof(chunk))
            n = sizeof(chunk);

        for (uint32_t i = 0; i < n; ) {
            uint32_t pos = h->sent + i;
            if (pos < h->head_len) {
                uint32_t k = h->head_len - pos < n - i ? h->head_len - pos : n - i;
                memcpy(chunk + i, h->head + pos, k);
                i += k;
            } else if (h->body_kind == BODY_BIG) {
                fill_big(chunk + i, pos - h->head_len, n - i);
                i = n;
            } else {
                memcpy(chunk + i, h->body + (pos - h->head_len), n - i);
                i = n;
            }
        }

        int sent = tcp_send(id, chunk, n);
        if (sent < 0)
            return -1;
        h->sent += (uint32_t)sent;
        h->deadline = timer_ticks() + SEND_TICKS;   /* 進んだので、待つ時間を延ばす */
        if ((uint32_t)sent < n)
            return 0;
    }
    return 1;
}

/* ---- つながりごとの処理 ---- */

static void start_reading(struct http_conn *h, int idle)
{
    h->phase = READING;
    h->idle = idle;
    h->deadline = timer_ticks() + (idle ? IDLE_TICKS : HEADER_TICKS);
}

/* 届いているだけ読む。相手が送り終えた(FIN)なら 1 を返す */
static int read_more(struct http_conn *h, int id)
{
    while (h->req_len < REQ_MAX) {
        int n = tcp_recv(id, h->req + h->req_len, REQ_MAX - h->req_len);
        if (n < 0)
            return 1;
        if (n == 0)
            break;
        h->req_len += (uint32_t)n;
    }

    /* リクエストの前の空行は読み飛ばしてよい決まり */
    uint32_t skip = 0;
    while (skip < h->req_len && (h->req[skip] == '\r' || h->req[skip] == '\n'))
        skip++;
    if (skip) {
        h->req_len -= skip;
        for (uint32_t i = 0; i < h->req_len; i++)
            h->req[i] = h->req[skip + i];
    }

    /* keep-alive で待っていたところに次のリクエストが来始めたら、見出しの持ち時間で測り直す */
    if (h->idle && h->req_len > 0) {
        h->idle = 0;
        h->deadline = timer_ticks() + HEADER_TICKS;
    }
    return 0;
}

/* 時間切れなどで、こちらから応答を返して閉じる(リクエストは途中なので空にする) */
static void fail(struct http_conn *h, int id, int status)
{
    struct request r;
    memset(&r, 0, sizeof(r));
    h->req_len = 0;
    h->keep_alive = 0;
    respond_error(h, &r, status);
    http_stats.requests++;
    http_stats.status_4xx++;
    if (http_log) {
        struct tcp_info t;
        tcp_get_info(id, &t);
        http_log(t.peer_ip, t.peer_port, "-", "-", "", status, h->body_len);
    }
}

/* 1つのつながりを進める。まだ続くなら 1、閉じたら 0 */
static int serve(struct http_conn *h, int id)
{
    for (;;) {
        uint32_t now = timer_ticks();

        if (h->phase == SENDING) {
            int done = push_response(h, id);
            if (done < 0) {                     /* 相手が切った(RST など) */
                tcp_close(id);
                return 0;
            }
            if (done == 0) {
                if (TIME_REACHED(now, h->deadline)) {
                    /* 受け取ってもらえないまま。FIN も届かないので RST で打ち切る */
                    http_stats.aborted++;
                    tcp_abort(id);
                    return 0;
                }
                return 1;
            }
            h->served++;
            if (!h->keep_alive) {
                tcp_close(id);                  /* 送信バッファの残りを送ってから FIN が出る */
                return 0;
            }
            start_reading(h, 1);
            continue;                           /* 次のリクエストがもう届いているかもしれない */
        }

        /* READING */
        int peer_done = read_more(h, id);
        uint32_t end = find_head_end(h->req, h->req_len);
        if (end) {
            handle_request(h, id, end);
            continue;
        }
        if (h->req_len == REQ_MAX) {            /* 空行が来ないまま入れ物が満杯 */
            fail(h, id, 431);
            continue;
        }
        if (peer_done) {                        /* 途中まで送って帰った(または何も送らずに) */
            tcp_close(id);
            return 0;
        }
        if (TIME_REACHED(now, h->deadline)) {
            http_stats.timeouts++;
            if (h->idle || h->req_len == 0) {   /* 何も来ていない: 黙って閉じる */
                tcp_close(id);
                return 0;
            }
            fail(h, id, 408);                   /* 途中まで来て止まった: 408 を返して閉じる */
            continue;
        }
        return 1;
    }
}

/* ---- 外から使う ---- */

int http_start(void)
{
    if (running)
        return 0;
    if (tcp_listen(HTTP_PORT) != 0)
        return -1;
    running = 1;
    return 0;
}

void http_stop(void)
{
    if (!running)
        return;
    tcp_unlisten(HTTP_PORT);
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (conns[i].used) {
            tcp_close(i);
            conns[i].used = 0;
        }
    }
    running = 0;
}

int http_running(void)
{
    return running;
}

void http_poll(void)
{
    if (!running)
        return;

    int id;
    while ((id = tcp_accept(HTTP_PORT)) >= 0) {
        struct http_conn *h = &conns[id];
        h->used = 1;
        h->served = 0;
        h->req_len = 0;
        start_reading(h, 0);
        http_stats.connections++;
    }

    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (conns[i].used && !serve(&conns[i], i))
            conns[i].used = 0;
    }
}
