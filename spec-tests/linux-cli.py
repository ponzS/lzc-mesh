#!/usr/bin/env python3
"""Read-only RPC adapter tests; all credentials and microservers are fixtures."""
import http.server
import pathlib
import resource
import shlex
import struct
import subprocess
import threading

ROOT = pathlib.Path(__file__).resolve().parents[1]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
BUILD = ROOT / "lzc-mesh-client/linux/build/tests"
BUILD.mkdir(parents=True, exist_ok=True)
flags = shlex.split(subprocess.check_output(
    ["pkg-config", "--cflags", "--libs", "libsoup-3.0", "json-glib-1.0"], text=True))
binary = BUILD / "linux-cli"
subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra",
                str(ROOT / "spec-tests/linux-cli.c"),
                str(ROOT / "lzc-mesh-client/linux/src/lzc_cli.c"),
                str(ROOT / "lzc-mesh-client/linux/src/lzc_http.c"),
                *flags, "-o", str(binary)], check=True)


def varint(n):
    out = bytearray()
    while n > 127:
        out.append((n & 127) | 128)
        n >>= 7
    return bytes(out) + bytes([n])


def text(field, value):
    raw = value.encode() if isinstance(value, str) else value
    return varint(field * 8 + 2) + varint(len(raw)) + raw


def integer(field, value):
    return varint(field * 8) + varint(value)


def box(name, default=False, admin=False):
    return (text(2, name) + text(4, name + ".heiyu.space") + integer(6, 2)
            + text(9, "fixture-user") + integer(10, admin) + integer(11, default))


def frame(payload, flag=0):
    return bytes([flag]) + struct.pack(">I", len(payload)) + payload


def reply(payload):
    return frame(payload) + frame(b"grpc-status: 0\r\n", 128)


class Fixture(http.server.BaseHTTPRequestHandler):
    response = b""
    status = 200
    extra_headers = {}
    requests = []

    def log_message(self, *args):
        pass

    def do_POST(self):
        self.requests.append(self.path)
        assert self.headers.get("lzc-shellapi-cred") == "fixture-credential"
        assert self.headers.get_content_type() == "application/grpc-web+proto"
        assert self.rfile.read(int(self.headers["Content-Length"])) == b"\0" * 5
        self.send_response(self.status)
        self.send_header("Content-Type", "application/grpc-web+proto")
        self.send_header("Content-Length", str(len(self.response)))
        for key, value in self.extra_headers.items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(self.response)


server = http.server.HTTPServer(("127.0.0.1", 0), Fixture)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f"http://127.0.0.1:{server.server_port}"
count = 0


def check(name, payload, mode="box", override="", expect="error", status=200, headers=None):
    global count
    Fixture.response, Fixture.status = payload, status
    Fixture.extra_headers = headers or {}
    Fixture.requests = []
    subprocess.run([str(binary), base, mode, override, expect], check=True)
    assert len(Fixture.requests) == 1, name
    expected_rpc = "QueryLzcAuthToken" if mode == "token" else "QueryBoxList"
    assert Fixture.requests[0].endswith("/" + expected_rpc), name
    count += 1
    print("PASS:", name)


try:
    # Admin rights (field 10) must not be confused with the default flag (field 11).
    boxes = text(1, box("other", admin=True)) + text(1, box("default", default=True))
    check("default non-admin microserver", reply(boxes), expect="default")
    check("explicit microserver", reply(boxes), override="other", expect="other")
    check("missing explicit microserver", reply(boxes), override="missing")
    check("no default microserver", reply(text(1, box("other", admin=True))))
    check("empty box list", reply(b""))
    check("duplicate defaults", reply(text(1, box("a", True)) + text(1, box("b", True))))
    unknown = integer(99, 7) + text(100, b"future") + varint(101 * 8 + 5) + b"\0" * 4
    check("unknown protobuf fields", reply(boxes + unknown), expect="default")
    check("login token", reply(text(1, "fixture-cookie")), mode="token", expect="ok")
    check("token with unknown fields", reply(unknown + text(1, "fixture-cookie")), mode="token", expect="ok")
    for name, payload in [
        ("empty token", text(1, "")),
        ("missing token", b""),
        ("token newline", text(1, "bad\r\ncookie")),
        ("token cookie injection", text(1, "bad;cookie")),
        ("token embedded NUL", text(1, b"bad\0cookie")),
        ("token invalid UTF-8", text(1, b"\xff")),
        ("token wrong wire type", integer(1, 7)),
        ("token oversized", text(1, "a" * 4097)),
    ]:
        check(name, reply(payload), mode="token")
    for name, response in [
        ("truncated frame header", b"\0"),
        ("truncated frame payload", b"\0\0\0\0\x0a\0"),
        ("compressed frame", frame(boxes, 1)),
        ("duplicate messages", frame(boxes) + reply(boxes)),
        ("missing RPC status", frame(boxes)),
        ("RPC status failure", frame(boxes) + frame(b"grpc-status: 7\r\n", 128)),
        ("trailer precedes message", frame(b"grpc-status: 0\r\n", 128) + frame(boxes)),
        ("truncated protobuf field", reply(b"\x0a\xff")),
        ("overflow protobuf varint", reply(b"\xff" * 10)),
        ("invalid protobuf field zero", reply(b"\0\0")),
        ("invalid box UTF-8", reply(text(1, text(2, b"\xff")))),
        ("non-message box field", reply(integer(1, 1))),
        ("oversized response", b"\0" * (1024 * 1024 + 1)),
    ]:
        check(name, response)
    check("authentication rejection", b"", headers={"grpc-status": "7"})
    check("HTTP failure", b"", status=503)
    check("redirect never followed", b"", status=302, headers={"Location": base + "/leak"})
    check("status in HTTP headers", frame(boxes), expect="default", headers={"grpc-status": "0"})
    print(f"PASS: {count} hclient-cli adapter cases")
finally:
    server.shutdown()
    thread.join()
    server.server_close()
