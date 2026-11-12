# Changelog

本文件记录各版本的变更内容。格式参考 [Keep a Changelog](https://keepachangelog.com/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

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
