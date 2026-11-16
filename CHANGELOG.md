# Changelog

本文件记录各版本的变更内容。格式参考 [Keep a Changelog](https://keepachangelog.com/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### Fixed

**ISO-TP 对端掉线后永久卡死（严重）**

现象：多帧传输过程中对端不发流控帧（掉线、复位、或 ID 配错），
本节点此后**再也发不出任何诊断响应**。外部只表现为「诊断没反应」。

根因是职责划分错误。原来的设计是「ISO-TP 不碰时间，超时由调用方管」，
调用方写成了按次数循环：

```c
for (int i = 0; i < 50 && isotp_tx_busy(&tp); i++) { poll; usleep(500); }
```

循环 25ms 后退出，但 `tx.state` 仍停在 `WAIT_FC`。下一次 `isotp_send`
因为 `state != IDLE` 返回 `ISOTP_ERR_STATE` —— 而状态机的复位只有它自己会做，
调用方没有任何手段让它退出来。

改为：**时间由调用方喂，超时判断归库**。

### Added

- `isotp_poll_at(tp, can, now_ms)`：带时间戳的 poll，实现 N_Bs（等流控）
  与 N_Cr（等连续帧）超时，默认各 1000ms，可在 `isotp_config_t` 里调整
- `ISOTP_ERR_TIMEOUT`：超时时返回，此时状态已自动复位，可直接发起新传输
- 接收侧超时会清空整个 `rx` 状态。只标记不清理的话，半个旧报文会占着
  `rx.active`，下一条新报文的首帧被当成序号错乱而丢弃 ——
  表现为「对端恢复后仍然收不到数据」
- `isotp_poll()` 保留为不检查超时的兼容版本，仅供单元测试精确控制时序使用

**UDS 固件传输（0x34 / 0x36 / 0x37）**

- `0x34` 请求下载：解析 addressAndLengthFormatIdentifier、校验地址与长度、
  擦除目标区、回报单块最大长度（含 SID+BSC 两字节）
- `0x36` 传输数据：块序号校验、重传识别、逐块写入存储器
- `0x37` 请求退出传输
- `uds_memory_t` 操作表注入：`write` / `read` / `erase` + 可写区范围。
  为 NULL 时三个服务回 NRC 0x11，不假装支持
- 越界检查先减后比，避免 `addr + size` 溢出后绕过检查

### Changed

- `isotp_config_t` 新增 `n_bs_ms` / `n_cr_ms`，0 表示用默认值

### 实现细节（都是踩过的坑）

- **计时器起算不能靠「时间戳是否为 0」判断**：调用方的时间可能就从 0 开始
  （PC 测试、刚上电的 SysTick），拿 0 当哨兵会让计时器永远起不来。改用
  `timer_armed` 标志
- **间隔用无符号减法**：`now - last`，uint32 毫秒 49.7 天回绕一次也不会判错。
  有符号比较或直接比大小都会在回绕点出错
- **刷新计时的判据是「有进度」而不是「收到帧」**：总线上可能有本模块不关心的帧，
  按收帧刷新会让无关 ID 的流量把超时无限推迟

### 测试

`test/isotp_test` 从 54 项增至 78 项，新增：

- N_Bs 超时（对端不发流控）、N_Cr 超时（对端不发连续帧）
- **超时后能立刻发起新传输**，以及**完整跑完一轮多帧**（断言总线上的真实帧序列
  FF→CF1→CF2，而不只是看状态字段）
- 时间从 `0xFFFFFF00` 跨越回绕点仍正确判超时

累计 297 项单元测试通过（matrix 17 / driver 36 / isotp 78 / uds 48 /
uds_transfer 51 / boot 40 / health 27）。

### 传输服务踩到的坑

- **块序号 0xFF 之后是 0x00，不是 0x01**。写成「回绕到 1」会让第 256 块
  被判成重传，数据永远少一块，而前 255 块全都"成功"
- **重传必须和序号错误分开处理**。tester 丢了一个响应会用同一序号重发，
  按 0x73 处理会让它中止整个刷写。重传时不能重复写——Flash 只能 1→0，
  把刚写的字节再写一遍必然失败
- **单块失败不能计入进度**。写失败却把 received 加上，后续字节会写到错误偏移，
  最终 CRC 不对但每块都回了正响应
- **越界检查先减后比**。`addr + size` 在 uint32 上溢出会变成很小的值，
  反而通过检查

### 注意

`bms-node` 里的协议栈是**复制**自本仓库的（见其 `src/PROTOCOL_STACK.md`），
本次修复尚未同步过去。等要做 bms 一侧时执行：

```bash
cd ~/work && bash bms-node/tools/sync_stack.sh
```

同步后必须重跑 `make test` —— 协议栈是地基。

---

## [0.1.0] - 2026-11-11

首个可用版本。核心链路可以在没有硬件的情况下完整验证：驱动抽象层 → ISO-TP → UDS → 应用。

### Added

**CAN 驱动抽象层**

- 操作表注入（`init` / `send` / `recv` / `get_status` / `deinit`），上层不依赖任何 MCU 头文件
- 三个实现：PC 内存桩、Linux SocketCAN、STM32（待硬件）
- 参数校验集中在通用包装层，平台实现不重复检查
- 桩驱动模拟真实控制器行为：Bus Off 拒发、成功发送使 TEC 递减

**ISO-TP（ISO 15765-2）**

- 单帧 / 首帧 / 连续帧 / 流控帧完整实现
- 连续帧序号 1..15 循环（0 表示第 16 帧），序号不连续整包废弃
- 发送遵守对端通告的 BS/STmin，自身接收时通告配置值
- 接收缓冲不足回 `OVFLW`，避免对端白等超时
- 一次 poll 处理一帧，不内部阻塞（裸机无线程）

**UDS（ISO 14229-1 子集）**

- 服务：`0x10` 会话控制、`0x11` ECU 复位、`0x22` 读数据、`0x27` 安全访问、
  `0x2E` 写数据、`0x3E` 会话保持
- 三层权限：会话、安全访问、DID 自身属性
- DID 表混合静态数据与回调读取
- 安全访问带失败锁定：连续 3 次错误锁定 10 秒，锁定期间不下发种子
- S3 会话超时，任何请求都刷新定时器

**Bootloader**

- CRC-32（IEEE 802.3）
- 分区表显式序列化，附表自身 CRC
- 跳转前三项检查：栈顶、复位向量 Thumb 位、映像 CRC
- 双区升级：目标永远是非运行分区，完成时从 Flash 重读校验

**通信协议**

- 机器可读定义 `docs/can_matrix.dbc`，5 条报文
- 由 DBC 生成 C 编解码代码，随仓库提交

**应用层**

- 完整节点主循环：周期报文 + 诊断响应 + 定时任务
- 总线健康管理：正常 / 降级 / Bus Off / 恢复观察，含反复 Bus Off 的长期降频判定

**示例与工具**

- `examples/node_main.c` 节点主循环
- `examples/ecu_server.c` 常驻 ECU
- `examples/vcan_demo.c` 端到端演示
- `examples/bus_load_test.c` 总线时序分析（纯计算）
- `tools/udsoncan_interop.py` 第三方互操作测试
- `tools/capture_periodic.py` 周期报文实测

### Verified

- 222 项单元测试全部通过（编解码 17 / 驱动 36 / ISO-TP 54 / UDS 48 / Bootloader 40 / 健康管理 27）
- 14 项第三方互操作验证通过：用 `udsoncan` 1.26.1 + `can-isotp` 2.0.7 作为独立实现，
  验证多帧重组、负响应格式、安全访问时序
- 周期报文实测：`0x180` 平均 100.00 ms、抖动 1.81 ms；`0x181` 平均 20.00 ms、抖动 2.04 ms

### Known Limitations

- STM32 平台驱动尚未实现，需硬件到位
- 总线时序数据为计算值，未经硬件实测（vcan 无位时间）
- 异常路径（超长报文、真实对端帧序错乱）仅有本实现自测
- `0x2E` 写入未持久化到非易失存储
- Bootloader 的跳转汇编与 Flash 擦写时序未实现

[0.1.0]: https://github.com/Dik-910/can-node/releases/tag/v0.1.0
