# hclient-cli 适配安全审查

日期：2026-11-09。

范围：本次 ARM 打包改动、Linux hclient-cli 认证适配，以及适配所调用的
原有 HTTP 认证流程。下文保留初次审查结果，当前修复状态见下一节。

## 合并后定向修复（2026-11-09）

贡献者原提交 `3e4907aa2816ec99b6919adb52c1932ac0a29257` 通过普通 merge
`e963d8756109dcc69ae0792ecf24f3a4460e901c` 保留；后续补丁不改写原作者历史。
本节是本地独立复测，后文原始审查及 ARM 验证作为历史记录保留。

### 三项修正

1. **响应读取上限**：Linux 认证 HTTP 帮助函数改用流式读取，Content-Length
   超限可提前拒绝；未知长度、chunked 和解压后的数据最多累计 1 MiB，
   只多读一个字节检测超限，随即取消并关闭流。本地 CLI RPC、桌面版认证
   及 HTTPS JSON 探测均生效，独立的文件下载流不受此上限影响。
2. **延迟 accept**：保留发送凭证前的 UID 检查，在 Session 的连接等待预算内
   等待服务 accept，替换固定 40 次／约 200 毫秒的等待；生产 Session 为
   8 秒。仍无法验证或 CLI 退出时失败关闭，不允许先发请求再校验。
3. **双栈连接**：IPv4 连接在 tcp 表中找不到服务端套接字时，继续按精确
   IPv4-mapped IPv6 端点在 tcp6 表中核对 UID。没有使用仅匹配端口、跳过
   UID 或只验证监听快照的兼容性捷径。

### 本地验证结果（x86_64 Omarchy）

- 旧代码／新代码同一实际 HTTP 帮助函数对照：500 ms 后 accept 与 IPv4→双栈
  监听，旧代码均拒绝、新代码均成功；16 MiB chunked 响应旧代码完整读入，
  新代码约读到 1 MiB 即中止（本次服务端实际写出 1,064,960 字节）。
- `python3 spec-tests/linux-cli.py`：34/34，通过原有 gRPC-Web／protobuf 回归。
- `LZC_FOREIGN_URL=http://127.0.0.54:53/ python3 spec-tests/linux-auth-security.py`：
  49/49，包含 35 项既有默认测试、13 项新回归和 1 项可选跨 UID 检查。
  外部 UID 使用本机既有的 systemd-resolved TCP 服务，只建连，验证 HTTP
  请求头写出为 0；不修改该服务、不发送真实凭证。
- 新测试覆盖半秒延迟 accept（桌面和 CLI）、未 accept 时 1 秒预算内停止且
  零 HTTP 字节、双栈、Content-Length／chunked 提前取消、恰好 1 MiB、gzip
  解压后超限及 HTTPS。16 MiB 原始响应被提前断开，不以最终拒绝代替读取上限。
- 相同 49 项在 ASan、UBSan、LeakSanitizer 下通过，无报错；启用
  `LZC_TEST_SANITIZERS=1 ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`
  和 `UBSAN_OPTIONS=halt_on_error=1`。
- Linux `make -B -j3` 完整重编译／链接及 `make dist pkg` 通过，无编译告警；
  检查 x86_64 ELF、Arch 包元数据、归档、CLI 文档与许可证。
- 既有 Linux 输入 JSON 序列化、本机输入监视器（主循环心跳、过期扫描、
  热插拔、FD 平衡）及会话 gate／活动分类回归均通过。

本补丁只改 Linux 认证实现、测试和说明，不改共享 Rust 核心、Hub、Mac、
Windows、Web、根目录工具链配置或 REQ/AC 产品合同。没有进行安装、重启、
真实账号登录或跨设备输入操作；没有把本地测试算作 ARM 实机、Mac↔Linux、
六方向实体键鼠或蓝牙硬件验收。不运行三端全量矩阵。

## 原 PR 修复状态（2026-11-09）

原 PR 已完成下文第 1、2、3 项加固：

1. 本地认证使用独立 Session，并在发送函数中再次显式禁用代理。
   不依赖环境里的 `no_proxy`，也不信任调用方原有的代理设置。
2. 认证探测、WebSocket 握手与文件请求均禁止 HTTP 自动重定向。
   HTTP 3xx 按失败处理，不转发 Cookie 或设备令牌。
3. CLI 运行配置和凭证文件检查所有者、私有权限、普通文件类型和链接数，
   使用 `O_NOFOLLOW` 打开并在读取期间限制长度。CLI PID 必须属于当前用户、
   名称匹配且仍存活；用 pidfd 防止进程退出后把复用的 PID 当作原进程。
   本地每次认证建立新连接，检查该实际 TCP 连接的服务端内核 UID 后，
   才在 `SoupMessage::starting` 附加凭证。拒绝其他 UID 或无法核实的连接。

第 3 项的防护边界是**当前用户 UID**，不是密码学意义上的指定程序认证。
本机 CLI 带 capability，普通用户不能读取其 `/proc/<pid>/fd`，因此实现
没有伪称已经证明服务端 FD 必然归指定 PID。进程名可以被同 UID 程序伪装，
同 UID 恶意代码和 root 也本来就可能读取 0600 凭证，均不在此防护范围。
若未来要抵御这些主体，需要 CLI 配合提供认证的 Unix 通道或其他可信
服务端身份机制，不能仅靠客户端单方面增加进程名检查。

原 PR 中第 4 项“完整读入后才检查响应大小”**未修复**；现已由上文的独立补丁解决。

验证：34 项原 RPC 测试通过；新增 36 项安全测试通过，包含一个针对
本机已有 root 监听服务的真实跨 UID 连接检查，确认写出的 HTTP 请求头为 0。
其余测试仅使用临时本机服务和虚构凭证。加固后的真实 CLI 登录和 Mesh
中枢连接成功，沿用原临时设备身份，进入等待批准状态。
同一组 36 项安全测试也在 AddressSanitizer 和 UndefinedBehaviorSanitizer
下通过，未报告越界或未定义行为；未启用 LeakSanitizer。

提交 PR 前再次运行：34 项 RPC 测试和 35 项默认安全测试通过。
真实跨 UID 测试需要显式设置 `LZC_FOREIGN_URL`，本次复跑未启用该可选项。

## 初次审查结论（修复前）

确认两处凭证外发风险，以及两处需要修复的本地信任和资源限制问题。
现有 34 项 RPC 测试仍通过，但没有覆盖系统代理、后续 Cookie GET 的跨主机
重定向、本地服务身份验证和读取过程中的响应大小限制。

初次问题复现仅使用虚构凭证、本机临时 HTTP 服务和临时配置目录；
未读取或发送真实登录凭证，未修改实际 hclient-cli 配置。

## 与上游基线的归因

已对照未修改的上游提交 `93bbfff5782703b10b0f58e1a52ae041d643949c`：

| 问题 | 修改前 | 本次改动 |
| --- | --- | --- |
| HTTP 代理收到本地凭证 | 桌面接口已使用默认代理 Session 发送 `LZC_MISC_AUTH_TOKEN` | 新 CLI 路径复用该 Session，新增 `lzc-shellapi-cred` 的泄露路径 |
| 跨主机跳转携带 Cookie | `get_json()` 已有该问题，旧的 Cookie 认证请求已使用它 | 继续复用；CLI 默认微服也增加了 `/sys/whoami` 校验 |
| 本地 TCP 服务身份未验证 | 桌面路径已经根据配置端口发送凭证，没有验证监听者身份 | 新 `read_cli_api()` 采用相同信任方式，没有核对 CLI 进程与端口归属 |
| 响应完整读入内存 | 旧 `get_json()` 已无流式大小限制 | 新 RPC 也完整读入；本次加入的 1 MiB 后置检查不能限制读取过程 |

因此，不能把四项都归为首次出现的新漏洞，也不能因旧代码有同类问题而
认为新 CLI 路径没有新增风险。新路径中的凭证发送、服务身份校验缺口和
无效的读取上限属于本次实现需要负责修复的内容。

## 1. 高：本机接口凭证可能被发送给 HTTP 代理

位置：`lzc-mesh-client/linux/src/lzc.c` 的 `http()`，
以及 `lzc-mesh-client/linux/src/lzc_cli.c` 的 `rpc()`。

新 RPC 复用了使用默认代理解析器的 SoupSession。URL 限制为回环地址，
并不保证实际连接不会经过 HTTP 代理。

触发条件：配置的 HTTP 代理会处理回环地址，例如设置 `http_proxy`，
且没有通过 `no_proxy` 等配置排除回环地址。

复现：设置一个本机虚构代理，调用实际生产 RPC 函数。代理收到
`lzc-shellapi-cred` 请求头。此结果确认接口凭证能够离开目标本地服务，
如果代理在远端，凭证也会被发送到远端代理。
未进一步使用泄露凭证调用真实服务，未验证具体可接管的权限。

建议：本地 CLI RPC 使用专用且显式禁用代理的 Session。
不能依赖用户设置 `no_proxy`。

## 2. 高：后续 Cookie 请求允许跨主机重定向

位置：`lzc-mesh-client/linux/src/lzc.c` 的 `get_json()`，以及调用它的
`/sys/whoami` 和 Mesh `/api/whoami` 请求。

gRPC POST 已设置 `SOUP_MESSAGE_NO_REDIRECT`，但是 `get_json()` 没有设置。
它直接附加 Cookie 请求头，自动跟随重定向时未限制目标主机。

复现：使用真实 `get_json()` 函数请求本地源服务，令其从 `127.0.0.1`
重定向到另一个端口的 `localhost` 服务。目标服务收到虚构 Cookie，
返回 `{}` 后调用还报告成功。复现使用 HTTP 本地服务，未使用真实 HTTPS
微服或真实 Cookie。

触发条件：所访问的微服、应用或其网关返回跨主机重定向。
这不是允许任意外部攻击者绕过 HTTPS，而是认证响应可将手工附加的
Cookie 引向其他主机。

这是原有 HTTP 帮助函数的问题，本次 CLI 路径也使用它，因而仍受影响。

建议：认证请求禁止自动重定向。若业务确实需要跳转，逐次校验 scheme、
host、port，跨源时禁止转发凭证；登录跳转应按会话失效处理。

## 3. 中：回环 TCP 地址没有验证服务身份

位置：`lzc-mesh-client/linux/src/lzc.c` 的 `read_cli_api()`。

程序检查地址为 IP 回环地址，但没有检查运行信息中的 PID 是否有效，
也没有验证该端口属于当前用户的 hclient-cli 进程。

复现：临时配置中的 PID 使用不存在的值，端口指向普通虚构 HTTP 服务。
程序仍向该服务发送 `shellapi_cred` 文件里的虚构凭证。

触发条件：运行信息过期，或实际服务退出后端口被其他本地进程占用。
在多用户主机上，回环监听端口不天然等于当前用户的受信任服务。
泄露的旧凭证能否用于之后的真实 daemon，取决于 hclient-cli 的凭证
生命周期，本次未验证；不据此断言已经可以接管真实账号。

建议：优先使用能够认证对端身份的本地通道。继续使用 TCP 时，
至少验证当前运行进程和套接字归属；单独核对 PID 存活仍不能替代完整的
对端身份验证。并限制配置及凭证文件的类型、所有者和权限。

## 4. 中：1 MiB 限制在完整响应读入后才生效

位置：`lzc-mesh-client/linux/src/lzc_cli.c` 的 `rpc()`，
`soup_session_send_and_read()` 与后面的大小检查。

`send_and_read()` 先把整个响应读入内存，随后才检查 `size > 1024 * 1024`。
因此这个限制不能约束读取过程中的内存使用。

复现：虚构接口返回 8 MiB 响应，客户端完整接收后才拒绝。
现有“oversized response”测试只验证最终拒绝结果，不能验证读取上限。

触发条件：本地接口、被冒充的本地服务或中间代理返回超大响应。
可能导致内存耗尽或客户端退出；未进行会耗尽真实机器内存的测试。

建议：流式读取并在读取过程中限制累计长度；在超过上限时立刻取消请求、
关闭流。Content-Length 预检查只能作为补充，还需覆盖 chunked 响应。

## 已确认的现有保护

- 本地接口地址拒绝非回环 IP。
- gRPC POST 禁止 HTTP 重定向。
- 解析器检查帧长度、protobuf varint 溢出、无效 UTF-8、重复响应和 RPC 状态。
- 登录 Cookie 拒绝 CR/LF、NUL 和分号等已测试的注入内容。
- 初次检查的真实本机凭证和运行信息文件权限均为当前用户所有的 0600；
  修复前的源码没有主动验证这些属性。

这些保护不消除上面列出的风险。

## 复现材料

仓库中包含可复现的生产代码测试驱动和临时本地服务：

```text
spec-tests/linux-cli.c
spec-tests/linux-cli.py
spec-tests/linux-auth-security.c
spec-tests/linux-auth-security.py
```

驱动直接调用生产源码中的函数，不改写生产逻辑。
测试仅使用虚构凭证；不会打印或发送真实登录凭证。
在仓库根目录运行 `python spec-tests/linux-cli.py` 和
`python spec-tests/linux-auth-security.py`。真实跨 UID 测试仅在设置
`LZC_FOREIGN_URL` 时运行，参见
[命令行客户端接入说明](../lzc-mesh-client/linux/HCLIENT-CLI.md)。

原 PR 落实第 1、2、3 项；合并后的独立补丁补齐第 4 项，并修正延迟 accept 和双栈兼容问题。
本审查不构成对项目全部远控、蓝牙、
文件传输或 Rust 依赖的完整安全审计。
