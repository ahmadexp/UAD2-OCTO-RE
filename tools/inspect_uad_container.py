#!/usr/bin/env python3
"""Inspect a UAD binary container without loading or modifying hardware."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from collections import Counter
from pathlib import Path


HEADER_DWORDS = 16
HEADER_SIZE = HEADER_DWORDS * 4
KNOWN_MAGICS = {b"FBUT", b"GBUT", b"HBUT"}


def shannon_entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = Counter(data)
    length = len(data)
    return -sum((count / length) * math.log2(count / length) for count in counts.values())


def inspect(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    if len(data) < HEADER_SIZE:
        raise ValueError(f"container is shorter than the {HEADER_SIZE}-byte header")

    magic = data[:4]
    if magic not in KNOWN_MAGICS:
        shown = magic.decode("ascii", "backslashreplace")
        raise ValueError(f"unsupported container magic {shown!r}")

    words = struct.unpack_from("<16I", data)
    payload = data[HEADER_SIZE:]
    declared_payload_bytes = words[6] * 4
    size_matches = declared_payload_bytes == len(payload)

    return {
        "path": str(path),
        "sha256": hashlib.sha256(data).hexdigest(),
        "file_size": len(data),
        "header_size": HEADER_SIZE,
        "magic": magic.decode("ascii"),
        "header_words": [f"0x{word:08x}" for word in words],
        "compatibility_id": f"0x{words[3]:08x}",
        "declared_payload_dwords": words[6],
        "declared_payload_bytes": declared_payload_bytes,
        "actual_payload_bytes": len(payload),
        "declared_size_matches_file": size_matches,
        "opaque_header_tail": data[32:64].hex(),
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "payload_entropy_bits_per_byte": round(shannon_entropy(payload), 6),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Parse the fixed 64-byte header of an FBUT/GBUT/HBUT container"
    )
    parser.add_argument("container", type=Path)
    args = parser.parse_args()

    try:
        result = inspect(args.container)
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error

    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["declared_size_matches_file"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
