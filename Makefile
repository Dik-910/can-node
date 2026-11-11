CC      ?= gcc
CFLAGS  += -std=c99 -Wall -Wextra -I. -Isrc/can -Isrc/net -Isrc/uds

GEN      := src/can/generated

.PHONY: all test dbc clean

all: test

# 由 DBC 重新生成报文编解码代码，需要 cantools：pip install cantools
dbc:
	python3 -m cantools generate_c_source docs/can_matrix.dbc -o $(GEN)

# PC 侧单元测试，不依赖硬件与 CAN 环境
test: test/can_matrix_test test/can_driver_test test/isotp_test test/uds_test
	./test/can_matrix_test
	./test/can_driver_test
	./test/isotp_test
	./test/uds_test

test/can_matrix_test: test/can_matrix_test.c $(GEN)/can_matrix.c
	$(CC) $(CFLAGS) $^ -o $@

test/can_driver_test: test/can_driver_test.c src/can/can_driver.c src/can/can_driver_stub.c
	$(CC) $(CFLAGS) $^ -o $@

test/isotp_test: test/isotp_test.c src/net/isotp.c src/can/can_driver.c src/can/can_driver_stub.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	$(RM) test/can_matrix_test test/can_driver_test test/isotp_test

test/uds_test: test/uds_test.c src/uds/uds.c
	$(CC) $(CFLAGS) $^ -o $@
