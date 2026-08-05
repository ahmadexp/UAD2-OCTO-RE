#!/usr/bin/env python3
"""Decode the OCTO startup branches recovered from the official driver."""

import argparse
import json


DEFAULT_FPGA_REVISION = 0xA012DC0D
DEFAULT_EXTENDED_CAPABILITIES = 0x00300811


def signed32(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def decode(fpga_revision: int, extended_capabilities: int) -> dict:
    fpga_revision &= 0xFFFFFFFF
    extended_capabilities &= 0xFFFFFFFF
    modern = signed32(fpga_revision) < 0
    dsp_count = (extended_capabilities >> 8) & 0xFF if modern else 4
    family = (extended_capabilities >> 20) & 0x3F if modern else None

    compressed = modern and dsp_count > 4
    extended_interrupt_bank = (
        modern
        and signed32(extended_capabilities) < 0
        and family != 6
    )
    audio_extension = (
        modern
        and signed32(extended_capabilities) < 0
        and family != 6
    )

    logical_count = dsp_count * 5
    mapping = []
    for logical in range(logical_count):
        if compressed:
            within_group = logical % 5
            physical = (logical // 5) * 4 + within_group
            if within_group == 4:
                physical = None
        else:
            physical = logical
        mapping.append({"logical": logical, "physical": physical})

    callback_shadow = 0
    for dsp in range(dsp_count):
        for logical in (dsp * 5 + 2, dsp * 5 + 3, dsp * 5 + 4):
            physical = mapping[logical]["physical"]
            if physical is not None:
                callback_shadow |= 1 << physical

    query_shadow = callback_shadow
    if dsp_count:
        query_shadow |= 1 << mapping[0]["physical"]
        query_shadow |= 1 << mapping[1]["physical"]

    dma_sequence = [1]
    for dsp in range(dsp_count):
        dma_sequence.append(dma_sequence[-1] | (1 << (dsp + 1)))

    return {
        "fpga_revision": f"0x{fpga_revision:08x}",
        "extended_capabilities": f"0x{extended_capabilities:08x}",
        "modern_fpga": modern,
        "dsp_count": dsp_count,
        "family_bits_25_20": family,
        "compressed_interrupt_mapping": compressed,
        "extended_interrupt_bank": extended_interrupt_bank,
        "audio_extension": audio_extension,
        "shared_4_mib_dma_applicable": audio_extension,
        "interrupt_mapping": mapping,
        "callback_shadow": f"0x{callback_shadow:016x}",
        "dsp0_query_shadow": f"0x{query_shadow:016x}",
        "dma_shadow_sequence": [f"0x{value:08x}" for value in dma_sequence],
    }


def parse_u32(value: str) -> int:
    parsed = int(value, 0)
    if parsed < 0 or parsed > 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("value must fit in an unsigned 32-bit word")
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fpga-revision", type=parse_u32, default=DEFAULT_FPGA_REVISION)
    parser.add_argument(
        "--extended-capabilities",
        type=parse_u32,
        default=DEFAULT_EXTENDED_CAPABILITIES,
    )
    args = parser.parse_args()
    print(json.dumps(decode(args.fpga_revision, args.extended_capabilities), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
