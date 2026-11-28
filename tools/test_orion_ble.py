#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the Orion BLE peripheral from a PC using Bleak."""

import argparse
import asyncio
import secrets
import sys

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError


SDK_SERVICE = "07af27a5-9c22-11ea-9afe-02fcdc4e7412"
SDK_READ = "07af27a6-9c22-11ea-9afe-02fcdc4e7412"
SDK_WRITE = "07af27a7-9c22-11ea-9afe-02fcdc4e7412"
SDK_INDICATE = "07af27a8-9c22-11ea-9afe-02fcdc4e7412"
SDK_NOTIFY = "07af27a9-9c22-11ea-9afe-02fcdc4e7412"
ORION_SERVICE = "79180b9c-f9dc-47b2-a71b-4cd98f201201"
ORION_VALUE = "79180b9c-f9dc-47b2-a71b-4cd98f201202"


def check(condition, message):
    if not condition:
        raise ValueError(message)


async def check_update(client, characteristic, expected, write=None):
    received = asyncio.get_running_loop().create_future()

    def on_update(_, data):
        if not received.done():
            received.set_result(bytes(data))

    await client.start_notify(characteristic, on_update)
    try:
        if write is not None:
            await client.write_gatt_char(characteristic, write, response=True)
        data = await asyncio.wait_for(received, timeout=5)
        check(data == expected, f"Unexpected GATT update payload {data.hex()}")
    finally:
        await client.stop_notify(characteristic)


async def smoke(args):
    example = args.profile == "sdk"
    name = "BTBLE-DEV" if example else "MicroPixel Orion"
    rssi = None

    def matches(device, advertisement):
        nonlocal rssi
        if advertisement.local_name != name:
            return False
        if args.address and device.address.casefold() != args.address.casefold():
            return False
        if not example and ORION_SERVICE not in advertisement.service_uuids:
            return False
        rssi = advertisement.rssi
        return True

    device = await BleakScanner.find_device_by_filter(matches, timeout=args.timeout)
    check(device is not None, f"Timed out waiting for {name} advertising")
    print(f"PASS ADV {name} {device.address} RSSI={rssi} dBm", flush=True)
    if args.scan_only:
        return

    async with BleakClient(device, timeout=args.timeout, winrt={"use_cached_services": False}) as client:
        print("PASS GAP connect", flush=True)
        service = client.services.get_service(SDK_SERVICE if example else ORION_SERVICE)
        check(service is not None, "Expected GATT service missing")
        print(f"PASS GATT discovery {service.uuid}", flush=True)
        if example:
            expected = bytes(range(1, 10))
            check(await client.read_gatt_char(SDK_READ) == expected, "SDK GATT read payload differs")
            print("PASS GATT read", flush=True)
            await client.write_gatt_char(SDK_WRITE, b"orion-request", response=True)
            await client.write_gatt_char(SDK_WRITE, b"orion-command", response=False)
            print("PASS GATT write request and command sent", flush=True)
            await check_update(client, SDK_NOTIFY, b"notify")
            print("PASS GATT notify", flush=True)
            await check_update(client, SDK_INDICATE, expected)
            print("PASS GATT indicate", flush=True)
        else:
            initial = await client.read_gatt_char(ORION_VALUE)
            check(0 < len(initial) <= 20, "Orion GATT initial value has invalid length")
            for index in range(args.rounds):
                payload = secrets.token_bytes(12)
                await check_update(client, ORION_VALUE, payload, write=payload)
                check(await client.read_gatt_char(ORION_VALUE) == payload,
                      f"GATT readback mismatch on round {index + 1}")
                if index + 1 < args.rounds:
                    await asyncio.sleep(0.25)
            print(f"PASS GATT write, notify and readback ({args.rounds} rounds)", flush=True)
    check(not client.is_connected, "GAP disconnect did not complete")
    print("PASS GAP disconnect", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=("sdk", "micropixel"))
    parser.add_argument("--address", help="Limit the scan to this device address")
    parser.add_argument("--scan-only", action="store_true")
    parser.add_argument("--timeout", type=float, default=15.0, help="Scan and connection timeout in seconds")
    parser.add_argument("--rounds", type=int, default=1, help="MicroPixel GATT round trips per connection")
    args = parser.parse_args()
    if args.rounds < 1 or (args.rounds != 1 and (args.profile != "micropixel" or args.scan_only)):
        parser.error("--rounds requires a positive count and a MicroPixel GATT test")
    try:
        asyncio.run(smoke(args))
    except (BleakError, TimeoutError, ValueError) as error:
        print(f"FAIL {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
