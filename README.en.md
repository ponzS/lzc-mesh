# LazyCat Device Relay (LZC Mesh)

[简体中文](README.md) | English

Control several computers in front of you (macOS, Windows, Omarchy) with one keyboard and trackpad/mouse. Only the cursor and keyboard are synced; no screen is streamed. It also offers a shared clipboard (text, on by default) and file sharing across devices. Devices talk to each other over the LazyCat network.

**Linux currently supports Omarchy (Hyprland) only. Other Linux distributions, desktop environments and Wayland compositors are not supported.** References to the Linux client below mean the Omarchy client.

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
- Crossing a neighbouring screen edge requests control from the hub. With a healthy direct link, clients exchange pointer positions and input actions directly without waiting for the hub per event. Automatic mode relays input when direct connectivity is unavailable. The hub retains device trust, layout, control authorization and emergency-stop authority. Keys use Linux evdev codes on every platform.
- While controlling another device, press Ctrl+Alt+Esc (Ctrl+Option+Esc on a Mac) to take control back immediately.
- The shared clipboard is on by default and can be turned off under "Settings" on the management page or from a client's tray menu. It syncs text only, up to 256 KB. The hub relays it between the account's online devices and never stores it. Content that password managers mark as concealed is never synced.
- File sharing: upload from the management page or an approved, running client; approved, running devices of the same account can download it. Files are stored on the box in `/lzcapp/var/files`, up to 4 GB each.

## Input transport and emergency pause

The web page and matching updated macOS/Linux/Windows native clients share Automatic (LAN preferred), Bluetooth only, LAN only, and Relay only modes. LAN uses mutually authenticated QUIC; Bluetooth uses RFCOMM with mutual TLS and the existing control grants. Bluetooth only and LAN only stop control when unavailable. Relay only closes Bluetooth services, connections and LAN UDP endpoints. Pointer and keyboard have separate logical status while sharing physical links. With device credentials required, older clients must upgrade and be approved; relaying does not bypass authorization.

Bluetooth is experimental. Mac publishes a service, Linux connects, and Windows can both connect to Mac and serve Linux. System pairing is required; a missing adapter is reported as unavailable without network fallback in strict Bluetooth mode. Mac initialization still has launch-context issues to verify, documented in the [test report](spec-tests/bluetooth-deployment-2026-11-02.md). MicroServer containers obtain actual Bluetooth descriptors through a private host Unix socket; see [BLUETOOTH.md](lzc-mesh-client/linux/BLUETOOTH.md).

Pause all device input revokes the account's current control session, releases held input, closes Bluetooth and LAN endpoints on upgraded clients, and prevents new control sessions. The management connection remains available for resuming. Transport mode and input enablement are persisted by the hub.

Direct input does not transmit screenshots and runs as the logged-in user. Input control can still operate that user's applications and data; authentication and session authorization remain essential. Relay only disables the LAN input entry point when desired.

## Device trust and security

Each client keeps a persistent private device key. First enrollment, including existing devices upgrading, requires approval after comparing the full public-key fingerprint shown on the client. Trust, device IDs and layout survive reconnects and restarts. Unapproved devices cannot participate in sharing or receive peer certificates and automatic shared data. Revoking trust is separate from the client running switch. Keys use macOS Keychain, Windows current-user DPAPI or an owner-only 0600 file on Omarchy. Invalid stored credentials stop sharing rather than silently generating a replacement.

Device signatures are used during connection registration, not per input event. Input retains QUIC/TLS encryption and local session/replay checks without a hub authorization round trip per event. Windows hub sends use a bounded background FIFO instead of synchronous network writes on the input injection thread.

The account's management web session can approve devices and manage files; protect it accordingly. Device binding does not protect a stolen management session or guarantee that authorized input cannot operate an elevated terminal. Clients do not automatically elevate or add a command-execution service. See the [trust protocol](spec/connection/device-trust/protocol.md). These changes require matching upgraded clients and hub; older Release packages do not gain device credentials automatically.

## Per-device switch and dormancy

Each device has a Client running switch on the management page, in native settings and in its tray/menu. Closing a client stops its keyboard/mouse, both clipboard directions, file transfers and direct endpoints, cancelling in-flight transfers without deleting stored shared files or stopping other devices. A background management connection remains for remote wake. The hub persists this choice across reconnects and restarts. Wake does not replay old input, publish clipboard content copied while dormant, or resume cancelled transfers.

This is separate from keyboard/mouse sharing and the account-wide keyboard/mouse pause, which leave clipboard and files enabled. Full dormancy requires matching updated native clients; older release packages do not gain it automatically. The LPK cannot launch a process that has really quit, power on a computer or wake an unreachable client; it can only save the choice for the next connection.

## Download

Packages are on [GitHub Releases](https://github.com/ponzS/lzc-mesh/releases). Every platform has an installer and an archive; use either one.

| Platform | Installer | Archive |
| --- | --- | --- |
| LazyCat MicroServer | `cloud.lazycat.app.lzc-mesh-v<version>.lpk`: drag it onto the box launcher page | — |
| macOS 12+ (Apple Silicon / Intel) | `lzc-mesh-macos.dmg`: open it and drag the app to Applications | `lzc-mesh-macos.zip`: unzip and move `LZC Mesh.app` to Applications |
| Windows 10/11 x64 | `lzc-mesh-windows-setup.exe`: double-click to install; uninstall from Settings › Apps | `lzc-mesh-windows.zip`: unzip, right-click `install.ps1` → Run with PowerShell |
| Linux x86_64 (Omarchy only) | `lzc-mesh-linux-x86_64.pkg.tar.zst` (Arch): `sudo pacman -U` | `lzc-mesh-linux-x86_64.tar.gz`: unpack and run `./install.sh` |

Installing on Windows and Linux needs no administrator rights (except the Arch package). The download links on the management page point to the matching release. Device credentials in development builds require matching upgraded clients; older Release packages do not support that mechanism.

## Build

```sh
# LPK (Linux x86_64; needs Rust's x86_64-unknown-linux-musl target and lzc-cli)
cd lzc-mesh-lpk && sh build.sh && lzc-cli project release

# macOS (on a Mac with the Xcode command line tools)
cd lzc-mesh-client/macos && ./build.sh

# Windows (requires Rust's x86_64-pc-windows-gnu target and zig; produces setup.exe and a zip)
cd lzc-mesh-client/windows && ./build.sh

# Omarchy (the only supported Linux environment; needs Rust, gtk4, libsoup-3.0, json-glib, wayland-client, xkbcommon)
cd lzc-mesh-client/linux && make && make dist   # tar.gz
cd lzc-mesh-client/linux && make pkg            # Arch package, needs makepkg
```

Output goes to `lzc-mesh-lpk/dist/` and `lzc-mesh-client/dist/`. The macOS `build.sh` produces both the dmg and the zip.

## Permissions

- macOS: needs Accessibility and Input Monitoring; missing permissions are requested when sharing is activated on an approved client.
- Windows: runs with normal rights. Controlling windows that run as administrator requires "Run as administrator"; the client window tells you when.
- Linux: Omarchy's Hyprland environment only. Sway, other desktop environments and other distributions are not supported.

## Status

0.3.3 fixes intermittent hangs when a Mac remotely operates the Windows client's own title-bar buttons. Receive-side input, mouse releases, key repeat and local takeover no longer wait for the main window's message loop; minimized windows skip invalid layout work, and closing still hides to the tray. Production input components passed 18 real Windows title-bar operations and disconnect-release checks; see the [fix report](spec-tests/windows-caption-stall.md). It also includes the 0.3.2 mobile keyboard/trackpad and immersive-mode support plus subsequently merged edge single-finger scrolling, automatic control start and iOS PWA bottom safe-area adjustments, aligns the LPK and desktop versions, and updates download links. Installers are distributed with the [v0.3.3 Release](https://github.com/ponzS/lzc-mesh/releases/tag/v0.3.3); these checks do not establish post-install Mac→Windows manual or Bluetooth hardware acceptance.

0.2.3 fixes periodic stalls when a Mac trackpad controls Omarchy by moving physical-input device discovery, capability queries and cleanup off the main loop, with stale scan results rejected across receive sessions. The user reported substantially smoother motion after updating. Existing displacement, scrolling and physical takeover rules are preserved. Linux currently supports Omarchy only; see the [receiver stall report](spec-tests/linux-receiver-stall.md). Matching installers have since been added to its GitHub Release; app-store publication is tracked separately. The feedback does not establish complete six-direction physical-input or Bluetooth acceptance.

0.2.2 fixes the intermittent lower-left cursor pinning or invisible cursor when a physical Windows mouse enters Mac. Capture now waits for its entry relocation, uses actual cursor displacement, and accommodates 32-bit event markers in the Windows input stack. The user confirmed successful Windows ↔ Mac manual retesting. It also strengthens native role handoff, local physical takeover and stale-session isolation, and fixes Omarchy key/button integer encoding without changing the established Mac/Omarchy displacement or scrolling algorithms. Linux currently supports Omarchy only. Full six-direction physical-input and Bluetooth hardware acceptance remains incomplete; see the [validation record](spec-tests/input-handoff-status.md).

0.2.1 adds Windows transport parity, background hub sends, cross-platform client dormancy, and device credential approval/revocation. The user confirmed that the deployed update passed functional testing. The Linux client supports Omarchy only. This does not establish acceptance of unperformed Bluetooth hardware tests.

The user accepted the 0.2.0 input improvements and reported substantially smoother cursor control. This feedback did not identify each transport mode; the report retains the limits of automated Bluetooth verification.

macOS ↔ Windows has been verified on real hardware: cursor crossing, keyboard, key repeat, shared clipboard, file sharing and layout sync. Mac → Linux cursor crossing, continuous trackpad scrolling and momentum scrolling have also been verified on real hardware. Version 0.1.3 fixes scroll-source misclassification and lost scroll phases. Other Linux input paths, the clipboard and the files panel have not yet been fully verified on real hardware.

## License

The code is licensed under the PolyForm Noncommercial License 1.0.0, SPDX identifier `PolyForm-Noncommercial-1.0.0`.

You may use, copy, modify and distribute the source for noncommercial purposes only. Commercial use is not permitted, including commercial products, commercial services, paid delivery, commercial integration or any other use aimed at commercial gain.

See [LICENSE](./LICENSE) for the full terms. The Wayland protocol files under `lzc-mesh-client/linux/protocols/` keep their original license notices.
