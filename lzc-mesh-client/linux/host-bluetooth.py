#!/usr/bin/env python3
"""Pass outbound RFCOMM descriptors to one local desktop UID.

No IP listener, input injection, file API or subprocess API. On a MicroServer,
root only binds the private UNIX socket, then permanently drops to nobody.
"""
import argparse
import array
import concurrent.futures
import json
import errno
import select
import time
import os
import re
import socket
import stat
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", required=True)
    parser.add_argument("--peer-uid", type=int, required=True)
    parser.add_argument("--drop-uid", type=int, default=65534)
    parser.add_argument("--drop-gid", type=int, default=65534)
    args = parser.parse_args()
    os.umask(0o177)
    # Do not follow a user-supplied final symlink or chown/chmod user paths.
    directory = os.open(args.directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    os.fchdir(directory)
    name = "host-bluetooth.sock"
    try:
        old = os.stat(name, follow_symlinks=False)
    except FileNotFoundError:
        pass
    else:
        if not stat.S_ISSOCK(old.st_mode) or old.st_uid != os.getuid():
            raise RuntimeError("Refusing to replace a foreign socket or non-socket")
        os.unlink(name)
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(name)
    listener.listen(4)
    os.close(directory)
    if os.getuid() == 0:
        os.setgroups([])
        os.setgid(args.drop_gid)
        os.setuid(args.drop_uid)
    # Steady-state CWD must not grant access to the shared document directory.
    os.chdir("/")
    print(f"Bluetooth descriptor broker ready uid={os.getuid()} peer_uid={args.peer_uid}", flush=True)

    def serve(client):
        with client:
            try:
                _, uid, _ = struct.unpack("3i", client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
                if uid != args.peer_uid:
                    return
                client.settimeout(3)
                raw = bytearray()
                while b"\n" not in raw and len(raw) <= 1024:
                    chunk = client.recv(1024)
                    if not chunk:
                        return
                    raw.extend(chunk)
                if len(raw) > 1024 or not raw.endswith(b"\n"):
                    return
                request = json.loads(raw)
                if request == {"op": "capabilities"}:
                    client.sendall(b'{"ok":true,"role":"connector"}\n')
                    return
                address, channel = request.get("address"), request.get("channel")
                if request.get("op") != "connect" or not isinstance(address, str) or not re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", address) or type(channel) is not int or not 1 <= channel <= 30:
                    return
                with socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM) as rfcomm:
                    rfcomm.setblocking(False)
                    result = rfcomm.connect_ex((address, channel))
                    if result not in (0, errno.EINPROGRESS, errno.EWOULDBLOCK, errno.EALREADY):
                        raise OSError(result, "Bluetooth connection failed")
                    deadline = time.monotonic() + 8
                    while result:
                        if time.monotonic() >= deadline:
                            raise TimeoutError(errno.ETIMEDOUT, "Bluetooth connection timed out")
                        readable, writable, exceptional = select.select([client], [rfcomm], [rfcomm], 0.1)
                        if readable:
                            # The desktop cancels pending attempts when switching modes.
                            raise ConnectionAbortedError(errno.ECANCELED, "Desktop canceled")
                        if writable or exceptional:
                            result = rfcomm.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                            if result:
                                raise OSError(result, "Bluetooth connection failed")
                            break
                    rfcomm.setblocking(True)
                    descriptors = array.array("i", [rfcomm.fileno()])
                    client.sendmsg([b'{"ok":true}\n'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, descriptors)])
            except (OSError, ValueError, TypeError, AttributeError) as error:
                # Fixed diagnostic text; no filenames, device names or credentials.
                try:
                    client.sendall(json.dumps({"ok": False, "errno": getattr(error, "errno", None)}).encode() + b"\n")
                except OSError:
                    pass

    # Bound simultaneous requests, including stalled Bluetooth connection attempts.
    pending = set()
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        while True:
            client, _ = listener.accept()
            # No unbounded executor queue: requests are tiny and desktop-scoped.
            # The listen backlog bounds clients while workers are occupied.
            future = pool.submit(serve, client)
            pending = {f for f in pending if not f.done()}
            pending.add(future)
            if len(pending) >= 4:
                concurrent.futures.wait(pending, return_when=concurrent.futures.FIRST_COMPLETED)


if __name__ == "__main__":
    main()
