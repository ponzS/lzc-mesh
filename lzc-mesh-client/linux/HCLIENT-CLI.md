# 使用 hclient-cli 登录 Linux 客户端

Linux 客户端可以使用已登录的懒猫桌面客户端或 `hclient-cli`。
不需要在 ARM 机器上安装桌面版，也不需要复制另一台电脑的 `core.cfg`。

## 使用

1. 启动并登录本机 `hclient-cli`，通过 `hclient-cli box list` 确认目标微服为 Ready。
2. 确认本机能通过懒猫网络访问微服 HTTPS 域名。本次验证使用 TUN；
   仅开启 HTTP/SOCKS 代理的环境未验证。
3. 启动 `lzc-mesh`。没有桌面版配置时，程序自动识别 `hclient-cli`，
   状态页会显示 `hclient-cli 运行中`、登录会话和中枢连接状态。
4. 首次使用时，在 Mesh 管理页面核对客户端显示的完整公钥指纹并批准设备。

默认使用命令行客户端的默认微服，也可在 Mesh 设置中指定已经登录的微服名称。
指定的微服必须出现在 `hclient-cli` 的列表里；没有匹配项时会明确报错。

如果 `~/.config/lzc-client-desktop/core.cfg` 存在，仍优先使用桌面版。
本次适配不会自动覆盖已有配置或替换已有的设备凭证。

## 本地接口

支持的运行信息路径：

```text
~/.config/hportal-client/hclient-cli-daemon.json
~/.config/hportal-client/shellapi_cred
```

从运行信息的 `shellapi_address` 读取 gRPC-Web HTTP 地址，
使用 `shellapi_cred` 文件内容作为 `lzc-shellapi-cred` 请求头，调用：

```text
POST /space.heiyu.hportal.shell.ShellCore/QueryBoxList
POST /space.heiyu.hportal.shell.ShellCore/QueryLzcAuthToken
```

两个请求均为 `google.protobuf.Empty`，Content-Type 为
`application/grpc-web+proto`。
微服列表提供域名、登录用户、连接状态和默认标记；认证 RPC 提供
`Lzc-Auth-Token`。客户端随后校验微服 `/sys/whoami` 和 Mesh `/api/whoami`，
并连接原有 WebSocket 中枢。

本地凭证接口仅接受 IP 回环地址，显式禁用代理，并禁止 HTTP 重定向。
微服认证、中枢 WebSocket 和文件请求也禁止自动重定向，避免把 Cookie
或设备令牌转发到其他主机。

CLI 的运行配置和凭证必须是当前用户所有、其他用户不可读写的普通文件
（建议权限 0600），不接受符号链接、硬链接、FIFO 或超限文件。
运行信息必须包含仍存活的 `hclient-cli` PID，程序使用 pidfd 固定进程实例。

每个本地认证请求建立新 TCP 连接，通过 Linux `/proc/self/net/tcp{,6}`
核对该连接的**服务端已建立套接字**属于当前用户后，才附加本机凭证。
校验发生在连接建立之后，避免仅检查旧监听端口留下的端口替换窗口。
对端尚未 accept 时会短暂等待；无法校验时停止认证。
此检查不需要 root，但要求能读取 Linux 的 TCP 状态表及 CLI 的进程状态。

该防护以当前用户 UID 为本地信任边界。它不能防御同一用户下的恶意代码
或 root；这些主体本就可能读取私有凭证文件。带 capability 的 CLI
可能禁止读取 `/proc/<pid>/fd`，因此这里不声称已经证明每个服务端 FD
必然属于指定 PID，也不将进程名检查当作密码学认证。

凭证只在内存中用于认证，不写入 Mesh 配置或日志。
自定义 hclient-cli 配置目录暂未支持。

## 验证范围

2026-11-09，在 aarch64 Arch Linux ARM / Omarchy 上验证：

- `hclient-cli` 1.5.1，已登录微服，TUN 和代理开启。
- 本地客户端 0.3.4，实际 Mesh 中枢 0.2.3。
- 微服发现、获取登录会话、微服和应用认证、WebSocket 连接、
  设备注册并进入等待批准状态。
- 34 项本地 RPC 回归测试通过，包括默认和指定微服、非管理员默认微服、
  认证拒绝、无效帧、异常 protobuf、Cookie 注入和重定向。
- 安全加固后新增 36 项凭证边界测试通过：环境与显式 Session 代理、
  HTTP/HTTPS 跨主机和降级跳转、文件权限和类型、PID 失效及进程退出、
  IPv6 和真实外部 UID 套接字拒绝。跨 UID 测试只建连，确认 HTTP 请求头
  写出次数为 0；使用本机已有 root 服务，不修改服务。

在仓库根目录运行回归测试：

```sh
python spec-tests/linux-cli.py
python spec-tests/linux-auth-security.py
# 可选：使用已知属于其他 UID 的回环测试端口，验证 HTTP 发送前拦截。
# LZC_FOREIGN_URL=http://127.0.0.1:<port>/ python spec-tests/linux-auth-security.py
```

此次未批准测试设备，未进行跨设备键鼠、剪贴板、文件或蓝牙实测。
这也不代表所有 hclient-cli 或中枢版本均已兼容。
