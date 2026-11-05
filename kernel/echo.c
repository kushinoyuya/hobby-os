/*
 * echo.c - エコーサーバー
 *
 * TCP の上で動く最初のアプリ。tcp.h の使い方の見本でもある。
 * OS にはまだ「プロセス」がないので、メインのループから echo_poll を呼んでもらい、
 * 少しずつ処理して、待たずにすぐ戻る。⑨ の Web サーバーも同じ形で作る。
 */
#include <stdint.h>
#include "echo.h"
#include "tcp.h"

static int running;
static int conns[TCP_MAX_CONNS];
static int conn_count;

int echo_start(void)
{
    if (running)
        return 0;
    if (tcp_listen(ECHO_PORT) != 0)
        return -1;
    running = 1;
    return 0;
}

void echo_stop(void)
{
    if (!running)
        return;
    tcp_unlisten(ECHO_PORT);
    for (int i = 0; i < conn_count; i++)
        tcp_close(conns[i]);
    conn_count = 0;
    running = 0;
}

int echo_running(void)
{
    return running;
}

/* 1つのつながりを処理する。まだ続くなら 1、閉じたら 0 を返す */
static int serve(int id)
{
    static uint8_t buf[1460];

    for (;;) {
        /*
         * 送信バッファに入る分だけ読む。読まなければ受信バッファが埋まり、
         * ウィンドウが 0 になって、相手に「待って」と伝わる(フロー制御)。
         */
        uint32_t space = tcp_send_space(id);
        uint32_t want = space < sizeof(buf) ? space : sizeof(buf);

        int n = tcp_recv(id, buf, want);
        if (n < 0) {                    /* 相手が送り終えた。こちらも閉じる */
            tcp_close(id);
            return 0;
        }
        if (n == 0)
            return 1;
        tcp_send(id, buf, (uint32_t)n);
    }
}

void echo_poll(void)
{
    if (!running)
        return;

    int id;
    while (conn_count < TCP_MAX_CONNS && (id = tcp_accept(ECHO_PORT)) >= 0)
        conns[conn_count++] = id;

    for (int i = 0; i < conn_count; ) {
        if (serve(conns[i])) {
            i++;
        } else {
            conns[i] = conns[--conn_count];     /* 最後のものを空いた場所へ移す */
        }
    }
}
