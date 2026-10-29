# 開発環境のセットアップ記録

Windows 11 + WSL2 (Ubuntu) + VS Code で自作OSを開発する環境を作ったときの記録(2026-10-29)。

## 構成

```
┌──────────── Windows ────────────────────────┐
│  VS Code の画面(UI)                         │
│     │  WSL 拡張機能でリモート接続(PC内で完結)│
│     ▼                                        │
│  ┌──── WSL2(軽量な仮想マシン)────┐          │
│  │  Ubuntu                         │          │
│  │   ├ VS Code Server              │          │
│  │   ├ ~/hobby-os(作業フォルダ)  │          │
│  │   └ nasm / gcc / make / qemu    │          │
│  └─────────────────────────────────┘          │
└──────────────────────────────────────────────┘
```

- 作業フォルダは Ubuntu 側の `~/hobby-os`。Windows 側(`C:\...`)に置くとビルドが遅くなるため。
- Windows のエクスプローラーからは `\\wsl.localhost\Ubuntu\home\<ユーザー名>\hobby-os` で見られる。
- QEMU のウィンドウは WSLg(WSL の画面表示機能)で Windows の画面に出る。

## 道具

| 道具 | バージョン | 役割 |
|---|---|---|
| nasm | 3.01 | アセンブラ。アセンブリ言語を機械語に変換する |
| gcc (build-essential) | 15.2 | C コンパイラ。カーネルを C で書く段階で使う |
| make | 4.4 | ビルド手順の自動化 |
| qemu-system-x86 | 10.2 | 仮想PC。作ったOSを起動して試す |
| gdb | 17.1 | デバッガ。QEMU 上のOSを1命令ずつ調べる |
| git | 2.53 | 変更履歴の管理 |

開発の流れ:

```
boot.asm, stage2.asm, kernel/ ──(nasm, gcc, ld、make で実行)──▶ build/os.img ──(QEMU)──▶ 画面に表示
```

## 手順

### 1. Ubuntu を入れる

PowerShell で:

```
wsl --install -d Ubuntu
```

初回起動時にユーザー名とパスワードを決める。パスワードは入力しても画面に表示されない。IME はオフ(半角英数)にして入力する。このパスワードは `sudo` のたびに聞かれる。

### 2. 開発ツールを入れる

Ubuntu で:

```
sudo apt update
sudo apt install -y build-essential nasm qemu-system-x86 gdb git
```

- `sudo`: 管理者権限で実行する
- `apt update`: インストールできるソフトの一覧を更新する(インストールはしない)
- `apt install -y`: 指定したソフトをインストールする(`-y` は確認に自動で Yes と答える)

### 3. VS Code の拡張機能を入れる

- **WSL**(`ms-vscode-remote.remote-wsl`): 必須。Windows 側の VS Code に入れる
- C/C++、x86 Assembly: WSL 接続後、右下の「おすすめの拡張機能」からインストールする

### 4. リポジトリを置いて開く

```
cd ~
git clone https://github.com/kushinoyuya/hobby-os.git
cd hobby-os
git config core.hooksPath .githooks
code .
```

左下に「WSL: Ubuntu」と出れば成功。「Restricted Mode」と出たらクリックして「Trust」を選ぶ。

## つまずいた点と解決方法

### `wsl --install` が反応しない / `WININET_E_TIMEOUT` で失敗する

```
'https://raw.githubusercontent.com/microsoft/WSL/master/distributions/DistributionInfo.json' から配布リストをフェッチできませんでした。処理がタイムアウトになりました
エラー コード: Wsl/InstallDistro/WININET_E_TIMEOUT
```

**原因:** このネット環境では IPv6 の通信が通らない(IPv4 は通る)。WSL のインストーラーは IPv6 を優先するためタイムアウトする。

確認方法:

```
curl.exe -4 -s -o NUL -w "%{http_code}\n" --max-time 20 https://raw.githubusercontent.com/microsoft/WSL/master/distributions/DistributionInfo.json
curl.exe -6 -s -o NUL -w "%{http_code}\n" --max-time 20 https://raw.githubusercontent.com/microsoft/WSL/master/distributions/DistributionInfo.json
```

`-4` が `200`、`-6` がタイムアウトなら同じ問題。

**解決:** Ubuntu のイメージを IPv4 で直接ダウンロードし、ファイルからインストールする。

```
curl.exe -4 -L -o ubuntu-wsl.wsl https://releases.ubuntu.com/26.04.1/ubuntu-26.04.1-wsl-amd64.wsl
wsl --install --from-file ubuntu-wsl.wsl --name Ubuntu
```

ダウンロード URL と SHA256 は上の `DistributionInfo.json` に載っている。`certutil -hashfile ubuntu-wsl.wsl SHA256` で一致を確認する。

`apt` でも同じ症状が出た場合は IPv4 を強制する:

```
sudo apt -o Acquire::ForceIPv4=true update
```

### パスワード設定で `No password has been supplied` / `passwords do not match`

入力が画面に出ないため、空のまま Enter を押したり、2回の入力が食い違ったりした。`Try again? [y/N]` に `y` で再入力できる。

### VS Code で「WSL: Ubuntu」と表示されない

WSL 拡張機能が入っていなかった。この状態で `code .` すると Windows 側で普通のフォルダとして開かれ、`make` が見つからない。PowerShell で次を実行してから開き直すと直る。

```
code --install-extension ms-vscode-remote.remote-wsl
```

初回接続時は Ubuntu 内に VS Code Server がダウンロードされるので1〜2分かかる。
