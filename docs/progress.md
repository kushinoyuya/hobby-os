# 進捗メモ

次に再開するときは、まずこのファイルを読む。

## 現在地(2026-11-14 時点)

**⑪ 本物の Apache と見比べた(push 済み)。** `apache/httpd.conf` に ⑩ と同じサイト構成を作り、`make apache-start` で 8081 番に Apache を起動する(sudo なし)。`make compare-apache` で同じ 30 通りのリクエストを両方に送ると、26 件が同じ答え、4 件(`Host: localhost:abc`、`:99999`、空の Host、`GET http://shop.localhost/`)が違う。違いの理由と、hobby-os をどう直せば Apache と同じになるかは [11-apache.md](11-apache.md) の 3 節。**Windows のブラウザで `http://blog.localhost:8081/` などを開くのはまだ試していない**(8080 と並べて見る)。

**⑩ バーチャルホストまで完了し、ゴールに到達(push 済み)。** OS は起動するとポート 80 で Web サーバーを動かし、Host ヘッダーでサイトを選ぶ。`make run` で起動し、**Windows のブラウザで `http://localhost:8080/`、`http://blog.localhost:8080/`、`http://shop.localhost:8080/`、`http://127.0.0.1:8080/` を開いて見比べる**(ブラウザで開くのはまだ試していない。最初に試す)。QEMU の画面にはリクエストごとに、答えたサイト付きのアクセスログが出る。`vhost` でサイトの一覧と数、`http` で回数、`tcp` でつながりの一覧が見られる(コマンドは `help` で一覧)。ポート 7 のエコーサーバーもそのまま動いている。

| 段階 | 状態 | ノート |
|---|---|---|
| ① 起動 | ✅ | [01-boot.md](01-boot.md) |
| ② 512バイトの壁を越える | ✅ | [02-stage2.md](02-stage2.md) |
| ③ 32ビットモードと C 言語 | ✅ | [03-protected-mode.md](03-protected-mode.md) |
| ④ 割り込み | ✅ | [04-interrupts.md](04-interrupts.md) |
| ⑤ メモリ管理 | ✅ | [05-memory.md](05-memory.md) |
| ⑥ ネットワークカード | ✅ | [06-network-card.md](06-network-card.md) |
| ⑦ Ethernet / ARP / IP / ICMP | ✅ | [07-network-stack.md](07-network-stack.md) |
| ⑧ TCP | ✅ | [08-tcp.md](08-tcp.md) |
| ⑨ HTTP サーバー | ✅ | [09-http.md](09-http.md) |
| ⑩ バーチャルホスト | ✅ | [10-vhost.md](10-vhost.md) |
| ⑪ Apache と見比べる | ✅ | [11-apache.md](11-apache.md) |

ゴール: 自作OSの上で自作の Web サーバーを動かし、`Host` ヘッダーで振り分けて Apache の VirtualHost のしくみを再現する。

これまでに学んだことのまとめは [learning-log.md](learning-log.md)。段階を終えるたびに書き足す。

## 再開のしかた

1. スタートメニューから **Ubuntu** を起動
2. `cd ~/hobby-os` → `code .`(左下に「WSL: Ubuntu」と出ることを確認)
3. **Ctrl+Shift+B** で起動して、前回の状態を確認する
4. Claude Code には「前回の続き」と伝える

## 学習の進め方(決めたこと)

- コードは Claude が書き、自分は **docs を読んで理解する** ことに集中する(写経はしない)
- サイクル: **読む → 動かす → いじる → 自分の言葉で説明する**
- 分からないところは「ファイル名と場所」を添えて質問する

## やり残し・保留中のこと

- [ ] docs/01〜10 を読んで理解する(特に 03〜10)。復習には [learning-log.md](learning-log.md) のキーワードと「自分で確かめること」を使う
- [x] ④ と ⑤ のコミットを GitHub に push する(2026-10-31 済み)
- [x] ⑥ を push する(2026-11-05 済み)
- [x] ⑦ とブートローダーの修正をコミット・push する(2026-11-05 済み)
- [x] ブートローダーの読み込み量を増やす(2026-11-04 済み。上限 128KB、カーネルは今 約18KB)
- [x] 外から ping を届ける方法: WSL の Linux から `make run-ping` で打つことにした(2026-11-04。理由は [07-network-stack.md](07-network-stack.md) の 6 節)
- [x] ⑨ の前に: QEMU の user モードの hostfwd で、Windows から TCP が届くか確かめる(2026-11-06 済み。Windows の PowerShell の TCP クライアントで往復を確認。ブラウザで開くのは ⑨ で)
- [x] ⑧ を push する(2026-11-06 済み)
- [x] ⑨ で考える: 何も送らないまま消えた相手のつながりは行を占領し続ける(2026-11-08 済み。Web サーバーの時間切れで閉じる。読まない相手は `tcp_abort` の RST で切る。[09-http.md](09-http.md) の 5 節)
- [x] ⑨ をコミットする(2026-11-08 済み、`e89c164`)
- [x] ⑨ を push する(2026-11-09 済み)
- [ ] Windows のブラウザで `http://localhost:8080/` と `http://blog.localhost:8080/` などを開いて確かめる(WSL から転送先への curl では、名前の URL も含めて確認済み)
- [x] ⑩ をコミットする(2026-11-09 済み)
- [x] ⑩ を push する(2026-11-09 済み)
- [x] ゴールの後に何をするか → 本物の Apache との見比べ(2026-11-14、⑪)
- [x] WSL の Ubuntu で本物の Apache + VirtualHost を動かす → ⑪ で実施。`sudo apt-get install apache2` 済み(標準の Apache もポート 80 で動いている)
- [x] ⑪ をコミットする(2026-11-14 済み、`6495f39`)
- [x] ⑪ を push する(2026-11-14 済み)
- [ ] Windows のブラウザで `http://localhost:8081/`、`http://blog.localhost:8081/` などを開き、8080 と並べて見る
- [ ] [11-apache.md](11-apache.md) の 5 節の練習(Ubuntu 標準の `sites-available` と `a2ensite` で blog.localhost を足す。sudo が要るので自分で打つ)
- [ ] 違った 4 件を hobby-os で Apache に合わせて直すか → 未回答(直し方は 11-apache.md の 3 節の「まとめ」)
- [ ] 次に何をするか → 未回答(例: ワイルドカードや 421、ファイルシステム)
- [ ] 各ノートに「理解度チェック」の問題を付けるか → 未回答
- [ ] コミットのメールアドレスを GitHub の noreply アドレスに切り替えるか → 未回答
- [ ] Windows 側の古いコピー `C:\Users\PC_User\Products\hobby-os` を消すか → 自分で判断(GitHub にあるので消しても失われない)

## 環境についての注意

- このネット環境は **IPv6 が通らない**。ダウンロードがタイムアウトしたら IPv4 を強制する(詳しくは [setup.md](setup.md))
- 作業フォルダは Ubuntu 側の `~/hobby-os`。Windows 側のフォルダは使わない
- `git push` は Windows の Git Credential Manager の認証を使う設定済み
