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
    transformed = data[:preserved_bytes] + transformed_tail

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
        "input_tail_sha256": hashlib.sha256(input_tail).hexdigest(),
        "transformed_tail_sha256": hashlib.sha256(transformed_tail).hexdigest(),
        "transformed_sha256": hashlib.sha256(transformed).hexdigest(),
        "input_tail_already_transformed": input_tail == transformed_tail,
        "transformed": transformed,
    }


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
    args = parser.parse_args()

    result = parse(args.path.read_bytes())
    transformed = result.pop("transformed")
    if args.write_transformed:
        args.write_transformed.write_bytes(transformed)
        result["transformed_path"] = str(args.write_transformed)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
