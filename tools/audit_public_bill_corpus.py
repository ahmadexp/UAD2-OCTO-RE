#!/usr/bin/env python3
"""Compare a hash-locked public Bill capture with an official cabinet.

Only container metadata, hashes, and differing byte offsets are emitted.  The
tool never writes or prints either corpus' resource bytes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

from inspect_bill_container import parse
from scan_bill_resources import iter_files, scan_bytes


PUBLIC_HEADER_SHA256 = "0e46c8480084c3dc4c1a7852c0cc271f30aaf78e6f4729ab39b4ab3ad254c1ee"
PUBLIC_COMMIT = "29a22b1254393488c5a5f55eb11db77c4d0251f9"
EXPECTED_ARRAYS = {
    "ua_dsp_prog_a5": 1940,
    "ua_dsp_prog_c2": 184,
    "ua_dsp_prog_db": 728,
    "ua_dsp_prog_eb": 716,
    "ua_dsp_prog_12b": 452,
}


def parse_public_header(text: str) -> dict[str, bytes]:
    arrays: dict[str, bytes] = {}
    pattern = re.compile(
        r"static const u8 (ua_dsp_prog_[a-z0-9]+)\[(\d+)\] = \{(.*?)\n\};",
        re.DOTALL,
    )
    for match in pattern.finditer(text):
        name = match.group(1)
        if name not in EXPECTED_ARRAYS:
            continue
        declared = int(match.group(2))
        raw = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group(3)))
        if declared != len(raw) or EXPECTED_ARRAYS[name] != len(raw):
            raise ValueError(f"public array size mismatch for {name}")
        parse(raw)
        arrays[name] = raw
    if set(arrays) != set(EXPECTED_ARRAYS):
        raise ValueError("public header does not contain the expected five arrays")
    return arrays


def _key(raw: bytes) -> tuple[int, int, int]:
    parsed = parse(raw)
    return (
        int(str(parsed["resource_id"]), 16) & 0x00FFFFFF,
        int(parsed["dsp_generation"]),
        len(raw),
    )


def compare_arrays(public: dict[str, bytes], official: dict[tuple[int, int, int], bytes]) -> list[dict[str, object]]:
    results = []
    for name in sorted(public):
        left = public[name]
        key = _key(left)
        if key not in official:
            raise ValueError(f"no official counterpart for {name}")
        right = official[key]
        differences = [index for index, (a, b) in enumerate(zip(left, right)) if a != b]
        left_parsed = parse(left)
        right_parsed = parse(right)
        results.append(
            {
                "public_name": name,
                "size_bytes": len(left),
                "public_resource_id": left_parsed["resource_id"],
                "official_resource_id": right_parsed["resource_id"],
                "public_sha256": hashlib.sha256(left).hexdigest(),
                "official_sha256": hashlib.sha256(right).hexdigest(),
                "differing_byte_count": len(differences),
                "differing_byte_offsets": differences,
                "all_bytes_after_resource_id_equal": left[8:] == right[8:],
            }
        )
    return results


def inspect(public_header: Path, cabinet: Path) -> dict[str, object]:
    header_data = public_header.read_bytes()
    digest = hashlib.sha256(header_data).hexdigest()
    if digest != PUBLIC_HEADER_SHA256:
        raise ValueError(
            f"public header hash mismatch: expected {PUBLIC_HEADER_SHA256}, found {digest}"
        )
    public = parse_public_header(header_data.decode("utf-8"))

    official: dict[tuple[int, int, int], bytes] = {}
    for path in iter_files([cabinet], recursive=True):
        data = path.read_bytes()
        for resource in scan_bytes(data):
            offset = int(resource["offset"])
            size = int(resource["file_size"])
            raw = data[offset : offset + size]
            official.setdefault(_key(raw), raw)

    comparisons = compare_arrays(public, official)
    return {
        "schema": 1,
        "public_commit": PUBLIC_COMMIT,
        "public_header_sha256": digest,
        "comparisons": comparisons,
        "all_five_match_except_resource_id_high_byte": all(
            item["differing_byte_offsets"] == [7]
            and item["all_bytes_after_resource_id_equal"]
            for item in comparisons
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("public_header", type=Path)
    parser.add_argument("cabinet", type=Path)
    args = parser.parse_args()
    try:
        result = inspect(args.public_header, args.cabinet)
    except (OSError, UnicodeError, ValueError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["all_five_match_except_resource_id_high_byte"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
