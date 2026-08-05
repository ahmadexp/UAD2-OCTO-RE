#!/usr/bin/env python3
"""Inspect and reproduce the host-side transform of a UAD2 "Bill" resource."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path


HEADER_SIZE = 20
MAGIC = b"Bill"
PRNG_MULTIPLIER = 0xBC8F
PRNG_MODULUS = 0x7FFFFFFF


def replacement_stream(resource_id: int, dwords: int) -> bytes:
    """Return the exact big-endian dword stream emitted by UAD2System.sys."""
    value = (~resource_id) & 0xFFFFFFFF
    output = bytearray()
    for _ in range(dwords):
        output.extend(value.to_bytes(4, "big"))
        value = (value * PRNG_MULTIPLIER) % PRNG_MODULUS
    return bytes(output)


def parse(data: bytes) -> dict[str, object]:
    if len(data) < HEADER_SIZE:
        raise ValueError(f"resource is shorter than the {HEADER_SIZE}-byte header")
    magic, resource_id, attributes, body_bytes, replacement_dwords = struct.unpack_from(
        "<4s4I", data
    )
    if magic != MAGIC:
        raise ValueError(f"unsupported resource magic {magic!r}")
    if body_bytes == 0 or replacement_dwords == 0:
        raise ValueError("body size and replacement count must be nonzero")
    if body_bytes + HEADER_SIZE != len(data):
        raise ValueError("declared body size does not match file size")

    resource_type = attributes & 0xFFFF
    payload_form = (attributes >> 16) & 0xFF
    dsp_generation = (attributes >> 24) & 0xFF
    if resource_type > 4:
        raise ValueError("resource type exceeds the official parser limit")
    if dsp_generation > 2:
        raise ValueError("DSP generation exceeds the official parser limit")
    if payload_form and resource_type == 0:
        raise ValueError("nonzero payload form requires a nonzero resource type")

    total_dwords = len(data) // 4
    if replacement_dwords > total_dwords - 13:
        raise ValueError("replacement region exceeds the official parser bound")
    preserved_bytes = len(data) - replacement_dwords * 4
    input_tail = data[preserved_bytes:]
    transformed_tail = replacement_stream(resource_id, replacement_dwords)
    transform_applied = payload_form != 0
    transformed = (
        data[:preserved_bytes] + transformed_tail if transform_applied else data
    )

    return {
        "magic": magic.decode("ascii"),
        "file_size": len(data),
        "resource_id": f"0x{resource_id:08x}",
        "attributes": f"0x{attributes:08x}",
        "resource_type": resource_type,
        "payload_form": payload_form,
        "dsp_generation": dsp_generation,
        "declared_body_bytes": body_bytes,
        "replacement_dwords": replacement_dwords,
        "preserved_bytes": preserved_bytes,
        "tail_transform_applied": transform_applied,
        "input_tail_sha256": hashlib.sha256(input_tail).hexdigest(),
        "transformed_tail_sha256": hashlib.sha256(transformed_tail).hexdigest(),
        "transformed_sha256": hashlib.sha256(transformed).hexdigest(),
        "input_tail_already_transformed": (
            input_tail == transformed_tail if transform_applied else None
        ),
        "transformed": transformed,
    }


def build_command(data: bytes, allocation_offset: int, pool_direction: str) -> bytes:
    """Build the exact two-dword resource envelope used by transmitResource."""
    if allocation_offset < 0 or allocation_offset > 0xFFFFFFFF:
        raise ValueError("allocation offset must fit in one dword")
    if len(data) % 4:
        raise ValueError("resource size must be dword aligned")
    result = parse(data)
    total_dwords = len(data) // 4 + 2
    if total_dwords >= 0x10000:
        raise ValueError("resource is too large for the short command word")
    if pool_direction == "low-to-high":
        command_base = 0x00010000
    elif pool_direction == "high-to-low":
        command_base = 0x00040000
    else:
        raise ValueError("pool direction must be low-to-high or high-to-low")
    return struct.pack(
        "<2I", command_base | total_dwords, allocation_offset
    ) + result["transformed"]


def inspect(path: Path) -> dict[str, object]:
    result = parse(path.read_bytes())
    return {key: value for key, value in result.items() if key != "transformed"}


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Parse a Bill resource and reproduce its host-side tail transform"
    )
    parser.add_argument("path", type=Path)
    parser.add_argument(
        "--write-transformed",
        type=Path,
        help="write the exact resource bytes produced by the official host transform",
    )
    parser.add_argument(
        "--write-command",
        type=Path,
        help="write the two-dword transmitResource envelope and payload",
    )
    parser.add_argument(
        "--allocation-offset",
        type=lambda value: int(value, 0),
        help="runtime pool offset required by --write-command",
    )
    parser.add_argument(
        "--pool-direction",
        choices=("low-to-high", "high-to-low"),
        help="runtime pool allocation direction required by --write-command",
    )
    args = parser.parse_args()

    result = parse(args.path.read_bytes())
    transformed = result.pop("transformed")
    if args.write_transformed:
        args.write_transformed.write_bytes(transformed)
        result["transformed_path"] = str(args.write_transformed)
    if args.write_command:
        if args.allocation_offset is None or args.pool_direction is None:
            parser.error(
                "--write-command requires --allocation-offset and --pool-direction"
            )
        command = build_command(
            args.path.read_bytes(), args.allocation_offset, args.pool_direction
        )
        args.write_command.write_bytes(command)
        result["command_path"] = str(args.write_command)
        result["command_dwords"] = len(command) // 4
        result["command_word"] = f"0x{int.from_bytes(command[:4], 'little'):08x}"
        result["allocation_offset"] = f"0x{args.allocation_offset:08x}"
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
