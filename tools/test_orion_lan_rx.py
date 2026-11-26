#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure Orion LAN RX with the opt-in MPX_ORION_RX_BENCHMARK firmware."""

import argparse
import concurrent.futures
import fcntl
import json
import os
from pathlib import Path
import re
import select
import socket
import struct
import termios
import threading
import time

from capture_orion_frame import open_console


PROFILES = {
    "full": [0] * 15,
    "small": [160] * 8 + [320] * 7,
    "mixed": [160] * 4 + [320] * 4 + [0] * 7,
}
PAYLOAD = memoryview(bytes(65536))
PASS_DRAIN_SECONDS = 12
DIAGNOSTIC_DRAIN_SECONDS = 30


class Console:
    def __init__(self, fd):
        self.fd = fd
        self.data = bytearray()

    def send(self, command):
        mark = len(self.data)
        os.write(self.fd, ("\r" + command + "\r").encode())
        return mark

    def expect(self, pattern, mark=0, seconds=8):
        deadline = time.monotonic() + seconds
        while True:
            found = re.search(pattern, self.data[mark:])
            if found:
                return found
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.fd], [], [], remaining)[0]:
                raise TimeoutError(f"UART response missing {pattern!r}")
            try:
                chunk = os.read(self.fd, 8192)
            except BlockingIOError:
                continue
            if not chunk:
                raise RuntimeError("Orion console disconnected")
            self.data.extend(chunk)
            if re.search(rb"mepc:[0-9a-f]{8}|vApplicationStackOverflowHook", chunk, re.I):
                raise RuntimeError("Orion firmware fault")

    def setup(self):
        self.data.clear()
        fcntl.ioctl(self.fd, termios.TIOCMBIS, struct.pack("i", termios.TIOCM_DTR))
        fcntl.ioctl(self.fd, termios.TIOCMBIS, struct.pack("i", termios.TIOCM_RTS))
        time.sleep(.12)
        termios.tcflush(self.fd, termios.TCIFLUSH)
        fcntl.ioctl(self.fd, termios.TIOCMBIC, struct.pack("i", termios.TIOCM_RTS))
        self.expect(rb"MPX BOOT home_setup_us=", seconds=25)
        self.expect(rb"MicroPixel Wi-Fi connected with IPv4", seconds=65)
        self.send("mpx stop weather")
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            mark = self.send("mpx list")
            self.expect(rb"main_failure [^\r\n]+", mark)
            if re.search(rb"(?m)^weather stopped exit=0", self.data[mark:]):
                return
            time.sleep(.2)
        raise TimeoutError("Weather Session did not release its TCP connection")

    def snapshot(self, label, detailed=False):
        mark = self.send("lanrx flows" if detailed else "lanrx stats")
        row = self.expect(rb"LAN RX [^\r\n]+\r?\n", mark)[0].strip()
        print(label, row.decode(), flush=True)
        result = {key.decode(): int(value) for key, value in re.findall(rb"(\w+)=(\d+)", row)}
        if detailed:
            self.expect(rb"LAN FLOWS end", mark)
            result["flows"] = {}
            for flow in re.findall(rb"LAN FLOW [^\r\n]+", self.data[mark:]):
                print(label, flow.decode(), flush=True)
                fields = {key.decode(): int(value) for key, value in re.findall(rb"(\w+)=(\d+)", flow)}
                if fields.get("id"):
                    result["flows"][fields["id"]] = fields
        return result


def tcp_info(sock):
    # Stable field offsets of the Linux UAPI struct tcp_info.
    data = sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 256)
    fields = {
        "state": (0, "B"), "options": (5, "B"), "unacked": (24, "I"), "sacked": (28, "I"), "lost": (32, "I"),
        "rtt_us": (68, "I"), "total_retrans": (100, "I"), "bytes_acked": (120, "Q"),
        "notsent": (144, "I"), "data_segs_out": (156, "I"), "bytes_retrans": (208, "Q"),
        "snd_wnd": (228, "I"),
    }
    return {key: struct.unpack_from("=" + kind, data, offset)[0]
            for key, (offset, kind) in fields.items()
            if len(data) >= offset + struct.calcsize("=" + kind)}


def transfer(peer, cap, index, barrier, gate, clock, rate, use_ids):
    sent = 0
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        try:
            sock.settimeout(4)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
            if cap:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, cap)
            time.sleep(index * .025)
            sock.connect(peer)
        except OSError as error:
            barrier.wait(timeout=10)
            return {"mss_cap": cap, "error": f"connect: {error}"}
        result = {"id": index + 1, "mss_cap": cap, "mss": sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG),
                  "source_port": sock.getsockname()[1]}
        barrier.wait(timeout=10)
        if not gate.wait(16) or not clock:
            return {**result, "error": "start gate failed"}
        try:
            sock.settimeout(.25)
            if use_ids:
                sock.sendall(bytes((index + 1,)))
            interval = 1 / rate if rate else 0
            next_send = clock[0] + interval * index / len(PROFILES["small"])
            segment = PAYLOAD[:result["mss"]] if rate else PAYLOAD
            while time.monotonic() < clock[1]:
                if rate:
                    time.sleep(max(0, next_send - time.monotonic()))
                    if time.monotonic() >= clock[1]:
                        break
                try:
                    sent += sock.send(segment)
                except TimeoutError:
                    pass
                if rate:
                    # Allow a late write, but never catch up with a burst.
                    next_send = max(next_send + interval, time.monotonic() + interval * .75)
            result["sent"] = sent
            result["stream_end"] = tcp_info(sock)
            sock.shutdown(socket.SHUT_WR)
            response = bytearray()
            drain_start = time.monotonic()
            for limit, key in ((PASS_DRAIN_SECONDS, "at_timeout"),
                               (DIAGNOSTIC_DRAIN_SECONDS, "diagnostic_end")):
                while len(response) < 4:
                    remaining = drain_start + limit - time.monotonic()
                    if remaining <= 0:
                        break
                    sock.settimeout(remaining)
                    try:
                        block = sock.recv(4 - len(response))
                    except TimeoutError:
                        break
                    if not block:
                        return {**result, "error": "EOF before final byte count", "final": tcp_info(sock)}
                    response.extend(block)
                if len(response) == 4:
                    received, = struct.unpack("!I", response)
                    return {**result, "received": received, "passed": received == sent and key == "at_timeout",
                            "drain_s": round(time.monotonic() - drain_start, 3), "final": tcp_info(sock)}
                result[key] = tcp_info(sock)
            return {**result, "error": "drain timed out"}
        except OSError as error:
            return {**result, "sent": sent, "error": str(error), "final": tcp_info(sock)}


def exercise(console, profile, rate, seconds, watermark):
    label = f"{profile}/{rate or 'full'}"
    mark = console.send(f"lanrx start {watermark}" if watermark else "lanrx start")
    ready = console.expect(rb"LAN READY address=([.0-9]+) port=(\d+) window=(\d+) pool_count=(\d+) "
                           rb"pool_bytes=(\d+) pcb=(\d+)( ids=1)?\r?\n", mark)
    caps = PROFILES[profile]
    peer = (ready[1].decode(), int(ready[2]))
    pool_count, pool_bytes, pcbs = map(int, ready.groups()[3:6])
    use_ids = ready[7] is not None
    if pcbs != len(caps) or pool_count * 1680 != pool_bytes:
        raise RuntimeError(f"Unexpected RX configuration {ready[0].decode()}")
    print("LAN START", label, ready[0].decode().strip(), flush=True)
    barrier = threading.Barrier(len(caps) + 1)
    gate = threading.Event()
    clock = []
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(caps)) as workers:
            futures = [workers.submit(transfer, peer, cap, i, barrier, gate, clock, rate, use_ids)
                       for i, cap in enumerate(caps)]
            try:
                barrier.wait(timeout=10)
                baseline = console.snapshot(label + " connected", detailed=True)
                if baseline["pcb_used"] != len(caps):
                    raise RuntimeError(f"Expected {len(caps)} connected TCP PCBs")
                start = time.monotonic()
                clock[:] = [start, start + seconds]
            finally:
                gate.set()

            samples = []
            offsets = [1, 2, *range(4, seconds, 2), seconds - .5, seconds + 2, seconds + 12, seconds + 26]
            for offset in sorted({n for n in offsets if n < seconds - .25 or n > seconds}):
                wait = max(0, start + offset - time.monotonic())
                _, pending = concurrent.futures.wait(futures, timeout=wait)
                if not pending:
                    break
                detail = offset == seconds - .5 or offset > seconds
                sample = console.snapshot(f"{label} at={offset}s", detailed=detail)
                samples.append({**sample, "offset": offset})
            results = [future.result(timeout=8) for future in futures]
        final = console.snapshot(label + " final", detailed=True)
    finally:
        mark = console.send("lanrx stop")
        closed = console.expect(rb"LAN CLOSED [^\r\n]+\r?\n", mark)[0]
        print(label, closed.decode().strip(), flush=True)

    successes = [row for row in results if row.get("passed")]
    for flow in results:
        if "id" in flow:
            flow["board_samples"] = [{"offset": sample["offset"], **sample["flows"][flow["id"]]}
                                     for sample in samples if flow["id"] in sample.get("flows", {})]
            if flow["id"] in final.get("flows", {}):
                flow["board_final"] = final["flows"][flow["id"]]
    total = sum(row["received"] for row in successes)
    stats = {
        "profile": profile, "rate": rate, "seconds": seconds, "pool": pool_count, "watermark": watermark,
        "complete": len(successes), "complete_by_30s": sum(
            row.get("received") == row.get("sent") and "received" in row for row in results),
        "validated_mbps": round(total * 8e-6 / seconds, 3),
        "rx_free_low": final["rx_free_low"], "empty_checks": final["empty_checks"] - baseline["empty_checks"],
        "heap_peak": final["mem_peak"], "heap_errors": final["mem_err"] - baseline["mem_err"],
        "seg_peak": final["seg_peak"], "seg_errors": final["seg_err"] - baseline["seg_err"],
        "ack_mem_errors": final["ack_mem_err"] - baseline["ack_mem_err"],
        "ack_calls": final["ack_calls"] - baseline["ack_calls"],
        "ack_other_errors": final["ack_other_err"] - baseline["ack_other_err"],
        "low_water_hits": final.get("low_hits", 0) - baseline.get("low_hits", 0),
        "ooseq_peak_sampled": max(row["ooseq_pbufs"] for row in [*samples, final]),
        "flows": results,
    }
    print("LAN RESULT " + json.dumps(stats), flush=True)
    return stats


def parse_case(case):
    profile, _, rate = case.partition(":")
    if profile not in PROFILES or (rate and (not rate.isdigit() or not 0 < int(rate) <= 10000)):
        raise argparse.ArgumentTypeError(f"Expected small, mixed or full, optionally followed by :rate, got {case}")
    return profile, int(rate or 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("serial_port", type=Path)
    parser.add_argument("cases", type=parse_case, nargs="+", metavar="PROFILE[:PPS]")
    parser.add_argument("--seconds", type=int, default=18)
    parser.add_argument("--low-watermark", type=int, choices=range(0, 64), default=0,
                        metavar="BUFFERS", help="Keep one OOSEQ pbuf per flow when RX free falls to this level")
    parser.add_argument("--strict", action="store_true", help="Return failure if any case has an incomplete flow")
    args = parser.parse_args()
    if not 3 <= args.seconds <= 120:
        parser.error("--seconds must be between 3 and 120")
    failed = False
    with open_console(args.serial_port) as fd:
        console = Console(fd)
        reboot = True
        for profile, rate in args.cases:
            if reboot:
                console.setup()
            result = exercise(console, profile, rate, args.seconds, args.low_watermark)
            reboot = result["complete"] != len(PROFILES[profile])
            failed |= reboot
        mark = console.send("mpx perf")
        console.expect(rb"MPX PERF end", mark, seconds=12)
        for row in re.findall(rb"MPX HEAP (?:OCRAM|WRAM|PSRAM)[^\r\n]+", console.data[mark:]):
            print(row.decode(), flush=True)
    return int(args.strict and failed)


if __name__ == "__main__":
    raise SystemExit(main())
