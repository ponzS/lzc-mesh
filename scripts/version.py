#!/usr/bin/env python3
"""Synchronize tool-required version fields from the tracked root .env.

No third-party dependencies; .env is parsed as data, never executed. Derived
files stay checked in so cargo --locked, IDEs and the web tree work directly.
"""
import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
KEYS = {"VERSION", "MACOS_BUILD", "MACOS_HELPER_BUILD"}


def metadata():
    values = {}
    for number, raw in enumerate((ROOT / ".env").read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or key not in KEYS or key in values:
            raise ValueError(f".env:{number}: expected one unique release KEY=value")
        values[key] = value
    if values.keys() != KEYS:
        raise ValueError(f".env: required keys are {', '.join(sorted(KEYS))}")
    version = values["VERSION"]
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version):
        raise ValueError(".env: VERSION must be major.minor.patch (no leading zeros)")
    if any(int(part) > 65535 for part in version.split(".")):
        raise ValueError(".env: VERSION components must fit Windows 16-bit resources")
    for key in ("MACOS_BUILD", "MACOS_HELPER_BUILD"):
        if not re.fullmatch(r"[1-9][0-9]*", values[key]):
            raise ValueError(f".env: {key} must be a positive integer")
    return values


def derived(values, scope):
    """Compute every edit first; fail without partial writes on schema drift."""
    version = values["VERSION"]
    pending = {}

    def replace(path, pattern, replacement, count=1):
        original = pending.get(path, (ROOT / path).read_text(encoding="utf-8"))
        if len(re.findall(pattern, original, flags=re.MULTILINE)) != count:
            raise ValueError(f"{path}: expected {count} version field(s) matching {pattern!r}")
        pending[path] = re.sub(pattern, replacement if callable(replacement) else lambda _: replacement,
                               original, flags=re.MULTILINE)

    crates = [("lzc-mesh-client/transport", "lzc-mesh-transport")]
    if scope == "all":
        crates.append(("lzc-mesh-lpk/hub", "lzc-mesh-hub"))
    for folder, name in crates:
        replace(f"{folder}/Cargo.toml", r'^version = "[^"]+"$', f'version = "{version}"')
        replace(f"{folder}/Cargo.lock", rf'name = "{name}"\nversion = "[^"]+"',
                f'name = "{name}"\nversion = "{version}"')
    if scope == "linux":
        return {path: text for path, text in pending.items() if text != (ROOT / path).read_text(encoding="utf-8")}
    replace("lzc-mesh-lpk/package.yml", r"^version: \S+$", f"version: {version}")
    replace("lzc-mesh-client/macos/Sources/AppState.swift", r'    static let version = "[^"]+"',
            f'    static let version = "{version}"')
    for path, build in (("lzc-mesh-client/macos/Info.plist", values["MACOS_BUILD"]),
                        ("lzc-mesh-client/macos/BluetoothHelper/Info.plist", values["MACOS_HELPER_BUILD"])):
        for key, value in (("CFBundleShortVersionString", version), ("CFBundleVersion", build)):
            text = pending.get(path, (ROOT / path).read_text(encoding="utf-8"))
            pattern = rf"<key>{key}</key>(\s*)<string>[^<]+</string>"
            match = re.search(pattern, text)
            if not match:
                raise ValueError(f"{path}: missing {key}")
            replace(path, pattern, f"<key>{key}</key>{match[1]}<string>{value}</string>")
    replace("lzc-mesh-client/windows/src/app.h", r'^#define LZC_MESH_VERSION "[^"]+"$',
            f'#define LZC_MESH_VERSION "{version}"')
    replace("lzc-mesh-client/windows/setup/setup.cpp", r'const wchar_t \*kVersion = L"[^"]+";',
            f'const wchar_t *kVersion = L"{version}";')
    for path in ("lzc-mesh-client/windows/lzc-mesh.rc", "lzc-mesh-client/windows/setup/setup.rc"):
        for key in ("FILEVERSION", "PRODUCTVERSION"):
            replace(path, rf"^{key} [0-9,]+$", f"{key} {version.replace('.', ',')},0")
        replace(path, r'      VALUE "ProductVersion", "[^"]+"', f'      VALUE "ProductVersion", "{version}"')
    for path in ("lzc-mesh-client/windows/lzc-mesh.manifest", "lzc-mesh-client/windows/setup/setup.manifest"):
        replace(path, r'(<assemblyIdentity[^>]*name="cloud\.lazycat\.[^"]+"[^>]*\bversion=")[^"]+"',
                lambda match: match[1] + version + '.0"')
    for path, count in (("lzc-mesh-lpk/web/index.html", 3),
                        ("lzc-mesh-lpk/web/controller.html", 4),
                        ("lzc-mesh-lpk/web/controller.js", 2)):
        replace(path, r"\?v=[0-9]+\.[0-9]+\.[0-9]+", f"?v={version}", count)
    replace("lzc-mesh-lpk/web/index.html", r"releases/download/v[0-9]+\.[0-9]+\.[0-9]+/",
            f"releases/download/v{version}/", 6)
    replace("lzc-mesh-lpk/web/index.html", r"releases/tag/v[0-9]+\.[0-9]+\.[0-9]+",
            f"releases/tag/v{version}")
    return {path: text for path, text in pending.items() if text != (ROOT / path).read_text(encoding="utf-8")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scope", choices=("all", "linux"), default="all",
                        help="linux is for the isolated makepkg source bundle")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--check", action="store_true", help="fail if any derived version is stale")
    group.add_argument("--print", dest="show", action="store_true", help="print only VERSION from .env")
    args = parser.parse_args()
    values = metadata()
    if args.show:
        print(values["VERSION"])
        return 0
    changes = derived(values, args.scope)
    if args.check:
        if changes:
            print("Stale version metadata; run python3 scripts/version.py:\n" + "\n".join(changes), file=sys.stderr)
            return 1
        print(f"Version metadata matches .env ({values['VERSION']})")
        return 0
    for path, text in changes.items():
        (ROOT / path).write_text(text, encoding="utf-8")
    print(f"Version {values['VERSION']}: synchronized {len(changes)} file(s)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"version: {error}", file=sys.stderr)
        sys.exit(1)
