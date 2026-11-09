# 懒猫设备接力（LZC Mesh）

简体中文 | [English](README.en.md)

用一套键盘和触控板/鼠标控制面前的多台电脑（macOS、Windows、Omarchy）。只同步光标和键盘，不传画面。还支持共享剪贴板（文本，默认开启）和跨设备文件共享。设备之间通过懒猫网络通信。

**Linux 暂时仅支持 Omarchy（Hyprland），其他 Linux 发行版、桌面环境及 Wayland 合成器不在支持范围内。** 下文中的 Linux 客户端均指 Omarchy 客户端。

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
- 光标推到相邻屏幕边缘时，向中枢申请控制权；直连可用时，光标位置和键鼠动作由客户端之间直接传输，不逐条等待中枢。自动模式在直连不可达时使用中转。中枢持续负责设备信任、布局、控制授权与急停；按键在线路上统一使用 Linux evdev 键码。
- 控制其他设备时按 Ctrl+Alt+Esc（Mac 上是 Ctrl+Option+Esc）立即收回。
- 共享剪贴板默认开启，可在管理页面“设置”或客户端托盘菜单中关闭。只同步文本，最大 256 KB；中枢只在同一账号的在线设备间转发，不保存内容。密码管理器标记为隐私的内容不会同步。
- 文件共享：在管理页面或已批准且运行中的客户端上传，同账号已批准且运行中的设备可以下载。文件保存在微服的 `/lzcapp/var/files`，单个文件最大 4 GB。

## 手机键盘触控板

LPK 首页点击“键盘触控板”，页面空闲且存在已批准、在线并开启接力的设备时自动开始控制，优先沿用记住的目标，也可手动停止或切换目标；不抢占已有控制会话。整屏采用妙控板式手势：单指移动、轻点左键、左右边缘单指上下自然滚动与加速、双指轻点右键、双指自然滚动与惯性、三指拖拽，也支持轻点两下后滑动拖拽，抬手松开。不再提供拖拽按钮或长按右键。手机网页无法复现实体按压、Force Touch 和触觉反馈；三指拖拽对应 Mac 中开启该辅助功能后的习惯，不等于所有 macOS 默认手势。光标跨过共边会按现有布局接力到相邻设备，底栏可手动切换目标。

底栏复用 LightOS 远程桌面的两行通用快捷键与系统键盘开关，不传画面。英文和快捷键直接发送按键；手机输入法提交的中文等文本需要开启共享剪贴板，发送到当前目标后按提示手动粘贴（默认设置下点 Ctrl+V，Mac 会按设置映射 Command）。返回首页、切后台、失焦或断线会释放当前输入；未手动停止时，页面恢复可用后可自动开始新的控制，不回放旧输入。

手机走中枢转发，请选择“自动”或“仅中枢转发”；不会绕过“仅直连/仅蓝牙”的设置。它是临时管理控制端，不在屏幕布局里创建设备，不抢占其他控制端。首页及控制页适配懒猫沉浸模式和宿主安全区。无需为此更新已经支持设备凭证的电脑客户端。

## 输入通道与急停

管理页面与配套更新后的 Mac、Linux、Windows 本地客户端的“设置”可切换自动（局域网优先）、仅蓝牙、仅局域网直连或仅中枢转发，设置同步到同账号设备。局域网使用双向证书认证的 QUIC；蓝牙使用 RFCOMM 与双向 TLS 认证。光标和键盘分别显示状态和性能统计，但会共享底层链路。仅蓝牙和仅局域网不可达时停止控制，不会静默回退。仅中转关闭本应用的蓝牙服务、连接、UDP 监听和探测。启用设备凭证要求后，无凭证的旧客户端必须升级并获批准，不能通过中转绕过授权。

蓝牙为实验功能，Mac 发布服务，Linux 主动连接；Windows 可连接 Mac，也可发布服务供 Linux 连接，需要先完成系统蓝牙配对。没有可用适配器时明确提示不可用，严格蓝牙模式不会回退网络。Mac 的蓝牙初始化仍有启动上下文相关的待核实项，详见[测试记录](spec-tests/bluetooth-deployment-2026-11-02.md)。微服中的 Omarchy 容器通过宿主机上的私有 Unix 套接字取得 RFCOMM 描述符，不经过 IP 转发。宿主机桥接只负责建立连接，启动后降权，不执行命令、不读文件、不注入输入；配置见 [蓝牙桥接说明](lzc-mesh-client/linux/BLUETOOTH.md)。

“暂停全部设备接力”会结束账号下当前的键鼠控制、释放按键和按钮、关闭新版客户端的直连入口并禁止新控制；管理连接保留，恢复后再次跨屏即可控制。该设置与通道模式都会保存在中枢。

直连不传屏幕画面，客户端以普通用户身份运行，但模拟键鼠仍可操作该用户的桌面和数据。认证和控制会话是安全边界，不能依赖“看不到画面”限制输入控制。户外使用时可选择仅中转，关闭局域网直连入口。

## 设备信任与安全

每台客户端持久保存设备私钥。首次接入（包括已有设备升级）需在管理页核对本机显示的完整公钥指纹并批准；之后重连与重启保留信任、设备 ID 和布局。未批准设备不参与共享，不收到对端证书或自动共享数据。撤销信任与客户端运行开关独立，重新开启客户端不能绕过撤销。私钥分别由 macOS Keychain、Windows 当前用户 DPAPI、Omarchy 当前用户专属 0600 文件保护；凭证损坏时停止共享，不自动替换。

身份签名在连接注册时完成，输入继续使用 QUIC/TLS 加密与本地会话、防重放校验，不逐事件请求中枢鉴权。Windows 中枢发送使用独立的有界后台队列，不在输入注入线程同步等待网络。

账号管理网页登录会话有批准设备和管理文件的权限，必须妥善保护；设备绑定不能防止管理会话被盗，也不能保证合法远控无法操作已提权终端。客户端不自动提权，不新增远程命令接口。协议与边界见[设备信任说明](spec/connection/device-trust/protocol.md)。新增能力需要配套更新后的三端与中枢；旧 Release 包不会自动获得设备凭证能力。

## 单台设备开关与休眠

管理页面的每台设备都有“客户端运行”图形开关，本机设置和托盘也可切换。关闭后停止该设备的键鼠、双向剪贴板、文件收发和所有直连，取消进行中的传输；已有共享文件保留，其他设备继续工作。后台管理连接保留，便于远程开启。状态由中枢保存，跨重连和重启保持，唤醒不补发旧输入或关闭期间的剪贴板，也不恢复已取消的文件任务。

这与单独的“键鼠共享”暂停、账号级“暂停全部键鼠接力”不同：后两者只暂停键鼠，不停用剪贴板和文件。完整休眠需要配套升级三端客户端；旧发布包不因此自动获得新能力。真正退出进程、关机或断网后，LPK 不能启动进程，只能保存下次上线时的选择。

## 下载

安装包在 [GitHub Releases](https://github.com/ponzS/lzc-mesh/releases)。每个平台都有安装包和压缩包，任选其一：

| 平台 | 安装包 | 压缩包 |
| --- | --- | --- |
| 懒猫微服 | `cloud.lazycat.app.lzc-mesh-v<版本>.lpk`，拖到微服启动器页面安装 | — |
| macOS 12+（Apple Silicon / Intel） | `lzc-mesh-macos.dmg`，打开后把 LZC Mesh 拖到“应用程序” | `lzc-mesh-macos.zip`，解压后把 `LZC Mesh.app` 移到“应用程序” |
| Windows 10/11 x64 | `lzc-mesh-windows-setup.exe`，双击安装，可在“设置 › 应用”中卸载 | `lzc-mesh-windows.zip`，解压后右键 `install.ps1` → 使用 PowerShell 运行 |
| Linux x86_64（仅 Omarchy） | `lzc-mesh-linux-x86_64.pkg.tar.zst`（Arch），`sudo pacman -U` 安装 | `lzc-mesh-linux-x86_64.tar.gz`，解压后运行 `./install.sh` |

Windows 和 Linux 的安装都不需要管理员权限（Arch 包除外）。LPK 管理页面上的下载链接指向对应版本的 Release。开发构建的设备凭证机制需要配套升级，旧 Release 包不支持该机制。

## 构建

```sh
# LPK（Linux x86_64，需要 Rust 的 x86_64-unknown-linux-musl target 和 lzc-cli）
cd lzc-mesh-lpk && sh build.sh && lzc-cli project release

# macOS（在 Mac 上，需要 Xcode 命令行工具和 Rust 的 aarch64/x86_64-apple-darwin target）
cd lzc-mesh-client/macos && ./build.sh

# Windows（需要 Rust 的 x86_64-pc-windows-gnu target 和 zig，生成 setup.exe 和 zip）
cd lzc-mesh-client/windows && ./build.sh

# Omarchy（Linux 客户端唯一支持的环境；需要 Rust、gtk4、libsoup-3.0、json-glib、wayland-client、xkbcommon）
cd lzc-mesh-client/linux && make && make dist   # tar.gz
cd lzc-mesh-client/linux && make pkg            # Arch 包，需要 makepkg
```

Linux 客户端支持在 `x86_64` 和 `aarch64` Omarchy 上原生构建，`make dist` 按本机架构命名压缩包，`make pkg` 保留 makepkg 配置的压缩格式。aarch64 的本地构建及验证记录见 [ARM Linux 构建记录](spec-tests/linux-aarch64-build.md)；上面的下载表仍对应上游已发布的安装包。

Linux 客户端也支持通过已登录的 `hclient-cli` 获取微服信息和登录会话，没有桌面版配置时自动启用。已验证 hclient-cli 1.5.1 的 TUN 环境，详见 [命令行客户端接入说明](lzc-mesh-client/linux/HCLIENT-CLI.md)。

产物输出到 `lzc-mesh-lpk/dist/` 和 `lzc-mesh-client/dist/`。macOS 的 `build.sh` 同时生成 dmg 和 zip。

## 权限

- macOS：需要“辅助功能”和“输入监控”，开启已批准客户端的共享功能时会请求缺失授权。蓝牙模式还需要系统蓝牙授权及配对。
- Windows：普通权限即可；控制管理员权限的窗口时需要“以管理员身份运行”，客户端窗口会提示。
- Linux：仅支持 Omarchy 自带的 Hyprland 环境；不支持 Sway、其他桌面环境或其他发行版。

## 状态

0.3.5 合入 [PR #4](https://github.com/ponzS/lzc-mesh/pull/4)，支持 Linux aarch64 原生构建、按架构及实际压缩格式打包，以及没有桌面版配置时通过已登录的 hclient-cli 获取微服与会话。加固 Linux 认证凭证发送，限制认证响应的读取大小，并修复延迟 accept 和 IPv4／IPv6 双栈服务的身份校验兼容性。安装包随 [v0.3.5 Release](https://github.com/ponzS/lzc-mesh/releases/tag/v0.3.5) 分发，Linux 附件仍为 x86_64，aarch64 可按[构建说明](spec-tests/linux-aarch64-build.md)原生构建；Linux 仍仅支持 Omarchy。详见 [CLI 接入说明](lzc-mesh-client/linux/HCLIENT-CLI.md)和[安全修复记录](spec-tests/linux-cli-security-review.md)。

0.3.4 合入 [PR #1](https://github.com/ponzS/lzc-mesh/pull/1)，新增默认开启的“光标共享，自动居中”账号设置，Mac、Windows 和管理页面均可切换，Omarchy 通过共享核心生效（本地未增加开关）。开启时跨屏、第三屏交接及跨回源屏落在目标中央；关闭时保留原有共边比例入口。修正中枢转发和原生路径的累计坐标，避免下一次移动或点击回跳，保留 Windows 独立接收线程及既有位移、滚动和急停规则。安装包随 [v0.3.4 Release](https://github.com/ponzS/lzc-mesh/releases/tag/v0.3.4) 分发；协议、组件和构建验证不代表安装后的实体键鼠或蓝牙硬件验收，详见[集成验证](spec-tests/cursor-auto-center.md)。

0.3.3 修复 Mac 远控操作 Windows 客户端自身标题栏时的偶发卡死：接收输入、鼠标松开、按键重复和接管释放不再等待主窗口消息循环，最小化跳过无效布局；关闭仍隐藏到托盘。生产输入组件在 Windows 实机上完成 18 次标题栏操作及断线释放验证，详见[修复记录](spec-tests/windows-caption-stall.md)。同时包含 0.3.2 的手机键盘触控板与沉浸模式，以及后续合入的边缘单指滚动、控制页自动开始和 iOS PWA 底部安全区调整，统一 LPK 和三端版本并更新下载链接；安装包随 [v0.3.3 Release](https://github.com/ponzS/lzc-mesh/releases/tag/v0.3.3) 分发，不代表已完成安装后 Mac→Windows 人工复测或蓝牙硬件验收。

0.2.3 修复 Mac 触控板控制 Omarchy 时的周期性卡顿：实体键鼠接管监视器的设备扫描、能力查询和关闭移到后台，旧扫描结果不能污染新接收会话。用户更新后反馈“现在丝滑多了”。保留原位移、滚动及实体接管规则；Linux 暂时仅支持 Omarchy。详见[接收侧阻塞修复记录](spec-tests/linux-receiver-stall.md)。安装包现已在对应 GitHub Release 补齐；商店更新状态另计，此手感确认不代表六方向全部实体键鼠或蓝牙验收完成。

0.2.2 修复 Windows 实体鼠标跨入 Mac 后偶发固定左下角或不可见的问题：入口定位确认后再捕获，按实际光标计算位移，兼容当前 Windows 输入栈的 32 位事件标记。用户已确认本次 Windows ↔ Mac 手动复测正常。另加强三端角色交接、实体输入接管和旧会话隔离，修复 Omarchy 键盘／鼠标按钮的整数编码；保留原 Mac／Omarchy 位移和滚动算法。Linux 暂时仅支持 Omarchy。六方向完整实体键鼠与蓝牙硬件验收仍未全部完成，详见[验证记录](spec-tests/input-handoff-status.md)。

0.2.1 包含 Windows 多通道能力对齐、中枢后台发送隔离、三端客户端休眠及设备凭证首次批准/撤销。本轮部署后的功能测试已获用户确认通过；Linux 客户端仅支持 Omarchy。此确认不扩大为未执行的蓝牙硬件专项验收。

0.2.0 的接力与光标改进已获用户手动验收，反馈明显更顺滑。该反馈未逐项指定传输模式；蓝牙的自动诊断边界仍保留在测试记录中。

macOS ↔ Windows 已真机验证光标跨屏、键盘、长按连发、共享剪贴板、文件共享和布局同步。Mac → Linux 已真机验证光标跨屏、触控板连续滚动和惯性滚动；0.1.3 修复了滚动类型误判及滚动阶段丢失。Linux 的其他输入、剪贴板和文件面板尚未完整真机验证。

## 许可证

本项目代码使用 PolyForm Noncommercial License 1.0.0 授权，SPDX 标识为 `PolyForm-Noncommercial-1.0.0`。

这意味着本项目源码允许在非商业场景下使用、复制、修改和分发，但不能用于商业目的。禁止将本项目用于商业产品、商业服务、付费交付、商业集成或其他以商业收益为目的的场景。

完整许可证条款见 [LICENSE](./LICENSE)。`lzc-mesh-client/linux/protocols/` 下的 Wayland 协议文件保留其原始许可证声明。
