# 進捗メモ

次に再開するときは、まずこのファイルを読む。

## 現在地(2026-10-31 時点)

**⑤ メモリ管理まで完了。** BIOS からメモリの地図を受け取り、ページ単位(`pmm_alloc`)と好きな大きさ(`kmalloc` / `kfree`)で貸し借りできる。起動後に `mem` と打つと様子が見られる(コマンドは `help` で一覧)。

| 段階 | 状態 | ノート |
|---|---|---|
| ① 起動 | ✅ | [01-boot.md](01-boot.md) |
| ② 512バイトの壁を越える | ✅ | [02-stage2.md](02-stage2.md) |
| ③ 32ビットモードと C 言語 | ✅ | [03-protected-mode.md](03-protected-mode.md) |
| ④ 割り込み | ✅ | [04-interrupts.md](04-interrupts.md) |
| ⑤ メモリ管理 | ✅ | [05-memory.md](05-memory.md) |
| ⑥ ネットワークカード | **次はここ** | |
| ⑦〜⑩ | ping → TCP → HTTP サーバー → バーチャルホスト | |

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

- [ ] docs/01〜05 を読んで理解する(特に 03〜05)。復習には [learning-log.md](learning-log.md) のキーワードと「自分で確かめること」を使う
- [x] ④ と ⑤ のコミットを GitHub に push する(2026-10-31 済み)
- [ ] 各ノートに「理解度チェック」の問題を付けるか → 未回答
- [ ] 並行して WSL の Ubuntu で本物の Apache + VirtualHost を動かす練習をするか → 未回答
- [ ] コミットのメールアドレスを GitHub の noreply アドレスに切り替えるか → 未回答
- [ ] Windows 側の古いコピー `C:\Users\PC_User\Products\hobby-os` を消すか → 自分で判断(GitHub にあるので消しても失われない)

## 環境についての注意

- このネット環境は **IPv6 が通らない**。ダウンロードがタイムアウトしたら IPv4 を強制する(詳しくは [setup.md](setup.md))
- 作業フォルダは Ubuntu 側の `~/hobby-os`。Windows 側のフォルダは使わない
- `git push` は Windows の Git Credential Manager の認証を使う設定済み
