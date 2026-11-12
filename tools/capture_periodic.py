#!/usr/bin/env python3
"""
实测周期报文的发送间隔。

抓 vcan0 上节点发出的周期帧，统计实际间隔与抖动。
注意这测的是**软件调度**，不是总线时序——vcan 没有位时间，
真实的总线占用要按 docs/architecture.md 里的方法计算，或用硬件测。

用法：
    ./node --seconds 15 &
    python3 tools/capture_periodic.py --seconds 12
"""

import argparse
import statistics
import sys
import time

import can

WATCH = {
    0x180: "NodeStatus",
    0x181: "BusStatus",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--channel", default="vcan0")
    args = ap.parse_args()

    bus = can.interface.Bus(channel=args.channel, interface="socketcan")
    stamps = {k: [] for k in WATCH}

    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        msg = bus.recv(timeout=0.2)
        if msg is None:
            continue
        if msg.arbitration_id in WATCH:
            stamps[msg.arbitration_id].append(time.monotonic())

    bus.shutdown()

    print(f"captured {args.seconds:.0f}s on {args.channel}\n")
    print(f"{'ID':<8}{'name':<13}{'count':>6}{'avg(ms)':>10}{'min':>8}{'max':>8}{'jitter':>9}")
    print("-" * 62)

    any_data = False
    for cid, name in WATCH.items():
        ts = stamps[cid]
        if len(ts) < 2:
            print(f"0x{cid:03X}  {name:<13}{len(ts):>6}  (not enough samples)")
            continue

        any_data = True
        gaps = [(b - a) * 1000.0 for a, b in zip(ts, ts[1:])]
        avg = statistics.mean(gaps)
        jitter = max(gaps) - min(gaps)
        print(f"0x{cid:03X}  {name:<13}{len(ts):>6}{avg:>10.2f}{min(gaps):>8.2f}{max(gaps):>8.2f}{jitter:>9.2f}")

    if not any_data:
        print("\nno periodic frames captured. is the node running on this channel?")
        return 1

    print("\nnote: interval here is software scheduling, not bus timing.")
    print("vcan has no bit rate; bus occupancy must be computed (see examples/bus_load_test.c).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
