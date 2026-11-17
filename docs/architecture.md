# 架构与实现

本文按数据流方向组织，从最底层的 CAN 帧到最上层的诊断服务。每一节说明该层承担的职责、接口边界，以及实现中受哪些约束。

---

## 0. 全景

一次诊断请求从 tester 到 ECU 的完整路径：

```
tester                          ecu
  │                              │
  │  UDS 报文（如 22 F1 90）      │
  ▼                              ▲
┌─────────┐                  ┌─────────┐
│  uds.c  │  服务分派、会话、安全  │  uds.c  │
└────┬────┘                  └────▲────┘
     │ 完整报文（可超过 8 字节）      │
┌────▼────┐                  ┌────┴────┐
│ isotp.c │  分包 / 重组、流控    │ isotp.c │
└────┬────┘                  └────▲────┘
     │ CAN 帧（≤ 8 字节）           │
┌────▼────┐                  ┌────┴────┐
│can_driver│ 抽象层：ops 表       │can_driver│
└────┬────┘                  └────▲────┘
     │                              │
     └──────── CAN 总线 ────────────┘
              （vcan / 真实总线）
```

三层的职责边界：

| 层 | 职责 | 不涉及 |
|---|---|---|
| `uds.c` | 服务分派、会话与安全权限 | 报文收发、是否分包 |
| `isotp.c` | 长报文的分包与重组、流控 | 报文语义 |
| `can_driver.h` | 单帧落到硬件 | 帧内容 |

这个边界的直接作用是可测性：每层都能替换下层，`make test` 的全部 437 项断言在无硬件环境下运行。

---

## 1. CAN 驱动抽象层

### 1.1 约束

CAN 驱动的平台差异集中在寄存器操作与中断处理。上层若直接调用厂商 HAL，会产生三个后果：

- 更换芯片需要修改上层代码
- 缺少硬件时无法验证逻辑
- 业务逻辑与硬件绑定，不能在 PC 上运行

### 1.2 接口：操作表注入

```c
typedef struct {
    int (*init)(void *ctx, uint32_t bitrate);
    int (*send)(void *ctx, const can_frame_t *frame);
    int (*recv)(void *ctx, can_frame_t *frame);
    int (*get_status)(void *ctx, can_bus_status_t *status);
    void (*deinit)(void *ctx);
} can_driver_ops_t;
```

上层只依赖 `src/can/can_driver.h`，不引用任何平台头文件。更换平台时只替换 `ops`。

### 1.3 实现

| 文件 | 平台 | 特点 |
|---|---|---|
| `can_driver_stub.c` | PC 测试 | 内存环形队列，不依赖操作系统 |
| `can_driver_socketcan.c` | Linux | 内核 CAN 子系统，非阻塞轮询 |
| `can_driver_stm32.c` | STM32 | 待硬件到位 |

SocketCAN 实现接入时，`isotp.c` 与 `uds.c` 未做任何修改，仅将 `can_stub_ops()` 替换为 `can_socketcan_ops()`。同一套用例在两种实现下行为一致。

### 1.4 桩驱动模拟的控制器行为

仅做存取转发的桩只能验证代码可运行，无法覆盖协议状态机对总线错误的响应。因此桩实现模拟了控制器的两条行为：

```c
/* Bus Off 状态下控制器停止收发 */
if (stub->status.level == CAN_LEVEL_BUSOFF)
    return CAN_ERR_BUSOFF;

/* 一次成功发送使 TEC 递减 1，且不会低于 0 */
if (stub->status.tec > 0)
{
    stub->status.tec--;
    if (stub->status.tec <= 127 && stub->status.level == CAN_LEVEL_PASSIVE)
        stub->status.level = CAN_LEVEL_ACTIVE;
}
```

缺少这两条，「Bus Off 时拒发」和「TEC 恢复后回到主动态」两条逻辑无法被用例覆盖。

### 1.5 参数校验集中在通用层

`can_driver.c` 除转发外还承担参数校验：

```c
int can_driver_send(can_driver_t *drv, const can_frame_t *frame)
{
    if (drv == 0 || drv->ops == 0)     return CAN_ERR_STATE;
    if (frame_is_valid(frame) == 0)    return CAN_ERR_PARAM;  /* ID/DLC/空指针 */
    return drv->ops->send(drv->ctx, frame);
}
```

校验只实现一次，避免各平台实现出现分歧（例如某平台检查 DLC、另一个平台遗漏）。平台实现只负责硬件操作。

---

## 2. ISO-TP（ISO 15765-2）

### 2.1 适用范围

经典 CAN 单帧上限 8 字节，UDS 报文常超出该限制（例如 17 字节 VIN 响应）。ISO-TP 负责长报文的分包与重组。

### 2.2 帧类型

首字节高 4 位决定类型：

| 类型 | 首字节 | 结构 |
|---|---|---|
| SF 单帧 | `0x0L` | L 是长度（0–7），数据跟在后面 |
| FF 首帧 | `0x1H` + `L` | 12 位总长度 = (H<<8)\|L，带 6 字节数据 |
| CF 连续帧 | `0x2N` | N 是序号，带 7 字节数据 |
| FC 流控帧 | `0x3S` + BS + STmin | 允许发送 / 等待 / 缓冲不足 |

一个 20 字节报文的完整序列：FF(6) → 等 FC → CF(7) → CF(7)。

### 2.3 序号编码

```c
tp->tx.sn = (uint8_t)((tp->tx.sn + 1) & 0x0F);
```

序号序列为 1,2,…,15,**0**,1,2,… —— 0 表示第 16 帧。

若实现为「从 0 开始、到 15 后回 1」，第 16 帧会被对端识别为第 1 帧，触发序号错误并丢弃整包。

### 2.4 序号不连续时整包废弃

```c
if (sn != tp->rx.next_sn)
{
    /* 丢帧会导致拼接出错，必须整包废弃而不是跳过继续 */
    tp->rx.active = 0;
    return ISOTP_ERR_SEQ;
}
```

跳过缺失帧继续拼接会使后续数据全部错位，产出的报文比丢弃更危险。

### 2.5 BS / STmin 的方向性

BS 与 STmin 在收发两个方向上含义不同：

- 发送长报文时：使用对端流控帧中通告的 BS / STmin
- 接收长报文时：向对端通告自身的 BS / STmin

```c
/* 发送时遵守对端的节奏 */
tp->tx.peer_bs    = f->data[1];
tp->tx.peer_stmin = f->data[2];
```

`cfg.block_size` 仅在作为接收方时生效。

### 2.6 轮询模型

```c
while (isotp_tx_busy(&tp))
{
    int rc = isotp_poll_at(&tp, &can, now_ms());  /* 每次最多收一帧、发一帧 */
    if (rc == ISOTP_ERR_TIMEOUT)
        break;                                    /* 状态已复位，可重新发 */
    usleep(500);
}
```

| 方案 | 问题 |
|---|---|
| 阻塞等待 FC | 裸机无并发上下文，阻塞会卡住主循环 |
| 回调（中断内执行） | 中断内执行耗时逻辑，且需处理重入 |
| 轮询 | 调用方控制节奏；超时判断仍需由库完成，见 2.7 |

轮询模型同时使测试可以逐帧断言中间状态，而不只校验最终结果。

### 2.7 超时责任划分

早期实现将超时责任完全交给调用方，调用方的写法为：

```c
for (int i = 0; i < 50 && isotp_tx_busy(&tp); i++)  /* 25ms 后放弃 */
{
    isotp_poll(&tp, &can);
    usleep(500);
}
```

该写法存在缺陷：循环退出后 `tx.state` 仍停留在 `WAIT_FC`，下一次 `isotp_send` 因 `state != IDLE` 返回 `ISOTP_ERR_STATE`。

失败模式：对端掉线一次，该节点此后无法发送任何报文。外部表现为「诊断无响应」，难以定位到状态机未复位。

原因是职责划分：状态机由模块私有，调用方只能选择不再轮询，而停止轮询恰恰使状态永久滞留。

现行划分：时间由调用方提供（只有它掌握时钟源），超时判断由模块完成（只有它知道已等待多久、在等待什么）。

```c
int isotp_poll_at(isotp_t *tp, can_driver_t *can, uint32_t now_ms);
```

三个实现约束：

**1. 计时器状态与时间戳解耦**

调用方的时间基准可能从 0 开始（PC 测试、刚上电的 SysTick）。以 0 作为「未启动」标志会导致计时器无法启动、超时不触发。使用独立的 `timer_armed` 标志。

**2. 时间间隔用无符号减法**

```c
uint32_t elapsed = now_ms - last_activity_ms;   /* 回绕时仍然正确 */
```

uint32 毫秒约 49.7 天回绕一次，有符号比较在回绕点判定错误。前提是输入单调递增：裸机用 SysTick，PC 用 `CLOCK_MONOTONIC`。使用墙钟时间会在系统对时后产生负间隔，被解释为巨大正数而误判超时。

**3. 刷新计时的判据是「有进度」而非「收到帧」**

总线上存在本模块不关心的帧。按收帧刷新会使无关 ID 的持续流量无限推迟超时。只有状态迁移或接收字节数增加才计为进度。

三种超时：

| 场景 | 参数 | 处理 |
|---|---|---|
| 发完 FF/CF 等不到流控 | N_Bs（默认 1000ms） | `isotp_abort`，回到 IDLE |
| 收到 FF/CF 等不到下一帧 | N_Cr（默认 1000ms） | 清空 `rx` 全部状态 |
| `SENDING` 长时间无进展 | 不检查 | 每次轮询发送一帧，发送失败直接回 IDLE |

接收侧超时必须清除半包状态，否则下一条新报文的首帧会被判为序号错误或长度不符而丢弃，表现为对端恢复后仍收不到数据。

---

## 3. UDS（ISO 14229）

### 3.1 基本约定

**服务号（SID）** 决定行为。正响应 = SID + 0x40，负响应固定 `7F + SID + NRC`：

```
请求：22 F1 90          （读数据，DID = F190）
正响应：62 F1 90 ...    （62 = 22 + 0x40）
负响应：7F 22 31        （31 = requestOutOfRange）
```

**会话**决定权限。默认会话只读，扩展会话可写，编程会话用于刷写。切换会话会重置安全等级。

**安全访问**决定写权限，采用种子-密钥挑战。

### 3.2 检查顺序

```c
长度 → 子功能/DID 合法性 → 会话 → 安全权限 → 业务动作
```

顺序固定的原因：先查权限后查长度会使 NRC 差异泄露内部结构——3 字节请求返回 `0x13`、6 字节返回 `0x33`，等价于向请求方确认「该 DID 存在且需要解锁」。

顺序颠倒的另一种后果是先执行业务动作再判权限，导致在不允许的会话中改写数据。

### 3.3 DID 表的两种读取方式

```c
typedef struct {
    uint16_t did;
    uint8_t len;
    const uint8_t *data;                     /* 静态：VIN、版本号 */
    int (*read)(uint8_t *out, uint8_t *len); /* 动态：电压、SOC */
    int (*write)(const uint8_t *in, uint8_t len);
    uint8_t min_session;                     /* 该 DID 的最低会话 */
    uint8_t need_unlock;                     /* 是否需要解锁 */
} uds_did_entry_t;
```

新增 DID 只需扩充表项，不修改服务层代码。

参考实现 `driftregion/iso14229` 采用单入口事件回调 `fn(srv, event, arg)`，适合作为通用库；本实现面向单一 ECU 业务，`switch (sid)` 分派更直接。

### 3.4 锁定期间不下发种子

```c
/* 冷却期内连种子都不给，否则攻击者能持续试探 */
if (srv->st.lock_remain_ms > 0)
    return send_negative(..., UDS_NRC_REQUIRED_TIME_DELAY);
```

连续 3 次密钥错误锁定 10 秒。若仅在密钥校验处限制，请求方仍可反复获取新种子，限制形同虚设。

### 3.5 S3 超时与心跳

```c
/* 只认 0x3E 会导致「发了别的请求反而掉线」 */
if (srv->st.session != UDS_SESSION_DEFAULT)
    srv->st.s3_remain_ms = UDS_S3_TIMEOUT_MS;
```

协议规定非默认会话在 S3（5 秒）内无活动即回落默认会话。「活动」指收到任意诊断请求，而非仅 `0x3E`。

---

### 3.6 固件传输：0x34 / 0x36 / 0x37

三个服务构成一次刷写：

```
0x34  我要写 0x08000000 起 2048 字节   →  ECU 擦除，回「每包最多 256 字节」
0x36  第 1 块（254 字节数据）           →  ECU 写入，回显序号 1
0x36  第 2 块                           →  ...
0x37  我发完了                          →  ECU 收尾
```

#### maxNumberOfBlockLength 包含 SID 与块序号

0x34 正响应中的长度是整包 0x36 请求的长度，包含 `SID(1) + BSC(1) + data`。若按纯数据长度返回（如回 254 表示可发 254 字节数据），请求方会发出 254 字节数据，实际请求达 256 字节，超出限制。表现为每块均被拒，或末块恰好落在边界上，流程看似成功但缺少两个字节。

#### 块序号 0xFF 之后为 0x00

```c
srv->xfer.bsc_next = (uint8_t)(bsc + 1);   /* 0xFF + 1 = 0x00 */
```

ISO 14229 规定首块为 1，第 256 块为 **0**。若实现为「超过 0xFF 回到 1」，第 256 块会被判为重传，数据永久缺少一块——2048 字节固件恰好跨越回绕时，前 255 块全部「成功」，最终校验失败。

uint8 自然回绕符合该语义，不应额外加条件修正。

#### 重传与序号错误的区分

请求方未收到响应时会以相同序号重发同一块。若按序号错误处理（回 NRC 0x73），请求方将中止整个刷写。

```c
if (blocks > 0 && bsc == bsc_last) {
    /* 重传：不重复写，只重发正响应 */
    return send_positive(bsc);
}
if (bsc != bsc_next) {
    return nrc(0x73);
}
```

「不重复写」不可省略：Flash 只能将 1 写为 0，重复写入已写过的字节会失败，重传反而导致写入错误。

#### 越界检查先减后比

```c
/* 错：addr + total 可能溢出，溢出后是个很小的数，反而通过检查 */
if (addr + total > base + capacity) reject();

/* 对：先确认 addr 在区内，再用减法算剩余空间 */
if (addr < base) reject();
if (addr - base > capacity || total > capacity - (addr - base)) reject();
```

`addr = 0xFFFFFFF0, total = 0x20` 这类请求会使和溢出为小值，通过检查后将固件写入任意地址。

#### 存储器抽象

传输服务只依赖三个函数：

```c
typedef struct {
    int (*write)(void *ctx, uint32_t addr, const uint8_t *data, uint16_t len);
    int (*read)(void *ctx, uint32_t addr, uint8_t *data, uint16_t len);
    int (*erase)(void *ctx, uint32_t addr, uint32_t len);   /* 可空 */
    void *ctx;
    uint32_t base_addr;
    uint32_t capacity;
} uds_memory_t;
```

与 CAN 驱动同样是操作表注入：块序号、长度、越界检查均为纯逻辑，PC 上以内存模拟，硬件到位后只需实现三个函数。

`mem` 为 NULL 时三个服务返回 NRC 0x11，即未实现传输能力的节点不声明支持。

#### 验证方式

单元测试使用带「1→0」约束的内存模拟（`test/uds_transfer_test.c`），可同时验证 0x34 是否真正擦除、重传是否重复写入。

链路验证使用 udsoncan 走完整刷写（`tools/flash_interop.py`）：写入 2048 字节后读回 DID 0x0301 的 CRC32 与本地值比对。仅验证流程返回成功不足以说明正确——写入位置错误或漏写一块同样会使流程全绿。

---

### 3.7 例程控制：擦除与自校验

`0x31` 与其他服务的区别在于它是可扩展表，而非固定功能。

```
0x31 01 <rid> <参数>   start
0x31 02 <rid>           stop
0x31 03 <rid>           requestResults
```

#### 内置例程与自定义例程

`0xFF00`（擦除）与 `0x0202`（CRC 校验）是刷写标配，逻辑固定。只要配置了 `uds_memory_t`，两者自动可用，无需在每个项目中重复实现。

其余例程（硬件自检、标定复位、产线模式）无法预置，通过 `uds_routine_entry_t` 表注册：

```c
{0x1234, start_cb, stop_cb, result_cb, min_session, need_unlock, ctx}
```

参数的语义由例程自身定义，协议层透传字节。UDS 无法约定所有例程的参数格式，强制统一会使接口失去实用性。

#### start 与 result 分离

```
0x31 01 0202 <addr,size>   →  ECU 算 CRC（可能几十毫秒）
0x31 03 0202               →  取结果
```

计算 64KB 数据的 CRC 需要读取 1024 次存储器，无法在单次请求的 P2（50ms）内完成。因此在 start 时计算并缓存，result 时读取。这是 ISO 14229 建议的用法，对应状态中的 `routine_crc_valid`。

若在 result 时现场计算，较大固件会直接超时，而数百字节的测试固件表现完全正常。

#### 校验路径的选择

刷写 2048 字节后，请求方有两种验证方式：

| 方式 | 代价 | 说明 |
|---|---|---|
| 0x22 读回全部数据 | 占用总线带宽，2048 字节需 9 个多帧响应 | 可逐字节比对 |
| **0x31 由 ECU 计算 CRC** | 一次请求 + 一次取结果 | 只能证明整体一致 |

实际刷写使用后者：带宽是稀缺资源，且请求方持有的固件本身可计算 CRC。

两条路径均需实现且结果一致——`flash_interop.py` 同时校验 0x31 与 0x22 报告的 CRC 相同。保留单条路径时，若某次改动使 CRC 计算偏移，另一条不会暴露该问题。

#### 与 Bootloader 的算法一致性

`0x0202` 使用的 CRC32 与 `src/boot/boot.c` 的 `boot_crc32` 是同一算法（反射，多项式 `0xEDB88320`）。Bootloader 跳转前校验固件，诊断仪刷写后校验，两处结果必须相同，否则会出现「诊断判定成功、Bootloader 判定映像损坏」。

---

### 3.8 故障码：判定、老化与快照

故障码分为两层，界线是业务归属：

| 层 | 文件 | 职责 |
|---|---|---|
| 故障存储 | `src/uds/dtc.c` | 故障判定、计数、老化与清除 |
| 协议编码 | `src/uds/uds.c` | `0x19` 状态位编码、`0x14` 清除 |

合并实现会使状态机被 CAN 收发与字节布局绑定，只能整机验证。拆分后，一条故障从发生到老化清除的完整生命周期可用纯逻辑用例覆盖，不需要总线，也不需要等待真实工况。

#### 状态位

八个状态位中，决定「条目是否有效」的是两位：

```
bit2 pendingDTC     本次失败过，但还没确认
bit3 confirmedDTC   确认下来的故障，售后要看的
```

其余：

| 位 | 含义 | 变更时机 |
|---|---|---|
| 0 | testFailed | 最近一次测试结果 |
| 1 | testFailedThisOperationCycle | 本循环失败过，循环切换时清 |
| 4 | testNotCompletedSinceLastClear | 清码后还没测过 |
| 5 | testFailedSinceLastClear | 清码后失败过，只有清码能清 |
| 6 | testNotCompletedThisOperationCycle | 本循环没测，循环切换时置 |
| 7 | warningIndicatorRequested | 点亮故障灯 |

#### 确认条件

上电瞬间的电压波动、单次静电放电都可能使测试失败一次。立即确认会让 ECU 积累大量无法复现的历史故障。

判定分两档：

```
循环 0 失败  ->  pending（偶发，只记录不报警）
循环 1 失败  ->  confirmed（反复出现，点亮故障灯）
```

「两个不同的操作循环」比「失败两次」严格：同一循环内连续失败一百次也只计一次。该条件区分偶发与持续。

#### 发生次数按沿计

发生次数仅在由通过转为失败时 +1：

```c
int rising = (e->status & DTC_STATUS_TEST_FAILED) == 0;
if (rising) { if (e->occurrence < 0xFF) e->occurrence++; ... }
```

主循环每 10ms 上报一次测试结果。按调用计数时，持续十秒的故障即达 1000 次，计数器饱和为 255，无法反映实际发生次数。按沿计数时，持续故障计 1 次，恢复后再次失败计 2 次。

#### 老化需排除未测试的循环

```
连续 N 个循环「测过且通过」 ->  confirmed 自动清除（默认 N = 40）
```

「测过」这一前提不可省略。若将每个循环都视为通过：

```
传感器掉线 -> 再也没有人调用 dtc_report()
-> 40 个循环后故障自己消失了
-> 售后永远查不到根因
```

`dtc_operation_cycle()` 先检查 bit6（本循环未测）与 bit1（本循环失败过），两者均为空才推进老化计数。`test/dtc_test.c` 中的 `silent cycles do not age the DTC out` 覆盖该行为。

#### 快照需带 DID 个数

`0x19 04` 返回的 `DTCSnapshotRecord` 格式为：

```
[DID 个数(1)] [DID(2)] [数据...]   ×  DID 个数
```

缺少「DID 个数」字节时，本实现的解析器仍能对上（收发两侧同一套代码），但 udsoncan 会抛出：

```
InvalidResponseException: Server returned snapshot record #1
with no data identifier included
```

按 DID 组织还有实用价值：通用 tester 能识别每段数据的含义与长度，裸字节只能显示为十六进制序列。

#### 清码的会话门槛

`0x14` 要求扩展会话。默认会话即可清码时，任何接入总线的设备都能抹除故障历史，售后现场只剩一个「无故障」的 ECU。

`dtc_interop.py` 验证该约束：默认会话收到 `NRC 0x22`，切换扩展会话后同一请求成功。

#### 支持的子功能

| 子功能 | 用途 |
|---|---|
| `0x19 01` 按掩码报数量 | 产线快速判断有无故障 |
| `0x19 02` 按掩码报列表 | 售后读码，最常用 |
| `0x19 04` 按码读快照 | 故障发生时的工况，用于复现 |
| `0x19 06` 按码读扩展数据 | 发生次数、老化进度 |
| `0x19 0A` 报支持的码 | 声明本 ECU 能报的全部码 |

ISO 14229 定义了十余个子功能，其余返回 `NRC 0x12`。返回空列表会使 tester 判定「ECU 正常」，而实际是「ECU 未查询」。

#### 本节点的故障来源

故障码全部来自总线健康状态与协议栈内部判定：

| 码 | 名称 | 触发条件 |
|---|---|---|
| `0xC00187` | can bus off | TEC 升到 256，控制器退出总线 |
| `0xC00287` | can error passive | TEC > 127，进入错误被动 |
| `0xC00387` | can bus stuck | 统计窗口内反复 Bus Off，判定为接线或硬件问题 |

`ecu_server.c` 另有一条 `0xC10087`（诊断响应超时）：请求方发出首帧后不发连续帧，ISO-TP 的 N_Cr 计时器到点后由协议栈判定。`dtc_interop.py` 通过发送不完整报文触发该路径，不依赖任何测试钩子。

---

## 4. Bootloader

### 4.1 双区布局

单区升级时擦除运行区，若中途掉电则设备无法启动。

双区约束：升级目标必须是当前非运行分区。

```c
u->target = (uint8_t)(1 - t->active);
```

### 4.2 分区表显式序列化

不能直接 `memcpy` 结构体：

```c
typedef struct {
    uint32_t offset;   /* 4 */
    uint32_t size;     /* 4 */
    uint32_t crc32;    /* 4 */
    uint32_t fw_version;/* 4 */
    uint8_t  valid;    /* 1 */
} boot_part_t;         /* sizeof 可能是 20，不是 17 —— 有填充 */
```

结构体存在对齐填充，结果随编译器变化。分区表需跨版本、跨工具链读取，必须是固定布局。因此按字节逐个 `put_u32` / `get_u32`，序列化后固定 47 字节。

分区表本身带 CRC：表被写坏时能够识别，避免以损坏的表执行跳转。

### 4.3 跳转前的三项检查

```c
/* 1. 初始 MSP 必须在 SRAM 内 */
if (stack < map->sram_base || stack > map->sram_base + map->sram_size)
    return BOOT_CHECK_BAD_STACK;

/* 2. 复位向量 bit0 必须为 1（Thumb 状态） */
if ((entry & 1u) == 0)
    return BOOT_CHECK_BAD_ENTRY;

/* 3. CRC 必须匹配 */
if (image_crc != expected_crc)
    return BOOT_CHECK_BAD_CRC;
```

三项缺一不可。仅校验 CRC 时，映像完整但向量表损坏仍会触发 HardFault。

栈顶上界取闭区间：Cortex-M 为满递减栈，首次压栈先减 4 再写入，MSP 等于 SRAM 末尾是标准做法。半开区间会拒绝合法向量表，该边界由用例覆盖。

### 4.4 升级完成后重读校验

```c
/* 从 Flash 重读一遍再算 CRC：写入可能静默失败，不能只信写入时的累积值 */
while (remain > 0) { ... verify_crc = boot_crc32_update(...); }

if (verify_crc != u->crc)
{
    boot_update_abort(u);   /* 整包作废，不要把坏分区标记为有效 */
    return BOOT_ERR_IO;
}
```

Flash 写入可能静默失败。仅依赖写入过程中累积的 CRC 等于未校验。

---

## 5. 测试策略

### 5.1 无硬件测试的基础

每层都可以替换下层：

- UDS 层验证「字节进、字节出」，不依赖 ISO-TP
- ISO-TP 层之上接桩驱动，桩的发送队列即为「已发出的帧」
- Bootloader 层之上接内存模拟的 Flash

### 5.2 边界条件用例覆盖的缺陷

| 问题 | 发现方式 |
|---|---|
| `isotp_poll` 吞掉 `ISOTP_BUSY`，导致「正在接收」与「无事件」返回值相同 | 多帧接收用例断言 `== ISOTP_BUSY` 时失败 |
| 栈顶上界写成半开区间，拒绝了合法的向量表 | 「栈顶等于 SRAM 末尾」边界用例 |

两类问题均无法通过阅读代码发现。

---

## 6. 相关文档

| 文件 | 内容 |
|---|---|
| `docs/protocol.md` | 报文 ID 分配、四类报文的信号布局 |
| `docs/can_matrix.dbc` | 机器可读定义，`cantools` 可直接加载 |
| `docs/design.md` | Flash 分区布局、升级流程、UDS 会话状态机 |
| `../README.md` | 项目说明、构建方式、测试矩阵 |
