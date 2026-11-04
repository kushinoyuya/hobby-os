# 進捗メモ

次に再開するときは、まずこのファイルを読む。

## 現在地(2026-11-04 時点)

**⑦ Ethernet / ARP / IP / ICMP まで完了。** 自作OSから `ping 10.0.2.2` が打てて、`make run-ping` の中では本物の Linux の `ping 10.0.2.15` に自作OSが答える。`arp` で覚えた MAC アドレス、`netlog on` で流れるフレームが見られる(コマンドは `help` で一覧)。

| 段階 | 状態 | ノート |
|---|---|---|
| ① 起動 | ✅ | [01-boot.md](01-boot.md) |
| ② 512バイトの壁を越える | ✅ | [02-stage2.md](02-stage2.md) |
| ③ 32ビットモードと C 言語 | ✅ | [03-protected-mode.md](03-protected-mode.md) |
| ④ 割り込み | ✅ | [04-interrupts.md](04-interrupts.md) |
| ⑤ メモリ管理 | ✅ | [05-memory.md](05-memory.md) |
| ⑥ ネットワークカード | ✅ | [06-network-card.md](06-network-card.md) |
| ⑦ Ethernet / ARP / IP / ICMP | ✅ | [07-network-stack.md](07-network-stack.md) |
| ⑧ TCP | **次はここ** | |
| ⑨〜⑩ | HTTP サーバー → バーチャルホスト | |

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

- [ ] docs/01〜07 を読んで理解する(特に 03〜07)。復習には [learning-log.md](learning-log.md) のキーワードと「自分で確かめること」を使う
- [x] ④ と ⑤ のコミットを GitHub に push する(2026-10-31 済み)
- [x] ⑥ を push する(2026-11-05 済み)
- [x] ⑦ とブートローダーの修正をコミット・push する(2026-11-05 済み)
- [x] ブートローダーの読み込み量を増やす(2026-11-04 済み。上限 128KB、カーネルは今 約18KB)
- [x] 外から ping を届ける方法: WSL の Linux から `make run-ping` で打つことにした(2026-11-04。理由は [07-network-stack.md](07-network-stack.md) の 6 節)
- [ ] ⑨ の前に: QEMU の user モードの hostfwd で、Windows のブラウザから TCP が届くか確かめる
- [ ] 各ノートに「理解度チェック」の問題を付けるか → 未回答
- [ ] 並行して WSL の Ubuntu で本物の Apache + VirtualHost を動かす練習をするか → 未回答
- [ ] コミットのメールアドレスを GitHub の noreply アドレスに切り替えるか → 未回答
- [ ] Windows 側の古いコピー `C:\Users\PC_User\Products\hobby-os` を消すか → 自分で判断(GitHub にあるので消しても失われない)

## 環境についての注意

- このネット環境は **IPv6 が通らない**。ダウンロードがタイムアウトしたら IPv4 を強制する(詳しくは [setup.md](setup.md))
- 作業フォルダは Ubuntu 側の `~/hobby-os`。Windows 側のフォルダは使わない
- `git push` は Windows の Git Credential Manager の認証を使う設定済み
