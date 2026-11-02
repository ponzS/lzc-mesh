# 场景 1：自动使用经过认证的局域网连接
ID: SC-AUTOMATIC-LAN-DIRECT
Profile: draft
Gate: required
Given 同一账号的 Mac 和 Linux 客户端在线且局域网可达
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
Given 新版客户端与旧版客户端按屏幕布局排列
When 用户跨屏移动、按紧急收回快捷键或断开控制会话
Then 控制权与屏幕归属一致，旧客户端仍可通过中枢使用
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
Given 同一账号的 Mac 与 Omarchy 在线且宿主机蓝牙已与 Mac 配对
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
