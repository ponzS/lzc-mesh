# 场景 1：双指滚动连续传递
ID: SC-CONTINUOUS-TRACKPAD-SCROLL
Profile: draft
Gate: required
Given Mac 通过设备接力控制 Linux，Linux 页面可以横向和纵向滚动
When 用户用双指慢速、快速和斜向滑动触控板
Then 页面持续按一致比例接收连续位移，不因位移恰好等于鼠标整格而改变滚动方式

# 场景 2：惯性自然结束且不重复
ID: SC-SINGLE-MOMENTUM-STREAM
Profile: draft
Gate: required
Given Mac 正在通过设备接力滚动 Linux 页面
When 用户快速滑动后抬手，等待惯性结束，或开始新手势打断惯性
Then 页面只跟随 Mac 提供的一轮惯性，结束或取消后不产生额外滚动
And 控制权离开或连接断开后，接收端没有遗留的滚动状态

# 场景 3：旧客户端滚轮保持兼容
ID: SC-LEGACY-WHEEL-COMPATIBILITY
Profile: draft
Gate: required
Given 新中枢连接新版客户端与没有升级的旧客户端
When 旧客户端发送普通鼠标滚轮输入
Then 接收端仍能滚动，中枢和连接保持可用
