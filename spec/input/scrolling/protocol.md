# 滚动消息

`wheel` 保留已有 `dx` / `dy`：每格滚轮 120 单位，连续滚动每点 12 单位，正值表示向上／向左。

0.1.3 新增可选字段，旧消息与旧接收端仍兼容：

- `source`: `wheel`（鼠标滚轮）、`finger`（接收端应用负责惯性）、`continuous`（发送端提供完整位移流，包括惯性）。一帧的两个轴使用同一个来源，不能由距离推断。
- `phase` 与 `momentum_phase`: `began` / `changed` / `ended` / `cancelled` / `may_begin`；无对应阶段时省略。
- `stop_x` / `stop_y`: 对应轴结束；即使两个位移均为零也必须转发。

Mac 将系统产生的双指与惯性滚动统一发送为 `continuous`，接收端不在手指抬起时再生成一次惯性。Linux 原生触控板发送 `finger` 和每轴结束标记。中枢完整保留这些字段。省略 `source` 的旧消息按旧行为接收。
