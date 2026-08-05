#!/usr/bin/env python3
"""Compare opaque UAD container payloads without extracting decoded material."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from inspect_uad_container import HEADER_SIZE, inspect, shannon_entropy


def longest_equal_run(left: bytes, right: bytes) -> int:
    longest = current = 0
    for left_byte, right_byte in zip(left, right):
        if left_byte == right_byte:
            current += 1
            longest = max(longest, current)
        else:
            current = 0
    return longest


def compare(left_path: Path, right_path: Path) -> dict[str, object]:
    left_meta = inspect(left_path)
    right_meta = inspect(right_path)
    left = left_path.read_bytes()[HEADER_SIZE:]
    right = right_path.read_bytes()[HEADER_SIZE:]
    common = min(len(left), len(right))
    equal = sum(a == b for a, b in zip(left, right))
    xor = bytes(a ^ b for a, b in zip(left, right))
    block_size = 16
    blocks = common // block_size
    equal_blocks = sum(
        left[offset : offset + block_size] == right[offset : offset + block_size]
        for offset in range(0, blocks * block_size, block_size)
    )
    left_words = struct.unpack_from("<16I", left_path.read_bytes())
    right_words = struct.unpack_from("<16I", right_path.read_bytes())
    return {
        "schema": 1,
        "left": {
            "name": left_path.name,
            "sha256": left_meta["sha256"],
            "magic": left_meta["magic"],
            "payload_bytes": len(left),
        },
        "right": {
            "name": right_path.name,
            "sha256": right_meta["sha256"],
            "magic": right_meta["magic"],
            "payload_bytes": len(right),
        },
        "common_payload_bytes": common,
        "equal_byte_count": equal,
        "equal_byte_fraction": round(equal / common, 9) if common else 0.0,
        "longest_equal_run": longest_equal_run(left, right),
        "equal_16_byte_blocks": equal_blocks,
        "compared_16_byte_blocks": blocks,
        "xor_entropy_bits_per_byte": round(shannon_entropy(xor), 6),
        "equal_header_words": [
            index for index, (a, b) in enumerate(zip(left_words, right_words)) if a == b
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="compare two opaque UAD containers")
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    args = parser.parse_args()
    try:
        result = compare(args.left, args.right)
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
