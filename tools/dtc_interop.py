#!/usr/bin/env python3
"""
dtc_interop.py —— 用第三方实现验证 0x19 / 0x14

关键点：**故障是真实产生的，不是插桩造出来的**。

触发方式：向 ECU 发一个首帧就收手，不再发连续帧。
ECU 侧的 ISO-TP 的 N_Cr 计时器（1 秒）到点后判定通信超时，
把它记成 DTC 0xC10087。整个过程里 ECU 只依赖自己的协议栈，
脚本这边甚至不解析 ECU 的任何内部状态。

然后让 udsoncan 按标准去读：

  1. 0x19 02 能不能把这条码按 <DTC3><status> 的格式吐出来
  2. 0x19 01 数量对不对
  3. 0x19 0A 支持列表里有没有它
  4. 0x19 04 快照能不能被通用 tester 解码
  5. 0x14 在默认会话里是否被拒绝、扩展会话里是否生效

第 5 条特别重要：如果清码不设会话门槛，任何接上总线的设备
都能把故障历史抹掉，售后就查不到真因了。

用法：
    ./ecu-server --if vcan0 --seconds 30 &
    python3 tools/dtc_interop.py
"""

import atexit
import os
import sys
import time

# 依赖装在固定目录里（本机 python3 没有 venv 支持，见 .pylibs）
for _cand in (os.environ.get("MG_PYLIB"), "/home/dikl/work/.pylibs"):
    if _cand and os.path.isdir(_cand):
        sys.path.insert(0, _cand)
        break

import can
import isotp
import udsoncan
from udsoncan.client import Client
from udsoncan.common.dtc import Dtc
from udsoncan.connections import PythonIsoTpConnection
from udsoncan.exceptions import NegativeResponseException

ID_REQUEST = 0x7E0
ID_RESPONSE = 0x7E8

DTC_TIMEOUT = 0xC10087  # 与 examples/ecu_server.c 保持一致
DID_FAULT_CONTEXT = 0x0001

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


def trigger_isotp_timeout(bus):
    """
    发首帧、不发连续帧。

    请求本身是 0x2E 写 DID 0x0201 加 20 字节填充，长度 23 字节，
    必须走多帧。ECU 收到首帧后会回流控帧、起 N_Cr 计时器，
    接着就等不到东西了 —— 1 秒后超时。
    """
    payload = bytes([0x2E, 0x02, 0x01]) + bytes(range(1, 21))
    ff = bytes([0x10 | ((len(payload) >> 8) & 0x0F), len(payload) & 0xFF]) + payload[:6]
    bus.send(can.Message(arbitration_id=ID_REQUEST, data=ff, is_extended_id=False))


def status_byte(s):
    """udsoncan 的状态字节封在 Dtc.Status 里，统一取成整数"""
    if hasattr(s, "get_byte_as_int"):
        return s.get_byte_as_int()
    return int(s)


def dtc_list(resp):
    """udsoncan 的 DTC 列表 -> [(dtcid, status_byte)]"""
    if resp is None:
        return []
    return [(int(d.id), status_byte(d.status)) for d in resp.service_data.dtcs]


def main():
    bus = can.interface.Bus(channel="vcan0", interface="socketcan")
    # 断言失败会抛异常，退出路径上不关总线会让 socketcan 打印
    # "not properly shut down"，CI 日志里看着像另一个错误
    atexit.register(bus.shutdown)

    addr = isotp.Address(isotp.AddressingMode.Normal_11bits,
                         txid=ID_REQUEST, rxid=ID_RESPONSE)
    stack = isotp.CanStack(bus=bus, address=addr,
                           params={"blocksize": 8, "stmin": 5})
    conn = PythonIsoTpConnection(stack)

    cfg = dict(udsoncan.configs.default_client_config)
    # 快照按 DID 解码，和 ECU 侧 snap_provider 发的 DID 0x0001 对应
    cfg["data_identifiers"] = {DID_FAULT_CONTEXT: "5s"}

    with Client(conn, request_timeout=5, config=cfg) as client:
        print("=== udsoncan DTC interop (0x19 / 0x14) ===\n")

        # ---------- 0. 基线：此刻应当没有故障 ----------
        print("Baseline")
        baseline = dtc_list(client.get_dtc_by_status_mask(0x0F))
        check(baseline == [], "no DTC before any fault",
              f"got {[hex(c) for c, _ in baseline]}")

        # ---------- 1. 造一个真实的通信超时 ----------
        print("\nTrigger: abandoned ISO-TP reception")
        trigger_isotp_timeout(bus)
        time.sleep(1.5)  # N_Cr 是 1000ms，留 500ms 余量

        # 超时后 ECU 的接收状态必须已经复位，否则下一帧请求会被当成连续帧
        resp = client.get_number_of_dtc_by_status_mask(0x0F)
        count = resp.service_data.dtc_count if resp else -1
        check(count >= 1, "timeout produces a DTC", f"count={count}")

        # ---------- 2. 按状态掩码读码 ----------
        print("\n0x19 02 report DTC by status mask")
        entries = dtc_list(client.get_dtc_by_status_mask(0x0F))
        codes = [c for c, _ in entries]

        check(DTC_TIMEOUT in codes, "the expected DTC is reported",
              f"{[hex(c) for c in codes]}")
        entry = next((s for c, s in entries if c == DTC_TIMEOUT), 0)
        check((entry & 0x02) != 0 and (entry & 0x04) != 0,
              "status has testFailedThisOperationCycle + pendingDTC",
              f"0x{entry:02X}")
        check((entry & 0x20) != 0, "status has testFailedSinceLastClear",
              f"0x{entry:02X}")

        # 用 admin/confirmed 掩码读不到的话，说明状态位写错了位
        confirmed_only = [c for c, _ in dtc_list(client.get_dtc_by_status_mask(0x08))]
        check(DTC_TIMEOUT not in confirmed_only,
              "a single timeout stays unconfirmed (needs a second power cycle)")

        # ---------- 3. 支持列表 ----------
        print("\n0x19 0A report supported DTC")
        supported = dtc_list(client.get_supported_dtc())
        check([c for c, _ in supported] == [DTC_TIMEOUT],
              "supported list holds exactly the declared DTC",
              f"{[hex(c) for c, _ in supported]}")

        # ---------- 4. 快照 ----------
        print("\n0x19 04 report snapshot")
        snap = client.get_dtc_snapshot_by_dtc_number(Dtc(DTC_TIMEOUT), 0x01)
        ok = snap is not None and len(snap.service_data.dtcs) == 1
        check(ok, "snapshot record 1 returned for the DTC")
        if ok:
            dtc = snap.service_data.dtcs[0]
            # 通用 tester 按 DID 解码：解不出来就说明记录格式（DID 个数那字节）错了
            check(len(dtc.snapshots) == 1, "one DID decoded from the snapshot record",
                  f"{[hex(s.did) for s in dtc.snapshots]}")
            if dtc.snapshots:
                s = dtc.snapshots[0]
                check(s.did == DID_FAULT_CONTEXT, "snapshot DID matches the ECU definition",
                      f"0x{s.did:04X}")
                check(s.record_number == 0x01, "snapshot record number echoed")
                check(len(s.raw_data) == 5, "snapshot payload is 5 bytes",
                      s.raw_data.hex())

        # ---------- 4b. 快照标识与按记录号读取 ----------
        #
        # 03 是 04 的前置：先问「哪些码有快照、用的几号记录」，才谈得上按需读。
        # udsoncan 把 03 的记录解析成 DTC+状态（记录号那一字节没单独暴露），
        # 所以这里直接校验原始载荷 —— 规范定义的本来就是这 4 个字节。
        print("\n0x19 03 report snapshot identification")
        resp = client.read_dtc_information(3)
        raw = resp.get_payload()

        check(raw[:2] == b"\x59\x03", "positive response 59 03", raw.hex())
        check(len(raw) == 6, "one record -> 2 + 4 bytes", raw.hex())

        want = bytes([(DTC_TIMEOUT >> 16) & 0xFF, (DTC_TIMEOUT >> 8) & 0xFF,
                      DTC_TIMEOUT & 0xFF])
        check(raw[2:5] == want, "record names the DTC that has a snapshot",
              raw[2:5].hex())
        check(raw[5] == 0x01, "record number is 1", f"0x{raw[5]:02X}")
        check(resp.service_data.dtc_count == 1, "udsoncan counts one record")

        # 05 是同一个内容的另一个入口：不经过 DTC，按记录号直接读。
        # 两个入口读到的东西必须一致，否则 04 和 05 其中之一编码错了。
        print("\n0x19 05 report stored data by record number")
        resp = client.read_dtc_information(5, snapshot_record_number=0x01)
        sd = resp.service_data

        check(sd.subfunction_echo == 5, "subfunction echoed", f"0x{sd.subfunction_echo:02X}")
        check(len(sd.dtcs) == 1 and int(sd.dtcs[0].id) == DTC_TIMEOUT,
              "record holds the expected DTC",
              f"{[hex(int(d.id)) for d in sd.dtcs]}")

        if sd.dtcs:
            snaps = sd.dtcs[0].snapshots
            check(len(snaps) == 1, "one DID decoded from the record",
                  f"{[hex(s.did) for s in snaps]}")
            if snaps:
                s = snaps[0]
                check(s.did == DID_FAULT_CONTEXT, "record DID matches the 04 path",
                      f"0x{s.did:04X}")
                check(len(s.raw_data) == 5, "record payload is 5 bytes",
                      s.raw_data.hex())

        # 不存在的记录号必须回 0x31，不能拿空响应糊过去
        try:
            client.read_dtc_information(5, snapshot_record_number=0x02)
            check(False, "unknown record number must be rejected")
        except NegativeResponseException as e:
            check(e.response.code == 0x31, "unknown record number -> NRC 0x31",
                  f"0x{e.response.code:02X}")

        # ---------- 5. 清码的会话门槛 ----------
        try:
            client.clear_dtc(0xFFFFFF)
            check(False, "clear in default session must be rejected")
        except NegativeResponseException as e:
            nrc = e.response.code
            check(nrc == 0x22, "clear in default session -> NRC 0x22",
                  f"0x{nrc:02X}")

        still = [c for c, _ in dtc_list(client.get_dtc_by_status_mask(0x0F))]
        check(DTC_TIMEOUT in still, "rejected clear leaves the DTC in place")

        client.change_session(0x03)
        client.clear_dtc(0xFFFFFF)
        check(True, "clear in extended session accepted")

        after = dtc_list(client.get_dtc_by_status_mask(0x0F))
        check(after == [], "nothing reported after clear",
              f"got {[hex(c) for c, _ in after]}")

        # 清完之后再触发一次，确认故障存储仍然可写（不是一次性状态）
        print("\nRe-arm after clear")
        trigger_isotp_timeout(bus)
        time.sleep(1.5)
        again = [c for c, _ in dtc_list(client.get_dtc_by_status_mask(0x0F))]
        check(DTC_TIMEOUT in again, "fault memory still records after a clear")

    print(f"\nResult: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
