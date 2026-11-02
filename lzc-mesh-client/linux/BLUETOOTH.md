# 蓝牙连接与微服宿主机桥接

0.2.0 支持 Mac 发布临时 RFCOMM 服务、Linux 主动连接。Mac 使用随模式启停的签名蓝牙辅助应用，避免系统蓝牙初始化等待阻塞 AppKit。主客户端通过自己创建的 0700 临时目录与 0600 FIFO 交换加密字节；停用模式移除目录，辅助应用退出；父进程退出也会停止辅助应用。两端先在系统中配对，再通过已认证的中枢交换证书和蓝牙服务地址。中枢继续负责账号、布局与控制授权；键鼠数据经真实蓝牙传输，使用双向 TLS。选择“仅蓝牙”时不监听局域网 UDP，不回退到中转。

微服 Omarchy 容器处在独立网络命名空间，挂载 `/dev` 并不能启用 AF_BLUETOOTH。`host-bluetooth.py` 在宿主机建立 RFCOMM 连接，通过 `SCM_RIGHTS` 将连接描述符交给客户端。描述符保留宿主网络命名空间，后续数据直接使用蓝牙，没有 IP 转发步骤。

## 当前微服部署

- 宿主程序：`/root/.local/share/lzc-mesh/host-bluetooth.py`。
- root 的**用户级**服务：`~/.config/systemd/user/lzc-mesh-bluetooth.service`。
- 宿主私有目录：`/lzcsys/data/document/ponzs/.lzc-mesh`，容器中为 `/lzcapp/document/.lzc-mesh`，目录权限 0700。
- 私有套接字：`host-bluetooth.sock`，权限 0600。宿主程序核对 `SO_PEERCRED`，只接受已配置的桌面进程 UID。
- 当前启动参数：`--directory /lzcsys/data/document/ponzs/.lzc-mesh --peer-uid 1000`。

root 只打开目录、绑定私有套接字并启动监听，随后清空附加组、降为 UID/GID 65534，再解析请求。不会对用户提供的路径执行 chown，不跟随末级目录符号链接；已有非套接字或不属于启动用户的节点会使启动失败。协议只有能力查询和出站 RFCOMM 连接两种操作，无命令执行、文件访问或输入注入接口，不监听 IP 端口。

这是一项本机安装步骤，LPK 不申请宿主 root 权限，也不自动安装宿主程序。更换账号或容器时，应核对共享目录映射及宿主实际看到的进程 UID，不根据 idmapped 文件所有者推断进程 UID。宿主文件和服务修改应遵守宿主机自己的管理规则。

维护命令在宿主 root SSH 中执行：

```sh
systemctl --user status lzc-mesh-bluetooth.service
systemctl --user restart lzc-mesh-bluetooth.service
systemctl --user disable --now lzc-mesh-bluetooth.service
```

停止桥接后，蓝牙模式显示不可用，局域网和中转模式仍可使用。切换到非蓝牙模式或暂停接力，会撤销蓝牙输入连接与尚未完成的连接请求；宿主桥接保留私有 Unix 监听以便下次启用，它不保留蓝牙监听或连接。

## 普通 Linux 主机

桥接也可以由桌面用户在宿主网络命名空间运行。创建用户自己的 0700 私有目录，传入该用户 UID；非 root 启动不会进行降权。用 `LZC_MESH_BLUETOOTH_SOCKET` 指定客户端应连接的私有套接字路径。脚本仅依赖 Python 标准库，普通 RFCOMM 不需要原始 HCI 或 root 权限。

## 数据顺序与安全边界

光标与键盘共享可靠有序的 RFCOMM 字节链路。客户端在加密前合并尚未发送的相邻位置更新，不跨越点击、拖拽或按键顺序屏障；已加密的 TLS 记录不会被丢弃。日志分别统计位置节拍及键盘确认延迟，不记录输入字符。

蓝牙配对不能替代同账号证书和当前控制授权。模拟键鼠可以操作用户桌面，因此看不到屏幕并不等于无法操作数据。暂停全部接力会撤销授权并释放已按下的键与按钮。蓝牙是否更平滑须通过实际设备测量及人工手感验收确定。
