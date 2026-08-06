#!/usr/bin/env python3
"""Summarize a captured UAD-2 response page without emitting payload bytes."""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import struct
from pathlib import Path


PAGE_BYTES = 4096
PAGE_DWORDS = PAGE_BYTES // 4
AUTHORIZATION_STATES = {
    0x80000000,
    0x81000000,
    0x82000000,
    0x83000000,
}
BILL_INTERMEDIATE_HEADER = 0x80070004
BILL_FINAL_HEADER = 0x80020044
BILL_FINAL_STATUS_CLASS = 0xF0060000


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def analyze(data: bytes) -> dict[str, object]:
    if len(data) != PAGE_BYTES:
        raise ValueError("response capture must be exactly 4096 bytes")
    words = struct.unpack(f"<{PAGE_DWORDS}I", data)
    header = words[0]
    if not any(data):
        return {
            "schema": 1,
            "page_sha256": sha256(data),
            "header": "0x00000000",
            "valid_bit": False,
            "response_class": "0x00000000",
            "declared_dwords": 0,
            "declared_bytes": 0,
            "request_token": "0x00000000",
            "body_dwords": 0,
            "body_sha256": sha256(b""),
            "body_value_counts": {},
            "trailing_bytes": PAGE_BYTES,
            "trailing_all_zero": True,
            "response_kind": "all-zero",
            "authorization_table_candidate": False,
            "bill_intermediate_success": False,
            "bill_final_response": False,
        }
    declared_dwords = header & 0xFFFF
    if declared_dwords < 2 or declared_dwords > PAGE_DWORDS:
        raise ValueError(
            f"declared response length {declared_dwords} is outside the page"
        )
    response_words = words[:declared_dwords]
    body = response_words[2:]
    counts = collections.Counter(body)
    body_bytes = data[8 : declared_dwords * 4]
    trailing = data[declared_dwords * 4 :]
    authorization_candidate = (
        declared_dwords == 770
        and len(body) == 768
        and set(body).issubset(AUTHORIZATION_STATES)
    )
    bill_intermediate = (
        header == BILL_INTERMEDIATE_HEADER
        and words[1] == 0
        and declared_dwords == 4
    )
    bill_final = (
        header == BILL_FINAL_HEADER
        and words[1] == 0
        and words[2] == 0
        and (words[3] & 0xFFFF0000) == BILL_FINAL_STATUS_CLASS
    )
    if authorization_candidate:
        response_kind = "authorization-table"
    elif bill_intermediate:
        response_kind = "bill-intermediate-success"
    elif bill_final:
        response_kind = "bill-final"
    else:
        response_kind = "unclassified"
    result: dict[str, object] = {
        "schema": 1,
        "page_sha256": sha256(data),
        "header": f"0x{header:08x}",
        "valid_bit": bool(header & 0x80000000),
        "response_class": f"0x{header & 0xFFFF0000:08x}",
        "declared_dwords": declared_dwords,
        "declared_bytes": declared_dwords * 4,
        "request_token": f"0x{words[1]:08x}",
        "body_dwords": len(body),
        "body_sha256": sha256(body_bytes),
        "body_value_counts": {
            f"0x{value:08x}": count for value, count in sorted(counts.items())
        },
        "trailing_bytes": len(trailing),
        "trailing_all_zero": not any(trailing),
        "response_kind": response_kind,
        "authorization_table_candidate": authorization_candidate,
        "bill_intermediate_success": bill_intermediate,
        "bill_final_response": bill_final,
    }
    if authorization_candidate:
        result["authorization_state_indices"] = {
            f"0x{state:08x}": [index for index, value in enumerate(body) if value == state]
            for state in sorted(AUTHORIZATION_STATES)
        }
    if bill_intermediate:
        result["resource_id"] = f"0x{words[2]:08x}"
        result["resource_command_word"] = f"0x{words[3]:08x}"
    if bill_final:
        result["bill_final_status"] = f"0x{words[3]:08x}"
        result["bill_final_status_code"] = words[3] & 0xFFFF
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    try:
        result = analyze(args.capture.read_bytes())
    except (OSError, ValueError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
