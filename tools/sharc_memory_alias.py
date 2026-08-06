#!/usr/bin/env python3
"""Translate ADSP-21469 block-0 DM32 spans to their PM48 aliases."""

from __future__ import annotations

import argparse
import json


BLOCK0_PM48_START = 0x0008C000
BLOCK0_PM48_END = 0x00093FFF
BLOCK0_DM32_START = 0x00092000
BLOCK0_DM32_END = 0x0009DFFF


def dm32_span_to_pm48(start: int, dwords: int) -> dict[str, object]:
    if dwords <= 0:
        raise ValueError("dword count must be positive")
    if start < BLOCK0_DM32_START or start > BLOCK0_DM32_END:
        raise ValueError("start is outside ADSP-21469 block-0 DM32")
    if dwords > BLOCK0_DM32_END - start + 1:
        raise ValueError("span exceeds ADSP-21469 block-0 DM32")

    delta = start - BLOCK0_DM32_START
    bit_offset = delta * 32
    bit_length = dwords * 32
    start_aligned = bit_offset % 48 == 0
    length_aligned = bit_length % 48 == 0
    result: dict[str, object] = {
        "dm32_start": f"0x{start:08x}",
        "dm32_dwords": dwords,
        "physical_bits": bit_length,
        "pm48_start_aligned": start_aligned,
        "pm48_length_aligned": length_aligned,
        "whole_pm48_span": start_aligned and length_aligned,
    }
    if start_aligned:
        pm_start = BLOCK0_PM48_START + bit_offset // 48
        result["pm48_start"] = f"0x{pm_start:08x}"
        if length_aligned:
            instructions = bit_length // 48
            result["pm48_words"] = instructions
            result["pm48_end_inclusive"] = f"0x{pm_start + instructions - 1:08x}"
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dm32_start", type=lambda value: int(value, 0))
    parser.add_argument("dwords", type=lambda value: int(value, 0))
    args = parser.parse_args()
    try:
        result = dm32_span_to_pm48(args.dm32_start, args.dwords)
    except ValueError as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
