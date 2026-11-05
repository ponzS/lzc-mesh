# 手机触控板 LPK 本地验证

开发验证阶段仅构建本地测试包，不自动安装或发布 Release。用户已确认 v0.3.2 测试通过，并授权提交、推送到 GitHub；此次不向 Gitee 推送。

## 0.3.2 妙控板式手势修订（当前测试包）

- 产物：`lzc-mesh-lpk/dist/cloud.lazycat.app.lzc-mesh-v0.3.2.lpk`，LPK V2，3,862,016 字节。
- SHA-256：`a100341eba9b5a82dbab52c8eb73dce6f0ed57ffd985192c1d3f78ae20f2df59`。
- 移除拖拽按钮和长按右键；单指移动/轻点、双击、双指轻点右键、双指自然滚动和可立即打断的惯性、三指拖拽、轻点两下后滑动拖拽，抬手释放。
- 多指识别使用同一时刻的原子触摸快照，避免分开的 Pointer Events 将加指、抬指当作位移；连续惯性由网页生成一段，外接触控板 wheel 不二次生成惯性。
- `node --test spec-tests/trackpad-gestures.test.mjs`：12/12 通过，包括逐个加指/抬指、惯性衰减/打断、取消/失焦清理和跨屏不遗留拖拽。
- `python spec-tests/mobile-controller.py --hub-bin lzc-mesh-lpk/content/lzc-mesh-hub --artifacts /tmp/lzc-mesh-trackpad-032-release-check`：8 组通过。实际 Chromium CDP 触控经过本次静态中枢与临时设备，验证新手势、键盘、跨屏、布局和退出释放。
- Rust 40/40 测试、六方向原有协议接管回归、规格结构与 JS 语法检查通过。
- 正式打包成功，LPK lint 无警告；核对全部包内运行资源，包含新增手势模块且没有拖拽按钮。
- 用户安装测试后反馈“测试通过”，本轮沉浸模式、手机触控板及妙控板式手势修订获用户验收；反馈未逐项列出手机型号、宿主或操作系统，不扩大为全部设备组合验收。这是启用轻点和三指拖拽后的妙控板式手势，不声称网页支持实体按压、Force Touch 或触觉反馈。

## 0.3.1 历史产物

- `lzc-mesh-lpk/dist/cloud.lazycat.app.lzc-mesh-v0.3.1.lpk`，LPK V2，3,856,384 字节。
- SHA-256：`8a2eb9933a55f92adb539e9c7b57fe6f0ed80ef68552813238e7e082e520addb`。
- `lzc-cli project release` 成功；`lzc-cli lpk lint` 无警告。
- 逐一核对包内中枢和全部网页资源，与本次构建内容一致。

## 已执行

- `cargo test --locked --manifest-path lzc-mesh-lpk/hub/Cargo.toml`：40/40 通过。新增临时浏览器控制、权限拒绝、不抢占、跨屏、释放、租约、旧续约拒绝、撤销和目标专属剪贴板测试。
- `python spec-tests/mobile-controller.py --hub-bin lzc-mesh-lpk/content/lzc-mesh-hub --artifacts /tmp/lzc-mesh-mobile-controller-release-check`：6 组通过。实际启动静态 musl 中枢、隔离 Chromium 和临时签名设备；使用 CDP 触控验证移动、点击、长按右键、双指滚动及停止；验证键盘焦点、触摸快捷键不抢焦点、英文、删除、中文剪贴板提示、同一手势连续跨屏、手动换设备、返回释放与服务端四秒租约。
- `spec-tests/hub-handoff.py`：六个平台方向的协议接管、释放、反向控制与旧会话隔离均通过。
- 现有 Project AC Executor 的 `hub-lifecycle` 4 项、`hub-trust` 5 项均通过，使用临时账号和独立数据目录。
- 规格结构校验、JS 语法检查、`git diff --check` 通过。

诊断及截图：`/tmp/lzc-mesh-mobile-controller-release-check/`、`/tmp/lzc-mesh-mobile-handoff-regression/`、`/tmp/lzc-mesh-hub-lifecycle-mobile-regression/`、`/tmp/lzc-mesh-hub-trust-mobile-regression/`。

## 验收边界

浏览器/协议验证不是手机真机或电脑 OS 输入注入验收。用户已对本轮安装包给出测试通过确认；未明确列出的 iOS/Android 机型、宿主、键盘和桌面组合不据此视为全部通过。新规格的自动化执行器承接尚未完成，继续保留 draft，不以人工反馈代替全部自动化场景覆盖。

使用时通道选择“自动”或“仅中枢转发”。沿用已支持设备凭证的电脑客户端；英文和快捷键直接发送，手机中文等文本按提示先送至当前设备剪贴板再手动粘贴，不假设旧客户端具有直接 Unicode 注入能力。
