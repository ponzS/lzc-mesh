#!/usr/bin/env python3
"""Version-source regressions in disposable trees; never edits the checkout."""
from pathlib import Path
import os
import plistlib
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
count = 0


def passed(name):
    global count
    count += 1
    print("PASS:", name, flush=True)


with tempfile.TemporaryDirectory(prefix="lzc-version-") as temporary:
    root = Path(temporary)
    # Copy tracked sources only, including new unstaged release files.
    paths = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode().split("\0")
    for name in set(paths + [".env", "scripts/version.py", "lzc-mesh-lpk/release.sh"]):
        source = ROOT / name
        if not name or not source.is_file():
            continue
        dest = root / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)

    def run(*args, ok=True):
        result = subprocess.run(["python3", str(root / "scripts/version.py"), *args],
                                cwd="/tmp", text=True, capture_output=True)
        assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
        return result

    def snapshot():
        return {str(p.relative_to(root)): p.read_bytes() for p in root.rglob("*") if p.is_file()}

    original_env = (root / ".env").read_text()
    run("--check")
    passed("checked-in metadata agrees with root .env from any working directory")
    env = "VERSION=1.2.3\nMACOS_BUILD=100\nMACOS_HELPER_BUILD=40\n"
    (root / ".env").write_text(env)
    before = snapshot()
    run("--check", ok=False)
    assert snapshot() == before
    assert run("--print").stdout.strip() == "1.2.3"
    run()
    run("--check")
    changed = {name for name, value in before.items() if snapshot()[name] != value}
    assert len(changed) == 17, changed
    for folder, name in (("lzc-mesh-client/transport", "lzc-mesh-transport"),
                         ("lzc-mesh-lpk/hub", "lzc-mesh-hub")):
        assert 'version = "1.2.3"' in (root / folder / "Cargo.toml").read_text()
        old = before[f"{folder}/Cargo.lock"].decode()
        new = (root / folder / "Cargo.lock").read_text()
        assert new == re.sub(rf'(name = "{name}"\nversion = ")[^"]+', r'\g<1>1.2.3', old)
    for file, build in (("Info.plist", "100"), ("BluetoothHelper/Info.plist", "40")):
        info = plistlib.loads((root / "lzc-mesh-client/macos" / file).read_bytes())
        assert info["CFBundleShortVersionString"] == "1.2.3" and info["CFBundleVersion"] == build
    for file in ("lzc-mesh.manifest", "setup/setup.manifest"):
        tree = ET.parse(root / "lzc-mesh-client/windows" / file)
        for element in tree.iter():
            if element.tag.endswith("assemblyIdentity"):
                assert element.attrib["version"] == ("1.2.3.0" if element.attrib["name"].startswith("cloud.lazycat.") else "6.0.0.0")
    assert (root / "README.md").read_bytes() == before["README.md"]
    assert (root / "README.en.md").read_bytes() == before["README.en.md"]
    assert all('?v=1.2.3' in (root / f"lzc-mesh-lpk/web/{file}").read_text()
               for file in ("index.html", "controller.html", "controller.js"))
    passed("one .env change updates all 17 derived files without touching dependencies or history")
    mtimes = {str(p): p.stat().st_mtime_ns for p in root.rglob("*") if p.is_file()}
    run()
    assert all(Path(p).stat().st_mtime_ns == mtime for p, mtime in mtimes.items())
    passed("sync is idempotent and does not force rebuilds of unchanged metadata")

    bad_values = [env + "VERSION=2.0.0\n", env.replace("1.2.3", "01.2.3"),
                  env.replace("1.2.3", "1.2.3-beta"), env.replace("1.2.3", "65536.0.0"),
                  env.replace("MACOS_BUILD=100\n", ""), env.replace("MACOS_BUILD=100", "MACOS_BUILD=0"),
                  env + "TOKEN=not-allowed\n", env.replace("1.2.3", "$(touch SHOULD_NOT_EXIST)")]
    for bad in bad_values:
        (root / ".env").write_text(bad)
        saved = snapshot()
        run(ok=False)
        assert snapshot() == saved
    passed("invalid, duplicate, missing or executable .env values fail without executing or writing")
    (root / ".env").write_text(env.replace("1.2.3", "2.3.4"))
    broken = root / "lzc-mesh-client/windows/src/app.h"
    broken.write_text(broken.read_text().replace('#define LZC_MESH_VERSION', '#define BROKEN_VERSION'))
    saved = snapshot()
    run(ok=False)
    assert snapshot() == saved
    broken.write_bytes(saved["lzc-mesh-client/windows/src/app.h"].replace(b'BROKEN_VERSION', b'LZC_MESH_VERSION'))
    passed("unexpected field structure is diagnosed before any partial metadata update")

    # Verify actual build entry points stop at version validation, before any
    # compiler/package invocation, rather than quietly using stale metadata.
    (root / ".env").write_text("VERSION=invalid\n")
    for entry in ("lzc-mesh-client/windows/build.sh", "lzc-mesh-client/macos/build.sh",
                  "lzc-mesh-lpk/build.sh", "lzc-mesh-lpk/release.sh"):
        result = subprocess.run(["sh", str(root / entry)], text=True, capture_output=True)
        assert result.returncode and "version:" in result.stderr, (entry, result.stderr)
    result = subprocess.run(["make", "-n", "-C", str(root / "lzc-mesh-client/linux")],
                            text=True, capture_output=True)
    assert result.returncode and "Failed to synchronize" in result.stderr
    passed("all four platform builds and the LPK release entry reject invalid metadata before building")

    # Exercise the release wrapper with a fake packaging CLI: it must see the
    # already-synchronized package and an explicit matching version override.
    fake_bin = root / "fake-bin"
    fake_bin.mkdir()
    fake_cli = fake_bin / "lzc-cli"
    fake_cli.write_text('#!/bin/sh\nset -eu\nprintf "%s\\n" "$LZC_VERSION"\n'
                        'python3 "$VERSION_TEST_ROOT/scripts/version.py" --check\n'
                        'printf "%s\\n" "$@"\n')
    fake_cli.chmod(0o755)
    (root / ".env").write_text(env.replace("1.2.3", "3.4.5"))
    result = subprocess.run(["sh", str(root / "lzc-mesh-lpk/release.sh"), "--log", "debug"],
                            env=dict(os.environ, PATH=str(fake_bin) + os.pathsep + os.environ["PATH"],
                                     VERSION_TEST_ROOT=str(root)), capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert '3.4.5' in result.stdout and '--log\ndebug' in result.stdout
    assert 'version: 3.4.5' in (root / "lzc-mesh-lpk/package.yml").read_text()
    passed("LPK wrapper synchronizes before CLI metadata loading and forwards arguments")

    (root / ".env").write_text(original_env)
    run()
    # An isolated makepkg bundle only carries Linux + transport + the same root
    # .env and script; the scoped sync must not require other platforms.
    bundle = root / "isolated"
    for name in (".env", "scripts/version.py", "lzc-mesh-client/transport/Cargo.toml", "lzc-mesh-client/transport/Cargo.lock"):
        dest = bundle / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / name, dest)
    (bundle / ".env").write_text(env)
    subprocess.run(["python3", str(bundle / "scripts/version.py"), "--scope", "linux"], check=True)
    subprocess.run(["python3", str(bundle / "scripts/version.py"), "--scope", "linux", "--check"], check=True)
    passed("makepkg scoped preflight works in the minimal staged Linux source tree")

    makefile = (root / "lzc-mesh-client/linux/Makefile").read_text()
    assert "$(ROOT)/.env | build" in makefile
    result = subprocess.run(["make", "-s", "-C", str(root / "lzc-mesh-client/linux"),
                             "VERSION=9.9.9", "-n", "all"], text=True, capture_output=True)
    assert result.returncode == 0 and '-DLZC_MESH_VERSION=\\"9.9.9\\"' not in result.stdout
    passed("Linux objects depend on .env and command-line VERSION cannot override the source of truth")

print(f"PASS: {count} version metadata groups")
