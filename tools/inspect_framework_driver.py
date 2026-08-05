#!/usr/bin/env python3
"""Verify boot and resource-property facts in the analyzed macOS UAD-2 driver."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys


KNOWN_SHA256 = "7b664e8ad67b8104d9797defcc0c707fff55ae4981a725559ed9554f2f55cdf6"
PUBLIC_COMMIT = "910a8f413d33bc3489d0da8fc613870153ccb4f2"

SYMBOLS = {
    "__ZN11CPcieDevice13HardResetDSPsEv": 0x416A,
    "__ZN8CPcieDSP18_waitFor469ToStartEv": 0x7F5C,
    "__ZN8CPcieDSP11GetPropertyE11DSPPropertyPvj": 0x8454,
    "__ZN8CPcieDSP12PropertySizeE11DSPProperty": 0x8D3C,
    "__ZN11CUAD2Device12LoadFirmwareEPN5CUAOS9DMABufferEjj": 0x12B08,
    "__ZN19CDSPResourceManager10InitializeEv": 0x20CBE,
}

SIGNATURES = (
    (0x4216, bytes.fromhex("83bb380c000000740e4881c7a8010000beafdee00b"),
     "firmware-aware hard reset writes 0x0be0deaf to DSP0 +0x1a8"),
    (0x422D, bytes.fromhex("41be1c2200004c01f7be01000000"),
     "ordinary hard reset asserts BAR +0x221c"),
    (0x4240, bytes.fromhex("4c03b3700c000031f64c89f7"),
     "ordinary hard reset then targets the same register with zero"),
    (0x7F9E, bytes.fromhex("41837e1000b864000000bb0a0000000f44d8"),
     "DSP0 receives 100 polls and other DSPs receive 10"),
    (0x7FBF, bytes.fromhex("4881c7a4010000"),
     "boot polling reads the per-DSP register window at +0x1a4"),
    (0x7FD2, bytes.fromhex("4181fd0fedcaa8"),
     "boot polling recognizes sentinel 0xa8caed0f"),
    (0x7FDB, bytes.fromhex("bf2c010000"),
     "boot polling waits 300 ms between retries"),
    (0x805A, bytes.fromhex("4531ff41f6c5017530498b3c2441bfebffffff"),
     "ready bit zero is required and failure returns -21"),
    (0x84B4, bytes.fromhex("4183fd0c0f87850300004489e8488d0d40080000"),
     "GetProperty dispatches property IDs 0 through 12 through a local switch"),
    (0x8764, bytes.fromhex("41bfeaffffff83fb2c0f82d0000000"),
     "property 6 requires a 44-byte output record"),
    (0x87EC, bytes.fromhex("4881c7a0010000"),
     "property 7 reads the per-DSP register window at +0x1a0"),
    (0x8838, bytes.fromhex("418b4610"),
     "property 8 returns the cached DSP index"),
    (0x8892, bytes.fromhex("498b7e304885ff0f84250100004883c710"),
     "property 6 begins its live register map with BAR offsets +0x10"),
    (0x8904, bytes.fromhex("0f84dd0000004881c784010000"),
     "property 6 reads the first pool size at BAR offset +0x184"),
    (0x89B1, bytes.fromhex("4885ff74674881c79c010000"),
     "property 6 ends with the reservation at BAR offset +0x19c"),
    (0x8D3C, bytes.fromhex("83fe0a7713554889e54863c6488d0d015c03008b04815dc331c0c3"),
     "PropertySize uses the embedded size table for IDs 0 through 10"),
    (0x12B19, bytes.fromhex("c787380c000001000000"),
     "LoadFirmware sets the framework-loaded host flag before dispatch"),
    (0x12B23, bytes.fromhex("c70424f0490200be00001200ba00000480"),
     "LoadFirmware selects command 0x00120000, response 0x80040000, and 150 seconds"),
    (0x20D28, bytes.fromhex("498b7f10498d97fc010000488b07be07000000b904000000ff5048"),
     "resource-manager initialization obtains property 7 locally"),
    (0x20D43, bytes.fromhex("498b7f10488b074c8db508ffffffbe060000004c89f2b92c000000ff5048"),
     "resource-manager initialization obtains the 44-byte property 6 map locally"),
    (0x20DE2, bytes.fromhex("498b7f10488b07488d9534ffffffbe08000000b904000000ff5048"),
     "resource-manager initialization obtains property 8 locally"),
)

SWITCH_TABLE_ADDRESS = 0x8D08
SWITCH_TABLE = bytes.fromhex(
    "c9f7ffff53faffff3bf8fffff2faffff11fbffff44f8ffff5cfaffff"
    "d3faffff30fbffff26fbffffe7f7ffff3bfbffff5ff8ffff"
)
PROPERTY_SIZE_TABLE_ADDRESS = 0x3E950
PROPERTY_SIZES = (4, 0, 4, 4, 0, 0, 44, 0, 0, 4, 4)

PROPERTY_PATHS = (
    {"id": 0, "source": "MMIO +0x0c", "size": 4},
    {"id": 1, "source": "cached object field +0x14", "size": 0},
    {"id": 2, "source": "cached object field +0x0c", "size": 4},
    {"id": 3, "source": "MMIO +0x00, masked with 0x7f", "size": 4},
    {"id": 4, "source": "64-bit command-ring object field", "size": 0},
    {"id": 5, "source": "64-bit response-ring object field", "size": 0},
    {
        "id": 6,
        "source": "eleven MMIO words forming resource bases, sizes, and reservations",
        "size": 44,
        "register_offsets": [
            "0x010", "0x018", "0x014", "0x01c", "0x184", "0x18c",
            "0x188", "0x190", "0x198", "0x194", "0x19c",
        ],
    },
    {"id": 7, "source": "MMIO +0x1a0", "size": 0},
    {"id": 8, "source": "cached DSP index at object +0x10", "size": 0},
    {"id": 9, "source": "constant zero", "size": 4},
    {"id": 10, "source": "diagnostic register and state dump", "size": 4},
    {"id": 11, "source": "unsupported switch case", "size": 0},
    {"id": 12, "source": "host device-family predicate", "size": 0},
)


class MachOImage:
    """Minimal, dependency-free reader for a thin 64-bit little-endian Mach-O."""

    LC_SEGMENT_64 = 0x19
    LC_SYMTAB = 0x02

    def __init__(self, data: bytes):
        self.data = data
        if len(data) < 32:
            raise ValueError("truncated Mach-O header")
        magic, cpu_type, _subtype, _filetype, ncmds, sizeofcmds, _flags, _reserved = (
            struct.unpack_from("<IiiIIIII", data, 0)
        )
        if magic != 0xFEEDFACF:
            raise ValueError("expected a thin 64-bit little-endian Mach-O")
        if cpu_type != 0x01000007:
            raise ValueError(f"expected x86-64 Mach-O CPU type, found 0x{cpu_type:08x}")
        if 32 + sizeofcmds > len(data):
            raise ValueError("truncated Mach-O load-command area")

        self.segments: list[tuple[str, int, int, int, int]] = []
        self.symtab: tuple[int, int, int, int] | None = None
        offset = 32
        for _index in range(ncmds):
            if offset + 8 > len(data):
                raise ValueError("truncated Mach-O load command")
            command, command_size = struct.unpack_from("<II", data, offset)
            if command_size < 8 or offset + command_size > len(data):
                raise ValueError("invalid Mach-O load-command size")
            if command == self.LC_SEGMENT_64:
                if command_size < 72:
                    raise ValueError("truncated LC_SEGMENT_64 command")
                name = data[offset + 8 : offset + 24].split(b"\0", 1)[0].decode(
                    "ascii", errors="replace"
                )
                vm_address, vm_size, file_offset, file_size = struct.unpack_from(
                    "<QQQQ", data, offset + 24
                )
                self.segments.append(
                    (name, vm_address, vm_size, file_offset, file_size)
                )
            elif command == self.LC_SYMTAB:
                if command_size < 24:
                    raise ValueError("truncated LC_SYMTAB command")
                self.symtab = struct.unpack_from("<IIII", data, offset + 8)
            offset += command_size

    def read_va(self, address: int, size: int) -> bytes:
        for name, vm_address, vm_size, file_offset, file_size in self.segments:
            if vm_address <= address and address + size <= vm_address + vm_size:
                within = address - vm_address
                if within + size > file_size:
                    raise ValueError(f"address 0x{address:x} is not file-backed in {name}")
                start = file_offset + within
                end = start + size
                if end > len(self.data):
                    raise ValueError(f"address 0x{address:x} maps past end of file")
                return self.data[start:end]
        raise ValueError(f"address 0x{address:x} is outside mapped Mach-O segments")

    def symbols(self) -> dict[str, int]:
        if self.symtab is None:
            raise ValueError("Mach-O has no symbol table")
        symbol_offset, symbol_count, string_offset, string_size = self.symtab
        if symbol_offset + symbol_count * 16 > len(self.data):
            raise ValueError("truncated Mach-O symbol table")
        if string_offset + string_size > len(self.data):
            raise ValueError("truncated Mach-O string table")
        strings = self.data[string_offset : string_offset + string_size]
        result: dict[str, int] = {}
        for index in range(symbol_count):
            entry = symbol_offset + index * 16
            string_index, _type, _section, _description, value = struct.unpack_from(
                "<IBBHQ", self.data, entry
            )
            if string_index >= len(strings):
                continue
            end = strings.find(b"\0", string_index)
            if end < 0:
                continue
            name = strings[string_index:end].decode("utf-8", errors="replace")
            result[name] = value
        return result


def inspect(path: Path, file_label: str | None = None) -> dict[str, object]:
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != KNOWN_SHA256:
        raise ValueError(
            "driver hash is not the analyzed macOS uad2.kext: "
            f"expected {KNOWN_SHA256}, found {digest}"
        )
    image = MachOImage(data)
    symbols = image.symbols()

    symbol_checks = []
    for name, expected_address in SYMBOLS.items():
        observed_address = symbols.get(name)
        symbol_checks.append(
            {
                "name": name,
                "expected_address": f"0x{expected_address:016x}",
                "observed_address": (
                    None if observed_address is None else f"0x{observed_address:016x}"
                ),
                "matches": observed_address == expected_address,
            }
        )

    signature_checks = []
    for address, expected, meaning in SIGNATURES:
        observed = image.read_va(address, len(expected))
        signature_checks.append(
            {
                "address": f"0x{address:016x}",
                "expected_hex": expected.hex(),
                "observed_hex": observed.hex(),
                "matches": observed == expected,
                "meaning": meaning,
            }
        )

    switch_observed = image.read_va(SWITCH_TABLE_ADDRESS, len(SWITCH_TABLE))
    property_size_bytes = image.read_va(
        PROPERTY_SIZE_TABLE_ADDRESS, len(PROPERTY_SIZES) * 4
    )
    property_sizes_observed = struct.unpack(
        f"<{len(PROPERTY_SIZES)}I", property_size_bytes
    )
    table_checks = {
        "switch_table_matches": switch_observed == SWITCH_TABLE,
        "property_sizes_expected": list(PROPERTY_SIZES),
        "property_sizes_observed": list(property_sizes_observed),
        "property_size_table_matches": property_sizes_observed == PROPERTY_SIZES,
    }
    all_match = (
        all(check["matches"] for check in symbol_checks)
        and all(check["matches"] for check in signature_checks)
        and table_checks["switch_table_matches"]
        and table_checks["property_size_table_matches"]
    )
    return {
        "schema": 1,
        "file": file_label or path.name,
        "sha256": digest,
        "public_source_commit": PUBLIC_COMMIT,
        "all_signatures_match": all_match,
        "symbols": symbol_checks,
        "signatures": signature_checks,
        "tables": table_checks,
        "property_paths": PROPERTY_PATHS,
        "conclusions": {
            "processor_family": "ADSP-21469",
            "resource_properties_use_command_ring": False,
            "resource_property_map": "property 6 is the eleven-word per-DSP BAR map",
            "framework_load_command": "0x00120000",
            "firmware_aware_hard_reset": "DSP0 +0x1a8 receives 0x0be0deaf",
            "ordinary_hard_reset": "BAR +0x221c is pulsed one then zero",
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path, help="path to the analyzed x86-64 uad2.kext")
    parser.add_argument("--label", default=None, help="stable label for JSON output")
    args = parser.parse_args()
    try:
        result = inspect(args.driver, args.label)
    except (OSError, ValueError, struct.error) as error:
        print(f"refusing: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["all_signatures_match"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
