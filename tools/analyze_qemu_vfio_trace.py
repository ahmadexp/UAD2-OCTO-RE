#!/usr/bin/env python3
"""Summarize QEMU VFIO traces from the official Windows-driver capture lab."""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import Counter
from pathlib import Path
from typing import TextIO


REGION_WRITE = re.compile(
    r"^vfio_region_write\s+\((?P<device>.+):region(?P<region>\d+)\+"
    r"0x(?P<offset>[0-9a-f]+), 0x(?P<value>[0-9a-f]+), (?P<size>\d+)\)$",
    re.IGNORECASE,
)
REGION_READ = re.compile(
    r"^vfio_region_read\s+\((?P<device>.+):region(?P<region>\d+)\+"
    r"0x(?P<offset>[0-9a-f]+), (?P<size>\d+)\) = 0x(?P<value>[0-9a-f]+)$",
    re.IGNORECASE,
)
CONFIG_WRITE = re.compile(
    r"^vfio_pci_write_config\s+\((?P<device>.+), @0x(?P<offset>[0-9a-f]+), "
    r"0x(?P<value>[0-9a-f]+), len=0x(?P<size>[0-9a-f]+)\)$",
    re.IGNORECASE,
)
CONFIG_READ = re.compile(
    r"^vfio_pci_read_config\s+\((?P<device>.+), @0x(?P<offset>[0-9a-f]+), "
    r"len=0x(?P<size>[0-9a-f]+)\) 0x(?P<value>[0-9a-f]+)$",
    re.IGNORECASE,
)


GLOBAL_REGISTERS = {
    0x2200: "dma_control",
    0x2204: "interrupt_enable_low",
    0x2208: "interrupt_arm_or_ack_low",
    0x2218: "fpga_revision",
    0x221C: "hard_reset",
    0x2220: "notification_control",
    0x2234: "capabilities",
    0x2238: "auxiliary_fpga_version",
    0x2264: "interrupt_enable_high",
    0x2268: "interrupt_arm_or_ack_high",
}


def dsp_register_base(dsp: int) -> int:
    if not 0 <= dsp < 8:
        raise ValueError("DSP index must be in range 0 through 7")
    return dsp * 0x800 if dsp < 4 else 0x4000 + (dsp - 4) * 0x800


def ring_base(dsp: int, response: bool) -> int:
    if not 0 <= dsp < 8:
        raise ValueError("DSP index must be in range 0 through 7")
    bank = 0x2000 + dsp * 0x80 if dsp < 4 else 0x6000 + (dsp - 4) * 0x80
    return bank + (0x40 if response else 0)


def label_bar0_offset(offset: int) -> str:
    if offset in GLOBAL_REGISTERS:
        return GLOBAL_REGISTERS[offset]
    for dsp in range(8):
        base = dsp_register_base(dsp)
        relative = offset - base
        if relative == 0x1A0:
            return f"dsp{dsp}_resource_property_7"
        if relative == 0x1A4:
            return f"dsp{dsp}_ready"
        if relative == 0x1A8:
            return f"dsp{dsp}_firmware_transition"
        for response in (False, True):
            base = ring_base(dsp, response)
            relative = offset - base
            ring = "response" if response else "command"
            if 0 <= relative <= 0x1C and relative % 4 == 0:
                page = relative // 8
                half = "high" if relative % 8 else "low"
                return f"dsp{dsp}_{ring}_page{page}_{half}"
            if relative == 0x20:
                return f"dsp{dsp}_{ring}_notify_index"
            if relative == 0x24:
                return f"dsp{dsp}_{ring}_host_write_index"
            if relative == 0x28:
                return f"dsp{dsp}_{ring}_hardware_read_index"
    return "unknown"


def parse_access(match: re.Match[str], kind: str, line_number: int) -> dict[str, object]:
    groups = match.groupdict()
    return {
        "line": line_number,
        "kind": kind,
        "device": groups["device"],
        "region": int(groups["region"]) if "region" in groups else None,
        "offset": int(groups["offset"], 16),
        "value": int(groups["value"], 16),
        "size": int(groups["size"], 16) if kind.startswith("config") else int(groups["size"]),
    }


def summarize(stream: TextIO, write_csv: TextIO | None = None) -> dict[str, object]:
    event_counts: Counter[str] = Counter()
    unparsed_counts: Counter[str] = Counter()
    registers: dict[tuple[int, int], dict[str, object]] = {}
    config: dict[int, dict[str, object]] = {}
    milestones = {
        "bar0_access_seen": False,
        "ordinary_hard_reset_assert_seen": False,
        "ordinary_hard_reset_deassert_seen": False,
        "firmware_transition_magic_seen": False,
        "all_eight_dma_enable_seen": False,
        "all_command_rings_published": False,
        "all_response_rings_published": False,
    }
    published_command: set[int] = set()
    published_response: set[int] = set()
    csv_writer = None
    if write_csv is not None:
        csv_writer = csv.DictWriter(
            write_csv,
            fieldnames=("line", "device", "region", "offset", "label", "size", "value"),
        )
        csv_writer.writeheader()

    total_lines = 0
    for line_number, raw_line in enumerate(stream, start=1):
        total_lines = line_number
        line = raw_line.rstrip("\r\n")
        event = line.split(maxsplit=1)[0] if line else "empty"
        event_counts[event] += 1
        match = REGION_WRITE.match(line)
        kind = "region_write"
        if match is None:
            match = REGION_READ.match(line)
            kind = "region_read"
        if match is not None:
            access = parse_access(match, kind, line_number)
            region = int(access["region"])
            offset = int(access["offset"])
            value = int(access["value"])
            key = (region, offset)
            record = registers.setdefault(
                key,
                {
                    "region": region,
                    "offset": f"0x{offset:04x}",
                    "label": label_bar0_offset(offset) if region == 0 else "non_bar0",
                    "reads": 0,
                    "writes": 0,
                    "first_line": line_number,
                    "last_line": line_number,
                    "last_value": f"0x{value:x}",
                    "values": [],
                },
            )
            record["reads" if kind == "region_read" else "writes"] += 1
            record["last_line"] = line_number
            record["last_value"] = f"0x{value:x}"
            values = record["values"]
            formatted_value = f"0x{value:x}"
            if formatted_value not in values and len(values) < 64:
                values.append(formatted_value)
            if region == 0:
                milestones["bar0_access_seen"] = True
                if offset == 0x221C and value == 1 and kind == "region_write":
                    milestones["ordinary_hard_reset_assert_seen"] = True
                if offset == 0x221C and value == 0 and kind == "region_write":
                    milestones["ordinary_hard_reset_deassert_seen"] = True
                if offset == 0x1A8 and value == 0x0BE0DEAF and kind == "region_write":
                    milestones["firmware_transition_magic_seen"] = True
                if offset == 0x2200 and value & 0x1FF == 0x1FF and kind == "region_write":
                    milestones["all_eight_dma_enable_seen"] = True
                if kind == "region_write" and value != 0:
                    for dsp in range(8):
                        if offset == ring_base(dsp, False):
                            published_command.add(dsp)
                        if offset == ring_base(dsp, True):
                            published_response.add(dsp)
            if kind == "region_write" and csv_writer is not None:
                csv_writer.writerow(
                    {
                        "line": line_number,
                        "device": access["device"],
                        "region": region,
                        "offset": f"0x{offset:04x}",
                        "label": label_bar0_offset(offset) if region == 0 else "non_bar0",
                        "size": access["size"],
                        "value": f"0x{value:x}",
                    }
                )
            continue

        match = CONFIG_WRITE.match(line)
        kind = "config_write"
        if match is None:
            match = CONFIG_READ.match(line)
            kind = "config_read"
        if match is not None:
            access = parse_access(match, kind, line_number)
            offset = int(access["offset"])
            value = int(access["value"])
            record = config.setdefault(
                offset,
                {
                    "offset": f"0x{offset:03x}",
                    "reads": 0,
                    "writes": 0,
                    "first_line": line_number,
                    "last_line": line_number,
                    "last_value": f"0x{value:x}",
                },
            )
            record["reads" if kind == "config_read" else "writes"] += 1
            record["last_line"] = line_number
            record["last_value"] = f"0x{value:x}"
            continue

        if event not in {
            "vfio_listener_region_add_ram",
            "vfio_listener_region_del",
            "vfio_pci_reset",
            "vfio_pci_reset_flr",
            "vfio_pci_reset_pm",
            "vfio_intx_enable",
            "vfio_intx_disable",
            "vfio_intx_interrupt",
            "vfio_msi_enable",
            "vfio_msi_disable",
            "vfio_msi_interrupt",
            "vfio_iommu_map_notify",
            "vfio_check_pcie_flr",
            "vfio_check_pm_reset",
        }:
            unparsed_counts[event] += 1

    milestones["all_command_rings_published"] = published_command == set(range(8))
    milestones["all_response_rings_published"] = published_response == set(range(8))
    return {
        "schema": 1,
        "total_lines": total_lines,
        "event_counts": dict(sorted(event_counts.items())),
        "unparsed_event_counts": dict(sorted(unparsed_counts.items())),
        "milestones": milestones,
        "published_command_dsps": sorted(published_command),
        "published_response_dsps": sorted(published_response),
        "bar_registers": [registers[key] for key in sorted(registers)],
        "config_registers": [config[key] for key in sorted(config)],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--writes-csv", type=Path)
    args = parser.parse_args()
    try:
        with args.trace.open(encoding="utf-8", errors="replace") as stream:
            if args.writes_csv is None:
                result = summarize(stream)
            else:
                with args.writes_csv.open("w", encoding="utf-8", newline="") as output:
                    result = summarize(stream, output)
    except OSError as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
