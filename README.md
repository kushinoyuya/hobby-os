# hobby-os

自作OSの勉強用リポジトリ。

ゴール: 自作OSの上で動く、オリジナルのモンスター収集RPG(簡易版)を作る。

現在の到達点: ② ブートセクタ(ステージ1)がディスクから続きのプログラム(ステージ2)を読み込んで実行する。

## ロードマップ

| 段階 | 内容 | 状態 |
|---|---|---|
| ① 起動 | ブートセクタで文字を表示する | ✅ [ノート](docs/01-boot.md) |
| ② 512バイトの壁を越える | ディスクから続きを読み込む | ✅ [ノート](docs/02-stage2.md) |
| ③ 32ビットモードとC言語 | プロテクトモードに切り替え、Cのカーネルを動かす | |
| ④ グラフィック | VGA 320×200・256色 | |
| ⑤ キーボード | キー入力を読む | |
| ⑥ タイマー | ゲームループ、アニメーション | |
| ⑦ ゲームの部品 | タイル、スプライト、フォント、メニュー | |
| ⑧ ゲーム本体 | マップ移動、エンカウント、バトル、捕獲 | |

開発環境の作り方とつまずいた点の記録は [docs/setup.md](docs/setup.md) を参照。

## ファイル構成

| ファイル | 役割 |
|---|---|
| `boot.asm` | ステージ1(ブートセクタ、512バイト)。ステージ2をディスクから読み込む |
| `stage2.asm` | ステージ2。ここから先のプログラムを書いていく |
| `print16.inc` | 文字列を表示する共通ルーチン(リアルモード用) |
| `Makefile` | ビルドと実行の手順 |
| `.vscode/tasks.json` | VS Code の Ctrl+Shift+B で `make run` を実行する設定 |
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

ビルドすると `build/os.img`(ステージ1の512バイト + ステージ2)ができる。`build/` は Git の管理対象外。

VS Code のターミナルは **Ctrl+@** で開く(Ubuntu の bash が開く)。

## QEMU の操作

| 操作 | 方法 |
|---|---|
| 終了する | ウィンドウの「×」/ **Ctrl+Alt+Q** / VS Code のターミナルで Ctrl+C |
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
