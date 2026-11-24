#!/usr/bin/env python3
"""
CGI + response + timeout tests for webserv.

    make && python3 tests/test_cgi.py            # starts ./Webserv itself
    python3 tests/test_cgi.py --quick            # skip the slow timeout tests (~40s)
    python3 tests/test_cgi.py --no-start         # use a server that is already running

Only the Python standard library. Requests are sent as raw bytes over a
socket, so we control exactly what the server receives (half requests,
chunked bodies, silence, disconnects...).
Test scripts live in www/cgi-bin/test/.
"""

import argparse
import os
import socket
import subprocess
import sys
import threading
import time

HOST, PORT = "localhost", 8080
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root
T = "/cgi-bin/test"

# Must match the constants at the top of src/Eventloop.cpp
CGI_TIMEOUT = 10
CLIENT_TIMEOUT = 30

server_pid = None
results = []  # (name, ok, detail)


# ---------- tiny HTTP client ----------

class Response:
    def __init__(self, raw, seconds):
        self.raw = raw
        self.seconds = seconds
        head, _, self.body = raw.partition(b"\r\n\r\n")
        lines = head.decode(errors="replace").split("\r\n")
        self.status_line = lines[0] if raw else ""
        parts = self.status_line.split(" ", 2)
        self.status = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
        self.reason = parts[2] if len(parts) > 2 else ""
        self.headers = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                self.headers[k.strip().lower()] = v.strip()


def read_response(s, buffered=b""):
    """Read ONE response from socket s: headers, then exactly Content-Length
    body bytes. Stops early if the server closes. Returns (raw response,
    leftover bytes that already belong to the next response)."""
    data = buffered
    while True:
        end = data.find(b"\r\n\r\n")
        if end != -1:
            length = 0
            for line in data[:end].split(b"\r\n")[1:]:
                k, _, v = line.partition(b":")
                if k.strip().lower() == b"content-length":
                    length = int(v.strip())
            total = end + 4 + length
            if len(data) >= total:
                return data[:total], data[total:]
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            return data, b""
        if not chunk:
            return data, b""
        data += chunk


def server_closed(s, wait=1.0):
    """True if the server closed this connection within `wait` seconds."""
    s.settimeout(wait)
    try:
        return s.recv(1) == b""
    except socket.timeout:
        return False
    except OSError:
        return True


def send(raw, timeout=CLIENT_TIMEOUT + 15):
    """Open a connection, send raw bytes, read one response, close."""
    start = time.time()
    s = socket.create_connection((HOST, PORT))
    s.settimeout(timeout)
    if raw:
        s.sendall(raw)
    data, _ = read_response(s)
    s.close()
    return Response(data, time.time() - start)


def get(path, headers=""):
    return send(f"GET {path} HTTP/1.1\r\nHost: {HOST}\r\n{headers}\r\n".encode())


def post(path, body, extra_headers=""):
    head = f"POST {path} HTTP/1.1\r\nHost: {HOST}\r\nContent-Length: {len(body)}\r\n{extra_headers}\r\n"
    return send(head.encode() + body)


def post_chunked(path, chunks):
    body = b"".join(b"%x\r\n%s\r\n" % (len(c), c) for c in chunks) + b"0\r\n\r\n"
    head = f"POST {path} HTTP/1.1\r\nHost: {HOST}\r\nTransfer-Encoding: chunked\r\n\r\n"
    return send(head.encode() + body)


# ---------- test bookkeeping ----------

def check(name, ok, detail=""):
    results.append((name, ok, detail))
    mark = "\033[32mPASS\033[0m" if ok else "\033[31mFAIL\033[0m"
    print(f"  {mark}  {name}" + (f"   ({detail})" if detail and not ok else ""), flush=True)


def length_matches(r):
    return r.headers.get("content-length") == str(len(r.body))


def children():
    """Child processes of the server (CGI scripts still running or not reaped)."""
    if server_pid is None:
        return None
    out = subprocess.run(["ps", "--ppid", str(server_pid), "-o", "pid="],
                         capture_output=True, text=True).stdout
    return len(out.split())


def server_alive():
    try:
        socket.create_connection((HOST, PORT), timeout=2).close()
        return True
    except OSError:
        return False


# ---------- tests ----------

def test_basic_cgi():
    print("Basic CGI")
    r = get("/cgi-bin/hello.py?name=dan&x=1", "X-Custom: hi\r\n")
    check("GET runs the script (200)", r.status == 200, r.status_line)
    check("QUERY_STRING reaches the script", b"Query: name=dan&amp;x=1" in r.body or b"Query: name=dan&x=1" in r.body)
    check("REQUEST_METHOD=GET", b"REQUEST_METHOD=GET" in r.body)
    check("request headers become HTTP_*", b"HTTP_X_CUSTOM=hi" in r.body)
    check("Content-Length matches body", length_matches(r))

    r = post("/cgi-bin/echo.py", b"hallo wereld")
    check("POST body reaches stdin", r.body == b"hallo wereld", repr(r.body[:50]))

    r = post_chunked("/cgi-bin/echo.py", [b"hallo", b" wereld", b"!"])
    check("chunked body is un-chunked for the CGI", r.body == b"hallo wereld!", repr(r.body[:50]))

    big = os.urandom(2 * 1024 * 1024)
    r = post("/cgi-bin/echo.py", big)
    check("2 MB body echoed back identically", r.body == big, f"got {len(r.body)} bytes")


def test_cgi_output_handling():
    print("CGI output handling")
    r = get("/cgi-bin/nope.py")
    check("missing script -> 404", r.status == 404, r.status_line)
    r = get(f"{T}/no_headers.py")
    check("output without headers -> 502", r.status == 502, r.status_line)
    r = get(f"{T}/teapot.py")
    check("Status: header -> 418 I'm a teapot", r.status_line == "HTTP/1.1 418 I'm a teapot", r.status_line)

    r = get(f"{T}/content_length.py?mode=exact")
    check("Content-Length exact -> full body", r.body == b"hello EXTRA", repr(r.body))
    r = get(f"{T}/content_length.py?mode=short")
    check("Content-Length smaller -> body cut off", r.body == b"hello" and length_matches(r), repr(r.body))
    r = get(f"{T}/content_length.py?mode=long")
    check("Content-Length larger than output -> 502", r.status == 502, r.status_line)
    r = get(f"{T}/content_length.py?mode=bad")
    check("Content-Length not a number -> 502", r.status == 502, r.status_line)


def test_errors():
    print("Parser errors -> responses")
    r = send(b"GET /\r\n\r\n")
    check("bad request line -> 400", r.status == 400, r.status_line)
    r = send(b"POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n")
    check("unsupported Transfer-Encoding -> 501", r.status == 501, r.status_line)
    check("error page has a body + right length", len(r.body) > 0 and length_matches(r))


def test_robustness():
    print("Robustness")
    r = post(f"{T}/no_read.py", os.urandom(2 * 1024 * 1024))
    check("script ignores 2 MB body: answered, no SIGPIPE crash", r.status == 200 and server_alive(), r.status_line)

    r = get(f"{T}/fds.py")
    fds = r.body.decode().split()
    check("script inherits no server fds", fds == ["0", "1", "2", "3"], " ".join(fds))

    # slow script in the background, a normal request must not wait for it
    slow = {}
    t = threading.Thread(target=lambda: slow.update(r=get(f"{T}/slow.py")))
    t.start()
    time.sleep(0.3)
    fast = get("/")
    t.join()
    check("normal request answered while a CGI runs", fast.status == 200 and fast.seconds < 0.5,
          f"{fast.seconds:.2f}s")
    check("the slow script still finishes", slow["r"].body == b"slow done")

    # 30 scripts at the same time
    out = [None] * 30
    threads = [threading.Thread(target=lambda i=i: out.__setitem__(i, post("/cgi-bin/echo.py", b"req %d" % i)))
               for i in range(30)]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    ok = all(out[i].body == b"req %d" % i for i in range(30))
    check("30 parallel CGI requests all get their own answer", ok)

    # client hangs up while the script runs -> script must be killed
    if server_pid is not None:
        s = socket.create_connection((HOST, PORT))
        s.sendall(f"GET {T}/hang.py HTTP/1.1\r\nHost: x\r\n\r\n".encode())
        time.sleep(0.5)
        s.close()
        time.sleep(2)  # poll wakes up every second and reaps
        check("client disconnect kills the script", children() == 0, f"{children()} children left")


def test_keep_alive():
    print("Keep-alive")
    s = socket.create_connection((HOST, PORT))
    s.settimeout(5)
    s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    r1 = Response(read_response(s)[0], 0)
    s.sendall(b"GET /cgi-bin/hello.py?again=1 HTTP/1.1\r\nHost: x\r\n\r\n")
    r2 = Response(read_response(s)[0], 0)
    check("HTTP/1.1 default: Connection: keep-alive", r1.headers.get("connection") == "keep-alive",
          r1.headers.get("connection"))
    check("second request (CGI) on the same socket", r2.status == 200 and b"again=1" in r2.body, r2.status_line)
    check("connection stays open after a response", not server_closed(s))
    s.close()

    # pipelining: three requests in ONE send, incl. a POST body and a chunked body,
    # so the server must remove exactly the bytes of each request from its buffer
    s = socket.create_connection((HOST, PORT))
    s.settimeout(5)
    s.sendall(b"POST /cgi-bin/echo.py HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nfirst"
              b"POST /cgi-bin/echo.py HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
              b"6\r\nsecond\r\n0\r\n\r\n"
              b"GET /cgi-bin/hello.py?third=3 HTTP/1.1\r\nHost: x\r\n\r\n")
    raw1, rest = read_response(s)
    raw2, rest = read_response(s, rest)
    raw3, rest = read_response(s, rest)
    a, b, c = Response(raw1, 0), Response(raw2, 0), Response(raw3, 0)
    check("pipelined: 3 requests in one packet, 3 answers in order",
          a.body == b"first" and b.body == b"second" and b"third=3" in c.body,
          f"{a.body[:20]!r} {b.body[:20]!r} {c.body[:30]!r}")
    s.close()

    s = socket.create_connection((HOST, PORT))
    s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    r = Response(read_response(s)[0], 0)
    check("Connection: close -> 'close' header + server closes",
          r.headers.get("connection") == "close" and server_closed(s))
    s.close()

    s = socket.create_connection((HOST, PORT))
    s.sendall(b"GET / HTTP/1.0\r\n\r\n")
    r = Response(read_response(s)[0], 0)
    check("HTTP/1.0 without keep-alive -> closes", r.headers.get("connection") == "close" and server_closed(s))
    s.close()

    s = socket.create_connection((HOST, PORT))
    s.sendall(b"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
    r = Response(read_response(s)[0], 0)
    check("HTTP/1.0 + Connection: keep-alive -> stays open",
          r.headers.get("connection") == "keep-alive" and not server_closed(s))
    s.close()

    s = socket.create_connection((HOST, PORT))
    s.sendall(b"GET /\r\n\r\n")
    r = Response(read_response(s)[0], 0)
    check("parse error -> 400 and close", r.status == 400 and server_closed(s), r.status_line)
    s.close()

    # 50 requests on one connection
    s = socket.create_connection((HOST, PORT))
    s.settimeout(5)
    ok = True
    for i in range(50):
        s.sendall(f"POST /cgi-bin/echo.py HTTP/1.1\r\nHost: x\r\nContent-Length: {len(str(i))}\r\n\r\n{i}".encode())
        if Response(read_response(s)[0], 0).body != str(i).encode():
            ok = False
            break
    check("50 CGI requests in a row on one connection", ok, f"failed at {i}")
    s.close()


def test_timeouts():
    print(f"Timeouts (running in parallel, ~{CLIENT_TIMEOUT}s)")
    cases = {
        "hanging CGI -> 504": (f"GET {T}/hang.py HTTP/1.1\r\nHost: x\r\n\r\n".encode(), 504, CGI_TIMEOUT),
        "half the headers, then silence -> 408": (b"GET / HTTP/1.1\r\nHost: x\r\n", 408, CLIENT_TIMEOUT),
        "half the body, then silence -> 408": (b"POST / HTTP/1.1\r\nContent-Length: 99\r\n\r\nhal", 408, CLIENT_TIMEOUT),
        "connect and send nothing -> closed": (b"", 0, CLIENT_TIMEOUT),
        "idle keep-alive connection -> closed": ("KEEPALIVE", 0, CLIENT_TIMEOUT),
    }

    def idle_keep_alive():
        # one request, then stay silent: the server must close after the timeout
        s = socket.create_connection((HOST, PORT))
        s.settimeout(CLIENT_TIMEOUT + 15)
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        read_response(s)
        start = time.time()
        closed = s.recv(1) == b""
        r = Response(b"", time.time() - start)
        if not closed:
            r.status = -1
        s.close()
        return r
    got = {}
    threads = [threading.Thread(target=lambda n=n, raw=c[0]: got.__setitem__(
                   n, idle_keep_alive() if raw == "KEEPALIVE" else send(raw)))
               for n, c in cases.items()]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    for name, (_, status, limit) in cases.items():
        r = got[name]
        on_time = limit - 0.5 <= r.seconds <= limit + 2.5
        check(name, r.status == status and on_time, f"status {r.status} after {r.seconds:.1f}s")
    if server_pid is not None:
        time.sleep(1.5)
        check("timed-out script was killed and reaped", children() == 0, f"{children()} children left")


# ---------- main ----------

def main():
    global server_pid
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true", help="skip the timeout tests")
    ap.add_argument("--no-start", action="store_true", help="use an already running server")
    args = ap.parse_args()

    proc = None
    if not args.no_start:
        if server_alive():
            sys.exit(f"Something already listens on {PORT}. Stop it, or use --no-start.")
        proc = subprocess.Popen([os.path.join(ROOT, "Webserv")], cwd=ROOT,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        server_pid = proc.pid
        for _ in range(20):
            if server_alive():
                break
            time.sleep(0.1)
        else:
            proc.kill()
            sys.exit(f"Server does not accept connections on {PORT}. If you just stopped "
                     "another instance, the port may still be in TIME_WAIT: wait ~60s.")

    try:
        test_basic_cgi()
        test_cgi_output_handling()
        test_errors()
        test_robustness()
        test_keep_alive()
        if not args.quick:
            test_timeouts()
        check("server still running at the end", server_alive())
    finally:
        if proc:
            proc.terminate()
            proc.wait()

    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
