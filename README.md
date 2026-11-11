# can-node

车载 CAN 节点（STM32）。**项目 2 主线**，目标是把项目 1「智能网关」的 CAN 能力延伸到 MCU 侧，
形成一条可讲完整链路：Linux 网关（`smartgateway`）↔ CAN 总线 ↔ MCU 节点。

## 目标能力

| 模块 | 内容 |
|---|---|
| CAN 收发 | 标准帧 / 扩展帧、ID 过滤、发送优先级、错误计数读取 |
| UDS 诊断 | `0x10` 会话控制、`0x22` 读数据、`0x2E` 写数据、`0x27` 安全访问、`0x31` 例程控制、`0x3E` 会话保持 |
| Bootloader | 双区升级 + CRC 校验 + 跳转 + 失败回滚 |

## 明确不做

`SOC` / `SOH` 估算、`FOC` 电机控制、电池均衡。被问到就直说没做，理由是不在能力边界内。

## 学习参照（三层）

1. **标准层** —— 先看协议怎么实现：`python-udsoncan`、`isotp-c`、`can-utils`
2. **架构层** —— 看工程怎么组织：`OpenBLT`（Bootloader）、`CANopenNode`、`foxBMS 2`
3. **完整流程层** —— 看完整产品怎么做：`Three-Tier`、`iglooom`、`lart_bms`

> 这个细分领域没有高 star 的完整节点项目，高 star 全是库/工具/平台。
> 所以别用 star 数判断项目好坏，要看它属于哪一层。

## 目录

```
src/bsp/    板级支持：时钟、GPIO、CAN 外设初始化
src/can/    CAN 驱动与收发（复用给 bms-node）
src/uds/    UDS 服务与 ISO-TP 分包
src/boot/   双区 Bootloader
test/       单测与 PC 侧桩测试
docs/       路线图、协议笔记
```

## 状态

立项，骨架阶段。详细路线见 [docs/roadmap.md](docs/roadmap.md)。

## 姊妹仓库

[bms-node](https://github.com/Dik-910/bms-node) —— 储能 profile，复用本仓库的 CAN 骨架。
