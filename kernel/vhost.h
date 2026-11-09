/*
 * vhost.h - バーチャルホスト(1つの IP アドレスとポートで、複数のサイトを動かす)
 *
 * http.c はリクエストの Host ヘッダーをここで調べて、どのサイトのどのファイルを返すか決める。
 * サイトの一覧は vhost.c にある(Apache の設定ファイルの <VirtualHost> にあたる)。
 */
#ifndef VHOST_H
#define VHOST_H

#include <stdint.h>

#define VHOST_NAME_MAX 128      /* 整えた後の名前の上限(http.c の Host の上限と同じ) */
#define VHOST_PORT_MAX 6        /* "65535" + 終わりの 0 */

/* ファイルの中身の出どころ */
enum vhost_file_kind {
    FILE_STATIC,                /* body に書いてある文字列をそのまま返す */
    FILE_STATUS,                /* その場で作る: リクエストとつながりの様子(⑨ のトップページ) */
    FILE_SITES,                 /* その場で作る: 当てはまるサイトがなかったことと、サイトの一覧 */
    FILE_BIG,                   /* その場で作る: 64KB のテキスト(⑨ の /big) */
};

struct vhost_file {
    const char *path;           /* "/" や "/about.html"。0 で一覧の終わり */
    const char *type;           /* Content-Type */
    enum vhost_file_kind kind;
    const char *body;           /* FILE_STATIC のときの中身 */
};

struct vhost {
    const char *name;           /* ServerName。既定のサイトは "_default_" */
    const char *const *aliases; /* ServerAlias。0 で終わる並び */
    const struct vhost_file *files;
    uint32_t requests;          /* このサイトが答えたリクエストの数 */
};

/* どうやってサイトが選ばれたか */
enum vhost_match {
    MATCH_NAME,                 /* ServerName と一致 */
    MATCH_ALIAS,                /* ServerAlias のどれかと一致 */
    MATCH_DEFAULT,              /* どれとも一致しない、または Host がない → 一覧の先頭 */
};

extern struct vhost vhosts[];
extern const int vhost_count;

/*
 * Host ヘッダーの値を、比べられる形に整える。
 *   "Blog.LocalHost:8080" → name "blog.localhost"、port "8080"
 * 大文字を小文字に、末尾のポート番号を port へ、名前の最後の "." を外す。
 */
void vhost_normalize(const char *host, char *name, char *port);

/* 整えた名前に当てはまるサイトを返す(必ずどれかが返る)。how に選ばれ方を入れる */
struct vhost *vhost_find(const char *name, enum vhost_match *how);

/* サイトの中からパスに当てはまるファイルを探す。なければ 0 */
const struct vhost_file *vhost_file(const struct vhost *v, const char *path);

#endif
