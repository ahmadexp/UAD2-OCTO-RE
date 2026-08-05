#!/usr/bin/env python3
"""Capture read-only PCI and sysfs metadata for a UAD endpoint."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import subprocess
import sys
import textwrap


REMOTE_PROGRAM = r'''
import json
import os
import pathlib
import sys

bdf = sys.argv[1]
base = pathlib.Path("/sys/bus/pci/devices") / bdf
if not base.is_dir():
    raise SystemExit(f"PCI device not found: {bdf}")

def text(name):
    try:
        return (base / name).read_text().strip()
    except (FileNotFoundError, PermissionError, OSError):
        return None

def integer(name):
    value = text(name)
    if value is None:
        return None
    try:
        return int(value, 0)
    except ValueError:
        return value

def link(name):
    try:
        return os.path.basename(os.path.realpath(base / name))
    except OSError:
        return None

config = b""
try:
    with (base / "config").open("rb") as stream:
        config = stream.read(256)
except (PermissionError, OSError):
    pass

resources = []
resource_text = text("resource")
if resource_text:
    for index, line in enumerate(resource_text.splitlines()):
        fields = line.split()
        if len(fields) == 3:
            start, end, flags = (int(field, 16) for field in fields)
            resources.append({
                "index": index,
                "start": f"0x{start:x}",
                "end": f"0x{end:x}",
                "size": end - start + 1 if end >= start and start else 0,
                "flags": f"0x{flags:x}",
            })

command = int.from_bytes(config[4:6], "little") if len(config) >= 6 else None
record = {
    "bdf": bdf,
    "vendor": text("vendor"),
    "device": text("device"),
    "subsystem_vendor": text("subsystem_vendor"),
    "subsystem_device": text("subsystem_device"),
    "class": text("class"),
    "revision": text("revision"),
    "irq": integer("irq"),
    "numa_node": integer("numa_node"),
    "enable_count": integer("enable"),
    "driver": link("driver") if (base / "driver").exists() else None,
    "iommu_group": link("iommu_group") if (base / "iommu_group").exists() else None,
    "current_link_speed": text("current_link_speed"),
    "current_link_width": integer("current_link_width"),
    "max_link_speed": text("max_link_speed"),
    "max_link_width": integer("max_link_width"),
    "power_state": text("power_state"),
    "power_control": text("power/control"),
    "config_hex": config.hex(),
    "config_bytes_read": len(config),
    "command": f"0x{command:04x}" if command is not None else None,
    "memory_space_enable": bool(command & 0x2) if command is not None else None,
    "bus_master_enable": bool(command & 0x4) if command is not None else None,
    "resources": resources,
}
print(json.dumps(record, sort_keys=True))
'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", help="SSH target. Omit to inspect this host.")
    parser.add_argument("--bdf", default="0000:03:00.0")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()

    command = ["python3", "-", args.bdf]
    if args.host:
        command = [
            "ssh",
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=8",
            args.host,
            *command,
        ]

    result = subprocess.run(
        command,
        input=textwrap.dedent(REMOTE_PROGRAM),
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode:
        print(result.stderr, file=sys.stderr, end="")
        return result.returncode

    device = json.loads(result.stdout)
    capture = {
        "schema": 1,
        "captured_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "target": args.host or "localhost",
        "device": device,
    }
    output = json.dumps(capture, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(output)
    else:
        print(output, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
