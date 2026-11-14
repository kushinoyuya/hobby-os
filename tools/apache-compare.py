#!/usr/bin/env python3
"""apache-compare.py - 同じリクエストを hobby-os と本物の Apache に送って、答えを並べる(make compare-apache)

  hobby-os : 127.0.0.1:8080(QEMU の転送で、自作OSの 10.0.2.15:80 へ)
  Apache   : 127.0.0.1:8081(apache/httpd.conf。サイトの構成は kernel/vhost.c と同じ)

curl ではなく、手でリクエストを組み立てて送る。Host を2つ付ける、Host を付けない、
といった「ブラウザが送らないリクエスト」も、まったく同じバイト列で両方に送るため。

どちらかが動いていなければ、この間だけ起動して、終わったら止める。
make run で自作OSを開いたままでも使える(そのときは開いている自作OSに送る)。
"""
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVERS = [("hobby-os", 8080), ("Apache", 8081)]
ACCESS_LOG = os.path.join(ROOT, "build", "apache", "access.log")


def get(host, path="/", version="1.1"):
    """よくある形のリクエスト。host が None なら Host を付けない"""
    lines = [f"GET {path} HTTP/{version}"]
    if host is not None:
        lines.append(f"Host: {host}")
    if version == "1.1":
        lines.append("Connection: close")
    return ("\r\n".join(lines) + "\r\n\r\n").encode()


# (分類, 説明, 送るバイト列 または 同じつながりで続けて送るバイト列のリスト)
CASES = [
    ("振り分け", "Host: localhost", get("localhost")),
    ("振り分け", "Host: hobby-os.localhost(ServerAlias)", get("hobby-os.localhost")),
    ("振り分け", "Host: blog.localhost", get("blog.localhost")),
    ("振り分け", "Host: www.blog.localhost(ServerAlias)", get("www.blog.localhost")),
    ("振り分け", "Host: shop.localhost", get("shop.localhost")),
    ("整える", "Host: BLOG.LocalHost(大文字)", get("BLOG.LocalHost")),
    ("整える", "Host: blog.localhost:8080(ポート付き)", get("blog.localhost:8080")),
    ("整える", "Host: blog.localhost.(最後に点)", get("blog.localhost.")),
    ("整える", "Host: Blog.Localhost.:80(全部)", get("Blog.Localhost.:80")),
    ("既定", "Host: nowhere.test(知らない名前)", get("nowhere.test")),
    ("既定", "Host: 127.0.0.1:8080(IP で開いた)", get("127.0.0.1:8080")),
    ("既定", "Host: [::1]:8080(IPv6)", get("[::1]:8080")),
    ("既定", "Host: _default_", get("_default_")),
    ("既定", "Host: default.invalid(Apache の既定のサイトの名前)", get("default.invalid")),
    ("既定", "Host: blog.localhost.evil.test(前が同じ)", get("blog.localhost.evil.test")),
    ("既定", "Host: evil-blog.localhost(後ろが同じ)", get("evil-blog.localhost")),
    ("変な Host", "Host: localhost:abc(ポートが数字でない)", get("localhost:abc")),
    ("変な Host", "Host: blog.localhost:99999(ポートが大きすぎる)", get("blog.localhost:99999")),
    ("変な Host", "Host: (空)", get("")),
    ("変な Host", "Host: blog localhost(空白入り)", get("blog localhost")),
    ("変な Host", "Host が2つ(blog と shop)",
     b"GET / HTTP/1.1\r\nHost: blog.localhost\r\nHost: shop.localhost\r\nConnection: close\r\n\r\n"),
    ("Host なし", "HTTP/1.0 で Host なし", get(None, version="1.0")),
    ("Host なし", "HTTP/1.1 で Host なし", get(None)),
    ("URL に名前", "GET http://shop.localhost/ と Host: blog.localhost",
     b"GET http://shop.localhost/ HTTP/1.1\r\nHost: blog.localhost\r\nConnection: close\r\n\r\n"),
    ("パス", "blog.localhost の /about.html", get("blog.localhost", "/about.html")),
    ("パス", "blog.localhost の /hello.txt(blog にはない)", get("blog.localhost", "/hello.txt")),
    ("パス", "localhost の /hello.txt", get("localhost", "/hello.txt")),
    ("パス", "shop.localhost の /items.txt", get("shop.localhost", "/items.txt")),
    ("パス", "nowhere.test の /hello.txt(既定のサイトの中を探す)", get("nowhere.test", "/hello.txt")),
    ("keep-alive", "1つのつながりで blog → shop", [
        b"GET / HTTP/1.1\r\nHost: blog.localhost\r\n\r\n",
        b"GET / HTTP/1.1\r\nHost: shop.localhost\r\nConnection: close\r\n\r\n",
    ]),
]

# 404 のページに書いてある名前 → 表に出す短い名前
SITE_NAMES = {
    "_default_": "default", "default.invalid": "default",
    "localhost": "localhost", "blog.localhost": "blog", "shop.localhost": "shop",
}


def site_of(status, body):
    """応答の中身から、どのサイトが答えたかを見分ける(両方のページに同じ目印を付けてある)"""
    text = body.decode("utf-8", "replace")
    if status == 404:
        m = re.search(r"<p>(?:hobby-os|Apache) \(([^)]*)\)</p>", text)
        return SITE_NAMES.get(m.group(1), m.group(1)) if m else "?"
    if status != 200:
        return "-"                       # サイトを選ぶ前に断られた
    m = re.search(r"<title>(.*?)</title>", text, re.S)
    title = m.group(1) if m else ""
    if "サイトが見つかりません" in title:
        return "default"
    if title.startswith("ブログ"):
        return "blog"
    if title.startswith("ショップ"):
        return "shop"
    if title in ("hobby-os", "Apache") or text.startswith("Hello from"):
        return "localhost"
    if text.startswith("フロッピー"):
        return "shop"
    return "?"


def read_response(f):
    """1つの応答を読む。(ステータス, 本文)。Content-Length がなければ閉じるまで読む"""
    line = f.readline()
    if not line:
        return None, b""
    status = int(line.split()[1])
    length = None
    while True:
        h = f.readline().rstrip(b"\r\n")
        if not h:
            break
        name, _, value = h.partition(b":")
        if name.strip().lower() == b"content-length":
            length = int(value.strip())
    body = f.read(length) if length is not None else f.read()
    return status, body


def send(port, raw):
    """raw(またはそのリスト)を1つのつながりで送り、応答ごとに (ステータス, サイト) を返す"""
    reqs = raw if isinstance(raw, list) else [raw]
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    f = s.makefile("rb")
    results = []
    try:
        for r in reqs:
            s.sendall(r)
            status, body = read_response(f)
            if status is None:
                results.append(("closed", "-"))
                break
            results.append((str(status), site_of(status, body)))
    except (socket.timeout, ConnectionError, ValueError, IndexError) as e:
        results.append((type(e).__name__, "-"))
    s.close()
    return results


def answering(port):
    """そのポートで Web サーバーが答えるか(QEMU の転送は、自作OSが起動する前でもつながるだけはする)"""
    try:
        return send(port, get("localhost"))[0][0] == "200"
    except OSError:
        return False


def start_servers():
    """動いていないほうを起動する。後で止めるものを返す"""
    started = []
    if not answering(8080):
        print("hobby-os を画面なしで起動しています(この比較の間だけ)...")
        qemu = subprocess.Popen(
            ["qemu-system-x86_64", "-drive", f"format=raw,file={ROOT}/build/os.img",
             "-display", "none",
             "-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:8080-:80",
             "-device", "rtl8139,netdev=net0"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        started.append(("qemu", qemu))
    if not answering(8081):
        print("Apache を起動しています(この比較の間だけ)...")
        subprocess.run(["make", "-s", "-C", ROOT, "apache-start"], check=True,
                       stdout=subprocess.DEVNULL)
        started.append(("apache", None))
    for name, port in SERVERS:
        for _ in range(60):
            if answering(port):
                break
            time.sleep(1)
        else:
            stop_servers(started)
            sys.exit(f"{name}(127.0.0.1:{port})が答えません")
    return started


def stop_servers(started):
    for what, proc in started:
        if what == "qemu":
            proc.terminate()
            proc.wait()
        else:
            subprocess.run(["make", "-s", "-C", ROOT, "apache-stop"], stdout=subprocess.DEVNULL)


def show(results):
    return ",".join(f"{status} {site}" for status, site in results)


def main():
    started = start_servers()
    log_start = os.path.getsize(ACCESS_LOG) if os.path.exists(ACCESS_LOG) else 0
    try:
        print()
        print("      hobby-os        Apache          送ったもの")
        print("      --------------  --------------  ----------")
        diffs = 0
        group = None
        for i, (g, label, raw) in enumerate(CASES, 1):
            if g != group:
                print(f"[{g}]")
                group = g
            ours = send(8080, raw)
            theirs = send(8081, raw)
            same = ours == theirs
            diffs += not same
            mark = "同じ" if same else "違う"
            print(f"{mark}  {show(ours):<14}  {show(theirs):<14}  {i:2}. {label}")
        print()
        print(f"{len(CASES)} 件中 {diffs} 件で答えが違った(数字はステータス、続く名前は答えたサイト。"
              f"- はサイトを選ぶ前に断られた)")
    finally:
        stop_servers(started)

    # Apache が自分で記録した「どのサイトが答えたか」(%v)。表の Apache の列と照らし合わせられる
    if os.path.exists(ACCESS_LOG):
        with open(ACCESS_LOG, "rb") as f:
            f.seek(log_start)
            lines = f.read().decode("utf-8", "replace").splitlines()
        print()
        print(f"Apache のアクセスログ(build/apache/access.log の今回の {len(lines)} 行。"
              "-> の後が答えたサイトの ServerName)")
        for line in lines:
            print("  " + line)


if __name__ == "__main__":
    main()
