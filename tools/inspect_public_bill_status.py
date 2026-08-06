#!/usr/bin/env python3
"""Verify the public UAD2 Bill pre-completion status decoder.

The decoder is locked to one symbolized x86-64 macOS driver capture. It emits
only control-flow facts and host error mappings, not proprietary payload data.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


KNOWN_SHA256 = "7b664e8ad67b8104d9797defcc0c707fff55ae4981a725559ed9554f2f55cdf6"
PUBLIC_COMMIT = "910a8f413d33bc3489d0da8fc613870153ccb4f2"

SIGNATURES = (
    (
        0x1FE8B,
        bytes.fromhex(
            "c7459400000000498b7c2408488b07be0d000000488d5594b904000000ff5048"
        ),
        "the wait loop requests operation 13 with a four-byte output",
    ),
    (
        0x1FF6F,
        bytes.fromhex(
            "4989c68b400441813e040007804c8b65d0751f85c0751b418b4608488b4d"
            "c83b41207549e9a70200004c8b65d0e99e02000089c181e10000ffff81f9"
            "000001f0752b8d48f46683f9090f87be0100000fb7c1488d0df402000048"
            "6304814801c8"
        ),
        "the loader accepts exact Bill success and dispatches 0xf001 low codes",
    ),
    (
        0x2017C,
        bytes.fromhex(
            "6683f801743441bfc9ffffff6683f805742e41bfc7ffffffeb2641bfc4ff"
            "ffffeb1e41bfbeffffffeb1641bfb6ffffffeb0e41bfb5ffffffeb0641bf"
            "cfffffff488d05a54702"
        ),
        "the non-table branches map low codes one, five, and the default",
    ),
    (
        0x202BC,
        bytes.fromhex(
            "15fdffffdafeffffd2feffffd2feffffd2feffffe2feffffd2feffffd2fe"
            "ffffeafefffff2feffff"
        ),
        "the ten-entry relative jump table covers low codes 12 through 21",
    ),
)

EXPLICIT_STATUS_TO_HOST_ERROR = {
    1: -49,
    5: -55,
    12: -56,
    13: -60,
    14: -57,
    15: -57,
    16: -57,
    17: -66,
    18: -57,
    19: -57,
    20: -74,
    21: -75,
}
DEFAULT_HOST_ERROR = -57


def host_error(status_low: int) -> int:
    return EXPLICIT_STATUS_TO_HOST_ERROR.get(status_low & 0xFFFF, DEFAULT_HOST_ERROR)


def inspect(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != KNOWN_SHA256:
        raise ValueError(
            f"driver hash mismatch: expected {KNOWN_SHA256}, found {digest}"
        )
    checks = []
    for offset, expected, meaning in SIGNATURES:
        observed = data[offset : offset + len(expected)]
        checks.append(
            {
                "file_offset": f"0x{offset:08x}",
                "matches": observed == expected,
                "meaning": meaning,
            }
        )
    return {
        "schema": 1,
        "public_commit": PUBLIC_COMMIT,
        "sha256": digest,
        "all_signatures_match": all(check["matches"] for check in checks),
        "precompletion_status_class": "0xf0010000",
        "status_low_to_host_error": {
            f"0x{status:04x}": error
            for status, error in sorted(EXPLICIT_STATUS_TO_HOST_ERROR.items())
        },
        "unlisted_status_host_error": DEFAULT_HOST_ERROR,
        "semantic_names_recovered": False,
        "checks": checks,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path, help="hash-locked public x86-64 kext")
    args = parser.parse_args()
    try:
        result = inspect(args.driver)
    except (OSError, ValueError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["all_signatures_match"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
