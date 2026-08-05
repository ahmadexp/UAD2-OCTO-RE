#!/usr/bin/env python3
"""Locate structurally valid UAD2 ``Bill`` resources without extracting them.

The scanner reports offsets, dimensions, and hashes only.  It intentionally does
not write resource bytes, which makes its output suitable for clean-room notes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

from inspect_bill_container import HEADER_SIZE, MAGIC, parse


MAX_RESOURCE_BYTES = 0x40000 * 4


def scan_bytes(data: bytes) -> list[dict[str, object]]:
    """Return metadata for every non-overlapping, structurally valid resource."""
    results: list[dict[str, object]] = []
    cursor = 0
    while True:
        offset = data.find(MAGIC, cursor)
        if offset < 0:
            break
        cursor = offset + 1
        if offset + HEADER_SIZE > len(data):
            continue
        body_bytes = struct.unpack_from("<I", data, offset + 12)[0]
        total_bytes = HEADER_SIZE + body_bytes
        if (
            total_bytes < HEADER_SIZE
            or total_bytes > MAX_RESOURCE_BYTES
            or total_bytes % 4
            or offset + total_bytes > len(data)
        ):
            continue
        candidate = data[offset : offset + total_bytes]
        try:
            parsed = parse(candidate)
        except ValueError:
            continue
        parsed.pop("transformed")
        parsed["offset"] = offset
        parsed["offset_hex"] = f"0x{offset:x}"
        parsed["input_sha256"] = hashlib.sha256(candidate).hexdigest()
        results.append(parsed)
        cursor = offset + total_bytes
    return results


def scan_path(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    return {
        "path": str(path),
        "file_size": len(data),
        "file_sha256": hashlib.sha256(data).hexdigest(),
        "resources": scan_bytes(data),
    }


def iter_files(paths: list[Path], recursive: bool) -> list[Path]:
    files: list[Path] = []
    for path in paths:
        if path.is_file():
            files.append(path)
        elif path.is_dir() and recursive:
            files.extend(candidate for candidate in path.rglob("*") if candidate.is_file())
        else:
            raise ValueError(f"not a file, or directory without --recursive: {path}")
    return sorted(set(files))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="scan files for valid Bill containers and report metadata only"
    )
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--recursive", action="store_true")
    parser.add_argument(
        "--include-empty", action="store_true", help="include files with no valid resources"
    )
    args = parser.parse_args()

    reports = [scan_path(path) for path in iter_files(args.paths, args.recursive)]
    if not args.include_empty:
        reports = [report for report in reports if report["resources"]]
    print(json.dumps(reports, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
