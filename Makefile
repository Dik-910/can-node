CC      ?= gcc
CFLAGS  += -std=c99 -Wall -Wextra \
           -I. -Isrc/can -Isrc/net -Isrc/uds -Isrc/boot -Isrc/app \
           -Isrc/nvm -Isrc/util -Itest

GEN      := src/can/generated

# 被多处链接的公共模块
UTIL     := src/util/crc32.c
DTC      := src/uds/dtc.c
NVM      := src/nvm/nvm_store.c

TESTS    := test/can_matrix_test test/can_driver_test test/isotp_test \
            test/uds_test test/uds_transfer_test test/uds_routine_test \
            test/uds_dtc_test test/dtc_test test/boot_test test/node_health_test \
            test/nvm_store_test test/persist_test test/nvm_file_test

.PHONY: all test dbc clean vcan flash-interop dtc-interop persist-interop

all: test

# 由 DBC 重新生成报文编解码代码，需要 cantools：pip install cantools
dbc:
	python3 -m cantools generate_c_source docs/can_matrix.dbc -o $(GEN)

# PC 侧单元测试，不依赖硬件与 CAN 环境
test: $(TESTS)
	@for t in $(TESTS); do echo "=== $$t ==="; ./$$t || exit 1; done

test/can_matrix_test: test/can_matrix_test.c $(GEN)/can_matrix.c
	$(CC) $(CFLAGS) $^ -o $@

test/can_driver_test: test/can_driver_test.c src/can/can_driver.c src/can/can_driver_stub.c
	$(CC) $(CFLAGS) $^ -o $@

test/isotp_test: test/isotp_test.c src/net/isotp.c src/can/can_driver.c src/can/can_driver_stub.c
	$(CC) $(CFLAGS) $^ -o $@

test/uds_test: test/uds_test.c src/uds/uds.c $(DTC) $(UTIL)
	$(CC) $(CFLAGS) $^ -o $@

test/uds_transfer_test: test/uds_transfer_test.c src/uds/uds.c $(DTC) $(UTIL)
	$(CC) $(CFLAGS) $^ -o $@

test/uds_routine_test: test/uds_routine_test.c src/uds/uds.c $(DTC) $(UTIL)
	$(CC) $(CFLAGS) $^ -o $@

test/uds_dtc_test: test/uds_dtc_test.c src/uds/uds.c $(DTC) $(UTIL)
	$(CC) $(CFLAGS) $^ -o $@

test/dtc_test: test/dtc_test.c $(DTC)
	$(CC) $(CFLAGS) $^ -o $@

test/boot_test: test/boot_test.c src/boot/boot.c src/boot/boot_flash_ram.c $(UTIL)
	$(CC) $(CFLAGS) $^ -o $@

test/node_health_test: test/node_health_test.c src/app/node_health.c
	$(CC) $(CFLAGS) $^ -o $@

# 双页参数存储：连测试用的内存设备一起编
test/nvm_store_test: test/nvm_store_test.c $(NVM) $(UTIL) test/mem_nvm.c
	$(CC) $(CFLAGS) $^ -o $@

# 文件版参数区：ecu-server 实际使用的实现
test/nvm_file_test: test/nvm_file_test.c src/nvm/nvm_file.c $(NVM) $(UTIL)
	$(CC) $(CFLAGS) $^ -o $@

# 跨复位持久化：参数 + 故障存储一起
test/persist_test: test/persist_test.c src/app/persist.c $(NVM) $(DTC) $(UTIL) test/mem_nvm.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	$(RM) $(TESTS) node ecu-server vcan-demo bus-load

# ---- 可执行示例 ----

# vcan 端到端演示：需要真实 CAN 接口（sudo ip link add dev vcan0 type vcan）
vcan-demo: examples/vcan_demo.c src/can/can_driver.c src/can/can_driver_socketcan.c \
           src/net/isotp.c src/uds/uds.c $(DTC) $(UTIL)
	$(CC) $(CFLAGS) $^ -o $@

.PHONY: vcan
vcan: vcan-demo
	./vcan-demo

# 常驻 ECU：供第三方 tester（udsoncan / can-utils）联调
ecu-server: examples/ecu_server.c src/can/can_driver.c src/can/can_driver_socketcan.c \
            src/net/isotp.c src/uds/uds.c $(DTC) $(NVM) src/nvm/nvm_file.c \
            src/app/persist.c $(UTIL)
	$(CC) $(CFLAGS) $^ -o $@

# 完整节点：周期报文 + 诊断响应，用 DBC 生成的打包函数
node: examples/node_main.c src/can/can_driver.c src/can/can_driver_socketcan.c \
      src/app/node_health.c $(GEN)/can_matrix.c src/net/isotp.c src/uds/uds.c \
      $(DTC) $(NVM) src/nvm/nvm_file.c src/app/persist.c $(UTIL)
	$(CC) $(CFLAGS) $^ -o $@

# 固件传输互操作：用 udsoncan 走完整刷写流程
# 前置：vcan0 已建（sudo ip link add dev vcan0 type vcan && ip link set up vcan0）
.PHONY: flash-interop
flash-interop: ecu-server
	@echo "--- starting ecu-server ---"
	@./ecu-server --if vcan0 --seconds 60 --nvm .interop-flash.nvm --nvm-reset > .ecu-flash.log 2>&1 & \
	 ECU=$$!; sleep 1; \
	 python3 tools/flash_interop.py; RC=$$?; \
	 kill $$ECU 2>/dev/null; wait $$ECU 2>/dev/null; \
	 echo "--- ecu log ---"; tail -8 .ecu-flash.log; \
	 exit $$RC

# 故障码互操作：让 udsoncan 触发真实的 ISO-TP 超时，再读码 / 清码
# 前置：vcan0 已建
.PHONY: dtc-interop
dtc-interop: ecu-server
	@echo "--- starting ecu-server ---"
	@./ecu-server --if vcan0 --seconds 30 --nvm .interop-dtc.nvm --nvm-reset > .ecu-dtc.log 2>&1 & \
	 ECU=$$!; sleep 1; \
	 python3 tools/dtc_interop.py; RC=$$?; \
	 kill $$ECU 2>/dev/null; wait $$ECU 2>/dev/null; \
	 echo "--- ecu log ---"; tail -6 .ecu-dtc.log; \
	 exit $$RC

# 跨复位持久化：脚本自己启停 ECU，验证参数与故障历史能活过重启
# 前置：vcan0 已建
.PHONY: persist-interop
persist-interop: ecu-server
	python3 tools/persist_interop.py; RC=$$?; \
	 echo "--- last ecu log ---"; tail -6 .persist-ecu.log 2>/dev/null; \
	 exit $$RC

# 总线时序分析（纯计算，不依赖 CAN 接口）
bus-load: examples/bus_load_test.c
	$(CC) $(CFLAGS) $^ -o $@
	./$@
