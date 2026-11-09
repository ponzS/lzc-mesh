#!/usr/bin/env python3
"""Credential boundary tests. Uses loopback servers and disposable fake secrets."""
import ctypes
import gzip
import http.server
import json
import os
import pathlib
import resource
import shlex
import socket
import ssl
import subprocess
import tempfile
import threading
import time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "lzc-mesh-client/linux/build/tests"
BUILD.mkdir(parents=True, exist_ok=True)
binary = BUILD / "linux-auth-security"
flags = shlex.split(subprocess.check_output(
    ["pkg-config", "--cflags", "--libs", "libsoup-3.0", "json-glib-1.0"], text=True))
if os.environ.get("LZC_TEST_SANITIZERS"):
    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
                '-DLZC_MESH_VERSION="security-fixture"', "-ffunction-sections", "-fdata-sections",
                "-Wl,--gc-sections", str(ROOT / "spec-tests/linux-auth-security.c"),
                str(ROOT / "lzc-mesh-client/linux/src/lzc_cli.c"),
                str(ROOT / "lzc-mesh-client/linux/src/lzc_http.c"),
                *flags, "-o", str(binary)], check=True)
# Same-UID fixture process name; this deliberately does NOT claim that a name
# authenticates against malicious code already executing under the same UID.
ctypes.CDLL(None).prctl(15, b"hclient-cli", 0, 0, 0)
servers = []
count = 0


class Handler(http.server.BaseHTTPRequestHandler):
    records = []
    status = 200
    location = None

    def log_message(self, *args):
        pass

    def handle_request(self):
        self.records.append((self.command, dict(self.headers)))
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self.send_response(self.status)
        if self.location:
            self.send_header("Location", self.location)
        if self.command == "POST":
            payload = b"\x0a\x0efixture-cookie"
            body = b"\0" + len(payload).to_bytes(4, "big") + payload
            trailers = b"grpc-status: 0\r\n"
            body += b"\x80" + len(trailers).to_bytes(4, "big") + trailers
            self.send_header("Content-Type", "application/grpc-web+proto")
        else:
            body = b"{}"
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    do_GET = handle_request
    do_POST = handle_request


def server(tls=None, ipv6=False, dual_stack=False, accept_delay=0, handler_class=Handler):
    handler = type("InstanceHandler", (handler_class,), {"records": [], "status": 200, "location": None})

    class Service(http.server.ThreadingHTTPServer):
        address_family = socket.AF_INET6 if ipv6 or dual_stack else socket.AF_INET

        def server_bind(self):
            if dual_stack:
                self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
            super().server_bind()

        def get_request(self):
            if accept_delay:
                time.sleep(accept_delay)
            return super().get_request()

    service = Service(("::" if dual_stack else "::1" if ipv6 else "127.0.0.1", 0), handler)
    if tls:
        service.socket = tls.wrap_socket(service.socket, server_side=True)
    thread = threading.Thread(target=service.serve_forever, daemon=True)
    thread.start()
    servers.append((service, thread))
    return service, handler


class LargeBody(Handler):
    """Paced body proves early stream cancellation, not just final rejection."""
    chunked = False
    length = 16 * 1024 * 1024
    compressed = False
    finished = threading.Event()
    sent = 0

    def do_GET(self):
        self.send_response(200)
        payload = gzip.compress(b"x" * self.length) if self.compressed else None
        wire_length = len(payload) if payload is not None else self.length
        if self.compressed:
            self.send_header("Content-Encoding", "gzip")
        if self.chunked:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(wire_length))
        self.end_headers()
        try:
            for offset in range(0, wire_length, 16384):
                data = payload[offset:offset+16384] if payload is not None else b"x" * min(16384, wire_length - offset)
                if self.chunked:
                    self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
                else:
                    self.wfile.write(data)
                self.wfile.flush()
                type(self).sent += len(data)
                time.sleep(.002)
            if self.chunked:
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass
        finally:
            self.finished.set()


def body_case(name, chunked, tls=None, env=None, length=16 * 1024 * 1024, compressed=False):
    kind = type("BodyFixture", (LargeBody,), {
        "chunked": chunked, "length": length, "compressed": compressed,
        "finished": threading.Event(), "sent": 0,
    })
    service, handler = server(tls=tls, handler_class=kind)
    mode = "body-ok" if length == 1024 * 1024 else "body-limit"
    run(name, mode, f"{'https' if tls else 'http'}://127.0.0.1:{service.server_port}", env=env)
    assert handler.finished.wait(5), name
    if mode == "body-limit" and not compressed:
        # Allow socket buffering, but not reading/draining the complete response.
        assert handler.sent < length // 2, (name, handler.sent, length)
    print(f"  response fixture sent {handler.sent} bytes for {length} decoded bytes", flush=True)


def run(name, mode, target, expect="ok", env=None):
    global count
    subprocess.run([str(binary), mode, str(target), expect], env=env, check=True, timeout=15)
    count += 1
    print("PASS:", name, flush=True)


try:
    with tempfile.TemporaryDirectory(prefix="lzc-auth-security-") as temporary:
        tmp = pathlib.Path(temporary)
        source, sh = server()
        proxy, ph = server()
        dest, dh = server()
        base = f"http://127.0.0.1:{source.server_port}"
        proxy_url = f"http://127.0.0.1:{proxy.server_port}"
        env = {k: v for k, v in os.environ.items() if "proxy" not in k.lower()}
        env["http_proxy"] = proxy_url
        env["LZC_FIXTURE_PROXY"] = proxy_url
        run("CLI RPC bypasses environment and explicit session proxies", "rpc", base, env=env)
        assert not ph.records and len(sh.records) == 1
        assert sh.records[-1][1].get("lzc-shellapi-cred") == "fixture-credential"
        run("desktop local API bypasses proxy", "desktop", base, env=env)
        assert not ph.records
        assert sh.records[-1][1].get("LZC_MISC_AUTH_TOKEN") == "fixture-credential"
        for code in (301, 302, 303, 307, 308):
            sh.status, sh.location = code, f"http://localhost:{dest.server_port}/leak"
            run(f"Cookie GET rejects HTTP {code} cross-host redirect", "get", base, "error")
            run(f"CLI RPC rejects HTTP {code} redirect", "rpc", base, "error")
            assert not dh.records
        sh.status, sh.location = 200, None
        try:
            six, six_handler = server(ipv6=True)
        except OSError:
            print("SKIP: IPv6 loopback unavailable")
        else:
            run("IPv6 server-side connection ownership", "rpc", f"http://[::1]:{six.server_port}")
            assert six_handler.records

        slow, slow_handler = server(accept_delay=.5)
        run("desktop authentication tolerates delayed accept without leaking early", "desktop",
            f"http://127.0.0.1:{slow.server_port}")
        run("CLI authentication tolerates delayed accept", "rpc",
            f"http://127.0.0.1:{slow.server_port}")
        assert len(slow_handler.records) == 2
        # No accepted socket means no provable owner. Stop within the Session
        # budget and confirm that not even a request header was queued to it.
        with socket.socket() as pending:
            pending.bind(("127.0.0.1", 0)); pending.listen()
            start = time.monotonic()
            run("unaccepted socket has bounded wait and sends zero headers", "unaccepted",
                f"http://127.0.0.1:{pending.getsockname()[1]}")
            assert .8 <= time.monotonic() - start < 3
            pending.settimeout(2)
            accepted, _ = pending.accept()
            with accepted:
                accepted.settimeout(2)
                assert accepted.recv(1) == b""
        try:
            dual, dual_handler = server(dual_stack=True)
        except OSError:
            print("SKIP: dual-stack IPv6 listener unavailable")
        else:
            run("IPv4 client authenticates dual-stack IPv6 server", "rpc",
                f"http://127.0.0.1:{dual.server_port}")
            assert dual_handler.records
        body_case("Content-Length over limit cancels before full body", False)
        body_case("chunked body over limit cancels before full body", True)
        body_case("Content-Length exactly 1 MiB accepted", False, length=1024 * 1024)
        body_case("chunked exactly 1 MiB accepted", True, length=1024 * 1024)
        body_case("decoded gzip body over limit rejected", False, compressed=True)
        body_case("chunked decoded gzip over limit rejected", True, compressed=True)
        body_case("gzip exactly 1 MiB accepted", False, length=1024 * 1024, compressed=True)

        # Verify HTTPS with a fixture trust root, rather than disabling TLS validation.
        key, cert = tmp / "key.pem", tmp / "cert.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        secure, hh = server(tls)
        secure_dest, hd = server(tls)
        secure_url = f"https://127.0.0.1:{secure.server_port}"
        secure_env = {k: v for k, v in os.environ.items() if "proxy" not in k.lower()}
        secure_env["LZC_FIXTURE_CA"] = str(cert)
        run("HTTPS authentication still works with verified certificates", "get", secure_url, env=secure_env)
        body_case("HTTPS chunked body is bounded too", True, tls, secure_env)
        body_case("HTTPS exactly 1 MiB accepted", True, tls, secure_env, length=1024 * 1024)
        for code in (302, 307, 308):
            hh.status, hh.location = code, f"https://localhost:{secure_dest.server_port}/leak"
            run(f"HTTPS Cookie rejects {code} cross-host redirect", "get", secure_url, "error", secure_env)
            assert not hd.records
        hh.location = base + "/downgrade"
        before = len(sh.records)
        run("HTTPS Cookie refuses downgrade redirect", "get", secure_url, "error", secure_env)
        assert len(sh.records) == before

        private = tmp / "private"
        private.write_text("fixture-credential")
        private.chmod(0o600)
        run("private regular credential file", "file", private)
        private.chmod(0o644)
        run("world-readable credential rejected", "file", private, "error")
        private.chmod(0o620)
        run("group-writable credential rejected", "file", private, "error")
        private.chmod(0o600)
        link = tmp / "symlink"; link.symlink_to(private)
        run("symlink credential rejected", "file", link, "error")
        hard = tmp / "hardlink"; os.link(private, hard)
        run("hardlinked credential rejected", "file", private, "error")
        hard.unlink()
        fifo = tmp / "fifo"; os.mkfifo(fifo, 0o600)
        run("FIFO rejected without blocking", "file", fifo, "error")
        private.write_bytes(b"fixture\0credential")
        run("NUL credential rejected", "file", private, "error")
        private.write_text("a" * 1025)
        run("oversized credential rejected", "file", private, "error")

        run("live CLI process can be pinned", "peer", os.getpid())
        run("nonexistent PID rejected", "peer", 2**30, "error")
        run("invalid PID rejected", "peer", 0, "error")
        child = subprocess.Popen(["python", "-c", "import time; time.sleep(60)"])
        try:
            run("different process name rejected", "peer", child.pid, "error")
        finally:
            child.terminate(); child.wait()
        child = subprocess.Popen(["python", "-u", "-c",
                                  'import ctypes,time; ctypes.CDLL(None).prctl(15,b"hclient-cli",0,0,0); print("ready"); time.sleep(60)'],
                                 stdout=subprocess.PIPE, text=True)
        try:
            assert child.stdout.readline().strip() == "ready"
            before = len(sh.records)
            run("pinned process exit prevents any credential send", "peer-exit", base, "error",
                dict(os.environ, LZC_FIXTURE_PID=str(child.pid)))
            assert len(sh.records) == before
        finally:
            if child.poll() is None: child.terminate()
            child.wait()
        home = tmp / "home"
        config = home / ".config/hportal-client"; config.mkdir(parents=True)
        state = config / "hclient-cli-daemon.json"
        secret = config / "shellapi_cred"
        state.write_text(json.dumps({"pid": os.getpid(), "shellapi_address": f"127.0.0.1:{source.server_port}"}))
        secret.write_text("fixture-credential")
        state.chmod(0o600); secret.chmod(0o600)
        home_env = dict(os.environ, HOME=str(home))
        run("full CLI discovery and authenticated RPC", "discover", "-", env=home_env)
        state.write_text(json.dumps({"pid": 2**30, "shellapi_address": f"127.0.0.1:{source.server_port}"}))
        before = len(sh.records)
        run("stale PID never contacts replacement server", "discover", "-", "error", home_env)
        assert len(sh.records) == before
        state.write_text(json.dumps({"pid": os.getpid(), "shellapi_address": "192.0.2.1:8080"}))
        run("non-loopback CLI endpoint rejected", "discover", "-", "error", home_env)
        run("local secret cannot be sent to remote HTTP endpoint", "rpc", "http://192.0.2.1:8080", "error")
        if os.environ.get("LZC_FOREIGN_URL"):
            run("foreign-UID connection rejected before writing HTTP headers",
                "foreign", os.environ["LZC_FOREIGN_URL"])
        print(f"PASS: {count} credential-boundary cases", flush=True)
finally:
    for service, thread in servers:
        service.shutdown(); thread.join(); service.server_close()
