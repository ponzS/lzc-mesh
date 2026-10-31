# LZC Mesh

用一套键盘和触控板/鼠标控制面前的多台电脑（macOS、Windows、Linux）。只同步光标和键盘，不传画面。设备之间通过懒猫网络通信。

## 目录

| 路径 | 内容 |
| --- | --- |
| `lzc-mesh-lpk/` | 懒猫微服应用：Rust 中枢（布局存储、贴边、输入转发）和布局管理页面 |
| `lzc-mesh-client/macos/` | macOS 客户端（Swift / AppKit，CGEventTap + CGEvent） |
| `lzc-mesh-client/windows/` | Windows 客户端（C++ / Win32，低级钩子 + SendInput，WinHTTP） |
| `lzc-mesh-client/linux/` | Linux 客户端（C / GTK4，wlroots 虚拟指针、虚拟键盘、layer-shell） |

## 工作方式

- 中枢运行在 LPK 里，地址 `https://lzc-mesh.<微服>.heiyu.space`，按懒猫账号隔离设备组，布局保存在 `/lzcapp/var/layouts`。
- 客户端通过懒猫网络连接中枢的 WebSocket `/api/ws`。
- 光标推到有相邻屏幕的边缘时，本机光标被锁住隐藏，移动量和按键转发给中枢，中枢按布局把光标交给相邻设备。按键在线路上统一使用 Linux evdev 键码。
- 控制其他设备时按 Ctrl+Alt+Esc（Mac 上是 Ctrl+Option+Esc）立即收回。

## 构建

```sh
# LPK（Linux x86_64，需要 Rust 的 x86_64-unknown-linux-musl target 和 lzc-cli）
cd lzc-mesh-lpk && sh build.sh && lzc-cli project release

# macOS（在 Mac 上，需要 Xcode 命令行工具）
cd lzc-mesh-client/macos && ./build.sh

# Windows（在 Linux 上用 zig 交叉编译）
cd lzc-mesh-client/windows && ./build.sh

# Linux（需要 gtk4、libsoup-3.0、json-glib、wayland-client、xkbcommon）
cd lzc-mesh-client/linux && make
```

产物输出到 `lzc-mesh-lpk/dist/` 和 `lzc-mesh-client/dist/`。

## 权限

- macOS：需要“辅助功能”和“输入监控”，客户端启动时会主动请求。
- Windows：普通权限即可；控制管理员权限的窗口时需要“以管理员身份运行”，客户端窗口会提示。
- Linux：需要 Hyprland、Sway 等支持 wlroots 协议的 Wayland 合成器。

## 状态

macOS ↔ Windows 已真机验证。Linux 客户端已编译并能连上中枢，输入注入尚未真机验证。
