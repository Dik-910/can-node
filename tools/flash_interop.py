#!/usr/bin/env python3
"""
flash_interop.py —— 用第三方实现跑完整刷写，验证 0x34/0x36/0x37

不是「自己发自己收」：udsoncan 按 ISO 14229 的规范发起刷写，
它不理解我们的代码，只在协议层面说话。能跑通才算符合协议。

验三件事：

1. **块大小协商正确**
   我们先回 maxNumberOfBlockLength，udsoncan 据此切块。
   如果这个值算错了（比如漏掉 SID+BSC 两字节），每块都会超界，
   服务端必然报 0x71 —— 但更糟的情况是「看起来成功，实际最后一块
   少两个字节」，靠 CRC 才能发现。

2. **每块都真的写进了存储器**
   刷完用标准 0x22 读 DID 0x0301 拿 CRC32 和长度，和本地算的比对。
   只比对「传输成功」是没有意义的 —— 传输成功但写错位置、
   写漏一块，流程照样全绿。

3. **大包经 ISO-TP 分包不丢不乱**
   2048 字节 = 9 块，每块 256 字节的 0x36 请求本身要拆成
   FF + 37 个 CF。序号错一个字节，CRC 就对不上。

用法：
    ./ecu-server --if vcan0 --seconds 40 &
    python3 tools/flash_interop.py
"""

import os
import sys
import zlib

for _cand in (os.environ.get("MG_PYLIB"), "/home/dikl/work/.pylibs"):
    if _cand and os.path.isdir(_cand):
        sys.path.insert(0, _cand)
        break

import can
import isotp
import udsoncan
from udsoncan import DataFormatIdentifier, MemoryLocation
from udsoncan.services import RoutineControl
from udsoncan.client import Client
from udsoncan.connections import PythonIsoTpConnection

ID_REQUEST = 0x7E0
ID_RESPONSE = 0x7E8
FLASH_BASE = 0x08000000
TOTAL_BYTES = 2048

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


def our_key_algo(level, seed, params):
    """与 can-node/src/uds/uds.c 的 default_key 保持一致"""
    seed_int = int.from_bytes(seed, "big")
    key = ((seed_int ^ 0x5A5A5A5A) + 0x12345678) & 0xFFFFFFFF
    return key.to_bytes(4, "big")


def make_payload(n):
    """
    确定性数据，不用随机数：失败时能逐字节复现，也便于比对。
    用 37 做步长是为了让每 256 字节的块内容都不一样，
    万一某一块被重复写或写错位置，CRC 立刻不同。
    """
    return bytes(((i * 37 + 11) & 0xFF) for i in range(n))


def did_bytes(resp, did):
    v = resp.service_data.values[did]
    return v[0] if isinstance(v, tuple) else v


def main():
    bus = can.interface.Bus(channel="vcan0", interface="socketcan")

    addr = isotp.Address(isotp.AddressingMode.Normal_11bits,
                         txid=ID_REQUEST, rxid=ID_RESPONSE)
    stack = isotp.CanStack(bus=bus, address=addr,
                           params={"blocksize": 8, "stmin": 5})
    conn = PythonIsoTpConnection(stack)

    cfg = dict(udsoncan.configs.default_client_config)
    cfg["security_algo"] = our_key_algo
    cfg["security_algo_params"] = None
    cfg["data_identifiers"] = {0x0301: "8s"}

    payload = make_payload(TOTAL_BYTES)
    expect_crc = zlib.crc32(payload) & 0xFFFFFFFF

    with Client(conn, request_timeout=5, config=cfg) as client:
        print("=== udsoncan flash interop (0x34 / 0x36 / 0x37) ===")
        print(f"payload {TOTAL_BYTES} bytes, expected crc32 0x{expect_crc:08X}\n")

        # ---------- 前置：解锁 ----------
        print("Prerequisites")
        r = client.change_session(0x03)
        check(r.positive, "extended session")

        r = client.unlock_security_access(1)
        check(r.positive, "security unlocked")

        # ---------- 0x34 ----------
        print("\nRequestDownload")
        r = client.request_download(
            memory_location=MemoryLocation(address=FLASH_BASE, memorysize=TOTAL_BYTES),
            dfi=DataFormatIdentifier(compression=0, encryption=0))
        check(r.positive, "0x34 accepted")

        max_block = getattr(r.service_data, "max_length", 0) if r.positive else 0
        check(max_block > 2, "server reported a block length", f"{max_block} bytes")

        # 块大小含 SID+BSC；udsoncan 不会替我们减，得自己算
        chunk = max(1, max_block - 2)
        print(f"  block size: {max_block} total, {chunk} bytes of payload")

        # ---------- 0x36 × N ----------
        print("\nTransferData")
        seq = 1
        offset = 0
        blocks = 0
        all_ok = True

        while offset < TOTAL_BYTES:
            n = min(chunk, TOTAL_BYTES - offset)
            r = client.transfer_data(sequence_number=seq, data=payload[offset:offset + n])

            if not (r and r.positive):
                all_ok = False
                print(f"  block {seq} at offset {offset} failed")
                break

            # 服务端必须原样回显序号，不匹配说明它对序号的实现有问题
            if r.service_data.sequence_number_echo != (seq & 0xFF):
                all_ok = False
                print(f"  block {seq} echoed wrong sequence")

            offset += n
            blocks += 1
            seq = (seq + 1) & 0xFF  # 0xFF 之后回绕到 0

        check(all_ok, "every block accepted", f"{blocks} blocks, {offset} bytes")

        # ---------- 0x37 ----------
        print("\nRequestTransferExit")
        r = client.request_transfer_exit()
        check(r.positive, "0x37 accepted")

        # ---------- 校验 1：0x31 让 ECU 自己算 ----------
        #
        # 这是真实刷写流程里的做法：tester 不读回全部数据去比对
        # （那样要占用大量带宽），而是发 0x31 让 ECU 自己算 CRC 回报。
        print("\nVerify via 0x31 checkMemory")
        opt = (FLASH_BASE.to_bytes(4, "big") + TOTAL_BYTES.to_bytes(4, "big"))

        r = client.routine_control(routine_id=0x0202,
                                   control_type=RoutineControl.ControlType.startRoutine,
                                   data=opt)
        check(r.positive, "0x31 start accepted")
        check(r.service_data.routine_id_echo == 0x0202, "routine id echoed")

        r = client.routine_control(routine_id=0x0202,
                                   control_type=RoutineControl.ControlType.requestRoutineResults)
        check(r.positive, "0x31 requestResults accepted")

        crc_from_ecu = None

        if r.positive:
            rec = r.service_data.routine_status_record
            check(rec is not None and len(rec) >= 4, "status record carries the CRC",
                  f"{len(rec) if rec else 0} bytes")

            if rec and len(rec) >= 4:
                crc_from_ecu = int.from_bytes(rec[0:4], "big")
                check(crc_from_ecu == expect_crc,
                      "CRC32 computed by the ECU matches",
                      f"got 0x{crc_from_ecu:08X}, expect 0x{expect_crc:08X}")

        # ---------- 校验 2：读 DID 交叉验证 ----------
        #
        # 两条路径（0x31 与 0x22）走的是不同代码，结果一致才说明
        # 「算 CRC」这件事没有在某一处被写歪。
        print("\nCross-check with 0x22 DID 0x0301")
        r = client.read_data_by_identifier([0x0301])
        check(r.positive, "digest DID readable")

        if r.positive:
            raw = did_bytes(r, 0x0301)
            if len(raw) == 8:
                did_crc = int.from_bytes(raw[0:4], "big")
                did_len = int.from_bytes(raw[4:8], "big")

                check(did_len == TOTAL_BYTES, "server recorded the right length",
                      f"{did_len}")
                check(crc_from_ecu is None or did_crc == crc_from_ecu,
                      "0x31 and 0x22 report the same CRC",
                      f"0x{did_crc:08X}")

    bus.shutdown()

    print(f"\nResult: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
