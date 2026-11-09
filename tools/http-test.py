#!/usr/bin/env python3
"""http-test.py - 自作OSの Web サーバーを確かめる(make test-http で、ping-lab の実験室の中から実行する)

本物の curl と、手で組み立てたリクエストを送る Python の両方で確かめる。
手で組み立てるのは、ブラウザが送らない「壊れたリクエスト」や「わざと遅い相手」を試すため。
"""
import hashlib
import socket
import subprocess
import sys
import threading
import time

HOST = "10.0.2.15"
PORT = 80
failed = 0


def check(ok, label, detail=""):
    global failed
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f"  ({detail})"))
    if not ok:
        failed = 1


def connect():
    return socket.create_connection((HOST, PORT), timeout=20)


def read_response(f):
    """ファイルのように読めるソケットから応答を1つ読む。(ステータス, ヘッダー, 本文) を返す"""
    status_line = f.readline()
    if not status_line:
        return None, {}, b""
    status = int(status_line.split()[1])
    headers = {}
    while True:
        line = f.readline().rstrip(b"\r\n")
        if not line:
            break
        name, value = line.split(b":", 1)
        headers[name.decode().lower()] = value.strip().decode()
    return status, headers, None


def exchange(raw, method_head=False):
    """raw を送って応答を1つ読む。相手が閉じたかどうかも返す"""
    s = connect()
    s.sendall(raw)
    f = s.makefile("rb")
    status, headers, _ = read_response(f)
    body = b""
    if status is not None and not method_head:
        body = f.read(int(headers.get("content-length", "0")))
    s.settimeout(2)
    try:
        closed = f.read(1) == b""
    except socket.timeout:
        closed = False
    s.close()
    return status, headers, body, closed


def big_expected():
    """/big の中身を Python 側でも作る(OS の fill_big と同じ規則)"""
    return b"".join(
        b"line %06d " % i + bytes([ord("a") + i % 26]) * 51 + b"\n" for i in range(1024)
    )


def curl(*args):
    return subprocess.run(["curl", "-s", "--max-time", "20", *args],
                          capture_output=True)


def curl_site(url, *args):
    """名前の URL のまま、つなぐ先だけ自作OSにする(DNS の代わり)。Host には URL の名前が入る"""
    return curl("--connect-to", f"::{HOST}:{PORT}", *args, url)


LH = ("-H", "Host: localhost")   # ⑨ のページは localhost というサイトにある


# OS が起動して ping に答えるまで待つ
for _ in range(20):
    if subprocess.run(["ping", "-c", "1", "-W", "1", HOST], capture_output=True).returncode == 0:
        break

# ---- 本物の curl で ----

r = curl("-i", *LH, f"http://{HOST}/")
out = r.stdout.decode(errors="replace")
check(out.startswith("HTTP/1.1 200 OK") and "hobby-os" in out and "localhost" in out,
      "curl GET / returns the page with the Host header in it", out[:80])

r = curl(*LH, f"http://{HOST}/hello.txt")
check(r.stdout == b"Hello from hobby-os!\n", "curl GET /hello.txt", r.stdout[:40])

r = curl(*LH, f"http://{HOST}/big")
check(hashlib.sha256(r.stdout).digest() == hashlib.sha256(big_expected()).digest(),
      "curl GET /big: 64 KB, larger than the TCP send buffer, arrives intact",
      f"{len(r.stdout)} bytes")

r = curl("-o", "/dev/null", "-w", "%{http_code}", *LH, f"http://{HOST}/nothing")
check(r.stdout == b"404", "curl GET /nothing is 404", r.stdout)

r = curl("-I", *LH, f"http://{HOST}/big")
out = r.stdout.decode(errors="replace")
check("200 OK" in out and "Content-Length: 65536" in out,
      "curl HEAD /big: headers only, with the full length", out[:80])

# curl に URL を2つ渡すと、同じつながりを使い回す(-v の表示で分かる)
r = curl("-v", *LH, f"http://{HOST}/hello.txt", f"http://{HOST}/hello.txt")
err = r.stderr.decode(errors="replace")
check(r.stdout == b"Hello from hobby-os!\n" * 2 and
      ("Re-using existing" in err or "Reusing existing" in err),
      "keep-alive: curl reuses one connection for two requests", err[-120:])

# 6 本同時(ブラウザは1つのサイトに最大 6 本つなぐ)
results = [None] * 6


def fetch_big(i):
    results[i] = curl(*LH, f"http://{HOST}/big").stdout


threads = [threading.Thread(target=fetch_big, args=(i,)) for i in range(6)]
for t in threads:
    t.start()
for t in threads:
    t.join()
check(all(x == big_expected() for x in results), "6 connections at once, /big each",
      [len(x or b"") for x in results])

# ---- ⑩ バーチャルホスト ----

def get(host, path="/"):
    """Host を指定して GET し、(ステータス, 本文) を返す"""
    st, _, body, _ = exchange(f"GET {path} HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
    return st, body


# 同じ IP、同じポート、同じパス "/" でも、Host が違えば別のサイトのページ
_, home = get("localhost")
_, blog = get("blog.localhost")
_, shop = get("shop.localhost")
check("hobby-os の Web サーバー".encode() in home and
      "hobby-os のブログ".encode() in blog and
      "hobby-os ショップ".encode() in shop,
      "same IP, port and path '/': three different sites by Host",
      (home[:40], blog[:40], shop[:40]))

# curl に名前の URL を渡す(ブラウザと同じく、Host には URL の名前が入る)
r = curl_site("http://blog.localhost/about.html")
check("このブログについて".encode() in r.stdout,
      "curl http://blog.localhost/about.html (connected to the OS by --connect-to)",
      r.stdout[:60])

# どう選ばれたかがトップページに出る
check("ServerName と一致".encode() in home, "localhost matches by ServerName")
_, alias_home = get("hobby-os.localhost")
check("ServerAlias と一致".encode() in alias_home, "hobby-os.localhost matches by ServerAlias")
st, body = get("www.blog.localhost")
check(st == 200 and body == blog, "www.blog.localhost is an alias of the blog", st)

# 名前は整えてから比べる: 大文字小文字、ポート番号、最後の "."
for host in ["BLOG.LocalHost", "blog.localhost:8080", "blog.localhost.", "Blog.Localhost.:80"]:
    st, body = get(host)
    check(st == 200 and body == blog, f"Host {host!r} is the blog", st)

# ファイルはサイトごと。localhost にある /hello.txt は blog にはない
st, body = get("blog.localhost", "/hello.txt")
check(st == 404 and b"(blog.localhost)" in body,
      "/hello.txt exists only on localhost: 404 on the blog, saying which site", (st, body[-60:]))
st, _ = get("shop.localhost", "/items.txt")
st2, _ = get("blog.localhost", "/items.txt")
check(st == 200 and st2 == 404, "/items.txt only on the shop", (st, st2))

# どれにも当てはまらない名前は、一覧の先頭の既定のサイトへ
for host in ["nowhere.test", "10.0.2.15", "localhost:abc", "[::1]:8080", "_default_",
             "blog.localhost.evil.test", "evil-blog.localhost"]:
    st, body = get(host)
    check(st == 200 and "このサイトはありません".encode() in body,
          f"Host {host!r} goes to the default site", (st, body[:60]))

# 既定のサイトはサイトの一覧を出す。リンクには Host に付いていたポートを引き継ぐ
_, body = get("nowhere.test:8080")
check(b'href="http://blog.localhost:8080/"' in body and
      b'href="http://shop.localhost:8080/"' in body,
      "the default site links to the real sites, keeping the port 8080", body[-300:])

# Host はリクエストごと。1つのつながりの中で別々のサイトに頼める
s = connect()
s.sendall(b"GET / HTTP/1.1\r\nHost: blog.localhost\r\n\r\n"
          b"GET / HTTP/1.1\r\nHost: shop.localhost\r\nConnection: close\r\n\r\n")
f = s.makefile("rb")
_, h1, _ = read_response(f)
b1 = f.read(int(h1["content-length"]))
_, h2, _ = read_response(f)
b2 = f.read(int(h2["content-length"]))
s.close()
check(b1 == blog and b2 == shop,
      "one keep-alive connection, two requests to two different sites")

# ---- 手で組み立てたリクエストで ----

# 2つのリクエストを一度に送る(パイプライン)。順番どおりに2つ返るか
s = connect()
s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n"
          b"GET /nothing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
f = s.makefile("rb")
st1, h1, _ = read_response(f)
b1 = f.read(int(h1["content-length"]))
st2, h2, _ = read_response(f)
f.read(int(h2["content-length"]))
closed = f.read(1) == b""
s.close()
check(st1 == 200 and b1 == b"Hello from hobby-os!\n" and st2 == 404 and
      h2.get("connection") == "close" and closed,
      "pipelining: two requests in one send, answered in order, then closed",
      (st1, st2, h2.get("connection"), closed))

# LF だけの行と、リクエストの前の空行も受け付ける
st, h, body, _ = exchange(b"\r\n\nGET /hello.txt HTTP/1.1\nHost: localhost\n\n")
check(st == 200, "bare LF line endings and leading empty lines are accepted", st)

# HTTP/1.0 は閉じるのが標準。keep-alive と書けば続ける。Host がないので既定のサイトが答える
st, h, body, closed = exchange(b"GET / HTTP/1.0\r\n\r\n")
check(st == 200 and closed and h.get("connection") == "close" and
      "Host ヘッダーがありません".encode() in body,
      "HTTP/1.0 without Host: the default site answers, then closed", (st, closed))
st, h, _, closed = exchange(b"GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n")
check(st == 200 and not closed and h.get("connection") == "keep-alive",
      "HTTP/1.0 with Connection: Keep-Alive stays open", (st, closed))

# HEAD は本文を送らない(送ってしまうと次の応答と混ざる)
s = connect()
s.sendall(b"HEAD / HTTP/1.1\r\nHost: localhost\r\n\r\n"
          b"GET /hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n")
f = s.makefile("rb")
st1, h1, _ = read_response(f)
st2, h2, _ = read_response(f)
b2 = f.read(int(h2.get("content-length", "0")))
s.close()
check(st1 == 200 and int(h1["content-length"]) > 0 and st2 == 200 and
      b2 == b"Hello from hobby-os!\n",
      "HEAD sends no body, so the next response follows right after", (st1, st2))

# 相手の文字列をそのまま HTML に入れない(XSS)
st, _, body, _ = exchange(b'GET / HTTP/1.1\r\nHost: a"><b>&\r\n\r\n')
check(st == 200 and b"a&quot;&gt;&lt;b&gt;&amp;" in body and b"<b>" not in body,
      "the Host header is escaped in the page (no HTML injection)", body[-200:])

# 壊れたリクエストたち
cases = [
    (b"GET / HTTP/1.1\r\n\r\n", 400, "HTTP/1.1 without Host"),
    (b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400, "two Host headers"),
    (b"GET / HTTP/1.1\r\nHost : a\r\n\r\n", 400, "space before the colon"),
    (b"GET / HTTP/1.1\r\nHost: a\r\nX-Folded: 1\r\n continued\r\n\r\n", 400, "folded header line"),
    (b"GET  / HTTP/1.1\r\nHost: a\r\n\r\n", 400, "two spaces in the request line"),
    (b"GET http://a/ HTTP/1.1\r\nHost: a\r\n\r\n", 400, "absolute URL as the target"),
    (b"GET / HTTP/2.0\r\nHost: a\r\n\r\n", 505, "unknown HTTP version"),
    (b"POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n\r\nabc", 405, "POST"),
    (b"BREW / HTTP/1.1\r\nHost: a\r\n\r\n", 501, "unknown method"),
    (b"GET /" + b"a" * 300 + b" HTTP/1.1\r\nHost: a\r\n\r\n", 414, "300-byte path"),
    (b"GET / HTTP/1.1\r\nHost: a\r\nX-Big: " + b"x" * 3000 + b"\r\n\r\n", 431, "3000-byte header"),
]
for raw, want, label in cases:
    st, h, _, closed = exchange(raw)
    check(st == want and closed and h.get("connection") == "close",
          f"{label} -> {want}, then closed", (st, closed))
st, h, _, closed = exchange(b"DELETE / HTTP/1.1\r\nHost: a\r\n\r\n")
check(st == 405 and h.get("allow") == "GET, HEAD" and not closed,
      "405 tells which methods are allowed and keeps the connection", (st, h.get("allow"), closed))

# 時間切れ
s = connect()
start = time.time()
s.settimeout(20)
closed = s.recv(1) == b""
took = time.time() - start
s.close()
check(closed and 9 <= took <= 13, "connect and send nothing: closed after about 10 s",
      f"{took:.1f}s")

s = connect()
s.sendall(b"GET / HTTP/1.1\r\nHost: a\r\n")     # 空行を送らずに止まる
start = time.time()
s.settimeout(20)
data = s.makefile("rb").read()
took = time.time() - start
s.close()
check(data.startswith(b"HTTP/1.1 408") and 9 <= took <= 13,
      "stop in the middle of the headers: 408 after about 10 s", f"{data[:20]!r} {took:.1f}s")

s = connect()
s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n")
f = s.makefile("rb")
st, h, _ = read_response(f)
f.read(int(h["content-length"]))
start = time.time()
closed = f.read(1) == b""
took = time.time() - start
s.close()
check(st == 200 and closed and 4 <= took <= 8,
      "keep-alive with no next request: closed after about 5 s", f"{took:.1f}s")

# 応答を受け取らない相手: こちらの受信バッファを小さくして読まずにいると、ウィンドウが 0 になる。
# OS は FIN も送れないので、30 秒進まなければ RST で打ち切るはず
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
s.connect((HOST, PORT))
s.sendall(b"GET /big HTTP/1.1\r\nHost: localhost\r\n\r\n" * 4)
time.sleep(40)
s.settimeout(10)
reset = False
try:
    while s.recv(65536):
        pass
except ConnectionResetError:
    reset = True
except socket.timeout:
    pass
s.close()
check(reset, "a client that never reads is cut off with RST after about 30 s")

# 最後にも普通に使えること
r = curl(*LH, f"http://{HOST}/hello.txt")
check(r.stdout == b"Hello from hobby-os!\n", "still works")

sys.exit(failed)
