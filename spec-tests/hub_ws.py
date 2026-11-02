"""Small authenticated hub WebSocket client for integration tests; never logs credentials."""
import base64
import json
import os
import socket
import ssl
import struct

class HubSocket:
    def __init__(self, host, cookie='', address=None, port=443, tls=True, headers=None):
        sock = socket.create_connection((address or host, port), timeout=10)
        self.socket = ssl.create_default_context().wrap_socket(sock, server_hostname=host) if tls else sock
        key = base64.b64encode(os.urandom(16)).decode()
        extra = ''.join(f'{name}: {value}\r\n' for name,value in (headers or {}).items())
        self.socket.sendall((f"GET /api/ws HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nCookie: Lzc-Auth-Token={cookie}\r\n{extra}\r\n").encode())
        header = b""
        while not header.endswith(b"\r\n\r\n"):
            byte = self.socket.recv(1)
            if not byte or len(header) > 16384: raise RuntimeError("Invalid hub handshake")
            header += byte
        if not header.startswith(b"HTTP/1.1 101 "):
            status=header.split(b'\r\n',1)[0].split(b' ',2)
            code=status[1].decode('ascii','replace') if len(status)>1 and status[1].isdigit() else 'invalid'
            self.socket.close()
            raise RuntimeError(f"Hub did not accept WebSocket (HTTP {code}; sign-in or origin rejected)")
        accept = base64.b64encode(__import__('hashlib').sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        if b"sec-websocket-accept: " + accept.lower() not in header.lower():
            self.socket.close(); raise RuntimeError("Invalid WebSocket accept")
        self.buffer = b""
        self.fragments = b""
    def frame(self, opcode, data):
        mask = os.urandom(4)
        n = len(data)
        size = bytes([n | 128]) if n < 126 else bytes([126 | 128]) + struct.pack('>H', n) if n <= 65535 else bytes([127 | 128]) + struct.pack('>Q', n)
        self.socket.sendall(bytes([0x80 | opcode]) + size + mask + bytes(b ^ mask[i % 4] for i,b in enumerate(data)))
    def send(self, value): self.frame(1, json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode())
    def read(self, timeout=3):
        self.socket.settimeout(timeout)
        def more(n):
            while len(self.buffer) < n:
                chunk = self.socket.recv(65536)
                if not chunk: raise EOFError("Hub disconnected")
                self.buffer += chunk
        while True:
            more(2)
            opcode = self.buffer[0] & 15
            final = bool(self.buffer[0] & 128)
            n = self.buffer[1] & 127; offset = 2
            if n == 126: more(4); n = struct.unpack('>H',self.buffer[2:4])[0]; offset = 4
            elif n == 127: more(10); n = struct.unpack('>Q',self.buffer[2:10])[0]; offset = 10
            if n > 1024*1024: raise RuntimeError("Hub frame exceeds limit")
            more(offset+n)
            data=self.buffer[offset:offset+n]; self.buffer=self.buffer[offset+n:]
            if opcode == 8: raise EOFError("Hub closed WebSocket")
            if opcode == 9: self.frame(10,data); continue
            if opcode == 10: continue
            if opcode in (0,1):
                self.fragments += data
                if final:
                    message=self.fragments; self.fragments=b""
                    return json.loads(message)
    def close(self):
        try: self.frame(8,b'')
        except OSError: pass
        self.socket.close()
    def __enter__(self): return self
    def __exit__(self,*unused): self.close()
