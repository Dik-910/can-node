CC      ?= gcc
CFLAGS  += -std=c99 -Wall -Wextra -I.

GEN      := src/can/generated
TEST_BIN := test/can_matrix_test

.PHONY: all test dbc clean

all: test

# 由 DBC 重新生成报文编解码代码，需要 cantools：pip install cantools
dbc:
	python3 -m cantools generate_c_source docs/can_matrix.dbc -o $(GEN)

# PC 侧单元测试，不依赖硬件与 CAN 环境
test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): test/can_matrix_test.c $(GEN)/can_matrix.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	$(RM) $(TEST_BIN)
