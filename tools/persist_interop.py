#!/usr/bin/env python3
"""
persist_interop.py —— 验证标定参数与故障历史跨复位保持

和另外几个 interop 脚本的区别：那些脚本假定 ECU 一直在跑，
这个脚本必须**重启 ECU 进程**，否则测不到任何东西。

分四个阶段，每阶段一个独立的 ecu-server 进程：

  A. --nvm-reset 启动（模拟新板子）
     读 0x0201 应为默认值，写 61，读回 61，记下当前操作循环号

  B. 不带 reset 启动（模拟断电重启）
     0x0201 应仍是 61 —— 这是参数持久化
     循环号应比 A 大 —— 说明 cycle 存在 NVM 里，不是每次从 1 开始
     发一个首帧就不管的请求，让 ISO-TP 判定超时，产生一条 pending 故障

  C. 再重启
     0x19 02 仍能读到那条 pending —— 这是故障历史持久化
     循环号再次增长

参数与故障分别验证，是因为它们走同一条写入路径但语义不同：
参数是用户主动写的，故障是协议栈自己判出来的。
"""

import atexit
import os
import subprocess
import sys
import time

for _cand in (os.environ.get("MG_PYLIB"), "/home/dikl/work/.pylibs"):
    if _cand and os.path.isdir(_cand):
        sys.path.insert(0, _cand)
        break

import can
import isotp
import udsoncan
from udsoncan.client import Client
from udsoncan.configs import default_client_config
from udsoncan.connections import PythonIsoTpConnection

ID_REQUEST = 0x7E0
ID_RESPONSE = 0x7E8

DID_THRESHOLD = 0x0201
DID_DTC_STATS = 0x0302
DTC_TIMEOUT = 0xC10087

NVM_PATH = ".persist-test.nvm"
DEFAULT_THRESHOLD = 50
NEW_THRESHOLD = 61

passed = 0
failed = 0


def check(cond, what, detail=""):
    global passed, failed
    if cond:
        print(f"  [PASS] {what}" + (f"  ({detail})" if detail else ""))
        passed += 1
    else:
        print(f"  [FAIL] {what}" + (f"  ({detail})" if detail else ""))
        failed += 1


class Ecu:
    """一个 ecu-server 进程的生命周期"""

    def __init__(self, reset: bool, seconds: int = 25):
        args = ["./ecu-server", "--if", "vcan0", "--seconds", str(seconds),
                "--nvm", NVM_PATH]
        if reset:
            args.append("--nvm-reset")

        self.log = os.path.join(".", ".persist-ecu.log")
        self.fh = open(self.log, "w")
        self.proc = subprocess.Popen(args, stdout=self.fh, stderr=subprocess.STDOUT)

        # 等它把 CAN socket 打开。固定 sleep 比轮询简单，这里 1 秒足够
        time.sleep(1.2)

        if self.proc.poll() is not None:
            raise RuntimeError(f"ecu-server exited immediately, see {self.log}")

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.fh.close()

    def log_text(self):
        with open(self.log, encoding="utf-8", errors="replace") as f:
            return f.read()


def our_key_algo(level, seed, params):
    """与 can-node/src/uds/uds.c 里的 default_key 保持一致"""
    seed_int = int.from_bytes(seed, "big")
    key = ((seed_int ^ 0x5A5A5A5A) + 0x12345678) & 0xFFFFFFFF
    return key.to_bytes(4, "big")


def open_client(bus, addr):
    stack = isotp.CanStack(bus=bus, address=addr,
                           params={"blocksize": 8, "stmin": 5})
    conn = PythonIsoTpConnection(stack)

    cfg = dict(default_client_config)
    cfg["security_algo"] = our_key_algo
    cfg["security_algo_params"] = None

    # 不声明格式的话，udsoncan 在读之前就会抛 ConfigError，
    # 请求根本不会发出去。1 字节按无符号整型解，6 字节的统计量取原始字节。
    cfg["data_identifiers"] = {
        DID_THRESHOLD: "B",
        DID_DTC_STATS: "6s",
    }
    return Client(conn, request_timeout=5, config=cfg)


def enter_extended_and_unlock(client):
    """
    0x0201 在 DID 表里声明为「扩展会话 + 需解锁」。
    这是刻意的：标定值不是随便能读的，产线和售后的权限不同。
    直接读会拿到 NRC 0x22（conditionsNotCorrect）。
    """
    client.change_session(0x03)
    client.unlock_security_access(1)


def did_value(resp, did):
    """
    udsoncan 对 struct 格式的 DID 返回 tuple：
      "B"  -> (61,)
      "6s" -> (b'\x00\x00\x00\x01\x00\x01',)
    多 DID 一次读时又可能是扁平值。两种都处理掉。
    """
    v = resp.service_data.values[did]
    return v[0] if isinstance(v, tuple) else v


def read_threshold(client):
    resp = client.read_data_by_identifier(DID_THRESHOLD)
    return did_value(resp, DID_THRESHOLD)


def read_dtc_stats(client):
    resp = client.read_data_by_identifier(DID_DTC_STATS)
    raw = did_value(resp, DID_DTC_STATS)
    cycle = int.from_bytes(raw[0:4], "big")
    count = int.from_bytes(raw[4:6], "big")
    return cycle, count


def trigger_isotp_timeout(bus):
    """发首帧、不发连续帧：让 ECU 的 N_Cr 计时器到点，产出一条真实故障"""
    payload = bytes([0x2E, 0x02, 0x01]) + bytes(range(1, 21))
    ff = bytes([0x10 | ((len(payload) >> 8) & 0x0F), len(payload) & 0xFF]) + payload[:6]
    bus.send(can.Message(arbitration_id=ID_REQUEST, data=ff, is_extended_id=False))


def dtc_codes(client):
    resp = client.get_dtc_by_status_mask(0x0F)
    if resp is None:
        return []
    return [int(d.id) for d in resp.service_data.dtcs]


def main():
    if not os.path.exists("./ecu-server"):
        print("ecu-server not built; run make ecu-server first")
        return 1

    # 每次从一块空板子开始，避免上一轮遗留的数据影响判断
    for p in (NVM_PATH,):
        if os.path.exists(p):
            os.remove(p)

    bus = can.interface.Bus(channel="vcan0", interface="socketcan")
    atexit.register(bus.shutdown)

    addr = isotp.Address(isotp.AddressingMode.Normal_11bits,
                         txid=ID_REQUEST, rxid=ID_RESPONSE)

    print("=== persistence interop (parameter + DTC across restart) ===\n")

    cycle_a = -1
    door = None

    # ---------- A：新板子 ----------
    print("Phase A: first boot (param area erased)")
    ecu = Ecu(reset=True)
    try:
        with open_client(bus, addr) as client:
            enter_extended_and_unlock(client)

            got = read_threshold(client)
            check(got == DEFAULT_THRESHOLD, "fresh board uses the default threshold",
                  f"got {got}")

            # 配置是 "B"，这里必须传整数；传 bytes 会被 struct 打包拒绝
            client.write_data_by_identifier(DID_THRESHOLD, NEW_THRESHOLD)
            got = read_threshold(client)
            check(got == NEW_THRESHOLD, "written threshold read back",
                  f"got {got}")

            cycle_a, _ = read_dtc_stats(client)
            check(cycle_a >= 1, "operation cycle counter starts at 1",
                  f"cycle={cycle_a}")
    finally:
        ecu.stop()

    check("persisted:" in ecu.log_text(), "ECU saved state before exit")

    # ---------- B：重启，参数应还在 ----------
    print("\nPhase B: restart without reset")
    ecu = Ecu(reset=False)
    try:
        with open_client(bus, addr) as client:
            enter_extended_and_unlock(client)

            got = read_threshold(client)
            check(got == NEW_THRESHOLD,
                  "threshold survives power cycle", f"got {got}")

            cycle_b, _ = read_dtc_stats(client)
            check(cycle_b == cycle_a + 1,
                  "operation cycle continues across restart",
                  f"{cycle_a} -> {cycle_b}")

            baseline = dtc_codes(client)
            check(DTC_TIMEOUT not in baseline, "no timeout DTC before triggering",
                  f"{[hex(c) for c in baseline]}")

            # 造一条真实故障
            trigger_isotp_timeout(bus)
            time.sleep(1.5)

            codes = dtc_codes(client)
            check(DTC_TIMEOUT in codes, "timeout produces a DTC",
                  f"{[hex(c) for c in codes]}")
    finally:
        ecu.stop()

    # ---------- C：再重启，故障应还在 ----------
    print("\nPhase C: restart again, DTC must survive")
    ecu = Ecu(reset=False)
    try:
        with open_client(bus, addr) as client:
            codes = dtc_codes(client)
            check(DTC_TIMEOUT in codes,
                  "DTC survives power cycle", f"{[hex(c) for c in codes]}")

            cycle_c, _ = read_dtc_stats(client)
            check(cycle_c == cycle_b + 1,
                  "cycle keeps increasing", f"{cycle_b} -> {cycle_c}")

            enter_extended_and_unlock(client)
            threshold = read_threshold(client)
            check(threshold == NEW_THRESHOLD,
                  "threshold still intact after second restart",
                  f"got {threshold}")

            # 清码后重启，故障应真的消失（证明清码也落盘了）。
            # 0x14 要求扩展会话，默认会话会被拒成 NRC 0x22；
            # enter_extended_and_unlock 已经切过去了。
            client.clear_dtc(0xFFFFFF)
            check(dtc_codes(client) == [], "clear removes the DTC",
                  f"{[hex(c) for c in dtc_codes(client)]}")
    finally:
        ecu.stop()

    # ---------- D：清码结果也要持久 ----------
    print("\nPhase D: clear must persist too")
    ecu = Ecu(reset=False)
    try:
        with open_client(bus, addr) as client:
            codes = dtc_codes(client)
            check(DTC_TIMEOUT not in codes,
                  "cleared DTC stays cleared after restart",
                  f"{[hex(c) for c in codes]}")
    finally:
        ecu.stop()

    if os.path.exists(NVM_PATH):
        os.remove(NVM_PATH)

    print(f"\nResult: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
