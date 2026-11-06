# Windows 自身窗口标题栏卡死修复

## 症状与原因

用户确认：Mac 控制 Windows 时，只有操作 LZC Mesh 自身窗口的最小化／关闭按钮会偶发卡死，其他应用正常。

旧路径将原生输入回调和兼容中枢输入都投递到主窗口，再调用 `SendInput`。自身窗口收到标题栏按下后，`DefWindowProc` 进入系统鼠标跟踪循环；稍后到达的松开仍等待主窗口处理自定义消息，不能及时注入。直接发送 `WM_CLOSE` 的旧托盘测试绕过了这个过程。

## 实现

- `windows/src/receiver.cpp`：独立、有界、有序的输入执行线程；会话门控、接收输入、远程按键重复、本机接管释放均不等待 UI。
- `direct.cpp`、`net.cpp`：原生回调、兼容输入、中枢原生授权／续约／中转和可靠确认绕过 UI。状态仍通知界面；已转发状态携带原生代次，避免重复应用或新运行时漏掉初始化状态。发送绑定管理连接代次。
- 接收队列只替换相邻、同会话绝对移动；超限、暂停、撤销、停用、断线使旧队列失效并释放状态。超限在当前管理连接内保持停用，普通刷新不能重新放行。
- 源端捕获与覆盖层保留 UI 所有权；进入接收或结束控制先原子取消捕获，迟到角色通知不能重新启用旧捕获。
- 蓝牙字节交接也绕过 UI，工作线程生命周期和回调代次增加同步保护。未进行蓝牙硬件验收。
- 最小化／零尺寸跳过布局；隐藏窗口不重复布局，恢复时重新布局。关闭仍隐藏到托盘，不退出或停用共享。

这是现有有序输入、控制释放和旧会话隔离的实现修复，不新增或修改 REQ/AC 产品口径。

## 验证

2026-11-05 在现有 Windows 测试主机执行；生产 Windows 客户端保持休眠、无 Windows 参与的控制会话。没有停止、升级、启用该客户端，没有修改账号设置或布局，没有终止其他设备正在进行的控制。交互输入只指向独立测试窗口，安全桌面或失焦时停止。

1. **旧 UI 队列对照**：`windows-caption-os.exe --ui-queue` 在真实标题栏按下后延迟投递松开，复现 `remote button-up did not complete caption command`，退出码 1；测试通过旁路释放完成清理。它模拟旧分发方式，不是声称旧安装包已被替换后复现。
2. **生产输入 OS 组件**：`windows-caption-os.cpp` 链接生产 `input.cpp`、`receiver.cpp`，替代传输和日志，不启动全局钩子。不直接发送 `WM_CLOSE` 或 `WM_SYSCOMMAND`，使用系统输入定位并真实点击标题栏。原生入口和兼容中枢入口各 3 轮最小化／最大化／关闭，共 **18 次**均通过，关闭只隐藏；额外验证按键重复／松开停止、断线释放实际系统按键和鼠标按钮。额外恢复窗口用于重复键测试，报告 `rounds=19` 不是 19 次标题栏动作。
3. **生产接收线程回归**：`windows-receiver.cpp` 不处理 UI 消息，验证按钮／键盘／移动顺序、会话与连接代次拒绝、本机接管、暂停／断线／撤销、重连隔离、兼容 enter-before-state 时序、有界超限和普通刷新不能解除超限。通过。
4. **实际适配层和共享核心**：`windows-direct-receiver.cpp` 链接生产 `direct.cpp`、Rust transport、`receiver.cpp`，不处理 UI 消息且不注入桌面，验证中转授权／进入、延迟按钮松开、按键、可靠确认和暂停释放。通过。
5. 既有 Windows 捕获门控、序列化、有序队列、19,320 像素坐标与小数滚动回归通过。共享 transport **23 项**通过。
6. Windows 主程序、安装器及 ZIP 完整构建通过；旧 `windows-capture-os.cpp` 更新执行线程接线并交叉编译通过，未在本轮重新执行其全局钩子维护测试。

诊断产物：`spec-tests/.run-ac/windows-caption-fix/results.json`、`windows-receiver.log`、`windows-direct-receiver.log`。原始构建与核心回归日志在本机 `/tmp/lzc-mesh-caption-*.log`。

构建入口：

```sh
sh lzc-mesh-client/windows/build.sh
sh spec-tests/build-windows-tests.sh
sh spec-tests/build-windows-receiver-tests.sh
```

`windows-receiver.exe`、`windows-direct-receiver.exe` 无桌面输入，可在 Windows 命令行执行。`windows-caption-os.exe` 只可在已确认上述维护条件的交互用户会话中显式执行；结果写入 `%USERPROFILE%\mesh-parity\caption-os-after.json`，`--ui-queue` 写入 `caption-os-before.json`。失败返回非零；目录须已存在。

## 边界与交付

这些是生产组件的真实 Windows OS／线程／传输适配回归，不是经过真实中枢的 Mac→Windows 完整人工验收。尚未安装修复包，也未进行实体 Mac 鼠标、蓝牙硬件或完整六方向复测。

构建产物：

- `lzc-mesh-client/dist/lzc-mesh-windows-setup.exe`
- `lzc-mesh-client/dist/lzc-mesh-windows.zip`

安装后需用 Mac 真实远控复测自身窗口标题栏、拖动／缩放、托盘恢复、断线及本机接管；保留既有身份、批准、布局与配置。没有在本轮创建版本标签、发布附件或提交代码。
