#!/usr/bin/env python3
"""Guarded, read-only MMIO identity probe for a 64 KiB UA PCI endpoint."""

from __future__ import annotations

import argparse
import json
import mmap
import os
import pathlib
import struct
import sys


EXPECTED_VENDOR = 0x1A00
EXPECTED_DEVICE = 0x0002
EXPECTED_BAR0_SIZE = 0x10000

PROFILES = {
    "identity": [
        (0x0020, "identity_word_0"),
        (0x0024, "identity_word_1"),
        (0x0028, "identity_word_2"),
        (0x002C, "identity_word_3"),
        (0x2218, "fpga_revision"),
        (0x2234, "extended_capabilities"),
    ],
}


def dsp_register_base(dsp: int) -> int:
    """Return the per-DSP register window recovered from CPcieDSP."""
    return (0x2000 if dsp > 3 else 0) + dsp * 0x800


RESOURCE_LAYOUT_FIELDS = (
    (0x010, "pool0_base"),
    (0x014, "pool2_base"),
    (0x018, "pool1_base"),
    (0x01C, "pool3_base"),
    (0x184, "pool0_size"),
    (0x188, "pool2_size"),
    (0x18C, "pool1_size"),
    (0x190, "pool3_size"),
    (0x194, "pool2_scratch"),
    (0x198, "pool1_scratch"),
    (0x19C, "pool3_scratch"),
)
PROFILES["resource-layout"] = [
    (dsp_register_base(dsp) + relative, f"dsp{dsp}_{name}")
    for dsp in range(8)
    for relative, name in RESOURCE_LAYOUT_FIELDS
]


def read_hex(path: pathlib.Path) -> int:
    return int(path.read_text().strip(), 0)


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"refusing MMIO access: {message}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bdf", default="0000:03:00.0")
    parser.add_argument("--profile", choices=sorted(PROFILES), default="identity")
    parser.add_argument(
        "--acknowledge-read-side-effects",
        action="store_true",
        help="Required. Confirms that device MMIO reads can have side effects.",
    )
    args = parser.parse_args()

    if not args.acknowledge_read_side_effects:
        fail("missing --acknowledge-read-side-effects")

    base = pathlib.Path("/sys/bus/pci/devices") / args.bdf
    if not base.is_dir():
        fail(f"device does not exist: {args.bdf}")
    if read_hex(base / "vendor") != EXPECTED_VENDOR:
        fail("vendor ID is not 0x1a00")
    if read_hex(base / "device") != EXPECTED_DEVICE:
        fail("device ID is not 0x0002")
    if (base / "driver").exists():
        fail(f"a kernel driver is bound: {(base / 'driver').resolve().name}")

    with (base / "config").open("rb") as stream:
        header = stream.read(64)
    if len(header) < 64:
        fail("could not read the complete PCI configuration header")
    command = struct.unpack_from("<H", header, 4)[0]
    if command & 0x4:
        fail("PCI bus mastering is enabled")
    if not command & 0x2:
        fail("PCI memory decoding is disabled; enable the endpoint first")

    resources = (base / "resource").read_text().splitlines()
    start, end, _flags = (int(field, 16) for field in resources[0].split())
    bar_size = end - start + 1
    if bar_size != EXPECTED_BAR0_SIZE:
        fail(f"BAR0 is {bar_size} bytes, expected exactly 65536")

    resource0 = base / "resource0"
    fd = os.open(resource0, os.O_RDONLY | os.O_SYNC)
    try:
        region = mmap.mmap(
            fd,
            EXPECTED_BAR0_SIZE,
            flags=mmap.MAP_SHARED,
            prot=mmap.PROT_READ,
        )
        try:
            words = []
            for offset, name in PROFILES[args.profile]:
                raw = region[offset : offset + 4]
                value = struct.unpack("<I", raw)[0]
                words.append({
                    "offset": f"0x{offset:04x}",
                    "name": name,
                    "value": f"0x{value:08x}",
                    "bytes_le": raw.hex(),
                })
        finally:
            region.close()
    finally:
        os.close(fd)

    result = {
        "schema": 1,
        "bdf": args.bdf,
        "pci_id": "1a00:0002",
        "subsystem_id": (
            f"{read_hex(base / 'subsystem_vendor'):04x}:"
            f"{read_hex(base / 'subsystem_device'):04x}"
        ),
        "pci_command": f"0x{command:04x}",
        "profile": args.profile,
        "words": words,
    }
    if args.profile == "identity":
        identity_bytes = b"".join(
            bytes.fromhex(item["bytes_le"]) for item in words[:4]
        )
        result["identity_ascii"] = "".join(
            chr(byte) if 32 <= byte < 127 else "." for byte in identity_bytes
        )
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
