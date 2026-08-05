#!/usr/bin/env python3
"""Verify the HBUT loader path in the exact UAD 11.0.1 UAD2Pcie.sys."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from inspect_official_driver import PEImage


KNOWN_SHA256 = "d6f980bd3ae94f9206e71302f7ac6f6579e8d2778e92d5e7c34eb5b60d1d7f0a"

SIGNATURES = (
    (0x14000E390, bytes.fromhex("c744243088130000"), "pre-load wrapper timeout is 5000 ms"),
    (0x14000E39F, bytes.fromhex("ba00000d00"), "pre-load wrapper command is 0x000d0000"),
    (0x14000E3A4, bytes.fromhex("41b800000580"), "pre-load response class is 0x80050000"),
    (0x14000E424, bytes.fromhex("c7442430f0490200"), "LoadFirmware timeout is 150000 ms"),
    (0x14000E439, bytes.fromhex("ba00001200"), "LoadFirmware command base is 0x00120000"),
    (0x14000E43E, bytes.fromhex("41b800000480"), "expected response class is 0x80040000"),
    (0x14000E46C, bytes.fromhex("c740e888130000"), "post-load wrapper timeout is 5000 ms"),
    (0x14000E481, bytes.fromhex("ba00000e00"), "post-load wrapper command is 0x000e0000"),
    (0x14000E486, bytes.fromhex("41b800000680"), "post-load response class is 0x80060000"),
    (0x14000EA5D, bytes.fromhex("81fffeff0000"), "extended form begins above 0xfffe dwords"),
    (0x14000EAE7, bytes.fromhex("81fffeff0000760d"), "large payload selects extended header"),
    (0x14000EAEF, bytes.fromhex("8d4702bf00000040894304"), "extended length adds two and command sets bit 30"),
    (0x14000EB3D, bytes.fromhex("488bd0498bcde82085ffff"), "header is emitted through the DMA descriptor initializer"),
    (0x14000EBA3, bytes.fromhex("41c7460804000080"), "response descriptor requests four dwords"),
    (0x14000EBF5, bytes.fromhex("25ff0f0000"), "payload page count includes source page offset"),
    (0x14000EC4E, bytes.fromhex("25ff0f00007410"), "each payload descriptor stops at a 4 KiB boundary"),
    (0x140007088, bytes.fromhex("4181f900000100"), "descriptor length must be below 0x10000 dwords"),
    (0x1400070B0, bytes.fromhex("83670c000fbaee1f"), "descriptor word one is zero and valid bit 31 is set"),
    (0x1400070B8, bytes.fromhex("895f1048c1eb20895f14"), "descriptor carries low and high DMA address words"),
)


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
        "signatures": checks,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    args = parser.parse_args()
    try:
        result = inspect(args.driver)
    except (OSError, ValueError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["all_signatures_match"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
