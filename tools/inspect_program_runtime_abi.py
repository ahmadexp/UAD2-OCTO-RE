#!/usr/bin/env python3
"""Verify the recovered UAD2 program-resource runtime ABI.

The verifier is locked to the exact public, symbolized x86-64 driver used for
the analysis. It reports only control-flow and structure facts. It does not
emit vendor resource bytes or implement authorization changes.
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
        0x26042,
        bytes.fromhex("8b023d2c0a0000742941bfeaffffff3df00a0000"),
        "the converter accepts legacy 0x0a2c and native 0x0af8 allocation records",
    ),
    (
        0x260AC,
        bytes.fromhex(
            "8b93c800000041899688010000498dbe8c010000488db3cc000000"
            "48c1e204e8000000"
        ),
        "memory-spec count is native offset 0x188 and each entry is 16 bytes",
    ),
    (
        0x260D0,
        bytes.fromhex(
            "8b93cc0800004189968c090000498dbe90090000488db3d0080000"
            "48c1e203e800000000"
        ),
        "readback count is native offset 0x98c and each entry is 8 bytes",
    ),
    (
        0x1FAB1,
        bytes.fromhex(
            "41833f00b900000100ba000004000f44d14c896da04409ea8910488b"
        ),
        "resource transmission selects command base 0x00010000 or 0x00040000 and appends two header dwords",
    ),
    (
        0x1FB98,
        bytes.fromhex(
            "4c89e04883c00841c74424080400080045897c240c41c744241000000000"
            "45897424144989442448"
        ),
        "zero-filled private resources use command 0x00080004 with mapped offset, zero, and length",
    ),
    (
        0x2044E,
        bytes.fromhex("31d241bd02000300"),
        "pool-zero resource unmap selects command 0x00030002",
    ),
    (
        0x2C674,
        bytes.fromhex(
            "468bb4a1900900004c89e24589f44181e4ffffff004501ec"
            "468bacf994090000"
        ),
        "readback resolves the low 24-bit offset against a mapped resource and reads the requested length",
    ),
    (
        0x2C6C3,
        bytes.fromhex(
            "488d4308c7430804000c004489630c44896b104489731448894348"
            "8b849194090000488945c88d40028945b0488b45"
        ),
        "readback emits command 0x000c0004 and allocates requested length plus two response dwords",
    ),
    (
        0x22549,
        bytes.fromhex("4181ce00001500488945b04489304c"),
        "runtime memory-spec updates use command class 0x00150000",
    ),
    (
        0x22651,
        bytes.fromhex(
            "4489f8c1e8184885db74143b82880100000f83260100000382c40a0000"
            "eb0d413b86a80100000f831101000041bddeffffff413986200b00000f86"
            "0c010000498b8e280b000089c04c8b34c14d85f60f84f60000004989f5"
            "4181e7ffffff00498b064c89f7ff"
        ),
        "each memory spec splits a resource selector from a low 24-bit offset before mapped-address resolution",
    ),
    (
        0x22FE2,
        bytes.fromhex("c7410804001f004489690c8959104489491448894148"),
        "synchronous plug-in control uses the separate four-dword command 0x001f0004",
    ),
    (
        0x2A749,
        bytes.fromhex(
            "488b7b1883bb200b000000740c488b83280b0000488b30eb0231f6e8796b"
            "ffff8983d80b000083f8ff750d488b43188b40788983d80b0000"
        ),
        "SetDSPResourceManager resolves the first private resource and stores its mapped address at object offset 0x0bd8",
    ),
    (
        0x2BF1D,
        bytes.fromhex(
            "458bbed80b0000458ba66c0c0000817b20efbeadde741a488d3dbce40000"
            "488d3515e40000bab800000031c0e8000000004183cc04488d4308448963"
            "088b4dd4894b0c44896b1044897b144889434848"
        ),
        "Process places the request counter and stored first-private-resource address in the four-dword main command",
    ),
)


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
        "allocation_record": {
            "native_bytes": 0xAF8,
            "legacy_bytes": 0xA2C,
            "memory_spec_count_offset": "0x0188",
            "memory_spec_entry_bytes": 16,
            "readback_count_offset": "0x098c",
            "readback_entry_bytes": 8,
        },
        "memory_spec_update": {
            "command_class": "0x00150000",
            "resource_selector_bits": "31:24",
            "resource_offset_bits": "23:0",
            "resolved_value": "mapped_resource_base + resource_offset",
            "destination": "mapped_private_resource_base",
        },
        "resource_lifecycle": {
            "zero_command": "0x00080004",
            "zero_command_words": ["command", "mapped_offset", "zero", "length"],
            "pool_zero_unmap_command": "0x00030002",
        },
        "readback": {
            "command": "0x000c0004",
            "command_words": [
                "command",
                "mapped_resource_base + low24_offset",
                "requested_dwords",
                "original_resource_spec",
            ],
            "response_dwords": "requested_dwords + 2",
        },
        "synchronous_control": {
            "command": "0x001f0004",
            "is_general_entry_point": False,
        },
        "process": {
            "main_command_dwords": 4,
            "request_counter_word": 2,
            "resource_address_word": 3,
            "resource_address_source": "mapped first private resource",
            "plugin_object_address_offset": "0x0bd8",
        },
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
