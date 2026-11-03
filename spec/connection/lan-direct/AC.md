# 场景 1：自动使用经过认证的局域网连接
ID: SC-AUTOMATIC-LAN-DIRECT
Profile: draft
Gate: required
Given 同一账号的新版客户端在线且局域网可达，包括 Windows 与 Mac 或 Linux 的组合
When 用户将光标移入相邻设备并移动、点击、拖拽和滚动
Then 输入优先直连，客户端显示直连状态，动作和位置保持一致
And 非当前控制会话或未认证设备的输入不会注入桌面

# 场景 2：移动与动作在抖动后保持正确
ID: SC-BOUNDED-MOTION-BACKLOG
Profile: draft
Gate: required
Given 用户已通过设备接力控制另一台设备
When 输入通道短暂延迟或部分移动更新丢失后恢复
Then 接收端恢复到最新累计位置，不逐条回放过期移动
And 点击和拖拽释放发生在对应的位置，不丢失或重复按钮动作

# 场景 3：直连和中转自动切换
ID: SC-AUTOMATIC-RELAY-FAILOVER
Profile: draft
Gate: required
Given 用户正在直连控制另一台设备且中枢连接正常
When 直连中断后恢复
Then 输入自动通过中枢继续并在直连恢复后重新使用直连
And 按键、按钮和滚动阶段不因路径切换重复执行

# 场景 4：跨屏与收回保持一致
ID: SC-LOCAL-ROUTING-AND-RELEASE
Profile: draft
Gate: required
Given 已批准的直连客户端与使用兼容输入格式的客户端按屏幕布局排列
When 用户跨屏移动、按紧急收回快捷键或断开控制会话
Then 控制权与屏幕归属一致，已批准客户端仍可通过中枢的兼容输入格式使用
And 会话结束后没有遗留按键、按钮或滚动状态


# 场景 5：仅中转关闭局域网入口
ID: SC-RELAY-MODE-CLOSES-LISTENERS
Profile: draft
Gate: required
Given 同一账号的新版客户端已建立局域网直连
When 用户在管理页面切换到仅中枢转发
Then 所有在线新版客户端关闭直连连接、停止探测并释放 UDP 监听端口
And 输入改走中转，模式在重连或重启后继续生效

# 场景 6：总开关撤销全部控制
ID: SC-GLOBAL-INPUT-PAUSE
Profile: draft
Gate: required
Given 账号中存在正在控制或受控的客户端
When 用户在管理页面暂停全部设备接力
Then 当前控制结束、按键和按钮释放、直连监听关闭，新控制会话被禁止
And 管理连接保持可用，用户恢复接力后可重新跨屏控制


# 场景 7：光标和键盘通道可分别观察
ID: SC-SEPARATE-INPUT-CHANNEL-OBSERVATION
Profile: draft
Gate: required
Given 同一账号的新版客户端已经连接中枢
When 用户移动光标或输入按键并查看客户端、管理页面和诊断日志
Then 光标与键盘分别显示逻辑通道状态，并分别记录实际传输路径和相应性能统计
And 日志不记录输入字符，不将共享网络连接声称为两条独立的物理链路


# 场景 8：用户可以在本地选择蓝牙并观察实际路径
ID: SC-NATIVE-BLUETOOTH-MODE
Profile: draft
Gate: required
Given 同一账号的不同平台新版客户端在线且已完成系统蓝牙配对，包括 Windows 连接 Mac 或 Linux 连接 Windows
When 用户在任一本地客户端选择仅蓝牙并跨屏移动、点击、拖拽、输入和滚动
Then 其他客户端与管理页面同步模式，输入通过经过双向认证的真实蓝牙连接传输
And 蓝牙不可达时停止控制，不静默改走局域网或中转

# 场景 9：切换模式关闭未选中的蓝牙入口
ID: SC-BLUETOOTH-CLOSE-AND-REVOKE
Profile: draft
Gate: required
Given 用户已建立蓝牙控制连接
When 用户选择仅中转或暂停全部接力
Then 蓝牙服务和输入连接关闭，控制权及按键、按钮按原有规则释放
And 未通过同账号证书认证或已失效控制会话的蓝牙输入不能注入桌面

# 场景 10：无蓝牙适配器时明确停止控制
ID: SC-BLUETOOTH-UNAVAILABLE
Profile: draft
Gate: required
Given Windows 客户端已连接中枢但系统未检测到蓝牙适配器
When 用户在本地选择仅蓝牙并尝试跨屏
Then 本地与管理页面同步模式，客户端明确提示蓝牙不可用，不能开始控制
And 不创建局域网 UDP 监听、不通过中枢转发输入，恢复自动模式后可重新跨屏

# 场景 11：管理发送不拖住已授权的直连输入
ID: SC-MANAGEMENT-SEND-ISOLATION
Profile: draft
Gate: required
Given Windows 正通过直连接收键鼠输入，管理连接正常且当前授权有效
When 中枢消息的发送暂时排队，同时用户移动光标、拖拽或输入按键
Then 直连输入不等待管理消息写完，动作和对应位置保持有序
And 管理连接撤销或授权失效仍会安全停止控制，重连不会发送旧连接积压的输入

# 场景 12：三端任意方向的真实键鼠接力
ID: SC-BIDIRECTIONAL-NATIVE-INPUT
Profile: draft
Gate: required
Given Mac、Windows、Omarchy 客户端已批准并在布局中共边，当前没有其他控制会话
When 用户分别以每台设备为控制端跨入另外两台设备，移动、点击、拖拽、输入普通键与修饰键并跨回
Then 六个方向的接收端均在对应边缘显示可见光标，键盘和鼠标实际生效，返回后本机键鼠恢复
And 切换前后没有遗留按键或按钮，不改变原有位移与滚动算法

# 场景 13：本机真实输入安全接管
ID: SC-LOCAL-PHYSICAL-TAKEOVER
Profile: draft
Gate: required
Given 任一设备正在被另一台已批准设备控制，且本机输入活动检测可用
When 用户操作受控设备的实体鼠标或键盘，再从布局共边移出
Then 受控设备立即停止旧会话注入并释放按键和按钮，旧控制端恢复本机，新会话由当前实体键鼠所属设备发起
And 远程注入事件不会自行接管，旧会话的迟到请求、输入和回调不能结束或污染新会话，其他账号和非当前接收端不能冒充本机接管
