#!/usr/bin/env python3
"""Decode nonzero 16-byte entries from captured UAD-2 ring pages."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path


RING_SIZE = 4 * 4096
ALL_DSP_RING_SIZE = 8 * 2 * RING_SIZE


def classify(words: tuple[int, int, int, int]) -> dict[str, object]:
    first, second, third, fourth = words
    result: dict[str, object] = {"kind": "inline-command"}
    if first & 0x80000000:
        result = {
            "kind": "dma-descriptor",
            "length_or_flags": f"0x{first & 0x7fffffff:08x}",
            "address": f"0x{(fourth << 32) | third:016x}",
        }
    elif first == 0 and second == 0 and (third != 0 or fourth != 0):
        result = {
            "kind": "address-only-record",
            "address": f"0x{(fourth << 32) | third:016x}",
        }
    return result


def decode_ring(data: bytes, dsp: int, ring: str) -> dict[str, object]:
    if len(data) != RING_SIZE:
        raise ValueError(f"ring must be exactly {RING_SIZE} bytes")
    entries = []
    for index in range(RING_SIZE // 16):
        words = struct.unpack_from("<4I", data, index * 16)
        if not any(words):
            continue
        entry = {
            "index": index,
            "words": [f"0x{word:08x}" for word in words],
        }
        entry.update(classify(words))
        entries.append(entry)
    return {"dsp": dsp, "ring": ring, "nonzero_entries": entries}


def analyze(data: bytes, layout: str, dsp: int = 0) -> dict[str, object]:
    if layout in {"command", "response"}:
        rings = [decode_ring(data, dsp, layout)]
    elif layout == "all-dsps":
        if len(data) != ALL_DSP_RING_SIZE:
            raise ValueError(
                f"all-DSP dump must be exactly {ALL_DSP_RING_SIZE} bytes"
            )
        rings = []
        for index in range(16):
            start = index * RING_SIZE
            rings.append(
                decode_ring(
                    data[start : start + RING_SIZE],
                    index // 2,
                    "command" if index % 2 == 0 else "response",
                )
            )
    else:
        raise ValueError(f"unsupported layout: {layout}")
    return {
        "schema": 1,
        "layout": layout,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "rings": rings,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument(
        "--layout", choices=("command", "response", "all-dsps"), required=True
    )
    parser.add_argument("--dsp", type=int, default=0)
    args = parser.parse_args()
    if not 0 <= args.dsp < 8:
        raise SystemExit("refusing: --dsp must be between 0 and 7")
    try:
        data = args.dump.read_bytes()
        result = analyze(data, args.layout, args.dsp)
    except (OSError, ValueError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
