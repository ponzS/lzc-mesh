# 懒猫设备接力（LZC Mesh）

简体中文 | [English](README.en.md)

用一套键盘和触控板/鼠标控制面前的多台电脑（macOS、Windows、Linux）。只同步光标和键盘，不传画面。还支持共享剪贴板（文本，默认开启）和跨设备文件共享。设备之间通过懒猫网络通信。

## 目录

| 路径 | 内容 |
| --- | --- |
| `lzc-mesh-lpk/` | 懒猫微服应用：Rust 中枢（布局存储、贴边、输入转发、剪贴板转发、文件存储）和管理页面 |
| `lzc-mesh-client/macos/` | macOS 客户端（Swift / AppKit，CGEventTap + CGEvent） |
| `lzc-mesh-client/windows/` | Windows 客户端（C++ / Win32，低级钩子 + SendInput，WinHTTP） |
| `lzc-mesh-client/linux/` | Linux 客户端（C / GTK4，wlroots 虚拟指针、虚拟键盘、layer-shell） |

## 工作方式

- 中枢运行在 LPK 里，地址 `https://lzc-mesh.<微服>.heiyu.space`，按懒猫账号隔离设备组，布局保存在 `/lzcapp/var/layouts`。
- 客户端通过懒猫网络连接中枢的 WebSocket `/api/ws`。
- 光标推到有相邻屏幕的边缘时，本机光标被锁住隐藏，移动量和按键转发给中枢，中枢按布局把光标交给相邻设备。按键在线路上统一使用 Linux evdev 键码。
- 控制其他设备时按 Ctrl+Alt+Esc（Mac 上是 Ctrl+Option+Esc）立即收回。
- 共享剪贴板默认开启，可在管理页面“设置”或客户端托盘菜单中关闭。只同步文本，最大 256 KB；中枢只在同一账号的在线设备间转发，不保存内容。密码管理器标记为隐私的内容不会同步。
- 文件共享：在管理页面或任一客户端窗口上传，同一账号的所有设备都能下载。文件保存在微服的 `/lzcapp/var/files`，单个文件最大 4 GB。

## 输入通道与急停

管理页面与 Mac、Linux 0.2.0 本地客户端的“设置”可切换自动（局域网优先）、仅蓝牙、仅局域网直连或仅中枢转发，设置同步到同账号设备。局域网使用双向证书认证的 QUIC；蓝牙使用 RFCOMM 与双向 TLS 认证。光标和键盘分别显示状态和性能统计，但会共享底层链路。仅蓝牙和仅局域网不可达时停止控制，不会静默回退。仅中转关闭本应用的蓝牙服务、连接、UDP 监听和探测。旧客户端继续通过中枢使用。

蓝牙为实验功能，采用 Mac 服务端与 Linux 连接端，需要先完成系统蓝牙配对。Mac 的蓝牙初始化仍有启动上下文相关的待核实项，详见[测试记录](spec-tests/bluetooth-deployment-2026-11-02.md)。微服中的 Omarchy 容器通过宿主机上的私有 Unix 套接字取得 RFCOMM 描述符，不经过 IP 转发。宿主机桥接只负责建立连接，启动后降权，不执行命令、不读文件、不注入输入；配置见 [蓝牙桥接说明](lzc-mesh-client/linux/BLUETOOTH.md)。

“暂停全部设备接力”会结束账号下当前的键鼠控制、释放按键和按钮、关闭新版客户端的直连入口并禁止新控制；管理连接保留，恢复后再次跨屏即可控制。该设置与通道模式都会保存在中枢。

直连不传屏幕画面，客户端以普通用户身份运行，但模拟键鼠仍可操作该用户的桌面和数据。认证和控制会话是安全边界，不能依赖“看不到画面”限制输入控制。户外使用时可选择仅中转，关闭局域网直连入口。

## 下载

安装包在 [GitHub Releases](https://github.com/ponzS/lzc-mesh/releases)。每个平台都有安装包和压缩包，任选其一：

| 平台 | 安装包 | 压缩包 |
| --- | --- | --- |
| 懒猫微服 | `cloud.lazycat.app.lzc-mesh-v<版本>.lpk`，拖到微服启动器页面安装 | — |
| macOS 12+（Apple Silicon / Intel） | `lzc-mesh-macos.dmg`，打开后把 LZC Mesh 拖到“应用程序” | `lzc-mesh-macos.zip`，解压后把 `LZC Mesh.app` 移到“应用程序” |
| Windows 10/11 x64 | `lzc-mesh-windows-setup.exe`，双击安装，可在“设置 › 应用”中卸载 | `lzc-mesh-windows.zip`，解压后右键 `install.ps1` → 使用 PowerShell 运行 |
| Linux x86_64 | `lzc-mesh-linux-x86_64.pkg.tar.zst`（Arch），`sudo pacman -U` 安装 | `lzc-mesh-linux-x86_64.tar.gz`，解压后运行 `./install.sh` |

Windows 和 Linux 的安装都不需要管理员权限（Arch 包除外）。LPK 管理页面上的下载链接指向对应版本的 Release。

## 构建

```sh
# LPK（Linux x86_64，需要 Rust 的 x86_64-unknown-linux-musl target 和 lzc-cli）
cd lzc-mesh-lpk && sh build.sh && lzc-cli project release

# macOS（在 Mac 上，需要 Xcode 命令行工具和 Rust 的 aarch64/x86_64-apple-darwin target）
cd lzc-mesh-client/macos && ./build.sh

# Windows（在 Linux 上用 zig 交叉编译，生成 setup.exe 和 zip）
cd lzc-mesh-client/windows && ./build.sh

# Linux（需要 Rust，以及 gtk4、libsoup-3.0、json-glib、wayland-client、xkbcommon）
cd lzc-mesh-client/linux && make && make dist   # tar.gz
cd lzc-mesh-client/linux && make pkg            # Arch 包，需要 makepkg
```

产物输出到 `lzc-mesh-lpk/dist/` 和 `lzc-mesh-client/dist/`。macOS 的 `build.sh` 同时生成 dmg 和 zip。

## 权限

- macOS：需要“辅助功能”和“输入监控”，客户端启动时会主动请求。蓝牙模式还需要系统蓝牙授权及配对。
- Windows：普通权限即可；控制管理员权限的窗口时需要“以管理员身份运行”，客户端窗口会提示。
- Linux：需要 Hyprland、Sway 等支持 wlroots 协议的 Wayland 合成器。

## 状态

0.2.0 的接力与光标改进已获用户手动验收，反馈明显更顺滑。该反馈未逐项指定传输模式；蓝牙的自动诊断边界仍保留在测试记录中。

macOS ↔ Windows 已真机验证光标跨屏、键盘、长按连发、共享剪贴板、文件共享和布局同步。Mac → Linux 已真机验证光标跨屏、触控板连续滚动和惯性滚动；0.1.3 修复了滚动类型误判及滚动阶段丢失。Linux 的其他输入、剪贴板和文件面板尚未完整真机验证。

## 许可证

本项目代码使用 PolyForm Noncommercial License 1.0.0 授权，SPDX 标识为 `PolyForm-Noncommercial-1.0.0`。

这意味着本项目源码允许在非商业场景下使用、复制、修改和分发，但不能用于商业目的。禁止将本项目用于商业产品、商业服务、付费交付、商业集成或其他以商业收益为目的的场景。

完整许可证条款见 [LICENSE](./LICENSE)。`lzc-mesh-client/linux/protocols/` 下的 Wayland 协议文件保留其原始许可证声明。
