#!/usr/bin/env python3
"""Verify the ordinary DSP resource loader in UAD 11.0.1 UAD2System.sys."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from inspect_official_driver import PEImage


KNOWN_SHA256 = "d09a451cc5843266aa304770c03d936eff967f0e2ffcc9ee556a44c04c58106e"

SIGNATURES = (
    (
        0x14000CC52,
        bytes.fromhex("418b0424f7d81bc981e10000030081c1000001000bcf"),
        "pool direction selects command class 0x00010000 or 0x00040000",
    ),
    (
        0x14000CD9D,
        bytes.fromhex("41bd00040000"),
        "resource copies are split at 0x400 dwords, or 4 KiB",
    ),
    (
        0x14000CEA0,
        bytes.fromhex("c78424e00000000a000000"),
        "completion wait is bounded to ten attempts",
    ),
    (
        0x14000CEB6,
        bytes.fromhex("81c158020000"),
        "ordinary completion wait adds 600 ms",
    ),
    (
        0x14000CFCE,
        bytes.fromhex("8954244841b904000000488b01418d5109"),
        "operation 13 requests a four-byte status output",
    ),
    (
        0x14000D099,
        bytes.fromhex("8138040007807511395804750c"),
        "intermediate success header is 0x80070004 and word one must be zero",
    ),
    (
        0x14000D0A6,
        bytes.fromhex("8b45283947080f84e9020000"),
        "intermediate success word two must equal the resource ID",
    ),
    (
        0x14000D0B2,
        bytes.fromhex("8b5704be0000ffff8bc223c63d000001f0"),
        "pre-completion status uses class 0xf0010000 in response word one",
    ),
    (
        0x14000D28B,
        bytes.fromhex(
            "3d440002800f85b4000000395f040f85c1000000395f080f85b8000000"
        ),
        "final response is 0x80020044 followed by two zero dwords",
    ),
    (
        0x14000D2AF,
        bytes.fromhex("3d000006f0"),
        "final response word three uses status class 0xf0060000",
    ),
    (
        0x14000D2F5,
        bytes.fromhex(
            "0fb7c283e801744683e801743a83e801742e83e801742283e801741683e803740a83f801"
        ),
        "final low status codes dispatch only 1 through 5, 8, and 9",
    ),
    (
        0x14000DE3E,
        bytes.fromhex("41b867000000"),
        "first firmware wrapper dispatches operation 0x67",
    ),
    (
        0x14000DE6E,
        bytes.fromhex("41b868000000"),
        "second firmware wrapper dispatches operation 0x68",
    ),
    (
        0x14000DE9E,
        bytes.fromhex("41b869000000"),
        "LoadFirmware wrapper dispatches operation 0x69",
    ),
    (
        0x14000DECE,
        bytes.fromhex("41b86a000000"),
        "LoadFPGAImage wrapper dispatches separate operation 0x6a",
    ),
    (
        0x140012F32,
        bytes.fromhex(
            "4183fc677509498b06488b4030eb1c4183fc687509498b06488b4038eb0d"
            "4183fc697521498b06488b4040"
        ),
        "operations 0x67, 0x68, and 0x69 select distinct target vtable slots",
    ),
)

FINAL_STATUS_TO_HOST_ERROR = {
    "0x0001": -91,
    "0x0002": -109,
    "0x0003": -93,
    "0x0004": -97,
    "0x0005": -98,
    "0x0008": -122,
    "0x0009": -123,
}


def inspect(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != KNOWN_SHA256:
        raise ValueError(
            f"driver hash mismatch: expected {KNOWN_SHA256}, found {digest}"
        )
    image = PEImage(data)
    checks = []
    for address, expected, meaning in SIGNATURES:
        observed = image.read_va(address, len(expected))
        checks.append(
            {
                "address": f"0x{address:016x}",
                "expected_hex": expected.hex(),
                "observed_hex": observed.hex(),
                "matches": observed == expected,
                "meaning": meaning,
            }
        )
    return {
        "schema": 1,
        "sha256": digest,
        "signature_count": len(checks),
        "all_signatures_match": all(check["matches"] for check in checks),
        "completion": {
            "intermediate_header": "0x80070004",
            "final_header": "0x80020044",
            "final_status_class": "0xf0060000",
            "all_zero_response_host_error": -38,
            "malformed_response_host_error": -50,
            "final_status_to_host_error": FINAL_STATUS_TO_HOST_ERROR,
        },
        "firmware_dispatch": {
            "operation_0x67_target_vtable_offset": "0x30",
            "operation_0x68_target_vtable_offset": "0x38",
            "operation_0x69_target_vtable_offset": "0x40",
            "operation_0x6a_is_separate": True,
            "automatic_0x67_0x68_bracketing_proven": False,
        },
        "signatures": checks,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path, help="extracted UAD2System.sys")
    args = parser.parse_args()
    try:
        result = inspect(args.driver)
    except (OSError, ValueError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["all_signatures_match"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
