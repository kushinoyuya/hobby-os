# ⑪ 本物の Apache と見比べる

⑩ で、自作OSの Web サーバーが Host ヘッダーを見てサイトを選べるようになった。⑪ では、**同じサイトの構成を本物の Apache で作り、同じリクエストを両方に送って答えを比べる**。

- 自作OSで作ったしくみが、Apache の設定ファイルではどの行にあたるかが分かる
- 答えが違ったところから、HTTP の決まり(RFC)の細かいところと、本物のサーバーの用心深さが見える
- Apache を自分で設定、起動、確認する練習になる(SRE の仕事でよく触る)

```
 Windows のブラウザ
   │ http://blog.localhost:8080/          │ http://blog.localhost:8081/
   ▼                                      ▼
 QEMU の転送 → 自作OS 10.0.2.15:80        WSL の Apache 127.0.0.1:8081
   kernel/vhost.c の vhosts[]               apache/httpd.conf の <VirtualHost>
```

## 動かしてみる

**打つ場所:** Ubuntu のターミナルで `cd ~/hobby-os`

### ブラウザで並べて見る

```
make apache-start        # Apache を 8081 番で起動(裏で動き続ける)
make run                 # 自作OSを 8080 番で起動(QEMU の窓が開く)
```

Windows のブラウザで、ポートだけ違う URL をタブに並べる。

| hobby-os | Apache | 答えるサイト |
|---|---|---|
| `http://localhost:8080/` | `http://localhost:8081/` | localhost |
| `http://blog.localhost:8080/` | `http://blog.localhost:8081/` | blog.localhost |
| `http://shop.localhost:8080/` | `http://shop.localhost:8081/` | shop.localhost |
| `http://127.0.0.1:8080/` | `http://127.0.0.1:8081/` | 既定のサイト |

ページの中身はわざと同じにしてある(見出しの「hobby-os」が「Apache」に変わるだけ)。終わったら `make apache-stop` で Apache を止める。

> Apache は WSL の中の 127.0.0.1:8081 で受けている。Windows の `localhost:8081` から届くのは、WSL が Windows の localhost への接続を WSL の中へ渡しているため(WSL2 の localhost 転送)。自作OSの 8080 は QEMU が転送している。届け方は違うが、ブラウザからはどちらも「この PC の 8080 / 8081」に見える。

### 自動で見比べる

```
make compare-apache
```

30 通りのリクエストを両方に送り、答えを1行ずつ並べる(約 30 秒)。自作OSや Apache が動いていなければ、その間だけ起動して、終わったら止める。

```
      hobby-os        Apache          送ったもの
      --------------  --------------  ----------
[振り分け]
同じ  200 localhost   200 localhost    1. Host: localhost
同じ  200 blog        200 blog         3. Host: blog.localhost
...
違う  200 default     400 -           17. Host: localhost:abc(ポートが数字でない)
...
30 件中 4 件で答えが違った
```

`200 blog` は「ステータス 200 で、blog.localhost が答えた」、`400 -` は「サイトを選ぶ前に 400 で断った」という意味。最後に Apache のアクセスログも出る。

## 1. 設定ファイルを読む

`apache/httpd.conf` が、自作OSの `kernel/vhost.c` にあたる。

```apache
<VirtualHost *:8081>                ← 一番上 = 既定のサイト(vhosts[0])
    ServerName    default.invalid
    DocumentRoot  .../apache/sites/default
</VirtualHost>

<VirtualHost *:8081>                ← vhosts[2]
    ServerName    blog.localhost             ← name
    ServerAlias   www.blog.localhost         ← aliases
    DocumentRoot  .../apache/sites/blog      ← files[](フォルダの中のファイル)
    ErrorDocument 404 "...Apache (blog.localhost)..."   ← respond_error の 404 ページ
</VirtualHost>
```

| hobby-os | Apache | 違い |
|---|---|---|
| `vhosts[]` の順番 | `<VirtualHost>` を書いた順番 | 同じ。どちらも先頭が既定のサイト |
| `_default_`(名前では選ばない) | `ServerName default.invalid` | Apache では既定のサイトにも名前が要る。`.invalid` は「どこにも存在しない」と決められた名前(RFC 6761)なので、本物の名前とぶつからない |
| `files[]`(C の文字列) | `DocumentRoot`(フォルダ) | Apache はディスクからファイルを読む。ページを足すのにビルドし直さなくてよい |
| `page_sites` / `page_status`(その場で作る) | `index.html` の中の SSI(`<!--#echo var="HTTP_HOST" -->`) | Apache は返す直前に Host の値を埋め込む。`encoding="entity"` が `put_html` の役目 |
| `vhost_normalize`、`vhost_find` | Apache の中(`server/vhost.c`) | 自分で書かなくてよい。名前まで同じ `vhost.c` |
| 受けるポートは 80 だけ | `Listen 127.0.0.1:8081` と `<VirtualHost *:8081>` | Apache は「どの IP とポートで受けたか」でも分けられる(IP ベース) |

### サーバー全体の設定

`<VirtualHost>` の外に書いたものは、サーバー全体に効く。自作OSでは C のコードに埋め込んでいたもの。

| 設定 | 意味 | hobby-os では |
|---|---|---|
| `LoadModule` | 使う部品を読み込む | 全部 `kernel/` に入っている |
| `Listen` | どこで受けるか | `http_init` で `tcp_listen(80)` |
| `KeepAlive On` | keep-alive をする | `handle_request` の `keep_alive` |
| `ErrorLog` / `CustomLog` | ログの置き場所と形 | QEMU の画面に出す |
| `<Directory />` の `Require all denied` | まず全部見せない。サイトのフォルダだけ見せる | ファイルシステムがないので、`files[]` にあるもの以外は出しようがない |

### sudo なしで動かしている理由

`make apache-start` は、次のコマンドを打っている。

```
HOBBY_OS=~/hobby-os /usr/sbin/apache2 -f ~/hobby-os/apache/httpd.conf -k start
```

- `-f` で、Ubuntu 標準の設定(`/etc/apache2/apache2.conf`)ではなく、リポジトリの中の設定を使う
- 1024 より小さいポート(80 など)は root でないと受けられないが、8081 なら自分のユーザーで受けられる
- ログと PID ファイルも `build/apache/` に置くので、`/var/log` などに書く権限は要らない

こうすると、設定を変えて試すのに sudo が要らず、リポジトリの中で完結する。Ubuntu 標準の置き方は 5 節。

## 2. 比べた結果

30 件中 **26 件は同じ答え**だった。⑩ で決めた「選び方」は、Apache と同じに作れている。

| 分類 | 件数 | 結果 |
|---|---|---|
| 振り分け(ServerName と ServerAlias) | 5 | 全部同じ |
| 整える(大文字、ポート、最後の点) | 4 | 全部同じ。Apache も小文字にして、ポートと最後の点を外してから比べる |
| 既定のサイト(知らない名前、IP、似た名前) | 7 | 全部同じ。`blog.localhost.evil.test` も `evil-blog.localhost` も、Apache でもぴったり一致しないので既定のサイト |
| Host なし(HTTP/1.0 は既定、HTTP/1.1 は 400) | 2 | 全部同じ |
| 空白入り、Host が2つ | 2 | 全部同じ(どちらも 400) |
| パス(サイトごとのファイル、サイトごとの 404) | 5 | 全部同じ |
| keep-alive で blog → shop | 1 | 同じ。Apache も1通ずつサイトを選ぶ |
| **変な Host、URL に名前** | 4 | **違った**(次の節) |

## 3. 違った4件

| # | 送ったもの | hobby-os | Apache | Apache のエラーログ(`LogLevel debug`) |
|---|---|---|---|---|
| 17 | `Host: localhost:abc` | 200 既定 | **400** | `AH02415: [strict] Invalid host name 'localhost:abc', problem near: :abc` |
| 18 | `Host: blog.localhost:99999` | 200 blog | **400** | `AH00550: Client sent malformed Host header` |
| 19 | `Host:`(空) | 200 既定 | **400** | `AH00569: client sent HTTP/1.1 request without hostname` |
| 24 | `GET http://shop.localhost/` + `Host: blog.localhost` | **400** | 200 shop | (エラーではない) |

### 17〜19: Apache は「おかしな Host」を断る

HTTP の決まり(RFC 9112 の 3.2 節)はこうなっている。

> サーバーは、Host がない HTTP/1.1 のリクエスト、Host が2つ以上あるリクエスト、**Host の値が正しい形でない**リクエストには、400 で答えなければならない(MUST)

Host の正しい形は `名前` か `名前:ポート` で、**ポートは数字だけ**。

| Host | 形 | 正しい答え | hobby-os がしたこと |
|---|---|---|---|
| `localhost:abc` | ポートが数字でない | 400 | `vhost_normalize` が「最後の `:` の後が数字でないので、全体が名前」とみなし、どれとも一致せず既定のサイト |
| `blog.localhost:99999` | 形は正しいが、ポートは 65535 まで | Apache は 400 | 数字なのでポートとして外し、blog |
| (空) | 名前がない | Apache は「Host なし」と同じ扱いで 400 | 空の名前として既定のサイト |

hobby-os は「どれとも一致しなければ既定のサイト」という **寛容な** 作りで、Apache は「形がおかしければ、サイトを選ぶ前に断る」という **厳しい** 作り。

Apache の厳しさは2段ある。17 の `[strict]` は `HttpProtocolOptions Strict`(2.4.24 から標準)による検査で、これを `Unsafe` に変えると 17 は hobby-os と同じく既定のサイトが答える。18(ポートが範囲の外)と 19(空)は、`Unsafe` にしても 400 のまま。昔からある、外せない検査。

なぜ厳しくするか: Host は相手が自由に書ける値で、サーバーの中では「サイトを選ぶ」「ログに書く」「リンクを作る」「後ろの別のサーバーへ渡す(リバースプロキシ)」に使われる。**部品によって読み方が違うと、そのずれが攻撃に使われる**(Host ヘッダー攻撃、リクエストスマグリング)。入り口で形を確かめて断るのが一番安全、というのが今の考え方。

> hobby-os でも、Host の文字に空白や制御文字が入っていれば 400 にしている(⑨ の `parse_header`)。そこまでは Apache と同じで、ポートの中身と空の名前までは見ていなかった。

### 24: URL に名前が入ったリクエスト

リクエスト行には、パスだけでなく URL を丸ごと書く形(absolute-form)もある。もともとはプロキシに送るための形。

```
GET http://shop.localhost/ HTTP/1.1     ← URL に shop.localhost
Host: blog.localhost                    ← Host は blog.localhost
```

RFC 9112 の 3.2.2 節の決まりは「**URL に名前があれば、Host は無視して URL の名前を使う**」。Apache はこのとおり shop を返し、アクセスログの Host の欄にも `shop.localhost` と書いた(Host を URL の名前で置き換えている)。

hobby-os はこの形に対応しておらず、⑨ で「パスが `/` で始まらなければ 400」にしている(`parse_request` の `/* "http://host/path" の形には対応していない */`)。ブラウザがプロキシを通さずにこの形を送ることはないので、断るのも1つの選び方。ただし、決まりどおりに読むなら Apache の答えが正しい。

### まとめ: どちらが正しいか

| # | 決まり(RFC)どおりなのは | 直すなら hobby-os のどこか |
|---|---|---|
| 17 `localhost:abc` | Apache(400) | `vhost_normalize` で「`:` の後が数字でない」を 400 にする |
| 18 `:99999` | どちらとも言える(RFC の形では数字なら何桁でもよい。Apache は 65535 までに絞っている) | 同じ所で 65535 を超えたら 400 |
| 19 空の Host | Apache(400) | `parse_header` で、空の Host を「Host なし」と同じにする |
| 24 absolute-form | Apache(URL の名前を使う) | `parse_request` で `http://名前/パス` を読み、名前を Host の代わりにする |

## 4. ログで見比べる

Apache のアクセスログ(`build/apache/access.log`)は、自作OSの QEMU の画面のログと並べられる形にした(`LogFormat` の行)。

```
hobby-os: HTTP 10.0.2.2:59026 GET / blog.localhost:8080 -> blog.localhost 200 630B
Apache  : 127.0.0.1:58292 "GET / HTTP/1.1" blog.localhost:8080 -> blog.localhost 200 630B
          相手            リクエスト        届いた Host           答えたサイト   結果 本文
```

`-> ` の後ろは `%v`(答えたサイトの ServerName)。本文の大きさ(630B)まで同じなのは、ページの中身をそろえたから。

面白いのは 400 の行。

```
127.0.0.1:58376 "GET / HTTP/1.1" localhost:abc -> default.invalid 400 266B
```

サイトを選ぶ前に断ったのに、答えたサイトの欄は `default.invalid`(先頭のサイト)になっている。Apache では「サイトが決まらないうちは、先頭のサイトの設定で動く」ため。400 のエラーページやログの書き方も先頭のサイトのものが使われる。**先頭のサイトは、名前が一致しなかったときだけでなく、壊れたリクエストの受け皿でもある**。

## 5. Ubuntu 標準の置き方

仕事のサーバーでは、`apt` で入れた Apache を、Ubuntu の決まった置き場所で設定することが多い。⑪ の `apache/httpd.conf` と、次のように対応する。

| ⑪(このリポジトリ) | Ubuntu 標準 |
|---|---|
| `apache/httpd.conf` の全体の設定 | `/etc/apache2/apache2.conf`、`ports.conf`(`Listen 80`) |
| `LoadModule` の行 | `/etc/apache2/mods-enabled/`(`a2enmod include` で足す) |
| `<VirtualHost>` 1つ | `/etc/apache2/sites-available/blog.conf` の1ファイル。`a2ensite blog` で `sites-enabled/` にリンクができて有効になる |
| 一番上のサイト | ファイル名の順で一番先に読まれるもの。標準の `000-default.conf` が `000-` で始まるのはこのため |
| `apache/sites/blog/` | `/var/www/blog/` など |
| `make apache-check` | `sudo apache2ctl configtest` |
| `make apache-start` / `apache-stop` | `sudo systemctl start apache2` / `stop apache2` |
| (設定を変えたら止めて起動) | `sudo systemctl reload apache2`(つながりを切らずに読み直す) |
| `build/apache/access.log` | `/var/log/apache2/access.log` |

`apt` で入れた時点で、標準の Apache がポート 80 で動いている(`000-default.conf` が、`/var/www/html` の「Apache2 Default Page」を返す)。WSL の中で `curl -s http://127.0.0.1/ | head -5` を打つと見られる。

### 練習: 標準の置き方で blog.localhost を足す(sudo が要る)

Ubuntu のターミナルで、1行ずつ打つ。

```bash
# 1. ページを置く
sudo mkdir -p /var/www/blog
echo '<h1>blog on standard Apache</h1>' | sudo tee /var/www/blog/index.html

# 2. サイトの設定を1ファイルで書く
sudo tee /etc/apache2/sites-available/blog.conf <<'EOF'
<VirtualHost *:80>
    ServerName   blog.localhost
    DocumentRoot /var/www/blog
</VirtualHost>
EOF

# 3. 有効にして、書き間違いを調べてから読み直す
sudo a2ensite blog
sudo apache2ctl configtest
sudo systemctl reload apache2

# 4. 確かめる(Host だけ違う2つ)
curl -s http://127.0.0.1/ | grep -o '<title>.*</title>'
curl -s -H 'Host: blog.localhost' http://127.0.0.1/
```

`127.0.0.1` で開くと「Apache2 Default Page」(先頭の `000-default.conf`)、`Host: blog.localhost` を付けると blog が返る。⑩ と同じことが、本物の置き方で起きる。片付けは `sudo a2dissite blog && sudo systemctl reload apache2`。

`sudo apache2ctl -S` を打つと、Apache が読み込んだサイトの一覧が「どのポートで、どれが既定か」の形で出る。hobby-os の `vhost` コマンドにあたる。

## いじってみよう

- `make compare-apache` の後、`build/apache/access.log` を開いて、表の Apache の列とログの `-> ` の後ろを照らし合わせる
- `apache/httpd.conf` で、`localhost` の `<VirtualHost>` を一番上に動かす。`make apache-stop` → `make apache-start` で起動し直して `http://127.0.0.1:8081/` を開く(⑩ の「いじってみよう」の Apache 版)。試したら戻す
- `apache/httpd.conf` の `Listen` の下に `HttpProtocolOptions Unsafe` を足して起動し直し、`make compare-apache` を打つ。違いが 4 件から 3 件に減るのを確かめる(3 節)。試したら戻す
- `apache/sites/blog/` に `hello.txt` を置いて、`http://blog.localhost:8081/hello.txt` を開く(ビルドなしでページが増える)。hobby-os で同じことをするには何をするか
- `apache/httpd.conf` に4つ目のサイト(`wiki.localhost`)を足す。⑩ の `vhost.c` に足すのと比べて、何か所を書くか
- 5 節の練習をして、`sudo apache2ctl -S` の表示を読む

## 補足: Apache にあって hobby-os にないもの

| Apache | 何のためか |
|---|---|
| `HttpProtocolOptions Strict` と Host の検査 | 形のおかしいリクエストを入り口で断る(3 節) |
| `apache2ctl configtest` | 設定の書き間違いを、起動し直す前に見つける。動いているサーバーを止めずに済む |
| `systemctl reload`(graceful) | つながりを切らずに設定を読み直す。hobby-os はビルドし直して起動し直すしかない |
| MPM(event) | 子プロセスとスレッドで、たくさんのつながりを同時に受ける。hobby-os は1つの CPU で順番に回している |
| SSI、CGI、PHP など | ページをその場で作るしくみを、モジュールで足せる |
| `.htaccess` | フォルダごとに設定を足せる(⑪ では `AllowOverride None` で切っている) |
| TLS(mod_ssl)と SNI | HTTPS。⑩ の補足のとおり、暗号のあいさつの中の名前で証明書を選ぶ |
