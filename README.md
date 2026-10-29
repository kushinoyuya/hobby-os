# hobby-os

自作OSの勉強用リポジトリ。

ゴール: 自作OSの上で自作の Web サーバーを動かし、Windows のブラウザからページを開く。最後に `Host` ヘッダーでサイトを振り分けて、Apache の VirtualHost のしくみを自分で再現する。

現在の到達点: ④ 割り込みが動く。タイマーで時間を計り、キーボードで打った文字が画面に出る。

**再開するときは [docs/progress.md](docs/progress.md)(進捗メモ・やり残し)から読む。**

## ロードマップ

| 段階 | 内容 | 状態 |
|---|---|---|
| ① 起動 | ブートセクタで文字を表示する | ✅ [ノート](docs/01-boot.md) |
| ② 512バイトの壁を越える | ディスクから続きを読み込む | ✅ [ノート](docs/02-stage2.md) |
| ③ 32ビットモードとC言語 | プロテクトモードに切り替え、C のカーネルを動かす | ✅ [ノート](docs/03-protected-mode.md) |
| ④ 割り込み | 割り込みのしくみ、タイマー、キーボード | ✅ [ノート](docs/04-interrupts.md) |
| ⑤ メモリ管理 | メモリを確保・解放できるようにする | |
| ⑥ ネットワークカード | QEMU の仮想 NIC のドライバ、パケットの送受信 | |
| ⑦ Ethernet / ARP / IP / ICMP | Windows から自作OSに `ping` が通る | |
| ⑧ TCP | 通信のつながりを作る | |
| ⑨ HTTP サーバー | Windows のブラウザで自作OSのページが開ける | |
| ⑩ バーチャルホスト | `Host` ヘッダーで返すサイトを切り替える | |

開発環境の作り方とつまずいた点の記録は [docs/setup.md](docs/setup.md) を参照。

## ファイル構成

| ファイル | 役割 |
|---|---|
| `boot.asm` | ステージ1(ブートセクタ、512バイト)。ステージ2とカーネルをディスクから読み込む |
| `stage2.asm` | ステージ2(512バイト)。32ビットのプロテクトモードに切り替えてカーネルへ |
| `print16.inc` | 文字列を表示する共通ルーチン(リアルモード用) |
| `kernel/entry.asm` | カーネルの入り口。C を動かす準備をして `kmain()` を呼ぶ |
| `kernel/kernel.c` | カーネル本体。各部品を初期化し、割り込みを待つループを回す |
| `kernel/vga.c` | 画面への文字表示 |
| `kernel/isr.asm` | 割り込みの入り口。レジスタを保存して C を呼ぶ |
| `kernel/idt.c` | 割り込みの登録(IDT)と振り分け、例外の表示 |
| `kernel/pic.c` | 割り込みコントローラ(PIC)の設定 |
| `kernel/timer.c` | タイマー(PIT)。OS の時計 |
| `kernel/keyboard.c` | キーボード。スキャンコードを文字に変換 |
| `kernel/io.h` | I/O ポートの読み書き(`inb` / `outb`) |
| `kernel/linker.ld` | カーネルをメモリの 0x8000 番地から並べる指示書 |
| `Makefile` | ビルドと実行の手順 |
| `.vscode/tasks.json` | VS Code の Ctrl+Shift+B で `make run` を実行する設定 |
| `.vscode/c_cpp_properties.json` | VS Code に C のカーネル用の設定を教える |
| `.vscode/extensions.json` | おすすめの拡張機能 |
| `.githooks/pre-commit` | コミット前にパスワード等の直書きを検査するスクリプト |
| `CLAUDE.md` | Claude Code 向けの作業ルール |
| `docs/setup.md` | 開発環境のセットアップ記録 |
| `docs/01-boot.md` ほか | 段階ごとの学習ノート |

## 毎回の作業の始め方

1. スタートメニューから **Ubuntu** を起動する
2. 次を実行して VS Code を開く
   ```
   cd ~/hobby-os
   code .
   ```
3. VS Code の左下に **「WSL: Ubuntu」** と出ていることを確認する

> エクスプローラーから `\\wsl.localhost\...` を開くと Windows 側で動いてしまい、ビルドできない。必ず Ubuntu から `code .` で開く。

## ビルドと実行

| やりたいこと | VS Code | ターミナル |
|---|---|---|
| ビルドして QEMU で起動 | **Ctrl+Shift+B** | `make run` |
| ビルドだけ | Ctrl+Shift+P →「タスクの実行」→ `build` | `make` |
| 生成物を消す | Ctrl+Shift+P →「タスクの実行」→ `clean` | `make clean` |

ビルドすると `build/os.img`(ステージ1 + ステージ2 + カーネル)ができる。`build/` は Git の管理対象外。

VS Code のターミナルは **Ctrl+@** で開く(Ubuntu の bash が開く)。

## QEMU の操作

| 操作 | 方法 |
|---|---|
| 終了する | ウィンドウの「×」/ **Ctrl+Alt+Q** / VS Code のターミナルで Ctrl+C |
| 文字を入力する | QEMU のウィンドウをクリックしてからキーを打つ(US 配列なので記号の位置は日本語キーボードとずれる) |
| マウスを解放する | **Ctrl+Alt+G**(タイトルに "Press Ctrl+Alt+G to release grab" と出ているとき) |

- 表示が終わった後は `hlt` で止まっているだけなので、閉じるまでそのままになる
- Ctrl+Shift+B を押すたびに新しい QEMU が起動するので、前のウィンドウは閉じてから押す

## コミット

コミット直前に `.githooks/pre-commit` が自動で実行され、次のものが見つかるとコミットが止まる。

- `.env`、`id_rsa`、`*.pem`、`*.key`、`credentials.json` などのファイル
- `password = "..."` のような直書き、GitHub / AWS / Slack のトークン、API キー(`sk-...`)、秘密鍵

止められたら該当箇所を取り除いてからコミットし直す。`--no-verify` で飛ばさない。

**新しく clone したときは、最初に1回だけ次を実行してフックを有効にする。**

```
git config core.hooksPath .githooks
```

より精度の高い検査が必要なら `sudo apt install -y gitleaks` を入れると、フックが自動で併用する。
