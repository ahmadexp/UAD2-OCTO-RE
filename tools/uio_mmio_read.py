#!/usr/bin/env python3
"""Read the OCTO identity allowlist through an in-tree UIO PCI mapping."""

from __future__ import annotations

import argparse
import glob
import json
import mmap
import os
import pathlib
import struct
import time


EXPECTED_VENDOR = 0x1A00
EXPECTED_DEVICE = 0x0002
EXPECTED_SUBVENDOR = 0x1A00
EXPECTED_SUBDEVICE = 0x0005
EXPECTED_BAR0_SIZE = 0x10000
EXPECTED_DRIVER = "uio_pci_generic"

IDENTITY_WORDS = [
    (0x0020, "identity_word_0"),
    (0x0024, "identity_word_1"),
    (0x0028, "identity_word_2"),
    (0x002C, "identity_word_3"),
    (0x2218, "fpga_revision"),
    (0x2234, "extended_capabilities"),
]


def dsp_bank(dsp: int) -> int:
    if dsp < 4:
        return 0x2000 + dsp * 0x80
    return 0x5E00 + dsp * 0x80


def dsp_poll(dsp: int) -> int:
    return (0x2000 if dsp > 3 else 0) + dsp * 0x800 + 0x1A4


DSP_STATUS_WORDS = [
    (0x2218, "fpga_revision"),
    (0x2234, "extended_capabilities"),
]
for dsp_index in range(8):
    DSP_STATUS_WORDS.extend(
        [
            (dsp_bank(dsp_index) + 0x28, f"dsp{dsp_index}_cmd_position"),
            (dsp_bank(dsp_index) + 0x68, f"dsp{dsp_index}_resp_position"),
            (dsp_poll(dsp_index), f"dsp{dsp_index}_boot_status"),
        ]
    )

RING_FIELD_NAMES = [
    "page0_lo", "page0_hi", "page1_lo", "page1_hi",
    "page2_lo", "page2_hi", "page3_lo", "page3_hi",
    "field_20", "field_24", "field_28",
]
RING_REGISTER_WORDS = [
    (0x2218, "fpga_revision"),
    (0x2234, "extended_capabilities"),
]
for dsp_index in range(8):
    for direction, direction_offset in (("cmd", 0x00), ("resp", 0x40)):
        ring_base = dsp_bank(dsp_index) + direction_offset
        RING_REGISTER_WORDS.extend(
            (
                ring_base + field_index * 4,
                f"dsp{dsp_index}_{direction}_{field_name}",
            )
            for field_index, field_name in enumerate(RING_FIELD_NAMES)
        )

DMA_CONTROL_WORDS = [
    (0x2200, "dma_control"),
    (0x2210, "unknown_2210"),
    (0x2214, "unknown_2214"),
    (0x2218, "fpga_revision"),
    (0x221C, "dsp_reset_control"),
    (0x2220, "transport_control"),
    (0x2224, "unknown_2224"),
    (0x2228, "unknown_2228"),
    (0x222C, "unknown_222c"),
    (0x2230, "unknown_2230"),
    (0x2234, "extended_capabilities"),
    (0x2238, "device_info_2238"),
    (0x223C, "unknown_223c"),
]

PROFILES = {
    "identity": IDENTITY_WORDS,
    "dsp-status": DSP_STATUS_WORDS,
    "ring-registers": RING_REGISTER_WORDS,
    "dma-control": DMA_CONTROL_WORDS,
}


def read_hex(path: pathlib.Path) -> int:
    return int(path.read_text().strip(), 0)


def refuse(message: str) -> "NoReturn":
    raise SystemExit(f"refusing UIO MMIO access: {message}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bdf", default="0000:03:00.0")
    parser.add_argument("--profile", choices=sorted(PROFILES), default="identity")
    parser.add_argument("--samples", type=int, choices=range(1, 5), default=1)
    parser.add_argument("--interval-ms", type=int, choices=range(0, 1001), default=0)
    parser.add_argument("--only-nonzero", action="store_true")
    parser.add_argument("--acknowledge-read-side-effects", action="store_true")
    args = parser.parse_args()

    if not args.acknowledge_read_side_effects:
        refuse("missing --acknowledge-read-side-effects")

    base = pathlib.Path("/sys/bus/pci/devices") / args.bdf
    expected = {
        "vendor": EXPECTED_VENDOR,
        "device": EXPECTED_DEVICE,
        "subsystem_vendor": EXPECTED_SUBVENDOR,
        "subsystem_device": EXPECTED_SUBDEVICE,
    }
    for name, value in expected.items():
        if read_hex(base / name) != value:
            refuse(f"unexpected {name}")

    driver = (base / "driver").resolve().name if (base / "driver").exists() else None
    if driver != EXPECTED_DRIVER:
        refuse(f"expected driver {EXPECTED_DRIVER}, found {driver}")

    with (base / "config").open("rb") as stream:
        config = stream.read(64)
    command = struct.unpack_from("<H", config, 4)[0]
    if command & 0x4:
        refuse("PCI bus mastering is enabled")
    if not command & 0x2:
        refuse("PCI memory decoding is disabled")

    candidates = glob.glob(str(base / "uio" / "uio*"))
    if len(candidates) != 1:
        refuse(f"expected one UIO mapping, found {len(candidates)}")
    uio_name = pathlib.Path(candidates[0]).name
    map0 = pathlib.Path("/sys/class/uio") / uio_name / "maps" / "map0"
    map_size = read_hex(map0 / "size")
    if map_size != EXPECTED_BAR0_SIZE:
        refuse(f"map0 size is {map_size}, expected 65536")

    fd = os.open(f"/dev/{uio_name}", os.O_RDONLY | os.O_SYNC)
    try:
        region = mmap.mmap(
            fd,
            map_size,
            flags=mmap.MAP_SHARED,
            prot=mmap.PROT_READ,
            offset=0,
        )
        try:
            samples = []
            for sample_index in range(args.samples):
                words = []
                for offset, name in PROFILES[args.profile]:
                    raw = region[offset : offset + 4]
                    words.append({
                        "offset": f"0x{offset:04x}",
                        "name": name,
                        "value": f"0x{struct.unpack('<I', raw)[0]:08x}",
                        "bytes_le": raw.hex(),
                    })
                samples.append(words)
                if sample_index + 1 < args.samples:
                    time.sleep(args.interval_ms / 1000)
        finally:
            region.close()
    finally:
        os.close(fd)

    result = {
        "schema": 1,
        "bdf": args.bdf,
        "pci_id": "1a00:0002",
        "subsystem_id": "1a00:0005",
        "transport": EXPECTED_DRIVER,
        "pci_command": f"0x{command:04x}",
        "profile": args.profile,
        "sample_count": args.samples,
        "interval_ms": args.interval_ms,
        "samples": samples,
    }
    if args.profile == "identity":
        identity = b"".join(
            bytes.fromhex(word["bytes_le"]) for word in samples[0][:4]
        )
        result["identity_ascii"] = "".join(
            chr(byte) if 32 <= byte < 127 else "." for byte in identity
        )
    if len(samples) > 1:
        result["changes"] = [
            {
                "offset": before["offset"],
                "name": before["name"],
                "before": before["value"],
                "after": after["value"],
            }
            for before, after in zip(samples[0], samples[-1])
            if before["value"] != after["value"]
        ]
    if args.only_nonzero:
        result["total_words_per_sample"] = len(samples[0])
        result["zero_words_per_sample"] = [
            sum(word["value"] == "0x00000000" for word in sample)
            for sample in samples
        ]
        result["samples"] = [
            [word for word in sample if word["value"] != "0x00000000"]
            for sample in samples
        ]
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
