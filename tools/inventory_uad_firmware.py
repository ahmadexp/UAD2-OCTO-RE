#!/usr/bin/env python3
"""Inventory UAD firmware containers from an extracted MSI without copying them."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
from collections import Counter
from pathlib import Path

from inspect_msi_tables import load_string_pool, read_columns, read_table
from inspect_uad_container import inspect


HEADER_PREFIX_SIZE = 32
HEADER_TAIL_SIZE = 32
HEADER_SIZE = HEADER_PREFIX_SIZE + HEADER_TAIL_SIZE


def timestamp_utc(value: int) -> str | None:
    """Decode plausible Unix build timestamps while rejecting legacy counters."""
    lower = int(datetime.datetime(2000, 1, 1, tzinfo=datetime.timezone.utc).timestamp())
    upper = int(datetime.datetime(2100, 1, 1, tzinfo=datetime.timezone.utc).timestamp())
    if not lower <= value < upper:
        return None
    return datetime.datetime.fromtimestamp(value, datetime.timezone.utc).isoformat().replace(
        "+00:00", "Z"
    )


def sha256_tail_matches(data: bytes) -> list[str]:
    """Return common digest constructions matching the opaque 32-byte tail."""
    if len(data) < HEADER_SIZE:
        raise ValueError("container is shorter than its 64-byte header")
    prefix = data[:HEADER_PREFIX_SIZE]
    tail = data[HEADER_PREFIX_SIZE:HEADER_SIZE]
    payload = data[HEADER_SIZE:]
    candidates = {
        "payload": payload,
        "prefix_plus_payload": prefix + payload,
        "payload_plus_prefix": payload + prefix,
        "prefix": prefix,
    }
    return [
        name
        for name, material in candidates.items()
        if hashlib.sha256(material).digest() == tail
    ]


def inventory(tables: Path, files: Path) -> dict[str, object]:
    strings = load_string_pool(tables)
    schemas = read_columns(tables, strings)
    rows = read_table(tables, "File", schemas["File"], strings)
    records: list[dict[str, object]] = []
    for row in rows:
        installed_name = str(row["FileName"]).split("|")[-1]
        if not installed_name.startswith("FirmwareUpdate"):
            continue
        path = files / str(row["File"])
        parsed = inspect(path)
        data = path.read_bytes()
        words = [int(word, 16) for word in parsed["header_words"]]
        records.append(
            {
                "name": installed_name,
                "msi_file_id": row["File"],
                "magic": parsed["magic"],
                "size": parsed["file_size"],
                "sha256": parsed["sha256"],
                "payload_sha256": parsed["payload_sha256"],
                "payload_entropy_bits_per_byte": parsed[
                    "payload_entropy_bits_per_byte"
                ],
                "build_word": f"0x{words[1]:08x}",
                "build_timestamp_utc": timestamp_utc(words[1]),
                "format_word": f"0x{words[2]:08x}",
                "compatibility_id": parsed["compatibility_id"],
                "version_word": f"0x{words[4]:08x}",
                "platform_word": f"0x{words[5]:08x}",
                "declared_payload_dwords": parsed["declared_payload_dwords"],
                "constraint_word": f"0x{words[7]:08x}",
                "declared_size_matches_file": parsed["declared_size_matches_file"],
                "opaque_header_tail": parsed["opaque_header_tail"],
                "sha256_tail_matches": sha256_tail_matches(data),
            }
        )
    records.sort(key=lambda record: str(record["name"]))
    magics = Counter(str(record["magic"]) for record in records)
    return {
        "schema": 1,
        "container_count": len(records),
        "magic_distribution": dict(sorted(magics.items())),
        "all_declared_sizes_match": all(
            bool(record["declared_size_matches_file"]) for record in records
        ),
        "all_sha256_tail_hypotheses_rejected": all(
            not record["sha256_tail_matches"] for record in records
        ),
        "records": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="inventory FirmwareUpdate containers from extracted MSI tables"
    )
    parser.add_argument("tables", type=Path, help="directory containing !_StringPool and !File")
    parser.add_argument("files", type=Path, help="directory containing MSI file IDs")
    args = parser.parse_args()
    try:
        result = inventory(args.tables, args.files)
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit(str(error)) from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["all_declared_sizes_match"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
