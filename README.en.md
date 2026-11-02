# LazyCat Device Relay (LZC Mesh)

[简体中文](README.md) | English

Control several computers in front of you (macOS, Windows, Linux) with one keyboard and trackpad/mouse. Only the cursor and keyboard are synced; no screen is streamed. It also offers a shared clipboard (text, on by default) and file sharing across devices. Devices talk to each other over the LazyCat network.

## Layout

| Path | Contents |
| --- | --- |
| `lzc-mesh-lpk/` | LazyCat MicroServer app: Rust hub (layout storage, edge snapping, input relay, clipboard relay, file storage) and the management page |
| `lzc-mesh-client/macos/` | macOS client (Swift / AppKit, CGEventTap + CGEvent) |
| `lzc-mesh-client/windows/` | Windows client (C++ / Win32, low-level hooks + SendInput, WinHTTP) |
| `lzc-mesh-client/linux/` | Linux client (C / GTK4, wlroots virtual pointer, virtual keyboard, layer-shell) |

## How it works

- The hub runs inside the LPK at `https://lzc-mesh.<box>.heiyu.space`. Devices are grouped per LazyCat account, and layouts are stored in `/lzcapp/var/layouts`.
- Clients connect to the hub's WebSocket `/api/ws` over the LazyCat network.
- When the cursor is pushed against an edge that has a neighbouring screen, the local cursor is locked and hidden, motion and keys are forwarded to the hub, and the hub hands the cursor to the neighbouring device according to the layout. Keys travel as Linux evdev key codes on every platform.
- While controlling another device, press Ctrl+Alt+Esc (Ctrl+Option+Esc on a Mac) to take control back immediately.
- The shared clipboard is on by default and can be turned off under "Settings" on the management page or from a client's tray menu. It syncs text only, up to 256 KB. The hub relays it between the account's online devices and never stores it. Content that password managers mark as concealed is never synced.
- File sharing: upload from the management page or any client window, and every device of the same account can download it. Files are stored on the box in `/lzcapp/var/files`, up to 4 GB each.

## Input transport and emergency pause

The web page and macOS/Linux 0.2.0 native settings share Automatic (LAN preferred), Bluetooth only, LAN only, and Relay only modes. LAN uses mutually authenticated QUIC; Bluetooth uses RFCOMM with mutual TLS and the existing control grants. Bluetooth only and LAN only stop control when unavailable. Relay only closes Bluetooth services, connections and LAN UDP endpoints. Pointer and keyboard have separate logical status while sharing physical links. Older clients continue to use the hub.

Bluetooth is experimental; Mac initialization still has launch-context issues to verify, documented in the [test report](spec-tests/bluetooth-deployment-2026-11-02.md). MicroServer containers obtain actual Bluetooth descriptors through a private host Unix socket; see [BLUETOOTH.md](lzc-mesh-client/linux/BLUETOOTH.md).

Pause all device input revokes the account's current control session, releases held input, closes Bluetooth and LAN endpoints on upgraded clients, and prevents new control sessions. The management connection remains available for resuming. Transport mode and input enablement are persisted by the hub.

Direct input does not transmit screenshots and runs as the logged-in user. Input control can still operate that user's applications and data; authentication and session authorization remain essential. Relay only disables the LAN input entry point when desired.

## Download

Packages are on [GitHub Releases](https://github.com/ponzS/lzc-mesh/releases). Every platform has an installer and an archive; use either one.

| Platform | Installer | Archive |
| --- | --- | --- |
| LazyCat MicroServer | `cloud.lazycat.app.lzc-mesh-v<version>.lpk`: drag it onto the box launcher page | — |
| macOS 12+ (Apple Silicon / Intel) | `lzc-mesh-macos.dmg`: open it and drag the app to Applications | `lzc-mesh-macos.zip`: unzip and move `LZC Mesh.app` to Applications |
| Windows 10/11 x64 | `lzc-mesh-windows-setup.exe`: double-click to install; uninstall from Settings › Apps | `lzc-mesh-windows.zip`: unzip, right-click `install.ps1` → Run with PowerShell |
| Linux x86_64 | `lzc-mesh-linux-x86_64.pkg.tar.zst` (Arch): `sudo pacman -U` | `lzc-mesh-linux-x86_64.tar.gz`: unpack and run `./install.sh` |

Installing on Windows and Linux needs no administrator rights (except the Arch package). The download links on the management page point to the matching release.

## Build

```sh
# LPK (Linux x86_64; needs Rust's x86_64-unknown-linux-musl target and lzc-cli)
cd lzc-mesh-lpk && sh build.sh && lzc-cli project release

# macOS (on a Mac with the Xcode command line tools)
cd lzc-mesh-client/macos && ./build.sh

# Windows (cross-compiled on Linux with zig; produces setup.exe and a zip)
cd lzc-mesh-client/windows && ./build.sh

# Linux (needs gtk4, libsoup-3.0, json-glib, wayland-client, xkbcommon)
cd lzc-mesh-client/linux && make && make dist   # tar.gz
cd lzc-mesh-client/linux && make pkg            # Arch package, needs makepkg
```

Output goes to `lzc-mesh-lpk/dist/` and `lzc-mesh-client/dist/`. The macOS `build.sh` produces both the dmg and the zip.

## Permissions

- macOS: needs Accessibility and Input Monitoring; the client asks for them on first launch.
- Windows: runs with normal rights. Controlling windows that run as administrator requires "Run as administrator"; the client window tells you when.
- Linux: needs a Wayland compositor with the wlroots protocols, such as Hyprland or Sway.

## Status

The user accepted the 0.2.0 input improvements and reported substantially smoother cursor control. This feedback did not identify each transport mode; the report retains the limits of automated Bluetooth verification.

macOS ↔ Windows has been verified on real hardware: cursor crossing, keyboard, key repeat, shared clipboard, file sharing and layout sync. Mac → Linux cursor crossing, continuous trackpad scrolling and momentum scrolling have also been verified on real hardware. Version 0.1.3 fixes scroll-source misclassification and lost scroll phases. Other Linux input paths, the clipboard and the files panel have not yet been fully verified on real hardware.

## License

The code is licensed under the PolyForm Noncommercial License 1.0.0, SPDX identifier `PolyForm-Noncommercial-1.0.0`.

You may use, copy, modify and distribute the source for noncommercial purposes only. Commercial use is not permitted, including commercial products, commercial services, paid delivery, commercial integration or any other use aimed at commercial gain.

See [LICENSE](./LICENSE) for the full terms. The Wayland protocol files under `lzc-mesh-client/linux/protocols/` keep their original license notices.
