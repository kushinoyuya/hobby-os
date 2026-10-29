# hobby-os

自作OSの学習用リポジトリ。WSL (Ubuntu) の `~/hobby-os` で作業する。

- ビルド: `make` / 実行: `make run` (QEMU) / 片付け: `make clean`
- 所有者は学習中なので、コードには日本語のコメントを付け、変更内容はかみ砕いて説明する。

## コミットのルール

- `.githooks/pre-commit` がクレデンシャルやパスワードの直書きを検査する。有効になっていなければ先に `git config core.hooksPath .githooks` を実行する。
- フックで止められたら、該当箇所を取り除いてからコミットし直す。`--no-verify` でフックを飛ばしてはいけない。
- コミット前に `git diff --cached` を自分でも読み、秘密情報や個人情報が含まれていないか確認する。
