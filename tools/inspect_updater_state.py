#!/usr/bin/env python3
"""Verify firmware-state and full-file dispatch paths in UAD 11.0.1."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from inspect_official_driver import PEImage


PERFMON_SHA256 = "18417006f5dab4e9f149d6f57e618e2c543f3ad1b9966b7be7052222086b9359"
CLIENT_SHA256 = "da5c7925d89a9e43574abcfebc4b2e03dd3b0059bf08d5f423ee51d5df8f89da"

PERFMON_SIGNATURES = (
    (0x14016107A, bytes.fromhex("41b9a8000000ff5068"), "GetSystemInfo requests 168 bytes through vtable slot 0x68"),
    (0x140161083, bytes.fromhex("83f8a4"), "status -0x5c is classified separately"),
    (0x140161BFB, bytes.fromhex("bf05000000"), "firmware-version comparison allows five retries"),
    (0x140161C33, bytes.fromhex("8d88c8000000"), "retry deadline advances by about 200 ms"),
    (0x14016219D, bytes.fromhex("ff5078"), "LoadBinFile allocates a buffer through vtable slot 0x78"),
    (0x1401621E5, bytes.fromhex("e846040300"), "LoadBinFile copies the complete requested byte count"),
    (0x1401621EA, bytes.fromhex("448b0f"), "dispatch reads magic from the copied buffer"),
    (0x14016228B, bytes.fromhex("ff5040"), "FBUT, GBUT, and HBUT use firmware-update vtable slot 0x40"),
)

CLIENT_SIGNATURES = (
    (0x180007431, bytes.fromhex("41b8ac000000"), "GetSystemInfo clears a 172-byte local record"),
    (0x180007454, bytes.fromhex("41b904000000"), "driver call returns a four-byte status"),
    (0x180007464, bytes.fromhex("c7442428b0000000"), "driver call uses a 176-byte request record"),
    (0x180007471, bytes.fromhex("418d516b"), "operation is 0x6f because r9 is four"),
    (0x180007482, bytes.fromhex("81ffa8000000"), "caller output is capped at 168 bytes"),
)


def verify(path: Path, digest: str, signatures: tuple[tuple[int, bytes, str], ...]) -> dict[str, object]:
    data = path.read_bytes()
    observed_digest = hashlib.sha256(data).hexdigest()
    if observed_digest != digest:
        raise ValueError(
            f"hash mismatch for {path.name}: expected {digest}, found {observed_digest}"
        )
    image = PEImage(data)
    checks = []
    for address, expected, meaning in signatures:
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
        "name": path.name,
        "sha256": observed_digest,
        "all_signatures_match": all(check["matches"] for check in checks),
        "signatures": checks,
    }


def inspect(perfmon: Path, client: Path) -> dict[str, object]:
    binaries = [
        verify(perfmon, PERFMON_SHA256, PERFMON_SIGNATURES),
        verify(client, CLIENT_SHA256, CLIENT_SIGNATURES),
    ]
    return {
        "schema": 1,
        "all_signatures_match": all(item["all_signatures_match"] for item in binaries),
        "binaries": binaries,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("perfmon", type=Path, help="extracted UADPerfMon executable")
    parser.add_argument("client", type=Path, help="extracted UAD2DriverClient DLL")
    args = parser.parse_args()
    try:
        result = inspect(args.perfmon, args.client)
    except (OSError, ValueError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["all_signatures_match"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
