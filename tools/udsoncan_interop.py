#!/usr/bin/env python3
"""
用第三方实现验证我们的 ECU。

python-can + can-isotp + udsoncan 都是独立维护的开源库（udsoncan 743★），
它们不理解我们的代码，只按 ISO 14229 / ISO 15765-2 的规范说话。
能用它们跑通，才说明我们的实现符合协议，而不是「自己发的自己收」。

运行前先启动 ECU：
    ./ecu-server --seconds 60 &
    python3 tools/udsoncan_interop.py
"""

import sys
import time

import can
import isotp
import udsoncan
from udsoncan.client import Client
from udsoncan.connections import PythonIsoTpConnection
import udsoncan.services as uds

ID_REQUEST = 0x7E0
ID_RESPONSE = 0x7E8

passed = 0
failed = 0


def check(cond, what):
    global passed, failed
    if cond:
        print(f"  [PASS] {what}")
        passed += 1
    else:
        print(f"  [FAIL] {what}")
        failed += 1


def did_value(resp, did):
    """udsoncan 对 '17s' 这类格式返回 (bytes,)，统一取出来"""
    v = resp.service_data.values[did]
    return v[0] if isinstance(v, tuple) else v


def our_key_algo(level, seed, params):
    """与 can-node/src/uds/uds.c 里的 default_key 保持一致"""
    seed_int = int.from_bytes(seed, "big")
    key = ((seed_int ^ 0x5A5A5A5A) + 0x12345678) & 0xFFFFFFFF
    return key.to_bytes(4, "big")


def main():
    bus = can.interface.Bus(channel="vcan0", interface="socketcan")

    addr = isotp.Address(isotp.AddressingMode.Normal_11bits,
                         txid=ID_REQUEST, rxid=ID_RESPONSE)
    stack = isotp.CanStack(bus=bus, address=addr,
                           params={"blocksize": 8, "stmin": 5})
    conn = PythonIsoTpConnection(stack)

    config = dict(udsoncan.configs.default_client_config)
    config["security_algo"] = our_key_algo
    config["security_algo_params"] = None

    # udsoncan 要求显式声明每个 DID 的编码格式，否则拒绝发送读取请求。
    # 这里的长度必须和 ECU 侧 DID 表一致，不一致会解析失败——正好也验证了两边定义是否同步。
    config["data_identifiers"] = {
        0xF190: "17s",  # VIN
        0x0101: "2s",   # 电压，原始 2 字节
        0x0201: "1s",   # 阈值
        0x0999: "1s",   # 故意不存在，用来测负响应
    }

    with Client(conn, request_timeout=3, config=config) as client:
        print("=== udsoncan -> can-node ECU interop test ===")
        print(f"client: udsoncan {udsoncan.__version__}, "
              f"can-isotp {isotp.__version__}, python-can {can.__version__}")
        print()

        # 1. 会话控制
        print("TesterPresent / SessionControl")
        r = client.tester_present()
        check(r.positive, "tester_present accepted")

        r = client.change_session(uds.DiagnosticSessionControl.Session.extendedDiagnosticSession)
        check(r.positive, "change to extended session")
        check(r.service_data.session_echo == 0x03, "session echoed as 0x03")

        # 2. 读多帧 DID（17 字节 VIN，必须走 ISO-TP 多帧）
        print("\nReadDataByIdentifier (multi-frame)")
        r = client.read_data_by_identifier([0xF190])
        check(r.positive, "read 0xF190 accepted")
        if r.positive:
            vin = did_value(r, 0xF190)
            check(len(vin) == 17, f"VIN length is 17 (got {len(vin)})")
            check(vin == b"LSVAA1234567890AB", f"VIN content matches: {vin!r}")

        # 3. 读单帧 DID
        print("\nReadDataByIdentifier (single frame)")
        r = client.read_data_by_identifier([0x0101])
        check(r.positive, "read 0x0101 accepted")
        if r.positive:
            v = did_value(r, 0x0101)
            check(int.from_bytes(v, "big") == 3660, f"voltage = 3660 (got {int.from_bytes(v,'big')})")

        # 4. 负响应：不存在的 DID
        print("\nNegative response")
        try:
            r = client.read_data_by_identifier([0x0999])
            check(not r.positive, "unknown DID rejected")
            check(r.code == 0x31, f"NRC is 0x31 (got 0x{r.code:02X})")
        except udsoncan.exceptions.NegativeResponseException as e:
            check(e.response.code == 0x31, f"unknown DID -> NRC 0x31 (got 0x{e.response.code:02X})")

        # 5. 未解锁写，应被拒
        print("\nSecurity access")
        try:
            client.write_data_by_identifier(0x0201, bytes([0x3C]))
            check(False, "write without unlock should be rejected")
        except udsoncan.exceptions.NegativeResponseException as e:
            check(e.response.code == 0x33, f"write without unlock -> NRC 0x33 (got 0x{e.response.code:02X})")

        # 6. 解锁
        r = client.unlock_security_access(1)
        check(r.positive, "security access level 1 unlocked")

        # 7. 解锁后写入
        print("\nWriteDataByIdentifier")
        r = client.write_data_by_identifier(0x0201, bytes([0x3C]))
        check(r.positive, "write 0x0201 accepted after unlock")

        # 8. 读回验证
        r = client.read_data_by_identifier([0x0201])
        check(r.positive, "read back accepted")
        if r.positive:
            back = did_value(r, 0x0201)
            check(back == bytes([0x3C]), f"read back is 0x3C (got {back.hex()})")

    bus.shutdown()

    print(f"\nResult: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
