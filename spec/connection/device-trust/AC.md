# 场景 1：待批准设备不能参与共享
ID: SC-UNAPPROVED-DEVICE-ISOLATION
Profile: draft
Gate: required
Given 同账号已有已批准设备，新客户端尚未被批准
When 新客户端接入并尝试键鼠控制、剪贴板和原生文件操作
Then 管理页与本机显示待批准，未经批准的客户端不能注入输入或获得自动共享数据与对端证书
And 已批准设备的会话与布局不被冒用设备 ID 的请求改变

# 场景 2：首次批准及身份跨重启保留
ID: SC-APPROVED-IDENTITY-PERSISTS
Profile: draft
Gate: required
Given 用户已核对设备指纹并在管理页批准该设备
When 客户端和中枢分别重启后重新连接
Then 原设备持有有效凭证即可恢复其原有运行或休眠状态，不需要再次批准
And 设备 ID、布局和系统输入权限不因升级或注册而重置

# 场景 3：伪造和重放的身份请求被拒绝
ID: SC-FORGED-IDENTITY-REJECTED
Profile: draft
Gate: required
Given 已批准设备在线且正常工作
When 请求方冒用其 ID、使用其他私钥，或重放已使用、过期或其他连接的身份应答
Then 请求被拒绝，原设备仍可继续工作，输入权限和设备绑定不发生变化

# 场景 4：撤销阻断旧权限
ID: SC-TRUST-REVOCATION
Profile: draft
Gate: required
Given 已批准设备存在控制会话及正在进行的原生文件传输
When 用户在管理页撤销该设备的信任
Then 接收撤销的客户端结束控制并释放按键和按钮，文件任务取消，旧直连与文件权限失效
And 客户端或中枢重启及切换运行开关不能绕过撤销，已有共享文件保留

# 场景 5：绑定和文件凭证不能跨账号冒用
ID: SC-TRUST-ACCOUNT-ISOLATION
Profile: draft
Gate: required
Given 两个账号分别具有设备与独立管理会话
When 一个账号尝试批准、撤销或使用另一个账号的设备与原生文件凭证
Then 请求被拒绝，两个账号原有绑定、布局及文件权限保持不变
