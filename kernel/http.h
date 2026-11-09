/*
 * http.h - Web サーバー(HTTP/1.1 の GET と HEAD だけ)
 *
 * エコーサーバー(echo.c)と同じく、メインのループから http_poll を呼んでもらい、
 * 少しずつ処理して、待たずにすぐ戻る。
 */
#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

#define HTTP_PORT 80            /* HTTP の決まりのポート番号 */

int  http_start(void);          /* 成功 0、失敗 -1 */
void http_stop(void);           /* 待ち受けをやめて、今のつながりをこちらから閉じる */
int  http_running(void);
void http_poll(void);           /* メインのループから呼び続ける */

struct http_stats {
    uint32_t connections;       /* 受け付けたつながり */
    uint32_t requests;          /* 応答したリクエスト(エラーを含む) */
    uint32_t status_2xx;        /* 成功 */
    uint32_t status_4xx;        /* 相手の間違い(404 など) */
    uint32_t status_5xx;        /* こちらが対応していない(501 など) */
    uint32_t keepalive_reused;  /* 同じつながりで2つ目以降のリクエストを受けた回数 */
    uint32_t timeouts;          /* 時間切れで閉じた回数 */
    uint32_t aborted;           /* 応答を受け取ってもらえず RST で切った回数 */
};
extern struct http_stats http_stats;

/*
 * 1つのリクエストに応答するたびに呼ばれる(http log on のとき)。
 * host は Host ヘッダーの値("" なら無し)、site は答えたサイトの名前("" ならサイトを選ぶ前のエラー)
 */
typedef void (*http_log_fn)(const uint8_t *peer_ip, uint16_t peer_port,
                            const char *method, const char *path, const char *host,
                            const char *site, int status, uint32_t body_len);
extern http_log_fn http_log;

#endif
