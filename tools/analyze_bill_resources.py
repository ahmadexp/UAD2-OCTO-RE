#!/usr/bin/env python3
"""Aggregate metadata-only structural tests over embedded ``Bill`` resources.

The analyzer reads user-supplied modules but emits no resource bytes, keys, or
decoded payloads.  Its hash tests are deliberately narrow negative tests, not
claims about the unknown inner format.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

from inspect_bill_container import HEADER_SIZE
from scan_bill_resources import iter_files, scan_bytes


HYPOTHESIS_NAMES = (
    "prefix_first_32_equals_sha256_replacement_region",
    "prefix_last_32_equals_sha256_replacement_region",
    "prefix_last_32_equals_sha256_outer_header_and_replacement_region",
    "prefix_last_32_equals_sha256_earlier_prefix_and_replacement_region",
)


def _counter(counter: Counter[int]) -> dict[str, int]:
    return {str(key): counter[key] for key in sorted(counter)}


def analyze_files(paths: list[Path]) -> dict[str, object]:
    instances: list[tuple[dict[str, object], bytes]] = []
    files_with_resources = 0
    for path in paths:
        data = path.read_bytes()
        resources = scan_bytes(data)
        if resources:
            files_with_resources += 1
        for resource in resources:
            offset = int(resource["offset"])
            size = int(resource["file_size"])
            instances.append((resource, data[offset : offset + size]))

    prefix_sizes: Counter[int] = Counter()
    preserved_sizes: Counter[int] = Counter()
    replacement_mod16: Counter[int] = Counter()
    generations: Counter[int] = Counter()
    resource_types: Counter[int] = Counter()
    payload_forms: Counter[int] = Counter()
    hypothesis = {
        name: {"eligible": 0, "matches": 0} for name in HYPOTHESIS_NAMES
    }

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

    prefix_dword_values = [set() for _ in range(8)]
    for _resource, raw in unique.values():
        prefix = raw[HEADER_SIZE:]
        for index in range(8):
            start = index * 4
            if start + 4 <= len(prefix):
                prefix_dword_values[index].add(struct.unpack_from("<I", prefix, start)[0])

    return {
        "schema": 1,
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
