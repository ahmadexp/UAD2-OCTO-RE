#!/usr/bin/env python3
"""Aggregate metadata-only structural tests over embedded ``Bill`` resources.

The analyzer reads user-supplied modules but emits no resource bytes, keys, or
decoded payloads.  Its hash tests are deliberately narrow negative tests, not
claims about the unknown inner format.
"""

from __future__ import annotations

import argparse
import binascii
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib

from inspect_bill_container import HEADER_SIZE
from scan_bill_resources import iter_files, scan_bytes


HYPOTHESIS_NAMES = (
    "prefix_first_32_equals_sha256_replacement_region",
    "prefix_last_32_equals_sha256_replacement_region",
    "prefix_last_32_equals_sha256_outer_header_and_replacement_region",
    "prefix_last_32_equals_sha256_earlier_prefix_and_replacement_region",
)

MAGIC_SIGNATURES = {
    "elf": b"\x7fELF",
    "gzip": b"\x1f\x8b",
    "xz": b"\xfd7zXZ\x00",
    "bzip2": b"BZh",
    "zip": b"PK\x03\x04",
    "zlib_78_01": b"\x78\x01",
    "zlib_78_9c": b"\x78\x9c",
    "zlib_78_da": b"\x78\xda",
    "lz4_frame": b"\x04\x22\x4d\x18",
    "zstd": b"\x28\xb5\x2f\xfd",
}

DIGEST_FACTORIES = {
    "md5": hashlib.md5,
    "sha1": hashlib.sha1,
    "sha224": hashlib.sha224,
    "sha256": hashlib.sha256,
    "sha384": hashlib.sha384,
    "sha512": hashlib.sha512,
    "blake2s": hashlib.blake2s,
    "blake2b": hashlib.blake2b,
}


def _entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = Counter(data)
    return -sum((count / len(data)) * math.log2(count / len(data)) for count in counts.values())


def _summary(values: list[float]) -> dict[str, float | int | None]:
    if not values:
        return {"count": 0, "minimum": None, "mean": None, "maximum": None}
    return {
        "count": len(values),
        "minimum": round(min(values), 6),
        "mean": round(sum(values) / len(values), 6),
        "maximum": round(max(values), 6),
    }


def _aligned_blocks(data: bytes, width: int = 16) -> list[bytes]:
    return [data[offset : offset + width] for offset in range(0, len(data) - width + 1, width)]


def _pair_metrics(left: tuple[dict[str, object], bytes], right: tuple[dict[str, object], bytes]) -> dict[str, object]:
    left_resource, left_raw = left
    right_resource, right_raw = right
    left_preserved = int(left_resource["preserved_bytes"])
    right_preserved = int(right_resource["preserved_bytes"])
    left_core = left_raw[left_preserved:]
    right_core = right_raw[right_preserved:]
    overlap = min(len(left_core), len(right_core))
    equal = sum(a == b for a, b in zip(left_core[:overlap], right_core[:overlap]))
    xor = bytes(a ^ b for a, b in zip(left_core[:overlap], right_core[:overlap]))
    return {
        "same_resource_id": left_resource["resource_id"] == right_resource["resource_id"],
        "same_file_size": len(left_raw) == len(right_raw),
        "same_preserved_size": left_preserved == right_preserved,
        "overlap_bytes": overlap,
        "equal_byte_fraction": equal / overlap if overlap else 0.0,
        "xor_entropy": _entropy(xor),
    }


def _counter(counter: Counter[int]) -> dict[str, int]:
    return {str(key): counter[key] for key in sorted(counter)}


def analyze_files(paths: list[Path]) -> dict[str, object]:
    instances: list[tuple[dict[str, object], bytes]] = []
    per_file: list[list[tuple[dict[str, object], bytes]]] = []
    files_with_resources = 0
    for path in paths:
        data = path.read_bytes()
        resources = scan_bytes(data)
        if resources:
            files_with_resources += 1
        file_instances = []
        for resource in resources:
            offset = int(resource["offset"])
            size = int(resource["file_size"])
            item = (resource, data[offset : offset + size])
            instances.append(item)
            file_instances.append(item)
        per_file.append(file_instances)

    prefix_sizes: Counter[int] = Counter()
    preserved_sizes: Counter[int] = Counter()
    replacement_mod16: Counter[int] = Counter()
    generations: Counter[int] = Counter()
    resource_types: Counter[int] = Counter()
    payload_forms: Counter[int] = Counter()
    hypothesis = {
        name: {"eligible": 0, "matches": 0} for name in HYPOTHESIS_NAMES
    }
    checksum_hypotheses: dict[str, dict[str, int]] = {}
    digest_subsequence_hypotheses: dict[str, dict[str, int]] = {}
    magic_at_core_start: Counter[str] = Counter()
    decompression_successes: Counter[str] = Counter()
    prefix_entropy: list[float] = []
    core_entropy: list[float] = []
    core_zlib_ratio: list[float] = []
    duplicate_blocks_per_resource: list[float] = []

    unique: dict[str, tuple[dict[str, object], bytes]] = {}
    for resource, raw in instances:
        preserved = int(resource["preserved_bytes"])
        prefix_size = preserved - HEADER_SIZE
        prefix = raw[HEADER_SIZE:preserved]
        replacement = raw[preserved:]
        preserved_sizes[preserved] += 1
        prefix_sizes[prefix_size] += 1
        replacement_mod16[len(replacement) % 16] += 1
        generations[int(resource["dsp_generation"])] += 1
        resource_types[int(resource["resource_type"])] += 1
        payload_forms[int(resource["payload_form"])] += 1
        unique.setdefault(str(resource["input_sha256"]), (resource, raw))

        if len(prefix) >= 32:
            tests = {
                HYPOTHESIS_NAMES[0]: prefix[:32]
                == hashlib.sha256(replacement).digest(),
                HYPOTHESIS_NAMES[1]: prefix[-32:]
                == hashlib.sha256(replacement).digest(),
                HYPOTHESIS_NAMES[2]: prefix[-32:]
                == hashlib.sha256(raw[:HEADER_SIZE] + replacement).digest(),
                HYPOTHESIS_NAMES[3]: prefix[-32:]
                == hashlib.sha256(prefix[:-32] + replacement).digest(),
            }
            for name, matches in tests.items():
                hypothesis[name]["eligible"] += 1
                hypothesis[name]["matches"] += int(matches)

    global_block_owners: dict[bytes, set[str]] = {}
    for digest, (resource, raw) in unique.items():
        preserved = int(resource["preserved_bytes"])
        prefix = raw[HEADER_SIZE:preserved]
        core = raw[preserved:]
        prefix_entropy.append(_entropy(prefix))
        core_entropy.append(_entropy(core))
        if core:
            core_zlib_ratio.append(len(zlib.compress(core, 9)) / len(core))

        blocks = _aligned_blocks(core)
        duplicate_blocks_per_resource.append(
            (len(blocks) - len(set(blocks))) / len(blocks) if blocks else 0.0
        )
        for block in set(blocks):
            global_block_owners.setdefault(block, set()).add(digest)

        for name, magic in MAGIC_SIGNATURES.items():
            if core.startswith(magic):
                magic_at_core_start[name] += 1
                if name.startswith("zlib_"):
                    try:
                        zlib.decompress(core)
                    except zlib.error:
                        pass
                    else:
                        decompression_successes["zlib"] += 1

        values = {
            "crc32_core": binascii.crc32(core) & 0xFFFFFFFF,
            "adler32_core": zlib.adler32(core) & 0xFFFFFFFF,
            "crc32_body": binascii.crc32(raw[HEADER_SIZE:]) & 0xFFFFFFFF,
            "adler32_body": zlib.adler32(raw[HEADER_SIZE:]) & 0xFFFFFFFF,
            "core_size_bytes": len(core),
            "resource_file_size_bytes": len(raw),
            "resource_id": int(str(resource["resource_id"]), 16),
            "attributes": int(str(resource["attributes"]), 16),
        }
        dwords = [struct.unpack_from("<I", prefix, offset)[0] for offset in range(0, len(prefix) - 3, 4)]
        for name, value in values.items():
            entry = checksum_hypotheses.setdefault(name, {"eligible": 0, "matches_any_prefix_dword": 0})
            entry["eligible"] += 1
            entry["matches_any_prefix_dword"] += int(value in dwords)

        digest_inputs = {
            "inner_core": core,
            "outer_header_and_inner_core": raw[:HEADER_SIZE] + core,
        }
        for algorithm, factory in DIGEST_FACTORIES.items():
            for layout, candidate in digest_inputs.items():
                digest = factory(candidate).digest()
                name = f"{algorithm}_{layout}"
                entry = digest_subsequence_hypotheses.setdefault(
                    name, {"eligible": 0, "matches_anywhere_in_prefix": 0}
                )
                if len(prefix) >= len(digest):
                    entry["eligible"] += 1
                    entry["matches_anywhere_in_prefix"] += int(digest in prefix)

    prefix_dword_values = [set() for _ in range(8)]
    for _resource, raw in unique.values():
        prefix = raw[HEADER_SIZE:]
        for index in range(8):
            start = index * 4
            if start + 4 <= len(prefix):
                prefix_dword_values[index].add(struct.unpack_from("<I", prefix, start)[0])

    adjacent_generation_pairs = []
    for file_instances in per_file:
        index = 0
        while index < len(file_instances) - 1:
            left = file_instances[index]
            right = file_instances[index + 1]
            if {int(left[0]["dsp_generation"]), int(right[0]["dsp_generation"])} == {1, 2}:
                adjacent_generation_pairs.append(_pair_metrics(left, right))
                index += 2
            else:
                index += 1

    shared_blocks = [owners for owners in global_block_owners.values() if len(owners) > 1]

    return {
        "schema": 2,
        "input_files": len(paths),
        "files_with_resources": files_with_resources,
        "resource_instances": len(instances),
        "unique_resource_sha256": len(unique),
        "duplicate_instances": len(instances) - len(unique),
        "distributions": {
            "dsp_generation": _counter(generations),
            "resource_type": _counter(resource_types),
            "payload_form": _counter(payload_forms),
            "preserved_bytes_including_header": _counter(preserved_sizes),
            "opaque_prefix_bytes_after_header": _counter(prefix_sizes),
            "replacement_region_size_mod_16": _counter(replacement_mod16),
        },
        "unique_values_by_first_eight_prefix_dwords": {
            f"0x{index * 4:02x}": len(values)
            for index, values in enumerate(prefix_dword_values)
        },
        "direct_sha256_hypotheses": hypothesis,
        "simple_32_bit_metadata_hypotheses": checksum_hypotheses,
        "standard_digest_subsequence_hypotheses": digest_subsequence_hypotheses,
        "inner_core_start_magic": {
            name: magic_at_core_start[name] for name in sorted(MAGIC_SIGNATURES)
        },
        "successful_standard_decompression": dict(sorted(decompression_successes.items())),
        "entropy_bits_per_byte": {
            "opaque_prefix": _summary(prefix_entropy),
            "inner_core": _summary(core_entropy),
        },
        "inner_core_zlib_size_ratio": _summary(core_zlib_ratio),
        "aligned_16_byte_block_tests": {
            "duplicate_fraction_within_unique_resource": _summary(duplicate_blocks_per_resource),
            "distinct_blocks_shared_by_multiple_unique_resources": len(shared_blocks),
            "maximum_unique_resource_owners_for_one_block": max((len(owners) for owners in shared_blocks), default=0),
        },
        "adjacent_generation_1_2_pairs": {
            "count": len(adjacent_generation_pairs),
            "same_resource_id": sum(bool(pair["same_resource_id"]) for pair in adjacent_generation_pairs),
            "same_file_size": sum(bool(pair["same_file_size"]) for pair in adjacent_generation_pairs),
            "same_preserved_size": sum(bool(pair["same_preserved_size"]) for pair in adjacent_generation_pairs),
            "core_equal_byte_fraction": _summary([float(pair["equal_byte_fraction"]) for pair in adjacent_generation_pairs]),
            "core_xor_entropy_bits_per_byte": _summary([float(pair["xor_entropy"]) for pair in adjacent_generation_pairs]),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--recursive", action="store_true")
    args = parser.parse_args()
    try:
        paths = iter_files(args.paths, args.recursive)
        result = analyze_files(paths)
    except (OSError, ValueError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
