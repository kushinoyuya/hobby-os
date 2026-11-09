/*
 * vhost.c - サイトの一覧と、Host ヘッダーからサイトを選ぶしくみ
 *
 * TCP の段階では「10.0.2.15 のポート 80 に来た」ことしか分からない。どのサイト宛てかは、
 * HTTP のリクエストの中の Host ヘッダーを読んで初めて分かる。
 *
 * 下の vhosts[] は、Apache の設定ファイルで書くと次のようなもの。
 *
 *   <VirtualHost *:80>                      ← 一番上に書いたものが「既定のサイト」になる
 *       ServerName _default_
 *   </VirtualHost>
 *   <VirtualHost *:80>
 *       ServerName  localhost
 *       ServerAlias hobby-os.localhost
 *   </VirtualHost>
 *   <VirtualHost *:80>
 *       ServerName  blog.localhost
 *       ServerAlias www.blog.localhost
 *   </VirtualHost>
 *   <VirtualHost *:80>
 *       ServerName  shop.localhost
 *   </VirtualHost>
 *
 * 選び方(Apache の名前ベースのバーチャルホストと同じ):
 *   1. Host を整える(小文字にする、ポート番号と最後の "." を外す)
 *   2. 上から順に(先頭の既定のサイトは飛ばして)ServerName と ServerAlias を比べ、最初に一致したサイト
 *   3. どれとも一致しなければ(Host がない HTTP/1.0 も)、一覧の先頭のサイト
 *
 * ファイルシステムがないので、各サイトのファイルはここに文字列で持っている。
 * 本物の Apache なら、サイトごとの DocumentRoot(フォルダ)から読む。
 */
#include "vhost.h"
#include "string.h"

/* ---- サイト 0: 既定のサイト(どの名前にも当てはまらなかったとき) ---- */

static const struct vhost_file default_files[] = {
    { "/", "text/html; charset=utf-8", FILE_SITES, 0 },
    { 0, 0, 0, 0 },
};

/* ---- サイト 1: localhost(⑨ で作ったページ) ---- */

static const char hello_txt[] = "Hello from hobby-os!\n";

static const char *const localhost_aliases[] = { "hobby-os.localhost", 0 };

static const struct vhost_file localhost_files[] = {
    { "/",          "text/html; charset=utf-8",  FILE_STATUS, 0 },
    { "/hello.txt", "text/plain; charset=utf-8", FILE_STATIC, hello_txt },
    { "/big",       "text/plain; charset=utf-8", FILE_BIG,    0 },
    { 0, 0, 0, 0 },
};

/* ---- サイト 2: blog.localhost ---- */

#define BLOG_HEAD \
    "<!doctype html>\n" \
    "<html lang=\"ja\"><head><meta charset=\"utf-8\"><title>ブログ - hobby-os</title>\n" \
    "<style>body{font-family:serif;margin:2em auto;max-width:40em;background:#fdf6e3;color:#333}" \
    "h1{color:#b58900}a{color:#268bd2}</style>\n" \
    "</head><body>\n"

static const char blog_index[] =
    BLOG_HEAD
    "<h1>hobby-os のブログ</h1>\n"
    "<p>このページは <b>blog.localhost</b> というサイトです。</p>\n"
    "<p>localhost のページと同じ OS、同じ IP アドレス、同じポートから返っています。"
    "違うのはリクエストの <code>Host:</code> だけです。</p>\n"
    "<h2>記事</h2>\n"
    "<ul><li><a href=\"/about.html\">このブログについて</a></li></ul>\n"
    "</body></html>\n";

static const char blog_about[] =
    BLOG_HEAD
    "<h1>このブログについて</h1>\n"
    "<p>自作OSの上で動く Web サーバーが、Host ヘッダーを見て返しています。</p>\n"
    "<p><a href=\"/\">記事の一覧へ</a></p>\n"
    "</body></html>\n";

static const char *const blog_aliases[] = { "www.blog.localhost", 0 };

static const struct vhost_file blog_files[] = {
    { "/",           "text/html; charset=utf-8", FILE_STATIC, blog_index },
    { "/about.html", "text/html; charset=utf-8", FILE_STATIC, blog_about },
    { 0, 0, 0, 0 },
};

/* ---- サイト 3: shop.localhost ---- */

static const char shop_index[] =
    "<!doctype html>\n"
    "<html lang=\"ja\"><head><meta charset=\"utf-8\"><title>ショップ - hobby-os</title>\n"
    "<style>body{font-family:sans-serif;margin:0;background:#1e2a38;color:#eee}"
    "header{background:#e4572e;padding:1em 2em}main{padding:1em 2em}a{color:#ffc857}</style>\n"
    "</head><body>\n"
    "<header><h1>hobby-os ショップ</h1></header>\n"
    "<main><p>このページは <b>shop.localhost</b> というサイトです。</p>\n"
    "<p><a href=\"/items.txt\">商品の一覧(テキスト)</a></p></main>\n"
    "</body></html>\n";

static const char shop_items[] =
    "フロッピーディスク  1枚\n"
    "ブートセクタ        512バイト\n"
    "割り込み            256本\n";

static const struct vhost_file shop_files[] = {
    { "/",          "text/html; charset=utf-8",  FILE_STATIC, shop_index },
    { "/items.txt", "text/plain; charset=utf-8", FILE_STATIC, shop_items },
    { 0, 0, 0, 0 },
};

/* ---- サイトの一覧(順番に意味がある: 先頭が既定のサイト) ---- */

static const char *const no_aliases[] = { 0 };

struct vhost vhosts[] = {
    { "_default_",      no_aliases,        default_files,   0 },
    { "localhost",      localhost_aliases, localhost_files, 0 },
    { "blog.localhost", blog_aliases,      blog_files,      0 },
    { "shop.localhost", no_aliases,        shop_files,      0 },
};
const int vhost_count = sizeof(vhosts) / sizeof(vhosts[0]);

/* ---- 選ぶしくみ ---- */

void vhost_normalize(const char *host, char *name, char *port)
{
    uint32_t len = 0;
    while (host[len])
        len++;

    /*
     * 最後の ':' より後が数字だけなら、それはポート番号。
     *   "localhost:8080" → "localhost" と "8080"
     *   "[::1]:8080"     → "[::1]" と "8080"(IPv6 のアドレスは [ ] で囲む決まり)
     *   "[::1]"          → ':' の後ろが "1]" で数字だけではないので、全体が名前
     * ブラウザは、ポートが 80 以外のとき Host に付けてくる。名前ベースの振り分けでは
     * ポートは使わない(どのポートで受けたかは、TCP の段階で決まっている)。
     */
    uint32_t name_len = len;
    port[0] = '\0';
    for (uint32_t i = len; i > 0; i--) {
        if (host[i - 1] != ':')
            continue;
        uint32_t digits = len - i;
        int all_digits = digits > 0 && digits < VHOST_PORT_MAX;
        for (uint32_t k = i; k < len && all_digits; k++) {
            if (host[k] < '0' || host[k] > '9')
                all_digits = 0;
        }
        if (all_digits) {
            name_len = i - 1;
            memcpy(port, host + i, digits);
            port[digits] = '\0';
        }
        break;
    }

    /* "blog.localhost." のように最後に "." を付けても同じ名前(DNS の書き方で「ここが根」の印) */
    if (name_len > 0 && host[name_len - 1] == '.')
        name_len--;

    /* 名前は大文字と小文字を区別しない(DNS の決まり) */
    if (name_len >= VHOST_NAME_MAX)
        name_len = VHOST_NAME_MAX - 1;
    for (uint32_t i = 0; i < name_len; i++) {
        char c = host[i];
        name[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    name[name_len] = '\0';
}

struct vhost *vhost_find(const char *name, enum vhost_match *how)
{
    /* 先頭(既定のサイト)は名前では選ばない。どれにも当てはまらなかったときの受け皿 */
    if (name[0] != '\0') {
        for (int i = 1; i < vhost_count; i++) {
            if (strcmp(name, vhosts[i].name) == 0) {
                *how = MATCH_NAME;
                return &vhosts[i];
            }
            for (const char *const *a = vhosts[i].aliases; *a; a++) {
                if (strcmp(name, *a) == 0) {
                    *how = MATCH_ALIAS;
                    return &vhosts[i];
                }
            }
        }
    }
    *how = MATCH_DEFAULT;
    return &vhosts[0];
}

const struct vhost_file *vhost_file(const struct vhost *v, const char *path)
{
    for (const struct vhost_file *f = v->files; f->path; f++) {
        if (strcmp(path, f->path) == 0)
            return f;
    }
    return 0;
}
