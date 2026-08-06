#!/usr/bin/env python3
"""Decode a metadata capture of the native UAD2 plug-in allocation record.

The input format is produced by tools/windows/uad2_alloc_capture_proxy.c.  The
report replaces process-local pointers with relative offsets or zeros and does
not emit proprietary resource bytes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct


CAPTURE_HEADER = struct.Struct("<III")
RECORD_ENVELOPE = struct.Struct("<IQQ")
CAPTURE_MAGIC = 0x31464941
CAPTURE_VERSION = 1
RECORD_SIZE = 0xAF8
RESOURCE_CAPACITY = 48
MEMORY_CAPACITY = 128
READBACK_CAPACITY = 32
NEXT_RECORD_POINTER_OFFSET = 0xAC0


def _u32(record: bytes, offset: int) -> int:
    return struct.unpack_from("<I", record, offset)[0]


def _u64(record: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", record, offset)[0]


def inspect(path: Path) -> dict[str, object]:
    blob = path.read_bytes()
    if len(blob) < CAPTURE_HEADER.size:
        raise ValueError("capture is shorter than its header")
    magic, version, count = CAPTURE_HEADER.unpack_from(blob)
    if magic != CAPTURE_MAGIC or version != CAPTURE_VERSION:
        raise ValueError("capture magic or version does not match")
    if count > 32:
        raise ValueError("capture record count exceeds the proxy bound")

    cursor = CAPTURE_HEADER.size
    records = []
    for record_index in range(count):
        if cursor + RECORD_ENVELOPE.size > len(blob):
            raise ValueError("truncated record envelope")
        record_size, _original_pointer, next_pointer = RECORD_ENVELOPE.unpack_from(
            blob, cursor
        )
        cursor += RECORD_ENVELOPE.size
        if record_size != RECORD_SIZE or cursor + record_size > len(blob):
            raise ValueError(f"unsupported or truncated record size {record_size:#x}")
        record = blob[cursor : cursor + record_size]
        cursor += record_size

        resource_count = _u32(record, 4)
        if resource_count > RESOURCE_CAPACITY:
            raise ValueError("resource count exceeds the native record capacity")
        resource_pointers = [
            _u64(record, 8 + index * 8) for index in range(resource_count)
        ]
        nonzero_pointers = [pointer for pointer in resource_pointers if pointer]
        resource_base = min(nonzero_pointers) if nonzero_pointers else 0
        resource_offsets = [
            pointer - resource_base if pointer else None for pointer in resource_pointers
        ]

        memory_count = _u32(record, 0x188)
        if memory_count > MEMORY_CAPACITY:
            raise ValueError("memory-spec count exceeds the native record capacity")
        memory_specs = []
        for index in range(memory_count):
            offset = 0x18C + index * 16
            memory_specs.append(
                {
                    "index": index,
                    "word0": _u32(record, offset),
                    "word1": _u32(record, offset + 4),
                    "word2": _u32(record, offset + 8),
                    "word3": _u32(record, offset + 12),
                }
            )

        readback_count = _u32(record, 0x98C)
        if readback_count > READBACK_CAPACITY:
            raise ValueError("readback count exceeds the native record capacity")
        readbacks = []
        for index in range(readback_count):
            offset = 0x990 + index * 8
            spec = _u32(record, offset)
            readbacks.append(
                {
                    "index": index,
                    "resource": spec >> 24,
                    "dword_offset": spec & 0xFFFFFF,
                    "dword_count": _u32(record, offset + 4),
                }
            )

        pointer_scrubbed = (
            record[:8]
            + bytes(RESOURCE_CAPACITY * 8)
            + record[8 + RESOURCE_CAPACITY * 8 : NEXT_RECORD_POINTER_OFFSET]
            + bytes(8)
            + record[NEXT_RECORD_POINTER_OFFSET + 8 :]
        )
        records.append(
            {
                "record_index": record_index,
                "record_bytes": record_size,
                "record_sha256_pointer_scrubbed": hashlib.sha256(
                    pointer_scrubbed
                ).hexdigest(),
                "chain_continues": bool(next_pointer),
                "resource_count": resource_count,
                "resource_relative_offsets": resource_offsets,
                "memory_count": memory_count,
                "memory_specs": memory_specs,
                "readback_count": readback_count,
                "readbacks": readbacks,
            }
        )

    if cursor != len(blob):
        raise ValueError(f"capture has {len(blob) - cursor} trailing bytes")
    return {
        "schema": 1,
        "capture_sha256": hashlib.sha256(blob).hexdigest(),
        "capture_version": version,
        "record_count": count,
        "records": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        result = {str(path): inspect(path) for path in args.capture}
    except (OSError, ValueError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
