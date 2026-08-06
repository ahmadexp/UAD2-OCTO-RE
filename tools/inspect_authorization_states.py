#!/usr/bin/env python3
"""Verify and report the UAD 11.0.1 plug-in authorization-state decoder.

The verifier is hash locked to the exact analyzed binaries. It publishes
control-flow facts and state meanings, never authorization credentials or a
mechanism for changing the device table.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter
from pathlib import Path

from inspect_official_driver import PEImage


PERFMON_SHA256 = "18417006f5dab4e9f149d6f57e618e2c543f3ad1b9966b7be7052222086b9359"
SYSTEM_SHA256 = "d09a451cc5843266aa304770c03d936eff967f0e2ffcc9ee556a44c04c58106e"
PCIE_SHA256 = "d6f980bd3ae94f9206e71302f7ac6f6579e8d2778e92d5e7c34eb5b60d1d7f0a"


PERFMON_SIGNATURES = (
    (0x140163450, bytes.fromhex("458bc78bd7488bcee8f3010000"),
     "the formatter tests every device with the authorized-state helper"),
    (0x1401634A3, bytes.fromhex("443bf0750c"),
     "authorization on every device selects the all-devices message"),
    (0x1401634E5, bytes.fromhex("418bd7488bcee8b0030000"),
     "when no device is authorized the formatter tests auth-update-required"),
    (0x140163650, bytes.fromhex("48895c240848896c2410488974241857"),
     "authorized-state helper entry"),
    (0x14016374C, bytes.fromhex("418bc0488d0c80837c8d04000f94c0"),
     "authorized-state helper returns true only for internal state zero"),
    (0x140163760, bytes.fromhex("48895c24084889742410574883ec20"),
     "demo-days helper entry"),
    (0x140163813, bytes.fromhex("8b4c810483f901740ab8a401000083f9037506"),
     "demo-days helper accepts internal states one and three"),
    (0x140163826, bytes.fromhex("418b400c8907"),
     "demo-days helper reads the record's day field"),
    (0x1401638A0, bytes.fromhex("48895c2408574883ec20"),
     "auth-update-required helper entry"),
    (0x14016394C, bytes.fromhex("4b8d0489837c820404"),
     "auth-update-required helper tests internal state four"),
    (0x14048FB10, b"Authorized for %s %s only\0",
     "partial-device authorization UI text"),
    (0x14048FB30, b"Authorized for all devices\0",
     "all-device authorization UI text"),
    (0x14048FB58, b"Demo (%d %s left)\0",
     "active-demo UI text"),
    (0x14048FB70, b"Demo expired\0", "expired-demo UI text"),
    (0x14048FB90, b"Demo not started\0", "not-started-demo UI text"),
    (0x14048FBA8, b"Auth update required\0", "authorization-update UI text"),
)


SYSTEM_SIGNATURES = (
    (0x14000DFFC, bytes.fromhex("41bd0400000033db"),
     "the multi-device merge initializes the aggregate to internal state four"),
    (0x14000E1D6, bytes.fromhex("428b0c0285c9745183e901742983e901741183f90175"),
     "the merge dispatches over internal states zero through three"),
    (0x14000E1F1, bytes.fromhex("c70203000000"),
     "internal state three wins over non-authorized demo states"),
    (0x14000E204, bytes.fromhex("c70202000000"),
     "internal state two is retained unless authorization or expiry already won"),
    (0x14000E21A, bytes.fromhex("c70201000000"),
     "internal state one selects active-demo aggregation"),
    (0x14000E220, bytes.fromhex("428b4402083b42087314894208"),
     "active-demo aggregation keeps the minimum remaining-day value"),
    (0x14000E232, bytes.fromhex("891ab801000000d3e009420c"),
     "internal state zero wins and records an authorizing-device bit"),
)


PCIE_SIGNATURES = (
    (0x14000E187, bytes.fromhex("e830120000"),
     "GetPluginAuth refreshes the cached 768-entry wire table"),
    (0x14000E1E9, bytes.fromhex("428b4c8b24"),
     "the decoder reads one raw table dword from object offset 0x24"),
    (0x14000E1EE, bytes.fromhex("8d810000007fa9fffffffe7453"),
     "wire values 0x81000000 and 0x82000000 share the authorized branch"),
    (0x14000E1FB, bytes.fromhex("85c97508c70203000000"),
     "wire value zero maps to internal state three"),
    (0x14000E207, bytes.fromhex("81f9000000807508c70202000000"),
     "wire value 0x80000000 maps to internal state two"),
    (0x14000E217, bytes.fromhex("81f9000000837508c70204000000"),
     "wire value 0x83000000 maps to internal state four"),
    (0x14000E227, bytes.fromhex("418bc6c70201000000"),
     "any other wire value maps to internal state one"),
    (0x14000E230, bytes.fromhex("428b4c8b24f7c1ff0f00000f95c0c1e90c81e1ffff070003c1"),
     "active-demo remaining days use a rounded-up 12-bit sub-day field"),
    (0x14000E24E, bytes.fromhex("44893244895208"),
     "the shared 0x81/0x82 branch writes internal state zero and 0xffffffff"),
    (0x14000F4D7, bytes.fromhex("b800000083b90003000033d2f3ab"),
     "a refresh defaults all 768 wire entries to 0x83000000"),
    (0x14000F53A, bytes.fromhex("c70702001000"),
     "the first authorization request command is 0x00100002"),
    (0x14000F570, bytes.fromhex("c70002001100"),
     "the second authorization request command is 0x00110002"),
    (0x14000F70A, bytes.fromhex("41817d0002030380757c413975047576"),
     "the response must use header 0x80030302 and echo the request token"),
    (0x14000F722, bytes.fromhex("b900030000"),
     "the accepted authorization table contains 768 dwords"),
    (0x14000F74F, bytes.fromhex("488d4b2441b8000c0000"),
     "the driver copies exactly 3072 table bytes into its cache"),
)


def decode_wire_value(value: int) -> tuple[int, str, int | None]:
    """Return internal state, display meaning, and demo days for one dword."""
    value &= 0xFFFFFFFF
    if value in (0x81000000, 0x82000000):
        return 0, "authorized", None
    if value == 0:
        return 3, "demo expired", 0
    if value == 0x80000000:
        return 2, "demo not started", None
    if value == 0x83000000:
        return 4, "authorization update required", None
    days = ((value >> 12) & 0x7FFFF) + int(bool(value & 0xFFF))
    return 1, "demo active", days


def summarize_wire_values(values: list[int]) -> dict[str, object]:
    wire_counts = Counter(value & 0xFFFFFFFF for value in values)
    state_counts: Counter[str] = Counter()
    for value, count in wire_counts.items():
        state_counts[decode_wire_value(value)[1]] += count
    return {
        "entry_count": len(values),
        "wire_value_counts": {
            f"0x{value:08x}": count for value, count in sorted(wire_counts.items())
        },
        "display_state_counts": dict(sorted(state_counts.items())),
    }


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
        checks.append({
            "address": f"0x{address:016x}",
            "matches": observed == expected,
            "meaning": meaning,
        })
    return {
        "name": path.name,
        "sha256": observed_digest,
        "all_signatures_match": all(check["matches"] for check in checks),
        "signatures": checks,
    }


def inspect(perfmon: Path, system: Path, pcie: Path) -> dict[str, object]:
    binaries = [
        verify(perfmon, PERFMON_SHA256, PERFMON_SIGNATURES),
        verify(system, SYSTEM_SHA256, SYSTEM_SIGNATURES),
        verify(pcie, PCIE_SHA256, PCIE_SIGNATURES),
    ]
    states = []
    for wire in (0x00000000, 0x80000000, 0x81000000, 0x82000000, 0x83000000):
        internal, meaning, days = decode_wire_value(wire)
        states.append({
            "wire_value": f"0x{wire:08x}",
            "internal_state": internal,
            "display_meaning": meaning,
            "demo_days": days,
        })
    return {
        "schema": 1,
        "all_signatures_match": all(item["all_signatures_match"] for item in binaries),
        "response": {
            "request_commands": ["0x00100002", "0x00110002"],
            "response_header": "0x80030302",
            "token_must_match": True,
            "table_entries": 768,
            "table_bytes": 3072,
        },
        "states": states,
        "active_demo_rule": "days=((wire>>12)&0x7ffff)+bool(wire&0xfff)",
        "authorized_subtype_distinction": (
            "not exposed by this interface; 0x81000000 and 0x82000000 "
            "collapse to the same internal and display state"
        ),
        "can_modify_authorization": False,
        "binaries": binaries,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("perfmon", type=Path, help="extracted UADPerfMon executable")
    parser.add_argument("system", type=Path, help="extracted UAD2System.sys")
    parser.add_argument("pcie", type=Path, help="extracted UAD2Pcie.sys")
    args = parser.parse_args()
    try:
        result = inspect(args.perfmon, args.system, args.pcie)
    except (OSError, ValueError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["all_signatures_match"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
