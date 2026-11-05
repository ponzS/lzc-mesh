# 场景 1：手机控制并跨屏接力
ID: SC-MOBILE-CONTROL-HANDOFF
Profile: draft
Gate: required
Given 同账号有两台已批准、在线且开启接力的相邻设备，输入通道允许中枢转发
When 用户从首页进入键盘触控板、选择设备并移动、单指轻点、双指轻点、双指滚动及输入快捷键，再把光标移过共边
Then 所选设备接收对应输入，跨边后相邻设备继续接收新输入，页面显示当前目标
And 手机不作为持久屏幕出现在布局中

# 场景 2：停止控制不遗留输入
ID: SC-MOBILE-CONTROL-RELEASE
Profile: draft
Gate: required
Given 手机正在控制设备且有按键或鼠标按钮按下
When 用户返回首页、切后台、断开连接，或接收端本地接管、暂停共享、撤销信任
Then 设备结束该次控制并释放按键与按钮
And 恢复页面或连接不自动恢复旧控制，也不重放旧输入

# 场景 3：控制受账号与共享权限保护
ID: SC-MOBILE-CONTROL-AUTHORITY
Profile: draft
Gate: required
Given 用户已打开键盘触控板，目标离线、未批准、已关闭或属于其他账号，或账号暂停输入、只允许直连或蓝牙
When 用户尝试开始控制
Then 请求被拒绝并有明确不可用提示，不向目标发送输入且不改变原共享设置
And 另一控制端正在使用设备时，不抢占其控制

# 场景 4：沉浸布局与系统键盘
ID: SC-MOBILE-IMMERSIVE-KEYBOARD
Profile: draft
Gate: required
Given 用户通过懒猫客户端打开首页和键盘触控板
When 用户在横竖屏打开并收起系统键盘
Then 宿主导航隐藏、系统状态栏保留，页面操作避开安全区且不重复留白
And 快捷栏保持在可见视口底部，不被键盘遮挡，剩余区域继续作为触控板

# 场景 5：无需按钮的妙控板式点击与拖拽
ID: SC-MOBILE-TRACKPAD-GESTURES
Profile: draft
Gate: required
Given 用户正在通过手机触控板控制设备，底栏没有拖拽按钮
When 用户分别执行单指轻点、连续两次轻点、双指轻点、双指滚动、三指滑动及轻点两下后滑动
Then 设备分别收到左键点击、左键双击、右键点击、自然滚动和左键拖拽，拖拽抬手松开
And 单指长按不触发右键，双指滚动或逐个抬指不误发点击和光标移动

# 场景 6：惯性可以立即打断且不重放
ID: SC-MOBILE-TRACKPAD-MOMENTUM
Profile: draft
Gate: required
Given 用户在触控板上快速双指滚动后抬手
When 滚动自然减速结束，或用户重新触摸、取消触摸、切换目标或停止控制
Then 自然结束时只产生一段逐渐减速的惯性，新的触摸或结束控制立即停止旧惯性
And 静止后抬手或手势取消不产生惯性，失焦和重新连接不回放旧滚动
