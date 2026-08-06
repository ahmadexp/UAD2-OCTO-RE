#!/usr/bin/env python3
"""Generate separately rounded binary32 reference vectors for affine tests."""

from __future__ import annotations

import argparse
import json
import math
import struct


MAX_FRAMES = 64
CANONICAL_SCALE = 0.625
CANONICAL_BIAS = -0.09375
CANONICAL_INPUT = (
    -1.0,
    -0.75,
    -0.5,
    -0.125,
    0.0,
    0.125,
    0.3333333432674408,
    0.5,
    0.75,
    1.0,
)


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", f32(value)))[0]


def affine_one(value: float, scale: float, bias: float) -> float:
    x = f32(value)
    a = f32(scale)
    b = f32(bias)
    product = f32(x * a)
    return f32(product + b)


def generate(values: list[float], scale: float, bias: float) -> dict[str, object]:
    if not values or len(values) > MAX_FRAMES:
        raise ValueError(f"frame count must be in 1..{MAX_FRAMES}")
    if not all(math.isfinite(value) for value in [*values, scale, bias]):
        raise ValueError("only finite binary32 values are allowed")

    inputs = [f32(value) for value in values]
    outputs = [affine_one(value, scale, bias) for value in inputs]
    return {
        "schema": 1,
        "operation": "y = round_binary32(round_binary32(x * scale) + bias)",
        "frames": len(inputs),
        "scale": {"value": f32(scale), "bits": f"0x{bits(scale):08x}"},
        "bias": {"value": f32(bias), "bits": f"0x{bits(bias):08x}"},
        "input": [
            {"index": index, "value": value, "bits": f"0x{bits(value):08x}"}
            for index, value in enumerate(inputs)
        ],
        "expected_output": [
            {"index": index, "value": value, "bits": f"0x{bits(value):08x}"}
            for index, value in enumerate(outputs)
        ],
        "guard_before": "0x5a5aa5a5",
        "guard_after": "0xa5a55a5a",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scale", type=float, default=CANONICAL_SCALE)
    parser.add_argument("--bias", type=float, default=CANONICAL_BIAS)
    parser.add_argument("values", nargs="*", type=float)
    args = parser.parse_args()
    values = args.values or list(CANONICAL_INPUT)
    try:
        result = generate(values, args.scale, args.bias)
    except ValueError as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
