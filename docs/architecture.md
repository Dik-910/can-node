# 架构与实现

本文说明各层解决什么问题、关键设计为什么这么做。建议按顺序读：从底层往上是数据流的方向。

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

三层的职责边界很清楚：

| 层 | 只关心 | 不关心 |
|---|---|---|
| `uds.c` | 这串字节是什么服务、当前有没有权限 | 报文怎么发出去、会不会分包 |
| `isotp.c` | 报文大于 8 字节时怎么切、怎么拼、对方能不能接 | 报文内容是什么意思 |
| `can_driver.h` | 一帧 8 字节怎么落到硬件 | 帧里装的是什么 |

这个边界不是理论洁癖。**因为它成立，`make test` 才能在没有任何硬件的情况下跑完 195 项测试。**

---

## 1. CAN 驱动抽象层

### 1.1 问题

CAN 驱动的平台差异在于寄存器操作和中断处理。如果上层直接调 HAL，那么：
- 换芯片要改上层
- 没有硬件就没法测
- 逻辑和硬件绑死，没法在 PC 上验证

### 1.2 解法：操作表注入

```c
typedef struct {
    int (*init)(void *ctx, uint32_t bitrate);
    int (*send)(void *ctx, const can_frame_t *frame);
    int (*recv)(void *ctx, can_frame_t *frame);
    int (*get_status)(void *ctx, can_bus_status_t *status);
    void (*deinit)(void *ctx);
} can_driver_ops_t;
```

上层只 `#include "can_driver.h"`，不碰任何平台头文件。换平台时只换 `ops`。

### 1.3 三个实现

| 文件 | 用途 | 特点 |
|---|---|---|
| `can_driver_stub.c` | PC 测试 | 内存环形队列，不依赖系统 |
| `can_driver_socketcan.c` | Linux | 真实内核 CAN，非阻塞轮询 |
| `can_driver_stm32.c` | STM32 | 待硬件到位 |

**这就是抽象层的价值证据**：写 SocketCAN 驱动时，ISO-TP 和 UDS 一行代码都没改，只把
`can_stub_ops()` 换成了 `can_socketcan_ops()`。

### 1.4 桩为什么要模拟 TEC 递减

如果桩只是「存进去、取出来」，它只能证明代码能跑，证明不了正确。

所以桩刻意模拟了两条真实控制器行为：

```c
/* Bus Off 状态下控制器停止收发 */
if (stub->status.level == CAN_LEVEL_BUSOFF)
    return CAN_ERR_BUSOFF;

/* 真实控制器：一次成功发送使 TEC 递减 1，且不会低于 0 */
if (stub->status.tec > 0)
{
    stub->status.tec--;
    if (stub->status.tec <= 127 && stub->status.level == CAN_LEVEL_PASSIVE)
        stub->status.level = CAN_LEVEL_ACTIVE;
}
```

没有这两条，上层「总线坏了不要发」「TEC 恢复到 127 回到主动态」的逻辑永远测不到。

### 1.5 一个容易忽略的决策：校验放在通用层

`can_driver.c` 看起来只做了转发，但校验集中在这里：

```c
int can_driver_send(can_driver_t *drv, const can_frame_t *frame)
{
    if (drv == 0 || drv->ops == 0)     return CAN_ERR_STATE;
    if (frame_is_valid(frame) == 0)    return CAN_ERR_PARAM;  /* ID/DLC/空指针 */
    return drv->ops->send(drv->ctx, frame);
}
```

**为什么要多这一层？** 因为校验只写一遍，就不会出现「STM32 版检查了 DLC，桩版忘了」这种分歧。
平台实现只管干活，合法性由通用层兜住。

---

## 2. ISO-TP（ISO 15765-2）

### 2.1 为什么需要

经典 CAN 单帧最多 8 字节，而 UDS 报文经常几十上百字节（比如读 VIN 的 17 字节响应）。
ISO-TP 负责把长报文切开，以及把多帧拼回来。

### 2.2 四种帧

首字节高 4 位决定类型：

| 类型 | 首字节 | 结构 |
|---|---|---|
| SF 单帧 | `0x0L` | L 是长度（0–7），数据跟在后面 |
| FF 首帧 | `0x1H` + `L` | 12 位总长度 = (H<<8)\|L，带 6 字节数据 |
| CF 连续帧 | `0x2N` | N 是序号，带 7 字节数据 |
| FC 流控帧 | `0x3S` + BS + STmin | 告诉对方「可以发 / 等等 / 我装不下」 |

一个 20 字节的报文：FF(6) → 等 FC → CF(7) → CF(7)。

### 2.3 序号回绕：0 表示第 16 帧

这是最容易写错的地方。

```c
tp->tx.sn = (uint8_t)((tp->tx.sn + 1) & 0x0F);
```

序号是 1,2,...,15,**0**,1,2,... —— 0 代表第 16 帧，不是第 0 帧。

写错的人通常让序号从 0 开始、到 15 后回 1，结果第 16 帧被对端当成第 1 帧，判定序号错乱、整包丢弃。

### 2.4 序号不连续必须整包废弃

```c
if (sn != tp->rx.next_sn)
{
    /* 丢帧会导致拼接出错，必须整包废弃而不是跳过继续 */
    tp->rx.active = 0;
    return ISOTP_ERR_SEQ;
}
```

不能想着「跳过这帧继续拼」——后面的数据位置全错了，拼出来的东西比没有更危险。

### 2.5 流控的 BS/STmin 是双向的

这条最容易搞混：

- **你发长报文时**：读对端给你的 FC，用**对端**的 BS/STmin
- **你收长报文时**：发 FC 给对端，用**你自己**的 BS/STmin

```c
/* 发送时遵守对端的节奏 */
tp->tx.peer_bs    = f->data[1];
tp->tx.peer_stmin = f->data[2];
```

而 `cfg.block_size` 只在「我方作为接收方」时使用。

### 2.6 为什么用 poll 而不是阻塞或回调

```c
while (isotp_tx_busy(&tp))
    isotp_poll(&tp, &can);   /* 每次最多收一帧、发一帧 */
```

| 方案 | 问题 |
|---|---|
| 阻塞等 FC | 裸机没有线程，会卡死主循环 |
| 回调（中断里执行） | 中断里做长耗时要命，且需处理重入 |
| **poll** | 节奏交给调用方，代价是要自己管超时 |

另外，poll 模型让测试能**逐帧断言中间状态**，而不是只看最终结果。

---

## 3. UDS（ISO 14229）

### 3.1 三个核心概念

**服务号（SID）** 决定行为。正响应 = SID + 0x40，负响应固定 `7F + SID + NRC`：

```
请求：22 F1 90          （读数据，DID = F190）
正响应：62 F1 90 ...    （62 = 22 + 0x40）
负响应：7F 22 31        （31 = requestOutOfRange）
```

**会话**决定权限。默认会话只能读，扩展会话可写，编程会话刷固件。切会话会重置安全等级。

**安全访问**决定写权限。种子-密钥挑战，验过才解锁。

### 3.2 检查顺序为什么不能乱

```c
长度 → 子功能/DID 合法性 → 会话 → 安全权限 → 业务动作
```

如果先查权限再查长度，攻击者靠 NRC 差异就能探测内部结构：
3 字节请求返回 `0x13`、6 字节返回 `0x33`，等于告诉他「这个 DID 存在且需要解锁」。

顺序反了也容易出另一种 bug：先查业务动作，导致在不该执行的会话里把数据改了。

### 3.3 DID 表混合两种读取方式

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

好处：**新增一个 DID 只需要填表，不改服务代码**。

对比参考实现 `driftregion/iso14229`：它用单入口事件回调
`fn(srv, event, arg)`，适合库作者；我们是自己写业务，`switch (sid)` 更直接。

### 3.4 防爆破：锁定期间不下发种子

```c
/* 冷却期内连种子都不给，否则攻击者能持续试探 */
if (srv->st.lock_remain_ms > 0)
    return send_negative(..., UDS_NRC_REQUIRED_TIME_DELAY);
```

连续 3 次密钥错误锁定 10 秒。如果只在密钥校验处限制，攻击者还能不断请求新种子，
这个限制就没意义了。

### 3.5 S3 超时：任何请求都算心跳

```c
/* 只认 0x3E 会导致「发了别的请求反而掉线」 */
if (srv->st.session != UDS_SESSION_DEFAULT)
    srv->st.s3_remain_ms = UDS_S3_TIMEOUT_MS;
```

协议规定非默认会话在 S3（5 秒）内没有活动就掉回默认会话。
「活动」指收到任何诊断请求，不只是 `0x3E`。

---

## 4. Bootloader

### 4.1 双区为什么必要

单区升级时，擦除运行区等于自杀——擦到一半掉电，板子变砖。

双区的铁律：**升级目标永远是「非运行分区」**。

```c
u->target = (uint8_t)(1 - t->active);
```

### 4.2 分区表为什么显式序列化

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

结构体有对齐填充，结果随编译器变化。而分区表要跨版本、跨工具链读取，**必须是固定布局**。
所以按字节逐个 `put_u32` / `get_u32`，序列化后固定 47 字节。

同时给表本身加 CRC：分区表被写坏时能发现，而不是拿着一张垃圾表去跳转。

### 4.3 跳转三项检查

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

**三个都要查。** 只查 CRC 是最常见的翻车点——映像完整但向量表不对，跳过去照样 HardFault。

栈顶上界用闭区间：Cortex-M 是满递减栈，第一次压栈会先减 4 再写，
所以 MSP = SRAM 末尾是标准做法，不是越界。这里最初写错成半开区间，被测试抓出来了。

### 4.4 升级完成时为什么要重读

```c
/* 从 Flash 重读一遍再算 CRC：写入可能静默失败，不能只信写入时的累积值 */
while (remain > 0) { ... verify_crc = boot_crc32_update(...); }

if (verify_crc != u->crc)
{
    boot_update_abort(u);   /* 整包作废，不要把坏分区标记为有效 */
    return BOOT_ERR_IO;
}
```

Flash 写入失败可能静默发生。只信「我写的时候算过 CRC」，等于没校验。

---

## 5. 测试策略

### 5.1 为什么能在没有硬件的情况下测 195 项

因为每一层都可以替换下层：

- UDS 测的是「字节进、字节出」，连 ISO-TP 都不需要
- ISO-TP 上面接桩驱动，桩的 tx 队列就是「发出去的帧」
- Bootloader 上面接内存模拟的 Flash

### 5.2 边界值测试抓到的两个真 bug

| 问题 | 怎么发现的 |
|---|---|
| `isotp_poll` 吞掉 `ISOTP_BUSY`，导致「正在接收」和「无事件」返回值相同 | 多帧接收测试断言 `== ISOTP_BUSY` 时失败 |
| 栈顶上界写成半开区间，拒绝了合法的向量表 | 「栈顶等于 SRAM 末尾」这个边界用例 |

这两个都不是靠读代码能发现的。

---

## 6. 自检问题

用来检验是否真的理解了，面试也会问到：

1. 正响应为什么是 SID + 0x40？负响应的三个字节分别是什么？
2. 默认会话、扩展会话、编程会话分别能做什么？为什么要分？
3. S3 超时多久？哪些报文算「活动」？
4. `0x27` 的种子和密钥分别用来干什么？为什么锁定期间不能下发种子？
5. ISO-TP 的连续帧序号到 15 之后下一个是几？
6. 收到序号不顺的连续帧，应该跳过继续拼还是整包废弃？为什么？
7. 发送长报文时，BS 和 STmin 用谁的？
8. `isotp_poll` 为什么不能在内部阻塞等流控帧？
9. Bootloader 双区升级，为什么不能写正在运行的分区？
10. 跳转前要检查哪三样？只查 CRC 会出什么问题？
11. 分区表为什么不能直接 memcpy 结构体？
12. 升级完成后为什么要从 Flash 重读校验？

---

## 7. 相关文档

| 文件 | 内容 |
|---|---|
| `docs/protocol.md` | 报文 ID 分配、四类报文的信号布局 |
| `docs/can_matrix.dbc` | 机器可读定义，`cantools` 可直接加载 |
| `docs/design.md` | Flash 分区布局、升级流程、UDS 会话状态机 |
| `../README.md` | 项目说明、构建方式、测试矩阵 |
