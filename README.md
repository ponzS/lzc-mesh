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

## 下载

安装包在 [GitHub Releases](https://github.com/ponzS/lzc-mesh/releases)，每个版本包含：

| 文件 | 内容 |
| --- | --- |
| `cloud.lazycat.app.lzc-mesh-v<版本>.lpk` | 懒猫微服应用，拖到微服启动器页面即可安装 |
| `lzc-mesh-macos.dmg` | macOS 12+，Apple Silicon / Intel 通用，打开后把 LZC Mesh 拖到“应用程序” |
| `lzc-mesh-windows.zip` | Windows 10/11 x64，解压后运行 `install.ps1` |
| `lzc-mesh-linux-x86_64.tar.gz` | Linux x86_64，解压后运行 `install.sh` |

LPK 管理页面上的下载链接指向对应版本的 Release。

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

产物输出到 `lzc-mesh-lpk/dist/` 和 `lzc-mesh-client/dist/`（macOS 为 `lzc-mesh-macos.dmg`）。

## 权限

- macOS：需要“辅助功能”和“输入监控”，客户端启动时会主动请求。
- Windows：普通权限即可；控制管理员权限的窗口时需要“以管理员身份运行”，客户端窗口会提示。
- Linux：需要 Hyprland、Sway 等支持 wlroots 协议的 Wayland 合成器。

## 状态

macOS ↔ Windows 已真机验证。Linux 客户端已编译并能连上中枢，输入注入尚未真机验证。

## 许可证

本项目代码使用 PolyForm Noncommercial License 1.0.0 授权，SPDX 标识为 `PolyForm-Noncommercial-1.0.0`。

这意味着本项目源码允许在非商业场景下使用、复制、修改和分发，但不能用于商业目的。禁止将本项目用于商业产品、商业服务、付费交付、商业集成或其他以商业收益为目的的场景。

完整许可证条款见 [LICENSE](./LICENSE)。`lzc-mesh-client/linux/protocols/` 下的 Wayland 协议文件保留其原始许可证声明。
