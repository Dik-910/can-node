# can-node

[![ci](https://github.com/Dik-910/can-node/actions/workflows/ci.yml/badge.svg)](https://github.com/Dik-910/can-node/actions/workflows/ci.yml)

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
| `0x14` | 清故障码 | 清除故障存储器，需扩展会话 |
| `0x19` | 读故障码 | 按掩码读码、读快照标识、读快照、按记录号读、读扩展数据、读支持列表 |
| `0x22` | 读数据 | 按 DID 读取运行参数与版本信息 |
| `0x27` | 安全访问 | 种子-密钥鉴权 |
| `0x2E` | 写数据 | 按 DID 写入配置与阈值 |
| `0x34` / `0x36` / `0x37` | 传输 | 请求下载 / 传输数据 / 请求退出传输 |
| `0x31` | 例程控制 | 擦除、CRC 校验（内置）+ 可扩展自定义例程 |
| `0x3E` | 会话保持 | 维持非默认会话不超时 |

### Bootloader

- A/B 双区布局，运行区与升级区分离
- 升级包 CRC-32 校验
- 跳转前校验向量表与初始栈顶
- 升级中断或校验失败时回滚到上一可用区

### 参数与故障持久化

- 标定参数（DID `0x0201`）与故障存储器写入 Flash 参数区，跨复位保持
- 参数区双页交替写入，掉电时至少一页完整；写完回读重算 CRC 才认账
- 操作循环号一并保存，否则「连续 40 个循环无故障才老化」跨不过上电周期
- PC 侧用文件模拟 Flash，`make persist-interop` 重启进程验证真实跨复位行为

## 架构

```
应用层      uds/      诊断服务、会话状态机、DID 表
中间层      net/      ISO-TP 分包与重组、流控
驱动层      can/      CAN 外设驱动、过滤器配置、错误计数
            flash/    分区管理、擦写、CRC
板级层      bsp/      时钟、GPIO、引脚复用、中断向量
```

上层不直接访问寄存器，驱动层通过 `can_driver.h` / `boot_flash.h` 暴露接口，便于在 PC 侧用桩替换。

各层的问题、设计决策与代码位置的完整说明见 [docs/architecture.md](docs/architecture.md)。

### 平台适配

CAN 驱动以操作表形式注入，上层只依赖 `src/can/can_driver.h`：

```c
can_socketcan_t sk = {0};
strncpy(sk.ifname, "vcan0", sizeof(sk.ifname) - 1);

can_driver_t drv;
can_driver_init(&drv, can_socketcan_ops(), &sk, 500000);
```

接入新平台只需实现 `init` / `send` / `recv` / `get_status` 四个函数（`deinit` 可选）。
参数校验（ID 越界、DLC 超限、空指针、未初始化调用）由通用层统一完成，平台实现不必重复。

目前有两个实现：

| 实现 | 用途 |
|---|---|
| `src/can/can_driver_stub.c` | PC 侧桩，内存队列模拟收发，不依赖系统 |
| `src/can/can_driver_socketcan.c` | Linux SocketCAN，真实内核 CAN 子系统 |

桩驱动同时模拟了控制器的两条行为——Bus Off 时拒发、每次成功发送 TEC 递减 1，
因此 ISO-TP 与 UDS 在没有硬件的情况下也能通过 `make test` 验证。

`make vcan` 的端到端演示使用 SocketCAN 实现，上层代码无需修改：同一套用例
在桩驱动与真实内核 CAN 子系统下行为一致。

### 网络层

ISO-TP（ISO 15765-2）负责把超过 8 字节的 UDS 报文切成多帧，以及把多帧拼回完整报文。
实现 `src/net/isotp.c`，支持四种帧型与流控：

- 单帧 `SF`、首帧 `FF`、连续帧 `CF`、流控帧 `FC`
- 连续帧序号按 1..15 循环，0 表示第 16 帧
- 按对端通告的块大小分批发送，发满一块回去等新的流控
- 接收缓冲不足时回 `OVFLW`，而不是让对端白等超时

采用「一次 poll 处理一帧」的模型，不在内部阻塞等待流控帧：裸机没有并发上下文，
阻塞会卡住主循环；测试中也能逐帧断言中间状态。

BS/STmin 有方向性：发送时遵守对端在流控帧中通告的值，配置里的 BS/STmin
只用于自身作为接收方时通告给对端。

### 应用层

UDS 服务层（`src/uds/uds.c`）只处理报文内容，不关心怎么收发——它把一帧完整的
UDS 请求转成一帧完整响应，因此可以直接用纯逻辑测试。

| 服务 | 说明 |
|---|---|
| `0x10` | 会话控制：默认 / 编程 / 扩展 |
| `0x11` | ECU 复位，先回响应再触发复位 |
| `0x14` | 清故障码，需扩展会话 |
| `0x19` | 读故障码：数量 / 列表 / 快照 / 扩展数据 / 支持列表 |
| `0x22` | 读数据，支持一次请求多个 DID |
| `0x27` | 安全访问，种子-密钥 + 失败锁定 |
| `0x2E` | 写数据，需扩展会话 + 已解锁 |
| `0x34` | 请求下载：校验地址与长度、擦除目标区、回报单块最大长度 |
| `0x36` | 传输数据：块序号校验、重传识别、逐块写入存储器 |
| `0x37` | 请求退出传输 |
| `0x31` | 例程控制：内置擦除（`0xFF00`）与 CRC 校验（`0x0202`），支持自定义例程表 |
| `0x3E` | 会话保持，重置 S3 定时器 |

三条实现约束：

**检查顺序固定**为「长度 → 子功能/DID 合法性 → 会话 → 安全权限 → 业务动作」。
顺序不可调整：先查权限再查长度时，NRC 差异会泄露内部结构。

**DID 表混合两种读取方式**：静态数据直接拷贝（VIN、版本号），动态数据走回调
（电压、SOC 这类会变的）。每个 DID 可独立声明最低会话与是否需要解锁。

**安全访问带失败锁定**：连续 3 次密钥错误锁定 10 秒，锁定期间不下发种子。
缺少这一层时，种子-密钥可被离线穷举。

非默认会话内，任何诊断请求都算心跳，不限于 `0x3E`；只认 `0x3E` 会导致
发出其他诊断请求时反而回落默认会话。

## 目录

```
src/bsp/           板级支持
src/can/           CAN 驱动与收发（抽象层 + PC 桩 + SocketCAN）
src/can/generated/ 由 DBC 生成的编解码代码，不要手改
src/net/           ISO-TP
src/uds/           UDS 服务、DID 表、故障存储
src/boot/          Bootloader（分区表、CRC、跳转检查、升级流程）
src/nvm/           参数区：块设备接口、双页存储、PC 侧文件实现
src/util/          公共工具（CRC32，Bootloader / 0x31 / 参数区共用）
src/app/           节点健康状态机、持久化打包
test/              单元测试
examples/          可运行的端到端示例
docs/              协议与设计文档
```

## 通信协议

报文 ID 分配、信号布局、字节序与周期定义见 [docs/protocol.md](docs/protocol.md)，
机器可读定义见 [docs/can_matrix.dbc](docs/can_matrix.dbc)。

DBC 是唯一权威来源，已通过 `cantools` 加载与编解码往返验证。节点端与网关端
都应从同一份 DBC 生成解析代码，不要各自手写字节偏移。任一侧变更需同步更新并提升版本号。

## 构建

提供 PC 侧编解码测试与代码生成链路：

```bash
make test       # 编译并运行全部 PC 侧测试，不依赖硬件
make dbc        # 由 docs/can_matrix.dbc 重新生成编解码代码
make vcan       # 在 vcan0 上跑端到端演示（需要先建好 CAN 接口）
```

`make test` 当前包含十三套用例，共 542 项断言：

| 套件 | 覆盖内容 |
|---|---|
| `test/can_matrix_test` | 报文编解码、字节序、带符号信号、量程拒绝 |
| `test/can_driver_test` | 驱动抽象层校验、收发往返、TEC 递减、Bus Off 拒发、队列上限 |
| `test/isotp_test` | 单帧/多帧收发、流控分块、序号错误、缓冲溢出、N_Bs/N_Cr 超时与超时后恢复、时间回绕 |
| `test/uds_test` | 服务分派、会话权限、安全访问锁定、写保护、S3 超时 |
| `test/uds_transfer_test` | 传输服务：完整刷写、块序号回绕、重传不重复写、越界与溢出、写失败 |
| `test/uds_routine_test` | 例程控制：内置擦除与 CRC（期望值由 zlib 独立算出）、自定义例程、权限与参数校验 |
| `test/dtc_test` | 故障存储：待确认与确认、老化（含「没测过不算好」）、发生次数按沿计、快照只抓一次、清码 |
| `test/uds_dtc_test` | `0x19` 七个子功能的逐字节响应格式、`0x14` 的会话门槛、无故障存储器时回 `NRC 0x11` |
| `test/boot_test` | CRC32、分区表序列化、跳转检查、双区升级与回滚前提 |
| `test/node_health_test` | 总线健康状态机：降级触发、Bus Off 计数窗口、恢复观察期、长期降级 |
| `test/nvm_store_test` | 双页参数存储：交替写入、序号回绕、擦除后掉电、页头写一半掉电、CRC 损坏回退、两页都坏 |
| `test/persist_test` | 跨复位持久化：参数与故障存活、老化跨 40 次上电推进、静默上电不推进老化、写频率控制 |
| `test/nvm_file_test` | 参数区的文件实现：新建即擦除态、重开后读回、两次写入取最后一份 |

### 端到端演示

`make vcan` 在真实 CAN 接口上依次走完「进扩展会话 → 解锁 → 写阈值 → 读回 → 读多帧」，
输出形如：

```
[tester]  10 03
[ecu]     50 03 00 32 13 88
[tester]  22 F1 90
[ecu]     62 F1 90 4C 53 56 41 41 ...   (20 字节，ISO-TP 自动分包)
```

需要先建好接口：

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
```

### 协议符合性验证

单元测试覆盖的是本实现自身的逻辑一致性，收发两侧使用同一套代码时，格式错误
在两端同时发生，用例仍会通过。协议符合性需要独立实现验证，因此额外提供互操作测试：

| 组件 | 版本 | 角色 |
|---|---|---|
| [udsoncan](https://github.com/pylessard/python-udsoncan) | 1.26.1 | UDS 客户端（业界主流实现） |
| can-isotp | 2.0.7 | 独立的 ISO 15765-2 实现 |
| python-can | 4.6.1 | SocketCAN 访问 |

上述组件不引用本项目的任何代码，仅通过 SocketCAN 按 ISO 14229 / ISO 15765-2 交互。

```bash
./ecu-server --seconds 60 &
python3 tools/udsoncan_interop.py   # 基础诊断服务，14 项断言
python3 tools/flash_interop.py      # 完整刷写 + CRC 交叉校验
python3 tools/dtc_interop.py        # 故障码：读码 / 快照标识 / 快照内容 / 清码，28 项断言

# 跨复位：脚本自己重启 ecu-server，验证写下去的东西还在
make persist-interop
```

故障码互操作中的故障由协议栈自身判定，不依赖测试钩子：脚本发送首帧后停止，
ECU 的 ISO-TP 在 N_Cr 到点后记录一条通信超时故障，再由 udsoncan 按标准读回。

覆盖项（14 项断言全部通过）：

- 会话切换，子功能回显正确
- **17 字节 VIN 的多帧响应被第三方完整重组，内容逐字节一致**
- 未知 DID 返回 `NRC 0x31`，未解锁写入返回 `NRC 0x33`
- 种子-密钥流程：客户端侧由第三方库独立实现，密钥被本实现接受
- 解锁后写入成功，回读值一致

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
| v0.5 | 故障码存储：确认 / 老化 / 快照，`0x14` / `0x19` |
| v1.0 | 协议冻结，与 bms-node 共用 CAN 与 ISO-TP 栈 |

## 相关仓库

- [bms-node](https://github.com/Dik-910/bms-node) —— 储能节点固件，复用本仓库的 CAN 与 ISO-TP 栈
- [smartgateway](https://github.com/Dik-910/smartgateway) —— Linux 网关端，通过 CAN 与本节点通信

## 许可证

木兰宽松许可证，第 2 版。见 [LICENSE](LICENSE)。
