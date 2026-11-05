#!/usr/bin/env python3
"""tcp-test.py - 自作OSの TCP の自動テスト(make test-tcp で実行する。3分ほどかかる)

本物の Linux は行儀がよいので、順番の入れ替えや ACK の無視は簡単に起こせない。
そこで QEMU の仮想の線を Python につなぎ、Python が相手役になって、
フレームを1つずつ手で組み立てて送り、返ってきたものを確かめる。

    python3 tools/tcp-test.py            全部のテスト
    python3 tools/tcp-test.py echo rst   名前に echo か rst を含むテストだけ
"""
import os
import random
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

FIN, SYN, RST, PSH, ACK = 0x01, 0x02, 0x04, 0x08, 0x10
OS_IP = bytes([10, 0, 2, 15])
PEER_IP = bytes([10, 0, 2, 99])
PEER_MAC = bytes.fromhex("020000000099")
ECHO = 7
KEY_HOLD_MS = 20


class TestFailed(Exception):
    pass


def check(cond, message):
    if not cond:
        raise TestFailed(message)


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return ~total & 0xFFFF


def flag_names(flags):
    names = [(SYN, "SYN"), (FIN, "FIN"), (RST, "RST"), (PSH, "PSH"), (ACK, "ACK")]
    return ",".join(n for bit, n in names if flags & bit) or "-"


class Seg:
    def __init__(self, sport, dport, seq, ack, flags, wnd, options, data):
        self.sport, self.dport, self.seq, self.ack = sport, dport, seq, ack
        self.flags, self.wnd, self.options, self.data = flags, wnd, options, data
        self.time = time.monotonic()

    def __repr__(self):
        return (f"<{flag_names(self.flags)} seq={self.seq} ack={self.ack} "
                f"win={self.wnd} len={len(self.data)}>")


class Lab:
    """QEMU を起動して、線の向こう側になる"""

    def __init__(self, image):
        self.tmp = tempfile.TemporaryDirectory()
        self.monitor_path = os.path.join(self.tmp.name, "monitor")
        server = socket.create_server(("127.0.0.1", 0))
        port = server.getsockname()[1]
        self.qemu = subprocess.Popen([
            "qemu-system-x86_64", "-drive", f"format=raw,file={image}",
            "-netdev", f"socket,id=net0,connect=127.0.0.1:{port}",
            "-device", "rtl8139,netdev=net0",
            "-display", "none",
            "-monitor", f"unix:{self.monitor_path},server,nowait",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        server.settimeout(10)
        self.wire, _ = server.accept()
        server.close()
        self.wire.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = b""
        self.queues = {}
        self.os_mac = None
        self.next_port = 40000
        self.monitor = None
        self.wait_for_boot()

    def close(self):
        self.qemu.kill()
        self.qemu.wait()
        self.tmp.cleanup()

    # ---- フレームの送受信。線の上は「4バイトの長さ + フレーム」の形 ----

    def send_frame(self, frame):
        self.wire.sendall(struct.pack("!I", len(frame)) + frame)

    def read_frame(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            if len(self.buffer) >= 4:
                size = struct.unpack("!I", self.buffer[:4])[0]
                if len(self.buffer) >= 4 + size:
                    frame = self.buffer[4:4 + size]
                    self.buffer = self.buffer[4 + size:]
                    return frame
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            self.wire.settimeout(left)
            try:
                chunk = self.wire.recv(65536)
            except socket.timeout:
                return None
            if not chunk:
                raise TestFailed("QEMU との接続が切れた(OS が落ちた?)")
            self.buffer += chunk

    def arp(self, op, target_mac, target_ip):
        payload = struct.pack("!HHBBH", 1, 0x0800, 6, 4, op) + PEER_MAC + PEER_IP
        payload += target_mac + target_ip
        dst = target_mac if op == 2 else b"\xff" * 6
        self.send_frame(dst + PEER_MAC + b"\x08\x06" + payload)

    def wait_for_boot(self):
        """ARP に答えるようになるまで待つ"""
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            self.arp(1, b"\0" * 6, OS_IP)
            if self.pump(0.2) and self.os_mac:
                return
        raise TestFailed("OS が起動しなかった")

    def pump(self, timeout):
        """フレームを1つ読んで振り分ける。何か読めたら True"""
        frame = self.read_frame(timeout)
        if frame is None:
            return False
        kind = frame[12:14]
        if kind == b"\x08\x06":
            arp = frame[14:]
            op = struct.unpack("!H", arp[6:8])[0]
            if op == 2 and arp[14:18] == OS_IP:
                self.os_mac = arp[8:14]
            elif op == 1 and arp[24:28] == PEER_IP:
                self.os_mac = arp[8:14]
                self.arp(2, arp[8:14], OS_IP)
        elif kind == b"\x08\x00":
            self.parse_ip(frame[14:])
        return True

    def parse_ip(self, packet):
        hdr = (packet[0] & 0x0F) * 4
        total = struct.unpack("!H", packet[2:4])[0]
        check(checksum(packet[:hdr]) == 0, "OS が送った IP のチェックサムが違う")
        if packet[9] != 6:
            return
        segment = packet[hdr:total]
        pseudo = packet[12:20] + struct.pack("!BBH", 0, 6, len(segment))
        check(checksum(pseudo + segment) == 0, "OS が送った TCP のチェックサムが違う")
        sport, dport, seq, ack, off, flags, wnd = struct.unpack("!HHIIBBH", segment[:16])
        offset = (off >> 4) * 4
        seg = Seg(sport, dport, seq, ack, flags, wnd, segment[20:offset], segment[offset:])
        self.queues.setdefault(dport, []).append(seg)

    def send_segment(self, sport, dport, seq, ack, flags, wnd=65535, data=b"", options=b"",
                     bad_checksum=False, data_offset=None, src_ip=PEER_IP):
        offset = 20 + len(options)
        doff = data_offset if data_offset is not None else offset // 4
        header = struct.pack("!HHIIBBHHH", sport, dport, seq & 0xFFFFFFFF, ack & 0xFFFFFFFF,
                             doff << 4, flags, wnd, 0, 0)
        segment = header + options + data
        pseudo = src_ip + OS_IP + struct.pack("!BBH", 0, 6, len(segment))
        csum = checksum(pseudo + segment)
        if bad_checksum:
            csum ^= 0x5555
        segment = segment[:16] + struct.pack("!H", csum) + segment[18:]
        self.send_ip(segment, src_ip=src_ip)

    def send_ip(self, segment, src_ip=PEER_IP, protocol=6):
        header = struct.pack("!BBHHHBBH", 0x45, 0, 20 + len(segment), 0, 0x4000, 64, protocol, 0)
        header += src_ip + OS_IP
        header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
        self.send_frame(self.os_mac + PEER_MAC + b"\x08\x00" + header + segment)

    def new_port(self):
        self.next_port += 1
        return self.next_port

    # ---- QEMU の monitor: キーを打つ、画面を読む ----

    def monitor_command(self, line):
        if self.monitor is None:
            self.monitor = socket.socket(socket.AF_UNIX)
            self.monitor.connect(self.monitor_path)
            self.monitor_read()
        self.monitor.sendall(line.encode() + b"\n")
        return self.monitor_read()

    def monitor_read(self):
        data = b""
        self.monitor.settimeout(5)
        while not data.endswith(b"(qemu) "):
            data += self.monitor.recv(65536)
        return data.decode(errors="replace")

    KEYS = {" ": "spc", ".": "dot", "-": "minus", "\n": "ret"}

    def type_line(self, text):
        # sendkey は順番に並ぶだけですぐ戻るので、コマンドが実行されるまで画面を見て待つ
        echoed = "> " + text
        before = self.screen().count(echoed)
        for ch in text + "\n":
            self.monitor_command(f"sendkey {self.KEYS.get(ch, ch)} {KEY_HOLD_MS}")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.settle(0.02)
            screen = self.screen()
            lines = [l for l in screen.splitlines() if l.strip()]
            if text == "clear":
                if len(lines) <= 2:
                    return
            elif screen.count(echoed) > before and lines[-1].strip() == ">":
                return
        raise TestFailed(f"コマンドが実行されなかった: {text}")

    def settle(self, seconds):
        """しばらくの間、届くものを受け取っておく"""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.pump(max(0, deadline - time.monotonic()))

    def screen(self):
        """画面のメモリ(0xB8000)を文字列として読む"""
        out = self.monitor_command("xp /2000hx 0xb8000")
        chars = []
        for line in out.splitlines():
            if re.match(r"^[0-9a-f]{16}:", line.strip()):
                for word in line.split(":", 1)[1].split():
                    chars.append(chr(int(word, 16) & 0xFF))
        text = "".join(chars)
        return "\n".join(text[i:i + 80].rstrip() for i in range(0, len(text), 80))


class Conn:
    """テストで使う相手役の TCP(番号の管理だけをする)"""

    def __init__(self, lab, port=ECHO, seq=None, sport=None):
        self.lab = lab
        self.port = port
        self.sport = sport or lab.new_port()
        self.iss = seq if seq is not None else random.getrandbits(32)
        self.seq = self.iss
        self.rcv = 0
        self.queue = lab.queues.setdefault(self.sport, [])

    def send(self, flags, data=b"", seq=None, ack=None, wnd=65535, **kw):
        self.lab.send_segment(self.sport, self.port,
                              self.seq if seq is None else seq,
                              self.rcv if ack is None else ack,
                              flags, wnd=wnd, data=data, **kw)

    def expect(self, what="segment", pred=lambda s: True, timeout=2.0):
        """条件に合うセグメントが来るまで待つ。合わないものは読み捨てる"""
        deadline = time.monotonic() + timeout
        while True:
            while self.queue:
                seg = self.queue.pop(0)
                if pred(seg):
                    return seg
            left = deadline - time.monotonic()
            if left <= 0:
                raise TestFailed(f"{what} が来なかった")
            self.lab.pump(left)

    def expect_flags(self, flags, timeout=2.0):
        return self.expect(flag_names(flags),
                           lambda s: (s.flags & ~PSH) == flags, timeout)

    def expect_silence(self, seconds, pred=lambda s: True):
        """しばらく何も(条件に合うものが)来ないこと"""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.lab.pump(max(0, deadline - time.monotonic()))
            for seg in self.queue:
                check(not pred(seg), f"来ないはずのものが来た: {seg}")
        self.queue.clear()

    def syn(self, options=b"\x02\x04\x05\xb4", wnd=65535):
        """SYN を送って SYN+ACK を受け取る。起動直後でも答えるので長めに待つ"""
        self.send(SYN, options=options, wnd=wnd, ack=0)
        seg = self.expect_flags(SYN | ACK, timeout=3.0)
        check(seg.ack == (self.iss + 1) & 0xFFFFFFFF, f"SYN+ACK の ack が違う: {seg}")
        self.seq = (self.iss + 1) & 0xFFFFFFFF
        self.rcv = (seg.seq + 1) & 0xFFFFFFFF
        return seg

    def connect(self, wnd=65535, options=b"\x02\x04\x05\xb4"):
        self.syn(options=options)
        self.send(ACK, wnd=wnd)
        return self

    def send_data(self, data, **kw):
        self.send(ACK | PSH, data=data, **kw)
        self.seq = (self.seq + len(data)) & 0xFFFFFFFF

    def recv_data(self, size, timeout=5.0, ack=True, wnd=65535):
        """size バイトを順番どおりに受け取る。順番が合わないものは無視する"""
        got = b""
        deadline = time.monotonic() + timeout
        while len(got) < size:
            left = deadline - time.monotonic()
            check(left > 0, f"データが {len(got)}/{size} バイトしか来なかった")
            seg = self.expect("データ", lambda s: s.data and s.seq == self.rcv, left)
            got += seg.data
            self.rcv = (self.rcv + len(seg.data)) & 0xFFFFFFFF
            if ack:
                self.send(ACK, wnd=wnd)
        return got

    def abort(self):
        """片付け: RST で終わらせる"""
        self.send(RST, ack=0)
        self.lab.settle(0.05)

    def assert_alive(self):
        payload = b"alive?"
        self.send_data(payload)
        check(self.recv_data(len(payload)) == payload, "つながりが生きていない")

    def assert_gone(self):
        self.send(ACK | PSH, data=b"x")
        self.expect_flags(RST)


# ---- テスト ----

def test_handshake_and_mss(lab):
    c = Conn(lab)
    synack = c.syn(options=b"\x02\x04\x03\xe8")     # 「こちらは 1000 バイトまで」
    check(synack.options[:4] == b"\x02\x04\x05\xb4", f"MSS が 1460 でない: {synack.options!r}")
    check(synack.wnd == 4096, f"ウィンドウが 4096 でない: {synack}")
    c.send(ACK)
    payload = bytes(range(256)) * 12
    c.send_data(payload[:1400])
    c.send_data(payload[1400:2800])
    sizes = []
    got = b""
    while len(got) < 2800:
        seg = c.expect("データ", lambda s: s.data and s.seq == c.rcv)
        sizes.append(len(seg.data))
        got += seg.data
        c.rcv += len(seg.data)
        c.send(ACK)
    check(got == payload[:2800], "内容が違う")
    check(max(sizes) <= 1000, f"相手の MSS を超えて送った: {sizes}")
    c.abort()


def test_mss_missing_uses_default(lab):
    c = Conn(lab).connect(options=b"")
    c.send_data(b"m" * 1400)
    seg = c.expect("データ", lambda s: s.data)
    check(len(seg.data) <= 536, f"MSS がないのに {len(seg.data)} バイト送った")
    c.abort()


def test_echo(lab):
    c = Conn(lab).connect()
    c.send_data(b"hello")
    check(c.recv_data(5) == b"hello", "内容が違う")
    c.abort()


def test_ack_acknowledges_data(lab):
    c = Conn(lab).connect()
    c.send_data(b"abc")
    seg = c.expect("ACK", lambda s: s.flags & ACK and s.ack == c.seq)
    check(seg.ack == c.seq, f"ACK が違う: {seg}")
    c.abort()


def test_bad_checksum_is_ignored(lab):
    c = Conn(lab).connect()
    c.send(ACK | PSH, data=b"broken", bad_checksum=True)
    c.expect_silence(0.3)
    c.assert_alive()
    c.abort()


def test_duplicate_data_is_not_echoed_twice(lab):
    c = Conn(lab).connect()
    seq = c.seq
    c.send_data(b"once")
    check(c.recv_data(4) == b"once", "内容が違う")
    c.send(ACK | PSH, data=b"once", seq=seq)        # ACK が届かなかったふりをして送り直す
    c.expect("もう一度の ACK", lambda s: s.ack == c.seq and not s.data)
    c.expect_silence(0.3, lambda s: bool(s.data))
    c.abort()


def test_out_of_order_is_dropped_and_recovered(lab):
    c = Conn(lab).connect()
    first, second = b"AAAA", b"BBBB"
    c.send(ACK | PSH, data=second, seq=c.seq + 4)   # 後ろを先に送る
    seg = c.expect("重複 ACK", lambda s: s.flags & ACK and not s.data)
    check(seg.ack == c.seq, f"欲しい番号を返していない: {seg}")
    c.expect_silence(0.2, lambda s: bool(s.data))
    c.send_data(first)
    check(c.recv_data(4) == first, "前半が違う")
    c.send_data(second)
    check(c.recv_data(4) == second, "後半が違う")
    c.abort()


def test_partial_overlap_is_trimmed(lab):
    c = Conn(lab).connect()
    seq = c.seq
    c.send_data(b"12345")
    check(c.recv_data(5) == b"12345", "内容が違う")
    c.send(ACK | PSH, data=b"45678", seq=seq + 3)   # 45 は受け取り済み、678 が新しい
    c.seq = seq + 8
    check(c.recv_data(3) == b"678", "重なった部分の切り落としが違う")
    c.abort()


def test_retransmit_with_backoff(lab):
    c = Conn(lab).connect()
    c.send_data(b"lost?")
    first = c.expect("データ", lambda s: s.data)
    times = [first.time]
    for _ in range(3):
        again = c.expect("再送", lambda s: s.data, timeout=5)
        check(again.seq == first.seq and again.data == first.data, f"再送の内容が違う: {again}")
        times.append(again.time)
    gaps = [b - a for a, b in zip(times, times[1:])]
    for gap, expected in zip(gaps, [0.5, 1.0, 2.0]):
        check(expected * 0.7 < gap < expected * 1.5, f"再送の間隔が違う: {gaps}")
    c.rcv += 5
    c.send(ACK)
    c.expect_silence(4.2, lambda s: bool(s.data))   # ACK したら止まる
    c.abort()


def test_fast_retransmit(lab):
    c = Conn(lab).connect()
    c.send_data(b"f" * 1400)
    c.send_data(b"g" * 1400)
    lab.settle(0.1)
    c.queue.clear()
    for _ in range(3):
        c.send(ACK)
    start = time.monotonic()
    seg = c.expect("高速再送", lambda s: s.data, timeout=0.3)
    check(seg.seq == c.rcv, f"先頭から送っていない: {seg}")
    check(time.monotonic() - start < 0.3, "時間切れを待った")
    c.abort()


def test_peer_zero_window_probe(lab):
    c = Conn(lab).connect(wnd=0)
    c.send_data(b"z" * 100, wnd=0)
    c.expect_silence(0.3, lambda s: bool(s.data))
    probe = c.expect("ウィンドウプローブ", lambda s: s.data, timeout=1)
    check(len(probe.data) == 1 and probe.seq == c.rcv, f"プローブの形が違う: {probe}")
    for _ in range(2):
        c.send(ACK, wnd=0)                          # まだ受け取れない
        again = c.expect("次のプローブ", lambda s: s.data, timeout=5)
        check(again.seq == c.rcv, f"プローブの番号が違う: {again}")
    c.send(ACK)                                     # 空いた
    check(c.recv_data(100) == b"z" * 100, "内容が違う")
    c.abort()


def test_zero_window_survives_past_retry_limit(lab):
    """受け取れないと答え続ける相手は、生きているので切らない(再送の上限 6 回を超えても)"""
    c = Conn(lab).connect(wnd=0)
    c.send_data(b"y" * 10, wnd=0)
    for _ in range(8):
        c.expect("プローブ", lambda s: s.data and s.seq == c.rcv, timeout=20)
        c.send(ACK, wnd=0)
    c.send(ACK)
    check(c.recv_data(10) == b"y" * 10, "内容が違う")
    c.abort()


def test_flow_control_closes_and_reopens_window(lab):
    """こちらが読まずに送り続けると、OS のウィンドウが 0 になり、データは失われない"""
    c = Conn(lab).connect(wnd=0)
    sent = b""
    pattern = bytes(random.getrandbits(8) for _ in range(40000))
    window = 4096
    while window > 0:
        chunk = pattern[len(sent):len(sent) + min(1000, window)]
        c.send_data(chunk, wnd=0)
        sent += chunk
        seg = c.expect("ACK", lambda s: s.ack == c.seq and not s.data)
        lab.settle(0.02)                            # エコーサーバーが読むのを待つ
        last = seg
        for s in c.queue:
            if not s.data:
                last = s
        c.queue[:] = [s for s in c.queue if s.data]
        window = last.wnd
        check(len(sent) < 30000, f"ウィンドウが閉じない: {len(sent)}")
    check(len(sent) == 8192 + 4096, f"送信 8192 + 受信 4096 で閉じるはず: {len(sent)}")

    # ウィンドウ 0 のところへ送っても、受け取らない
    c.send(ACK | PSH, data=b"!" * 100, wnd=0)
    seg = c.expect("ACK", lambda s: not s.data)
    check(seg.ack == c.seq and seg.wnd == 0, f"受け取らないはずなのに: {seg}")

    # 読み始めると全部届く
    c.send(ACK)
    check(c.recv_data(len(sent), timeout=15) == sent, "内容が違う")
    c.abort()


def test_closed_port_gets_rst(lab):
    c = Conn(lab, port=9)
    c.send(SYN, ack=0)
    seg = c.expect_flags(RST | ACK)
    check(seg.ack == (c.iss + 1) & 0xFFFFFFFF, f"RST の ack が違う: {seg}")


def test_stray_ack_gets_rst(lab):
    c = Conn(lab)
    c.send(ACK, ack=12345, seq=678)
    seg = c.expect_flags(RST)
    check(seg.seq == 12345, f"RST の seq は相手の ack と同じはず: {seg}")


def test_rst_is_never_answered_with_rst(lab):
    c = Conn(lab)
    c.send(RST, ack=0)
    c.expect_silence(0.3)


def test_rst_with_exact_seq_closes(lab):
    c = Conn(lab).connect()
    c.send(RST, ack=0)
    lab.settle(0.05)
    c.assert_gone()


def test_rst_in_window_gets_challenge_ack(lab):
    c = Conn(lab).connect()
    c.send(RST, seq=c.seq + 100, ack=0)
    seg = c.expect_flags(ACK)
    check(seg.ack == c.seq, f"正しい番号を返していない: {seg}")
    c.assert_alive()
    c.abort()


def test_rst_out_of_window_is_ignored(lab):
    c = Conn(lab).connect()
    c.send(RST, seq=c.seq + 100000, ack=0)
    c.expect_silence(0.3)
    c.assert_alive()
    c.abort()


def test_syn_on_established_gets_challenge_ack(lab):
    c = Conn(lab).connect()
    c.send(SYN, seq=c.seq + 5000, ack=0)
    seg = c.expect_flags(ACK)
    check(seg.ack == c.seq, f"正しい番号を返していない: {seg}")
    c.assert_alive()
    c.abort()


def test_duplicate_syn_gets_same_synack(lab):
    c = Conn(lab)
    first = c.syn()
    c.send(SYN, seq=c.iss, ack=0, options=b"\x02\x04\x05\xb4")
    second = c.expect_flags(SYN | ACK)
    check(first.seq == second.seq, f"番号が変わった: {first} {second}")
    c.send(ACK)
    c.assert_alive()
    c.abort()


def test_wrong_ack_in_handshake_gets_rst(lab):
    c = Conn(lab)
    c.syn()
    c.send(ACK, ack=c.rcv + 1000)
    seg = c.expect_flags(RST)
    check(seg.seq == (c.rcv + 1000) & 0xFFFFFFFF, f"RST の seq が違う: {seg}")
    c.send(RST, ack=0)
    lab.settle(0.05)


def test_ack_of_unsent_data_is_ignored(lab):
    c = Conn(lab).connect()
    c.send(ACK, ack=c.rcv + 100000)
    seg = c.expect_flags(ACK)
    check(seg.ack == c.seq, f"{seg}")
    c.assert_alive()
    c.abort()


def test_passive_close(lab):
    c = Conn(lab).connect()
    c.send(FIN | ACK)
    c.seq += 1
    ack = c.expect("FIN の ACK", lambda s: s.ack == c.seq)
    check(ack.ack == c.seq, f"{ack}")
    fin = c.expect("OS の FIN", lambda s: s.flags & FIN)
    again = c.expect("FIN の再送", lambda s: s.flags & FIN, timeout=2)
    check(again.seq == fin.seq, f"再送の番号が違う: {fin} {again}")
    c.rcv = fin.seq + 1
    c.send(ACK)
    c.expect_silence(1.0)                           # LAST_ACK は終わり。もう再送しない
    c.assert_gone()


def test_data_with_fin_is_echoed_then_closed(lab):
    c = Conn(lab).connect()
    c.send(ACK | FIN | PSH, data=b"bye")
    c.seq += 4
    check(c.recv_data(3) == b"bye", "内容が違う")
    fin = c.expect("OS の FIN", lambda s: s.flags & FIN)
    check(fin.seq == c.rcv, f"データの直後に FIN が来ていない: {fin}")
    c.rcv += 1
    c.send(ACK)
    lab.settle(0.05)
    c.assert_gone()


def test_fin_retransmission_gets_ack_again(lab):
    c = Conn(lab).connect()
    c.send(FIN | ACK)
    fin_seq = c.seq
    c.seq += 1
    c.expect("FIN の ACK", lambda s: s.ack == c.seq)
    c.send(FIN | ACK, seq=fin_seq)                  # 送り直す
    c.expect("もう一度の ACK", lambda s: s.ack == c.seq)
    fin = c.expect("OS の FIN", lambda s: s.flags & FIN)
    c.rcv = fin.seq + 1
    c.send(ACK)
    lab.settle(0.05)


def test_active_close_and_time_wait(lab):
    c = Conn(lab).connect()
    lab.settle(0.05)
    lab.type_line("echo off")
    fin = c.expect("OS の FIN", lambda s: s.flags & FIN)
    c.rcv = fin.seq + 1
    c.send(ACK)                                     # FIN_WAIT_2 へ

    # 閉じる途中でもデータは受け取ったことにして ACK する
    c.send_data(b"late")
    seg = c.expect("ACK", lambda s: s.ack == c.seq)
    check(seg.ack == c.seq, f"{seg}")

    closed = Conn(lab)                              # 待ち受けをやめたので断られる
    closed.send(SYN, ack=0)
    closed.expect_flags(RST | ACK)

    lab.type_line("echo on")
    fin_seq = c.seq
    c.send(FIN | ACK)                               # TIME_WAIT へ
    c.seq += 1
    c.expect("FIN の ACK", lambda s: s.ack == c.seq)
    c.send(FIN | ACK, seq=fin_seq)                  # TIME_WAIT でも ACK を返し直す
    c.expect("もう一度の ACK", lambda s: s.ack == c.seq)
    c.send(RST, ack=0)                              # TIME_WAIT は RST で消えない
    c.send(ACK | PSH, data=b"x", seq=fin_seq)       # 古い番号 → ACK が返るなら残っている
    c.expect("ACK", lambda s: s.ack == c.seq)

    lab.settle(4.5)                                 # 4 秒で消える
    c.queue.clear()
    c.assert_gone()


def test_time_wait_allows_new_connection(lab):
    sport = lab.new_port()
    c = Conn(lab, sport=sport, seq=1000).connect()
    lab.settle(0.05)
    lab.type_line("echo off")
    fin = c.expect("OS の FIN", lambda s: s.flags & FIN)
    c.rcv = fin.seq + 1
    c.send(FIN | ACK)
    c.seq += 1
    c.expect("ACK", lambda s: s.ack == c.seq)
    lab.type_line("echo on")
    again = Conn(lab, sport=sport, seq=c.seq + 100000).connect()
    again.assert_alive()
    again.abort()


def test_simultaneous_close(lab):
    c = Conn(lab).connect()
    lab.settle(0.05)
    lab.type_line("echo off")
    fin = c.expect("OS の FIN", lambda s: s.flags & FIN)
    # OS の FIN にまだ ACK を返さず、こちらも FIN を出す
    c.rcv = fin.seq
    c.send(FIN | ACK)
    c.seq += 1
    ack = c.expect("FIN の ACK", lambda s: s.ack == c.seq)
    check(ack.ack == c.seq, f"{ack}")
    lab.type_line("echo on")
    again = c.expect("CLOSING での FIN の再送", lambda s: s.flags & FIN, timeout=3)
    check(again.seq == fin.seq, f"{again}")
    c.rcv = fin.seq + 1
    c.send(ACK)                                     # TIME_WAIT へ
    c.queue.clear()
    c.expect_silence(1.0)


def test_peer_sequence_wraps(lab):
    c = Conn(lab, seq=0xFFFFFFF0).connect()
    payload = bytes(range(100))
    c.send_data(payload)
    check(c.seq < 1000, "テストの前提: 番号が 0 を越えている")
    check(c.recv_data(100) == payload, "内容が違う")
    c.send_data(b"after")
    check(c.recv_data(5) == b"after", "内容が違う")
    c.abort()


def test_our_sequence_wraps(lab):
    lab.type_line("tcp isn 4294967280")
    c = Conn(lab).connect()
    check(c.rcv == 0xFFFFFFF1, f"tcp isn が効いていない: {c.rcv}")
    payload = bytes(range(100))
    c.send_data(payload)
    check(c.recv_data(100) == payload, "内容が違う")
    check(c.rcv < 1000, "テストの前提: 番号が 0 を越えている")
    c.send_data(b"after")
    check(c.recv_data(5) == b"after", "内容が違う")
    c.send(FIN | ACK)
    c.seq += 1
    fin = c.expect("OS の FIN", lambda s: s.flags & FIN)
    c.rcv = fin.seq + 1
    c.send(ACK)
    lab.settle(0.05)
    c.assert_gone()


def test_handshake_timeout_frees_slot(lab):
    c = Conn(lab)
    first = c.syn()
    times = [first.time]
    while True:
        seg = c.expect("SYN+ACK の再送か RST", timeout=10)
        if seg.flags & RST:
            times.append(seg.time)
            break
        check(seg.flags & SYN and seg.seq == first.seq, f"{seg}")
        times.append(seg.time)
    check(len(times) == 6, f"SYN+ACK の再送は 4 回のはず: {len(times) - 2}")
    total = times[-1] - times[0]
    check(13 < total < 19, f"あきらめるまで約 15.5 秒のはず: {total:.1f}")


def test_data_timeout_aborts(lab):
    c = Conn(lab).connect()
    c.send_data(b"nobody home")
    start = time.monotonic()
    count = 0
    while True:
        seg = c.expect("再送か RST", lambda s: s.data or s.flags & RST, timeout=20)
        if seg.flags & RST:
            break
        count += 1
    took = time.monotonic() - start
    check(count == 7, f"最初の送信 + 再送 6 回のはず: {count}")
    check(40 < took < 56, f"あきらめるまで約 47.5 秒のはず: {took:.1f}")


def test_table_full_then_recovers(lab):
    conns = []
    for _ in range(8):
        conns.append(Conn(lab).connect())
    lab.settle(0.1)
    extra = Conn(lab)
    extra.send(SYN, ack=0)
    extra.expect_silence(0.3)                       # 満杯。黙って捨てる
    for c in conns:
        c.assert_alive()
    conns[0].abort()
    lab.settle(0.1)
    conns[0] = Conn(lab).connect()
    conns[0].assert_alive()
    for c in conns:
        c.abort()


def test_syn_flood_then_recovers(lab):
    for _ in range(30):
        c = Conn(lab)
        c.send(SYN, ack=0)
    lab.settle(17)                                  # SYN+ACK をあきらめて行が空くまで
    c = Conn(lab).connect()
    c.assert_alive()
    c.abort()


def test_bad_header_lengths(lab):
    c = Conn(lab).connect()
    for offset in (0, 4, 15):                       # 短すぎる、中身より長い
        c.send(ACK, data_offset=offset, data=b"xx")
    lab.send_ip(b"\x00" * 10)                       # 20 バイトに満たない
    lab.send_ip(b"")
    c.expect_silence(0.3)
    c.assert_alive()
    c.abort()


def test_bad_options_do_not_hang(lab):
    for options in (b"\x02\x00\x00\x00",            # 長さ 0。そのまま進むと無限ループ
                    b"\x02\x01\x00\x00",            # 長さ 1
                    b"\x08\xff\x00\x00",            # 中身より長い
                    b"\x02\x04\x00\x00",            # MSS 0
                    b"\x01\x01\x01\x01",            # 何もしない、だけ
                    b"\x00\x02\x04\x05"):           # 終わりの印の後ろは読まない
        c = Conn(lab)
        synack = c.syn(options=options)
        check(synack.flags & SYN, f"{options!r}")
        c.send(ACK)
        c.assert_alive()
        c.abort()


def test_fuzz_random_segments(lab):
    """でたらめなセグメントを大量に送っても落ちず、最後に普通に使える"""
    rng = random.Random(8)
    live = Conn(lab).connect()
    for i in range(4000):
        flags = rng.choice([0, SYN, ACK, FIN, RST, ACK | PSH, FIN | ACK, SYN | ACK, SYN | FIN,
                            RST | ACK, rng.getrandbits(8)])
        data = rng.randbytes(rng.choice([0, 0, 1, 7, 100, 1460]))
        options = rng.randbytes(rng.choice([0, 0, 4, 8, 40]))
        lab.send_segment(rng.getrandbits(16) or 1, rng.choice([ECHO, ECHO, 9, 80]),
                         rng.getrandbits(32), rng.getrandbits(32), flags,
                         wnd=rng.getrandbits(16), data=data, options=options)
        if i % 8 == 0:
            lab.pump(0)
    # でたらめな IP パケットとフレームも
    for _ in range(1000):
        lab.send_ip(rng.randbytes(rng.randrange(0, 200)))
        lab.send_frame(rng.randbytes(rng.randrange(14, 300)))
    lab.settle(17)                                  # SYN で作られた行があきらめて空くまで
    for q in lab.queues.values():
        q.clear()
    live.assert_alive()
    live.abort()
    c = Conn(lab).connect()
    c.assert_alive()
    c.abort()


def test_commands_show_state(lab):
    c = Conn(lab).connect()
    lab.settle(0.05)
    lab.type_line("clear")
    lab.type_line("tcp")
    screen = lab.screen()
    check("ESTABLISHED" in screen, f"tcp の表示に ESTABLISHED がない:\n{screen}")
    check(f"10.0.2.99:{c.sport}" in screen, f"相手の番号がない:\n{screen}")
    c.abort()

    lab.type_line("clear")
    lab.type_line("tcp log on")
    c = Conn(lab).connect()
    lab.settle(0.1)
    lab.type_line("tcp log off")
    screen = lab.screen()
    for text in ("SYN seq=", "SYN,ACK", "SYN_RECEIVED -> ESTABLISHED"):
        check(text in screen, f"tcp log に {text} がない:\n{screen}")
    c.abort()


def main():
    image = "build/os.img"
    if not os.path.exists(image):
        sys.exit("build/os.img がありません。先に make してください")
    filters = sys.argv[1:]
    tests = [(name, fn) for name, fn in list(globals().items())
             if name.startswith("test_") and (not filters or any(f in name for f in filters))]

    failed = []
    for name, fn in tests:
        lab = Lab(image)                            # テストごとに起動し直して影響を残さない
        start = time.monotonic()
        try:
            fn(lab)
            print(f"ok    {name} ({time.monotonic() - start:.1f}s)", flush=True)
        except (TestFailed, OSError) as e:
            failed.append(name)
            print(f"FAIL  {name}: {e}", flush=True)
        finally:
            lab.close()

    print(f"\n{len(tests) - len(failed)}/{len(tests)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
