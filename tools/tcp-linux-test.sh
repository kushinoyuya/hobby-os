#!/usr/bin/env bash
# tcp-linux-test.sh - 本物の Linux の TCP と自作OSをつないで確かめる(make test-tcp-linux で実行する)
#
# ping-lab.sh の実験室の中で、ランダムなデータをエコーサーバーへ送り、
# 返ってきたものが1バイトも違わないことを SHA-256 で確かめる。
# わざとセグメントを落とす設定(tcp drop N)でも同じことを試す。
set -eu

if [ "${1:-}" != "--inside" ]; then
    monitor=$(mktemp -u /tmp/hobby-os-monitor.XXXXXX)
    export MONITOR="$monitor"
    export QEMU_DISPLAY="-display none"
    export QEMU_EXTRA="-monitor unix:$monitor,server,nowait"
    export LAB_COMMAND="bash tools/tcp-linux-test.sh --inside"
    exec bash tools/ping-lab.sh
fi

# monitor へキー入力を送って、OS のコマンドを打つ
type_command() {
    python3 - "$MONITOR" "$1" <<'PY'
import socket, sys, time
keys = {" ": "spc", "\n": "ret"}
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
for ch in sys.argv[2] + "\n":
    s.sendall(f"sendkey {keys.get(ch, ch)} 20\n".encode())
    time.sleep(0.03)
time.sleep(len(sys.argv[2]) * 0.1 + 0.5)    # sendkey は順に並ぶだけなので、打ち終わるまで待つ
PY
}

# 記録したフレームに、OS が最初の番号 $1 で出した SYN+ACK があり、番号が 0 を越えたか
check_wrapped() {
    python3 - "$1" <<'PY'
import struct, sys
data = open("build/net.pcap", "rb").read()
isn = int(sys.argv[1])
pos, syn_ack, wrapped = 24, False, False
while pos + 16 <= len(data):
    size = struct.unpack("<I", data[pos + 8:pos + 12])[0]
    f = data[pos + 16:pos + 16 + size]
    pos += 16 + size
    if len(f) < 54 or f[12:14] != b"\x08\x00" or f[23] != 6 or f[26:30] != bytes([10, 0, 2, 15]):
        continue
    seq = struct.unpack("!I", f[38:42])[0]
    if f[47] & 0x12 == 0x12 and seq == isn:
        syn_ack = True
    elif syn_ack and seq < 1000000:
        wrapped = True
sys.exit(0 if syn_ack and wrapped else 1)
PY
}

# size バイトのデータを送り、返ってきたものと比べる
transfer() {
    local size=$1 label=$2 sent got start took
    head -c "$size" /dev/urandom > "$work/in"
    sent=$(sha256sum < "$work/in")
    start=$(date +%s.%N)
    # -N: 送り終わったら FIN を出す  -w: 動きがなくなったらあきらめる
    nc -N -w 60 10.0.2.15 7 < "$work/in" > "$work/out"
    took=$(python3 -c "import time; print(f'{time.time() - $start:.1f}')")
    got=$(sha256sum < "$work/out")
    if [ "$sent" = "$got" ]; then
        echo "ok    $label: $size bytes, ${took}s"
    else
        echo "FAIL  $label: $(stat -c %s "$work/out") / $size bytes returned"
        failed=1
    fi
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failed=0

# OS が起動して ping に答えるまで待つ
for _ in $(seq 1 20); do
    ping -c 1 -W 1 10.0.2.15 > /dev/null 2>&1 && break
done

transfer 5 "short message"
transfer 1000000 "1 MB"

# 接続を断られる(待ち受けていないポート)
if nc -z -w 2 10.0.2.15 80 2>/dev/null; then
    echo "FAIL  port 80 should refuse"
    failed=1
else
    echo "ok    closed port refuses"
fi

# 同時に4本
for i in 1 2 3 4; do
    head -c 200000 /dev/urandom > "$work/p$i"
    nc -N -w 60 10.0.2.15 7 < "$work/p$i" > "$work/q$i" &
done
wait
parallel_ok=1
for i in 1 2 3 4; do
    cmp -s "$work/p$i" "$work/q$i" || parallel_ok=0
done
if [ "$parallel_ok" = 1 ]; then
    echo "ok    4 connections at once: 200000 bytes each"
else
    echo "FAIL  4 connections at once"
    failed=1
fi

# 落とす(7個に1個)。Linux と自作OSの両方が再送で補うはず
type_command "tcp drop 7"
transfer 200000 "drop every 7th segment"
type_command "tcp drop 0"

# 番号が一周する(最初の番号を 0 の少し手前にする)
type_command "tcp isn 4294960000"
transfer 100000 "sequence number wraps past 0"
sleep 1     # QEMU が記録をファイルに書き出すのを待つ
if check_wrapped 4294960000; then
    echo "ok    capture shows the sequence number really wrapped"
else
    echo "FAIL  capture does not show the wrap"
    failed=1
fi

# 最後にも普通に使えること
transfer 100000 "still works"

exit "$failed"
