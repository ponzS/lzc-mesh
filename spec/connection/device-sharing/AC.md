# 场景 1：单台设备关闭后不再共享
ID: SC-DEVICE-DORMANCY
Profile: draft
Gate: required
Given 同账号的多个客户端在线，某设备正在参与键鼠控制并交换剪贴板或文件
When 用户在管理页面关闭该设备的图形开关
Then 该设备结束控制并释放按键和按钮，停止双向键鼠、剪贴板、文件收发及直连入口
And 设备仍可接收管理指令，其他设备与已有共享文件不受影响

# 场景 2：远程选择跨重启保持
ID: SC-DORMANCY-SURVIVES-RESTART
Profile: draft
Gate: required
Given 用户已在管理页面关闭某设备，或在该设备离线时保存关闭状态
When 该客户端重连、重启或中枢重启
Then 管理页面和本机仍显示设备已关闭，不因本机旧偏好自动开启

# 场景 3：唤醒只恢复新动作
ID: SC-WAKE-WITHOUT-REPLAY
Profile: draft
Gate: required
Given 某客户端处于休眠，期间本机剪贴板已经变化且原文件传输已取消
When 用户从管理页面或本机开启该客户端
Then 客户端恢复可用，新发生的控制和共享可以正常工作
And 不补发休眠期间的输入和剪贴板、不恢复已取消的传输

# 场景 4：设备控制仅作用于本账号
ID: SC-DEVICE-SWITCH-ACCOUNT-ISOLATION
Profile: draft
Gate: required
Given 两个账号各自拥有设备
When 用户请求关闭不存在或属于另一个账号的设备
Then 请求被拒绝，两个账号原有设备状态和共享数据不变
