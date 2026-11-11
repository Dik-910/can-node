# can-node

车载 CAN 节点固件，基于 STM32 实现 CAN 通信、UDS 诊断与双区 Bootloader 在线升级。

## 功能

### CAN 通信

- CAN 2.0A / 2.0B，标准帧与扩展帧收发
- 硬件 ID 过滤器，按 ID 分组投递
- 发送优先级与拥塞处理
- 总线错误计数（TEC / REC）读取，Bus Off 检测与自动恢复

### UDS 诊断

实现 ISO 14229-1（应用层）与 ISO 15765-2（网络层分包）。

| 服务 | 名称 | 用途 |
|---|---|---|
| `0x10` | 会话控制 | 默认 / 编程 / 扩展会话切换 |
| `0x11` | ECU 复位 | 硬复位与软复位 |
| `0x22` | 读数据 | 按 DID 读取运行参数与版本信息 |
| `0x27` | 安全访问 | 种子-密钥鉴权 |
| `0x2E` | 写数据 | 按 DID 写入配置与阈值 |
| `0x31` | 例程控制 | 擦除、校验、自检 |
| `0x34` / `0x36` / `0x37` | 传输 | 请求下载 / 传输数据 / 请求退出传输 |
| `0x3E` | 会话保持 | 维持非默认会话不超时 |

### Bootloader

- A/B 双区布局，运行区与升级区分离
- 升级包 CRC-32 校验
- 跳转前校验向量表与初始栈顶
- 升级中断或校验失败时回滚到上一可用区

## 架构

```
应用层      uds/      诊断服务、会话状态机、DID 表
中间层      net/      ISO-TP 分包与重组、流控
驱动层      can/      CAN 外设驱动、过滤器配置、错误计数
            flash/    分区管理、擦写、CRC
板级层      bsp/      时钟、GPIO、引脚复用、中断向量
```

上层不直接访问寄存器，驱动层通过 `can_driver.h` / `flash_driver.h` 暴露接口，便于在 PC 侧用桩替换。

## 目录

```
src/bsp/     板级支持
src/can/     CAN 驱动与收发
src/net/     ISO-TP
src/uds/     UDS 服务与 DID 表
src/flash/   Flash 分区与擦写
src/boot/    Bootloader
test/        单元测试与 PC 侧桩
docs/        协议与设计文档
```

## 通信协议

报文 ID 分配、信号布局、字节序与周期定义见 [docs/protocol.md](docs/protocol.md)，
机器可读定义见 [docs/can_matrix.dbc](docs/can_matrix.dbc)。

DBC 是唯一权威来源，已通过 `cantools` 加载与编解码往返验证。节点端与网关端
都应从同一份 DBC 生成解析代码，不要各自手写字节偏移。任一侧变更需同步更新并提升版本号。

## 构建

```bash
make            # 生成 build/can-node.elf 与 .bin
make test       # PC 侧单元测试，不依赖硬件
make flash      # 通过 st-link 烧写
```

依赖 `arm-none-eabi-gcc`。PC 侧测试使用本机 `gcc`。

## 版本计划

| 版本 | 内容 |
|---|---|
| v0.1 | CAN 收发、硬件过滤、错误计数 |
| v0.2 | ISO-TP 分包重组，UDS `0x10` / `0x22` / `0x2E` / `0x3E` |
| v0.3 | 安全访问 `0x27`、例程控制 `0x31` |
| v0.4 | 双区 Bootloader、CRC 校验、回滚 |
| v1.0 | 协议冻结，与 bms-node 共用 CAN 与 ISO-TP 栈 |

## 相关仓库

- [bms-node](https://github.com/Dik-910/bms-node) —— 储能节点固件，复用本仓库的 CAN 与 ISO-TP 栈
- [smartgateway](https://github.com/Dik-910/smartgateway) —— Linux 网关端，通过 CAN 与本节点通信

## 许可证

木兰宽松许可证，第 2 版。见 [LICENSE](LICENSE)。
