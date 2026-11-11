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

### 平台适配

CAN 驱动以操作表形式注入，上层只依赖 `src/can/can_driver.h`：

```c
const can_driver_ops_t *ops = can_stm32_ops();
can_driver_t drv;
can_driver_init(&drv, ops, &ctx, 500000);
```

接入新平台只需实现 `init` / `send` / `recv` / `get_status` 四个函数（`deinit` 可选）。
参数校验（ID 越界、DLC 超限、空指针、未初始化调用）由通用层统一完成，平台实现不必重复。

仓库自带 PC 侧桩驱动 `src/can/can_driver_stub.c`，用内存队列模拟收发，并刻意模拟了
两处真实控制器行为——**Bus Off 时拒发**、**每次成功发送 TEC 递减 1**。
因此 ISO-TP 与 UDS 在没有硬件的情况下也能用 `make test` 验证。

新平台的 ops 实现只要跑通同一套用例，就说明适配没有引入行为差异。

## 目录

```
src/bsp/           板级支持
src/can/           CAN 驱动与收发
src/can/generated/ 由 DBC 生成的编解码代码，不要手改
src/net/           ISO-TP
src/uds/           UDS 服务与 DID 表
src/flash/         Flash 分区与擦写
src/boot/          Bootloader
test/              单元测试与 PC 侧桩
docs/              协议与设计文档
```

## 通信协议

报文 ID 分配、信号布局、字节序与周期定义见 [docs/protocol.md](docs/protocol.md)，
机器可读定义见 [docs/can_matrix.dbc](docs/can_matrix.dbc)。

DBC 是唯一权威来源，已通过 `cantools` 加载与编解码往返验证。节点端与网关端
都应从同一份 DBC 生成解析代码，不要各自手写字节偏移。任一侧变更需同步更新并提升版本号。

## 构建

当前处于骨架阶段，已提供 PC 侧编解码测试与代码生成链路：

```bash
make test       # 编译并运行全部 PC 侧测试，不依赖硬件
make dbc        # 由 docs/can_matrix.dbc 重新生成编解码代码
```

`make test` 当前包含两套用例：

| 套件 | 覆盖内容 |
|---|---|
| `test/can_matrix_test` | 报文编解码、字节序、带符号信号、量程拒绝 |
| `test/can_driver_test` | 驱动抽象层校验、收发往返、TEC 递减、Bus Off 拒发、队列上限 |

依赖：PC 侧测试需要 `gcc`；重新生成代码需要 `pip install cantools`。

STM32 目标工程（`make` / `make flash`）在硬件平台确定后补齐。
生成的编解码代码随仓库提交，使用者无需安装 Python 环境即可编译。

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
