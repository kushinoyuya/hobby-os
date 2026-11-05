#!/usr/bin/env bash
# ping-lab.sh - 本物の Linux の ping を自作OSへ届ける実験室(make run-ping で起動する)
#
# QEMU の user モードは、外から中へ ping を通さない。そこで tap(仮想の線)で
# Linux と QEMU を直接つなぐ。ふつう tap を作るには管理者の権限が要るが、
# unshare で「自分専用のネットワーク空間」を作り、その中で作れば sudo は要らない。
# この空間の外(WSL や Windows の本来のネットワーク)には何も影響しない。
#
#   ┌──── 専用のネットワーク空間 ────────────────┐
#   │ bash / ping                                │
#   │  tap0 (10.0.2.2) ── 仮想の線 ── QEMU       │
#   │                                 hobby-os   │
#   │                                 (10.0.2.15)│
#   └────────────────────────────────────────────┘
set -eu

if [ "${1:-}" != "--inside" ]; then
    # -r: この空間の中では管理者として振る舞う  -n: 新しいネットワーク空間を作る
    exec unshare -r -n "$0" --inside
fi

ip link set lo up
ip tuntap add dev tap0 mode tap
ip addr add 10.0.2.2/24 dev tap0
ip link set tap0 up

# QEMU_DISPLAY を指定すると画面の出し方を変えられる(自動テストでは "-display none")
# shellcheck disable=SC2086
qemu-system-x86_64 -drive format=raw,file=build/os.img \
    -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
    -device rtl8139,netdev=net0 \
    -object filter-dump,id=dump0,netdev=net0,file=build/net.pcap \
    ${QEMU_DISPLAY:-} ${QEMU_EXTRA:-} &
qemu_pid=$!
trap 'kill "$qemu_pid" 2>/dev/null || true' EXIT

# LAB_COMMAND を指定すると、シェルを開く代わりにそれを実行する(自動テスト用)
if [ -n "${LAB_COMMAND:-}" ]; then
    sleep 2
    bash -c "$LAB_COMMAND"
    exit $?
fi

cat <<'MSG'

  実験室に入りました。ここは QEMU と直接つながった専用のネットワーク空間です。
  QEMU の起動後に試してみてください:

      ping 10.0.2.15
      nc 10.0.2.15 7      (TCP のエコーサーバー。打った行がそのまま返る。Ctrl+C で終わる)

  QEMU の画面で net と arp を打つと、答えた回数や覚えた MAC アドレスが見られます。
  tcp log on を打ってから nc でつなぐと、TCP のやり取りが見られます。
  終わるときは exit(QEMU も一緒に終了します)。

MSG
PS1='(ping-lab) \$ ' bash --norc || true
