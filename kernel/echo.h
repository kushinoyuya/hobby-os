/*
 * echo.h - エコーサーバー(受け取ったものをそのまま送り返す。TCP の動作確認用)
 */
#ifndef ECHO_H
#define ECHO_H

#define ECHO_PORT 7             /* エコーの決まりのポート番号 */

int  echo_start(void);          /* 成功 0、失敗 -1 */
void echo_stop(void);           /* 待ち受けをやめて、今のつながりをこちらから閉じる */
int  echo_running(void);
void echo_poll(void);           /* メインのループから呼び続ける */

#endif
