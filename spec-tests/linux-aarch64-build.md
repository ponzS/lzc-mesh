# Omarchy aarch64 本地构建记录

日期：2026-11-08。

上游基线：`93bbfff5782703b10b0f58e1a52ae041d643949c`（客户端 0.3.4）。

## 环境

- Arch Linux ARM，`aarch64`，内核 `7.2.11-14-1-ARCH`。
- Omarchy / Hyprland 0.56.2。
- Rust 由 Omarchy 的 mise 管理；本次 `rustc` / `cargo` 为 1.99.1，
  host 为 `aarch64-unknown-linux-gnu`。项目 `mise.toml` 选择 stable 通道。
- GTK 4.24.1、libsoup 3.8.0、json-glib 1.10.8、Wayland 1.26.0、
  libxkbcommon 1.13.2。

## 修改

- Linux PKGBUILD 的架构列表加入 `aarch64`。
- `make dist` 使用本机架构命名压缩包。
- `make pkg` 使用本机架构复制产物，并保留 makepkg 的压缩格式。
  本机 `/etc/makepkg.conf` 的 `PKGEXT` 为 `.pkg.tar.xz`，
  原来固定复制 `.pkg.tar.zst` 会失败。
- 输入、GTK 和 Rust 传输库无需修改源码即可完成本机 ARM 构建。

## 复现

在仓库根目录运行：

```sh
mise install
mise exec -- make -C lzc-mesh-client/linux -j4
mise exec -- make -C lzc-mesh-client/linux dist
mise exec -- make -C lzc-mesh-client/linux pkg
mise exec -- cargo test --manifest-path lzc-mesh-client/transport/Cargo.toml --locked --release
```

GTK、libsoup、json-glib、Wayland、xkbcommon、C 编译器、pkgconf 和 makepkg
需要由系统提供。`make pkg` 使用项目原有的 `--nodeps` 选项，
所以可以使用 mise 的 Rust，无需额外安装 pacman 的 rust 包。

## 已验证

- 所有 C 源文件和 Rust 静态库编译、链接成功。
- `file` 确认程序为 ARM aarch64 ELF；`ldd` 未发现缺失的共享库。
- Rust 传输层现有测试：26 通过、0 失败，包括本机真实 QUIC 通信。
- `spec-tests/linux-input-json.c`：通过。
- `spec-tests/linux-local-input.c`：通过，主循环心跳最大间隔 14.275 ms，
  设备扫描取消、热插拔与描述符释放检查通过。
- 当前 Hyprland 公布全部 8 个相关协议：layer-shell、virtual-pointer、
  virtual-keyboard、pointer-constraints、relative-pointer、xdg-output、
  data-control、keyboard-shortcuts-inhibit。
- 使用 `/tmp/lzc-mesh-arm-smoke/` 下的临时 XDG 配置启动客户端 15 秒，
  未提前退出，日志确认窗口建立并完成配置协商，且报告：

  ```text
  [status] input ok: Wayland 输入就绪（Hyprland：虚拟指针、虚拟键盘、边缘检测、指针锁定）
  ```

- 压缩包与 Arch 包生成成功；`pacman -Qip` 确认包架构为 aarch64，
  并检查了包内二进制、桌面入口、图标和文档路径。

产物位于 `lzc-mesh-client/dist/`：

```text
lzc-mesh-linux-aarch64.tar.gz
lzc-mesh-linux-aarch64.pkg.tar.xz
```

## 当前登录依赖

原客户端固定读取 `~/.config/lzc-client-desktop/core.cfg`：

```json
{
  "http_port": 12345,
  "extra_headers": {
    "LZC_MISC_AUTH_TOKEN": "<本机桌面客户端的接口令牌>"
  }
}
```

然后向 `http://127.0.0.1:<http_port>` 发送带 `LZC_MISC_AUTH_TOKEN` 请求头的 GET：

1. `/default-box`：读取 `boxName`、`boxDomain`、`loginUser` 和 `status`；
   `status == 2` 才继续获取登录会话。
2. `/box-ext-info?url=<URL 编码后的 https://微服域名/>`：
   从 `cookies` 数组中取 `name == "Lzc-Auth-Token"` 的 `value`。

程序随后使用该 Cookie 访问 `https://lzc-mesh.<boxDomain>/api/whoami`
并连接 `wss://lzc-mesh.<boxDomain>/api/ws`。
手动指定微服时还会校验 `https://<boxDomain>/sys/whoami`。

本机没有桌面版 `core.cfg`，但已安装并运行 `hclient-cli` 1.5.1，
命令行显示默认微服为 Ready，TUN 和代理均开启。
其状态文件在 `~/.config/hportal-client/`，使用的配置结构不同。
本机初步探测未发现兼容的 GET `/default-box` 接口：
`shellapi_address` 返回 HTTP 405，控制地址重置 HTTP 连接，
代理地址返回 HTTP 500。此结果只说明不能直接把这些端口当作原桌面接口。

不能直接复制另一台电脑的 `core.cfg` 使用：原程序固定连接当前机器的
`127.0.0.1`，另一台电脑的端口和接口令牌不对应本机服务。

## 未验证

尚未安装本次产物到系统。未完成登录中枢后的设备批准、跨设备实体键鼠接力、
剪贴板、文件传输及蓝牙硬件验证。
本次结果确认 aarch64 原生构建、打包和本地 Wayland 初始化可行，
不代表已完成全部功能验收。

## 后续 hclient-cli 适配（2026-11-09）

已加入原生 hclient-cli 认证路径，并通过本机实际连接验证：
微服发现、登录会话、Mesh 中枢 0.2.3 的 WebSocket 连接和设备注册均成功，
测试设备进入等待批准状态。34 项新增 RPC 回归测试通过。
详情和使用限制见 [命令行客户端接入说明](../lzc-mesh-client/linux/HCLIENT-CLI.md)。

同日完成认证安全加固：本地代理绕过、认证请求禁止重定向、CLI 进程和
实际 TCP 对端 UID 校验、私有配置文件检查。原 34 项 RPC 测试和新增
36 项安全测试通过；加固后再次完成真实微服登录与中枢连接。
第 4 项响应流式大小限制未在本轮修改，详见
[安全审查及修复状态](linux-cli-security-review.md)。
