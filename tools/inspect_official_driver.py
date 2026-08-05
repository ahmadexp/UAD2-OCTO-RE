#!/usr/bin/env python3
"""Verify protocol-relevant signatures in the exact analyzed UAD2Pcie.sys."""

import argparse
import hashlib
import json
import pathlib
import struct
import sys


KNOWN_SHA256 = "20a11d5b51a4c5093f0c6c3cb80ba132959dcae3362547f4a41ea59f0dd99a6b"

# Virtual address, expected bytes, and the fact established by that sequence.
SIGNATURES = (
    (0x14000C4E0, bytes.fromhex("488d4a28"), "ring initializer reads MMIO +0x28"),
    (0x14000C4EC, bytes.fromhex("3d00040000"), "ring index is bounded to 1024 entries"),
    (0x14000C57B, bytes.fromhex("4883c124"), "ring initializer writes MMIO +0x24"),
    (0x14000C58B, bytes.fromhex("4883c120"), "ring initializer writes MMIO +0x20"),
    (0x14000C62B, bytes.fromhex("83fd04"), "ring initializer programs four pages"),
    (0x14000A510, bytes.fromhex("448d0489"), "command vector base is DSP index times five"),
    (0x14000A54F, bytes.fromhex("448d048d01000000"), "response vector adds one"),
    (0x14000BB1A, bytes.fromhex("81f900040000"), "more than four DSPs selects compressed interrupt mapping"),
    (0x14000BB7E, bytes.fromhex("478d0492"), "compressed interrupt mapping groups logical vectors in fives"),
    (0x14000BBC3, bytes.fromhex("838c83d0040000ff"), "fifth logical vector in each compressed group is unmapped"),
    (0x14000B926, bytes.fromhex("4881c108220000"), "armed interrupt bits are written at BAR +0x2208"),
    (0x14000B939, bytes.fromhex("4881c104220000"), "interrupt enable shadow is written at BAR +0x2204"),
    (0x14000BA1A, bytes.fromhex("c783bc04000001000000"), "interrupt manager initializes the DMA shadow to global bit one"),
    (0x14001059F, bytes.fromhex("4183cc01"), "short-command helper ORs command with one"),
    (0x14000F54B, bytes.fromhex("ba00002600"), "query 026 command base is 0x00260000"),
    (0x14000F550, bytes.fromhex("41b805000c80"), "query 026 response header is 0x800c0005"),
    (0x14000F5BB, bytes.fromhex("ba00002700"), "query 027 command base is 0x00270000"),
    (0x14000F5C0, bytes.fromhex("41b802000d80"), "query 027 response header is 0x800d0002"),
    (0x14000BCA3, bytes.fromhex("4881c104220000"), "interrupt reset clears BAR +0x2204"),
    (0x14000BCB5, bytes.fromhex("4881c108220000"), "interrupt reset clears BAR +0x2208"),
    (0x14000BEDD, bytes.fromhex("81e201feffff"), "modern DMA reset preserves only mask 0xfffffe01"),
    (0x14000BEE3, bytes.fromhex("81ca00fe0100"), "modern DMA reset asserts mask 0x0001fe00"),
    (0x14000BEF6, bytes.fromhex("bf00220000"), "DMA shadow is written at BAR +0x2200"),
    (0x140007CC2, bytes.fromhex("488d8a20220000"), "resume path first targets BAR +0x2220"),
    (0x140007CF9, bytes.fromhex("4881c108220000"), "resume path acknowledges all low interrupt bits at BAR +0x2208"),
    (0x140007D1E, bytes.fromhex("e8a5290000"), "resume path calls per-DSP start in a loop"),
    (0x140007D3F, bytes.fromhex("e840acffff"), "resume path initializes shared DMA tables after DSP rings"),
    (0x14000748B, bytes.fromhex("85c07913"), "nonnegative capability word disables the audio extension"),
    (0x14000748F, bytes.fromhex("250000f003"), "negative capability word is filtered by family bits 25:20"),
    (0x140007494, bytes.fromhex("3d00006000"), "capability family six also disables the audio extension"),
    (0x1400074A4, bytes.fromhex("8983f40c0000"), "audio-extension predicate is saved in the device object"),
    (0x140007D33, bytes.fromhex("488b8bf80c0000"), "resume path fetches the optional audio-extension object"),
    (0x140007D3A, bytes.fromhex("4885c97407"), "shared 4 MiB setup is skipped when the audio-extension object is absent"),
    (0x14000A6F2, bytes.fromhex("4183f904"), "per-DSP start splits ring banks at DSP index four"),
    (0x14000A81D, bytes.fromhex("e85a1c0000"), "per-DSP start initializes the command ring first"),
    (0x14000A871, bytes.fromhex("4883c240"), "response ring window is command ring plus 0x40"),
    (0x14000A875, bytes.fromhex("e8021c0000"), "per-DSP start initializes the response ring second"),
    (0x14000B1DB, bytes.fromhex("4881c1a4010000"), "DSP boot polling reads each core's status offset +0x1a4"),
    (0x14000B28F, bytes.fromhex("4184d9"), "DSP boot polling requires ready bit zero to be set"),
    (0x14000B713, bytes.fromhex("8d4f01"), "DSP DMA bit position is DSP index plus one"),
    (0x14000B721, bytes.fromhex("0983bc040000"), "DSP DMA bit is accumulated in the DMA shadow"),
    (0x140002B3D, bytes.fromhex("8d8f00800000"), "first shared DMA table begins at BAR +0x8000"),
    (0x140002B80, bytes.fromhex("8d8f00a00000"), "second shared DMA table begins at BAR +0xa000"),
    (0x140002BB8, bytes.fromhex("81fd00004000"), "shared DMA tables describe 4 MiB per direction"),
    (0x140002C3D, bytes.fromhex("ba28000000"), "shared DMA setup enables interrupt vector 40"),
)


STARTUP_SEQUENCE = (
    {
        "stage": 1,
        "name": "quiesce_notification_path",
        "operation": "write BAR +0x2220 = 0",
        "source_function": "0x140007bc8",
    },
    {
        "stage": 2,
        "name": "clear_interrupt_state",
        "operation": "write zero to BAR +0x2204 and +0x2208, plus extended registers when present",
        "source_function": "0x14000bc64",
    },
    {
        "stage": 3,
        "name": "reset_dma_engines",
        "operation": "assert modern reset mask at BAR +0x2200, then write the retained DMA shadow",
        "source_function": "0x14000bea0",
    },
    {
        "stage": 4,
        "name": "acknowledge_low_interrupts",
        "operation": "write 0xffffffff to BAR +0x2208",
        "source_function": "0x140007bc8",
    },
    {
        "stage": 5,
        "name": "start_each_dsp",
        "operation": "for each reported DSP, initialize command then response ring and add its DMA bit",
        "source_function": "0x14000a6c8",
    },
    {
        "stage": 6,
        "name": "initialize_shared_dma",
        "operation": "if the optional audio extension exists, publish two 4 MiB page tables at BAR +0x8000 and +0xa000",
        "source_function": "0x140002984",
    },
    {
        "stage": 7,
        "name": "enable_shared_dma_interrupt",
        "operation": "if the optional audio extension exists, enable and arm interrupt vector 40",
        "source_function": "0x140002984",
    },
)


class PEImage:
    def __init__(self, data: bytes):
        self.data = data
        if len(data) < 0x40 or data[:2] != b"MZ":
            raise ValueError("not a DOS/PE image")
        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if pe_offset + 24 > len(data) or data[pe_offset : pe_offset + 4] != b"PE\0\0":
            raise ValueError("missing PE signature")
        coff = pe_offset + 4
        machine, section_count, _, _, _, optional_size, _ = struct.unpack_from(
            "<HHIIIHH", data, coff
        )
        if machine != 0x8664:
            raise ValueError(f"expected x86-64 PE machine, found 0x{machine:04x}")
        optional = coff + 20
        if optional + optional_size > len(data):
            raise ValueError("truncated PE optional header")
        magic = struct.unpack_from("<H", data, optional)[0]
        if magic != 0x20B:
            raise ValueError(f"expected PE32+, found optional magic 0x{magic:04x}")
        self.image_base = struct.unpack_from("<Q", data, optional + 24)[0]
        section_offset = optional + optional_size
        self.sections = []
        for index in range(section_count):
            offset = section_offset + index * 40
            if offset + 40 > len(data):
                raise ValueError("truncated PE section table")
            name = data[offset : offset + 8].split(b"\0", 1)[0].decode(
                "ascii", errors="replace"
            )
            virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
                "<IIII", data, offset + 8
            )
            self.sections.append(
                (name, virtual_address, virtual_size, raw_offset, raw_size)
            )

    def read_va(self, address: int, size: int) -> bytes:
        rva = address - self.image_base
        if rva < 0:
            raise ValueError(f"address 0x{address:x} precedes image base")
        for name, virtual_address, virtual_size, raw_offset, raw_size in self.sections:
            span = max(virtual_size, raw_size)
            if virtual_address <= rva and rva + size <= virtual_address + span:
                within = rva - virtual_address
                if within + size > raw_size:
                    raise ValueError(f"address 0x{address:x} is not file-backed in {name}")
                start = raw_offset + within
                end = start + size
                if end > len(self.data):
                    raise ValueError(f"address 0x{address:x} maps past end of file")
                return self.data[start:end]
        raise ValueError(f"address 0x{address:x} is outside mapped PE sections")


def inspect(path: pathlib.Path, file_label: str | None = None) -> dict:
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != KNOWN_SHA256:
        raise ValueError(
            "driver hash is not the analyzed UAD2Pcie.sys: "
            f"expected {KNOWN_SHA256}, found {digest}"
        )
    image = PEImage(data)
    checks = []
    for address, expected, meaning in SIGNATURES:
        observed = image.read_va(address, len(expected))
        checks.append(
            {
                "address": f"0x{address:016x}",
                "expected_hex": expected.hex(),
                "observed_hex": observed.hex(),
                "matches": observed == expected,
                "meaning": meaning,
            }
        )
    return {
        "file": file_label or path.name,
        "sha256": digest,
        "image_base": f"0x{image.image_base:016x}",
        "signature_count": len(checks),
        "all_signatures_match": all(check["matches"] for check in checks),
        "signatures": checks,
        "startup_sequence": STARTUP_SEQUENCE,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("driver", type=pathlib.Path, help="path to UAD2Pcie.sys")
    parser.add_argument(
        "--label",
        default=None,
        help="stable non-path label to place in JSON output",
    )
    args = parser.parse_args()
    try:
        result = inspect(args.driver, args.label)
    except (OSError, ValueError) as error:
        print(f"refusing: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0 if result["all_signatures_match"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
