# ⑩ バーチャルホスト: 1つの住所で、いくつものサイトを動かす

⑨ で、自作OSの Web サーバーがブラウザにページを返せるようになった。⑩ では、**同じ IP アドレス、同じポートで、Host ヘッダーを見て別々のサイトを返す**。Apache の `<VirtualHost>` がやっていることを、自分の OS の上で再現する。これがこのリポジトリのゴール。

```
  ⑩ バーチャルホスト   ← ここ。Host: blog.localhost → ブログのサイト
  ⑨ HTTP               GET / HTTP/1.1
  ⑧ TCP                つながりを作る
  ⑦ IP / ARP / ICMP    番号で相手を探す、ping
  ⑥ Ethernet フレーム  線の上を流れる「ひとかたまり」
```

## 動かしてみる

```
make run
```

起動したら、**Windows のブラウザ(Chrome か Edge)** で次の4つを開いて見比べる。

| URL | 答えるサイト | 見えるもの |
|---|---|---|
| `http://localhost:8080/` | localhost | ⑨ のページ。「答えたサイト」の行と、サイトへのリンクが増えた |
| `http://blog.localhost:8080/` | blog.localhost | 薄い黄色のブログ |
| `http://shop.localhost:8080/` | shop.localhost | 紺とオレンジのショップ |
| `http://127.0.0.1:8080/` | 既定のサイト | 「このサイトはありません」とサイトの一覧 |

4つとも、行き先は **まったく同じ**(Windows の 127.0.0.1:8080 → QEMU の転送 → 自作OSの 10.0.2.15:80)。違うのはブラウザが送る `Host:` の1行だけ。

> **`blog.localhost` は hosts ファイルを書き換えなくても開ける。** `.localhost` で終わる名前は「この PC 自身」と決められていて(RFC 6761)、Chrome と Edge は DNS に問い合わせずに 127.0.0.1 につなぐ。
>
> 開けないときは、Windows の PowerShell から `curl.exe -H "Host: blog.localhost" http://localhost:8080/` で同じことを確かめられる。

QEMU の画面のアクセスログには、**どのサイトが答えたか** が出るようになった(80 桁に収めるため、⑨ より詰めた)。

```
HTTP 10.0.2.2:59026 GET / blog.localhost:8080 -> blog.localhost 200 630B
HTTP 10.0.2.2:59046 GET / 127.0.0.1:8080 -> _default_ 200 878B
HTTP 10.0.2.2:59060 GET /hello.txt blog.localhost:8080 -> blog.localhost 404 91B
     相手             リクエスト   Host ヘッダー        答えたサイト  結果 本文
```

`vhost` コマンドで、サイトの一覧と、それぞれが答えた数が見られる。

```
> vhost
_default_ [default: no name matched]: requests 1, files /
localhost (alias hobby-os.localhost): requests 3, files / /hello.txt /big
blog.localhost (alias www.blog.localhost): requests 2, files / /about.html
shop.localhost: requests 1, files / /items.txt
```

実験室(`make run-ping`)の中からは、`curl --connect-to ::10.0.2.15:80 http://blog.localhost/` でつながる。`--connect-to` は「URL の名前はそのままで、つなぐ先だけ 10.0.2.15 にする」指定で、DNS の代わりになる。

## 1. なぜ Host で振り分けるのか

TCP の段階で分かるのは「**10.0.2.15 のポート 80 に来た**」ことだけ。ブログ宛てでもショップ宛てでも、ここまでは区別がつかない。

```
 ブラウザ                                   hobby-os
 ┌────────────────────┐                   ┌──────────────────────────────┐
 │ blog.localhost を開く│                   │ TCP: 10.0.2.15:80 に来た     │
 │   │                 │                   │      (どのサイト宛てか不明)  │
 │   ▼ 名前 → 番号     │                   │                              │
 │ 127.0.0.1 にする    │── TCP でつなぐ ──▶│ HTTP: Host: blog.localhost   │
 │                     │                   │      ここで初めて分かる      │
 │ GET / HTTP/1.1      │── リクエスト ───▶ │   ▼                          │
 │ Host: blog.localhost│                   │ vhost_find → ブログのサイト  │
 └────────────────────┘                   └──────────────────────────────┘
```

名前を番号に変える段階(DNS)で、名前の情報は捨てられてしまう。そこで **ブラウザが、開こうとした名前を Host ヘッダーに書いて届ける**。これが ⑨ で「HTTP/1.1 では Host が必須」とした理由。

| 方式 | 何でサイトを区別するか | 困ること |
|---|---|---|
| IP ベース | サイトごとに別の IP アドレス | IPv4 のアドレスは足りない |
| **名前ベース**(⑩) | Host ヘッダー | HTTP/1.0 の古いブラウザは Host を送らないことがある |

今の Web サーバーのほとんどは名前ベース。1台のサーバー(1つの IP)で何百ものサイトを動かすレンタルサーバーも、このしくみで成り立っている。

## 2. 選び方

`kernel/vhost.c` の `vhosts[]` がサイトの一覧で、Apache の設定ファイルにあたる。

| hobby-os(`vhost.c`) | Apache の設定 |
|---|---|
| `vhosts[0]`(`_default_`) | 一番上に書いた `<VirtualHost>` |
| `name` | `ServerName` |
| `aliases` | `ServerAlias` |
| `files` | `DocumentRoot` のフォルダの中身(ファイルシステムがないので文字列で持つ) |

リクエストが来るたびに、`handle_request` がこの順で決める。

```
 Host: Blog.LocalHost.:8080
        │
        ▼ (1) 整える vhost_normalize
 "blog.localhost"   (ポート "8080" は別に取っておく)
        │
        ▼ (2) 一覧の上から比べる vhost_find
 ServerName / ServerAlias と一致? ── 一致しない ──▶ 一覧の先頭(既定のサイト)
        │ 一致
        ▼ (3) サイトの中でパスを探す vhost_file
 "/" がある? ── ない ──▶ 404(どのサイトの 404 かをページに書く)
        │ ある
        ▼
 200 とそのページ
```

### (1) 整える

同じサイトを指しているのに書き方が違う、という Host を同じ名前にそろえる。

| 届いた Host | 整えた名前 | なぜ |
|---|---|---|
| `BLOG.LocalHost` | `blog.localhost` | 名前は大文字と小文字を区別しない(DNS の決まり) |
| `blog.localhost:8080` | `blog.localhost` | ポートが 80 以外だと、ブラウザが付けてくる |
| `blog.localhost.` | `blog.localhost` | 最後の `.` は「ここが根」の印で、付けても同じ名前 |
| `[::1]:8080` | `[::1]` | IPv6 のアドレスは `[ ]` で囲む。`:` が多いので、最後の `:` の後が数字のときだけポートとみなす |
| `localhost:abc` | `localhost:abc` | `:` の後が数字でないのでポートではない → どのサイトとも一致しない |

**ポートでは選ばない**。どのポートで受けたかは TCP の段階で決まっている。Host に付いている `8080` はブラウザから見た番号で、QEMU の転送の後、自作OSは 80 で受けている。食い違うのが普通。

### (2) 比べる

整えた名前を、一覧の上から ServerName と ServerAlias に **ぴったり一致するか** で比べる。

- `blog.localhost.evil.test` は、`blog.localhost` で始まっていても一致しない
- `evil-blog.localhost` は、`blog.localhost` で終わっていても一致しない

「含んでいれば OK」のような緩い比べ方をすると、他人の名前で自分のサイトを開かれたり、その逆が起きたりする。

### (3) 既定のサイト

どれとも一致しないときは **一覧の先頭のサイト** が答える。Apache も同じ決まり。こんなときに使われる。

- IP アドレスで直接開いた(`http://127.0.0.1:8080/`、実験室の `curl http://10.0.2.15/`)
- 知らない名前を誰かが自分の IP に向けた
- Host を送らない HTTP/1.0

**本物のサイトを先頭に置くと、どんな名前で来ても、そのサイトが答えてしまう**。インターネットには IP アドレスを順番に試して回るプログラムがいて、名前を知らなくてもサイトの中身を見られてしまう。そのため、先頭には中身のない受け皿を置くのがよくある使い方。hobby-os でも `_default_` を先頭に置いて、サイトの一覧だけを見せている。

既定のサイトは名前では選ばない。`Host: _default_` と送っても「名前で一致」にはならず、「どれとも一致しないので既定のサイト」になる。

## 3. Host はリクエストごと

1つの TCP のつながり(keep-alive)の中で、**リクエストごとに別のサイトを頼める**。サイトを選ぶのはつながりではなく、1通ずつの手紙。

```
GET / HTTP/1.1          ← 1通目: ブログに答えてもらう
Host: blog.localhost

GET / HTTP/1.1          ← 2通目: 同じつながりで、ショップに答えてもらう
Host: shop.localhost
```

テストで確かめている。ふつうのブラウザはサイトごとに別のつながりを使うが、サーバーはこの形も受けられないといけない。

> **HTTPS では順番が問題になる。** 暗号のつながり(TLS)を作るとき、サーバーは「どのサイトの証明書を出すか」を、Host を読む **前に** 決めないといけない。そこで TLS のあいさつの中に名前を入れて送る **SNI** というしくみが追加された。名前ベースの振り分けが、暗号の段階まで下りてきた形。

## 4. コードの中

| ファイル | 変わったところ |
|---|---|
| `kernel/vhost.c` / `vhost.h` | **新しく作った**。サイトの一覧(各サイトのファイルもここ)、`vhost_normalize`、`vhost_find`、`vhost_file` |
| `kernel/http.c` | `handle_request` の「何を返すか決める」を、パスだけでなく **Host とパスの組** で選ぶ形にした。既定のサイトのページ(`page_sites`)と、サイトの一覧のリンクを足した。404 のページにサイトの名前を書く |
| `kernel/kernel.c` | `vhost` コマンド。アクセスログに答えたサイトを出す |
| `tools/http-test.py` | ⑩ のテストを 20 件足した。⑨ のテストは `Host: localhost` を付ける形に直した(下の 5 節) |

ファイルの中身は3種類ある。

| 種類 | 使っているところ |
|---|---|
| `FILE_STATIC` | 文字列をそのまま返す。ブログ、ショップ、`/hello.txt` |
| `FILE_STATUS` / `FILE_SITES` | その場で作る。localhost のトップページ、既定のサイトのページ |
| `FILE_BIG` | その場で作る。⑨ の `/big` |

### サイトへのリンクにポートを引き継ぐ

既定のサイトと localhost のトップページは、各サイトへのリンクを出している。Windows から `localhost:8080` で開いているなら、リンクも `http://blog.localhost:8080/` でないと届かない。そこで **Host に付いていたポートをリンクに付ける**。

ポートは `vhost_normalize` で「数字だけ」と確かめてあるので、HTML にそのまま入れても安全。相手から来た Host の名前そのものは、⑨ と同じく `put_html` でエスケープしてから入れている。

## 5. どう確かめたか

### `make test-http`(約 3 分、50 件)

⑨ の 30 件に、⑩ の 20 件を足した。

| 分類 | 確かめたこと |
|---|---|
| 振り分け | 同じ IP、ポート、パス `/` で、Host が localhost / blog / shop なら3つの別のページ |
| curl | `curl --connect-to` で `http://blog.localhost/about.html`(Host に URL の名前が入る) |
| 選ばれ方 | localhost は ServerName、hobby-os.localhost と www.blog.localhost は ServerAlias で一致 |
| 整える | `BLOG.LocalHost`、`blog.localhost:8080`、`blog.localhost.`、`Blog.Localhost.:80` が全部ブログ |
| サイトごとのファイル | `/hello.txt` はブログでは 404(ページに `blog.localhost` と出る)、`/items.txt` はショップだけ |
| 既定のサイト | `nowhere.test`、`10.0.2.15`、`localhost:abc`、`[::1]:8080`、`_default_`、`blog.localhost.evil.test`、`evil-blog.localhost` が全部既定のサイト |
| リンク | 既定のサイトのリンクが `http://blog.localhost:8080/` になる |
| keep-alive | 1つのつながりで、ブログとショップに1通ずつ |

**⑨ のテストの直し方**: ⑨ では、どんな Host でも ⑨ のページが答えていたので、テストは `Host: a` や `http://10.0.2.15/hello.txt` で `/hello.txt` を取っていた。⑩ では、それらは既定のサイトに回り、`/hello.txt` は 404 になる。そこで ⑨ のページを取るテストには `Host: localhost` を付けた。HTTP/1.0 で Host を送らないテストは、既定のサイトの `/` を取る形にし、「Host ヘッダーがありません」と出ることも確かめるようにした。

`make test-tcp` と `make test-tcp-linux` も、⑩ の変更の後に通ることを確かめた。

### 転送経由

`make run` と同じ転送の設定で起動し、WSL から curl で `http://localhost:8080/`、`http://blog.localhost:8080/`、`http://shop.localhost:8080/`、`http://127.0.0.1:8080/` を開いて、それぞれ別のサイトが答えることを確かめた。WSL の curl も `.localhost` の名前を自分で 127.0.0.1 につなぐので、ブラウザと同じ形で試せる。QEMU の画面のアクセスログと `vhost` の表示も、画面のメモリを読んで確かめた。

**Windows のブラウザで開くのは、まだ試していない**(⑨ と同じく、最初にやってみてほしい)。

## いじってみよう

- ブラウザで3つのサイトを開いてから `vhost` を打つ。数は合っているか。`favicon.ico` の分はどのサイトに数えられているか
- `vhost.c` の `vhosts[]` で、`localhost` の行を `_default_` より上に動かす。`http://127.0.0.1:8080/` を開くと何が出るか(Apache で「一番上の VirtualHost」が大事な理由)。試したら戻す
- `vhost.c` に4つ目のサイト(たとえば `wiki.localhost`)を足す。何か所を書き足せば済むか
- `blog_aliases` に `"blog.test"` を足して、Windows の PowerShell から `curl.exe -H "Host: blog.test" http://localhost:8080/` を打つ
- 実験室から `curl -v http://10.0.2.15/` と `curl -v --connect-to ::10.0.2.15:80 http://shop.localhost/` を打ち、`-v` で見える `Host:` の行を比べる
- `vhost_normalize` の小文字にする処理を外すと、テストのどれが落ちるか(試したら戻す)

## 補足: 作っていないもの

| 作っていないもの | 本物では | ここで困らない理由、困るとき |
|---|---|---|
| ワイルドカード(`ServerAlias *.blog.localhost`) | 名前の一部が何でも一致させる | 名前が少ない。足すなら「どこまでを一致とみなすか」を決める必要がある |
| サイトごとのフォルダ(DocumentRoot) | ディスクのフォルダからファイルを読む | ファイルシステムがない。ページは `vhost.c` の中にある |
| IP ベース、ポートごとのサイト(`<VirtualHost 10.0.2.15:8080>`) | 受けた IP とポートでも分ける | 受けているのはポート 80 の1つだけ |
| 421 Misdirected Request | このサーバーでは答えられない名前に、200 ではなくエラーを返す | 既定のサイトで一覧を見せるほうが、学習には分かりやすい |
| HTTPS と SNI | 暗号のあいさつの中の名前で証明書を選ぶ | TLS を作っていない(⑨ の補足) |
| 日本語のドメイン名(`ブログ.localhost`) | `xn--` で始まる英数字の形(Punycode)に直して送られる | ASCII の名前として比べるだけで動く |

## ゴールに着いた

① で画面に1文字を出したところから、**自作の OS の上で、自作のネットワークの部品(ドライバ、ARP、IP、TCP)を通って、自作の Web サーバーが Host を見てサイトを選び分ける** ところまで来た。

ブラウザで `blog.localhost:8080` を開いたとき、自作OSの中で起きていることを ① から順に言えるか、[learning-log.md](learning-log.md) のキーワードで確かめてみてほしい。
