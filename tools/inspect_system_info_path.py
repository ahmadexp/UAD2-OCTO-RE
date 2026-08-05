#!/usr/bin/env python3
"""Verify the host-side UAD-2 GetSystemInfo path in UAD 11.0.1.

The verifier is intentionally hash locked.  It reports control-flow and data
sources, not proprietary bytes from either driver.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from inspect_official_driver import PEImage


SYSTEM_SHA256 = "d09a451cc5843266aa304770c03d936eff967f0e2ffcc9ee556a44c04c58106e"
PCIE_SHA256 = "d6f980bd3ae94f9206e71302f7ac6f6579e8d2778e92d5e7c34eb5b60d1d7f0a"

SYSTEM_SIGNATURES = (
    (0x14000E590, bytes.fromhex("41b8a8000000488b4160"), "wrapper requests a 168-byte device record through vtable slot 0x60"),
    (0x14000E5C2, bytes.fromhex("8b07bbb0000000"), "wrapper constructs a 176-byte outer result envelope"),
    (0x14000E5E1, bytes.fromhex("448bc3488d542420"), "wrapper copies the bounded envelope to the caller"),
)

PCIE_SIGNATURES = (
    (0x1400147F8, bytes.fromhex("f060004001000000"), "PCIe interface vtable points to the recovered GetSystemInfo implementation"),
    (0x140006124, bytes.fromhex("4885d20f847402000081fea8000000"), "implementation rejects null output and lengths above 168 bytes"),
    (0x140006139, bytes.fromhex("4439a1400c0000753a"), "device-state field at object offset 0xc40 gates the request"),
    (0x140006172, bytes.fromhex("b8a4ffffff"), "a cleared state field returns -92"),
    (0x14000617C, bytes.fromhex("33d2488d4c244441b8a4000000"), "implementation clears the 164-byte body after the length word"),
    (0x140006197, bytes.fromhex("488bcbe8fdfaffff"), "implementation calls the local base-record assembler"),
    (0x1400061D8, bytes.fromhex("8b4320"), "record construction reads cached object state"),
    (0x140006262, bytes.fromhex("4881c118220000"), "record construction reads BAR offset 0x2218"),
    (0x140006281, bytes.fromhex("4881c138220000"), "record construction reads BAR offset 0x2238"),
    (0x140006297, bytes.fromhex("4883c108"), "record construction reads BAR offset 0x8"),
    (0x1400062AA, bytes.fromhex("4883c104"), "record construction reads BAR offset 0x4"),
    (0x1400062D7, bytes.fromhex("e814060000"), "record construction copies a 40-byte cached subrecord"),
    (0x140006313, bytes.fromhex("4881c1ac010000"), "supported device families also read BAR offset 0x1ac"),
    (0x14000638D, bytes.fromhex("4c8bc6488d542440"), "implementation performs the final bounded copy"),
    (0x140005CD4, bytes.fromhex("c702a8000000"), "base assembler declares a 168-byte record"),
    (0x140005D2D, bytes.fromhex("4881c134220000"), "base assembler uses BAR offset 0x2234 as fallback identity state"),
    (0x140005D69, bytes.fromhex("0f10442440f30f7f4358"), "base assembler stores four BAR identity words at record offset 0x58"),
    (0x140004BB8, bytes.fromhex("48898b400c0000"), "constructor initializes the object state at offset 0xc40"),
    (0x140004F68, bytes.fromhex("c783400c000001000000"), "device start path sets the state field"),
    (0x14000715B, bytes.fromhex("83a1400c000000"), "device stop path clears the state field"),
    (0x14000E30B, bytes.fromhex("8983400c0000"), "base initialization sets the state field"),
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


def inspect(system: Path, pcie: Path) -> dict[str, object]:
    binaries = [
        verify(system, SYSTEM_SHA256, SYSTEM_SIGNATURES),
        verify(pcie, PCIE_SHA256, PCIE_SIGNATURES),
    ]
    return {
        "schema": 1,
        "all_signatures_match": all(item["all_signatures_match"] for item in binaries),
        "conclusion": {
            "record_size_bytes": 168,
            "outer_envelope_size_bytes": 176,
            "not_ready_status": -92,
            "readiness_object_offset": "0x0c40",
            "record_sources": ["cached driver object fields", "BAR MMIO"],
            "uses_dsp_command_ring": False,
            "is_valid_dsp_response_oracle": False,
        },
        "binaries": binaries,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("system", type=Path, help="extracted UAD2System.sys")
    parser.add_argument("pcie", type=Path, help="extracted UAD2Pcie.sys")
    args = parser.parse_args()
    try:
        result = inspect(args.system, args.pcie)
    except (OSError, ValueError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["all_signatures_match"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
