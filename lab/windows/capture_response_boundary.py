#!/usr/bin/env python3
"""Pause traced QEMU at or after a DSP0 response index and capture its target."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import socket
import struct
import time
from pathlib import Path


RESPONSE_READ = re.compile(
    r"^vfio_region_read\s+\(.+:region0\+0x2068, 4\) = 0x([0-9a-f]+)$",
    re.IGNORECASE,
)
REGION_WRITE = re.compile(
    r"^vfio_region_write\s+\(.+:region0\+0x([0-9a-f]+), "
    r"0x([0-9a-f]+), 4\)$",
    re.IGNORECASE,
)
RESPONSE_PAGE_REGISTERS = tuple(0x2040 + index * 4 for index in range(8))


def update_response_page_words(line: str, words: dict[int, int]) -> None:
    match = REGION_WRITE.match(line.rstrip("\r\n"))
    if match is None:
        return
    offset, value = (int(item, 16) for item in match.groups())
    if offset in RESPONSE_PAGE_REGISTERS:
        words[offset] = value


def response_page_addresses(words: dict[int, int]) -> list[int]:
    missing = [offset for offset in RESPONSE_PAGE_REGISTERS if offset not in words]
    if missing:
        raise ValueError(
            "response ring base is incomplete; missing "
            + ", ".join(f"0x{offset:04x}" for offset in missing)
        )
    return [
        words[0x2040 + page * 8] | (words[0x2044 + page * 8] << 32)
        for page in range(4)
    ]


def descriptor_for_consumed_index(page: bytes, consumed_index: int) -> dict[str, object]:
    descriptor_index = (consumed_index - 1) % 1024
    expected_page = descriptor_index // 256
    if len(page) != 4096:
        raise ValueError("captured ring page must be exactly 4096 bytes")
    words = struct.unpack_from("<4I", page, (descriptor_index % 256) * 16)
    return {
        "descriptor_index": descriptor_index,
        "ring_page": expected_page,
        "words": [f"0x{word:08x}" for word in words],
        "target_address": (words[3] << 32) | words[2],
        "length_or_flags": words[0] & 0x7FFFFFFF,
        "descriptor_valid": bool(words[0] & 0x80000000),
    }


def hmp_command(host: str, port: int, command: str) -> str:
    with socket.create_connection((host, port), timeout=5) as connection:
        connection.settimeout(5)
        received = bytearray()
        while b"(qemu)" not in received:
            chunk = connection.recv(4096)
            if not chunk:
                raise RuntimeError("QEMU monitor closed before prompt")
            received.extend(chunk)
        connection.sendall(command.encode("ascii") + b"\n")
        received.clear()
        while b"(qemu)" not in received:
            chunk = connection.recv(4096)
            if not chunk:
                raise RuntimeError("QEMU monitor closed during command")
            received.extend(chunk)
    response = received.decode("utf-8", errors="replace")
    failure_markers = (
        "invalid char",
        "unknown command",
        "try \"help",
        "could not open",
        "failed",
    )
    if any(marker in response.lower() for marker in failure_markers):
        raise RuntimeError(f"QEMU monitor rejected {command!r}: {response.strip()}")
    return response


def hmp_filename(path: Path) -> str:
    value = str(path)
    if '"' in value or "\n" in value or "\r" in value:
        raise ValueError("QEMU output path contains an unsupported character")
    return f'"{value}"'


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def capture(
    trace: Path,
    output: Path,
    target_index: int,
    monitor_host: str,
    monitor_port: int,
    timeout: float,
) -> dict[str, object]:
    output.mkdir(parents=True, exist_ok=True)
    if not output.is_absolute():
        raise ValueError("output directory must be absolute for QEMU pmemsave")
    words: dict[int, int] = {}
    with trace.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            update_response_page_words(line, words)
        deadline = time.monotonic() + timeout
        observed_index = None
        while time.monotonic() < deadline:
            line = stream.readline()
            if not line:
                time.sleep(0.01)
                continue
            update_response_page_words(line, words)
            match = RESPONSE_READ.match(line.rstrip("\r\n"))
            if match is not None and int(match.group(1), 16) >= target_index:
                observed_index = int(match.group(1), 16)
                break
    if observed_index is None:
        raise TimeoutError(
            f"response read index at or above {target_index} was not observed"
        )

    ring_pages = response_page_addresses(words)
    hmp_command(monitor_host, monitor_port, "stop")
    descriptor_index = (observed_index - 1) % 1024
    ring_page_index = descriptor_index // 256
    ring_capture = output / f"response-ring-page{ring_page_index}.bin"
    hmp_command(
        monitor_host,
        monitor_port,
        f"pmemsave 0x{ring_pages[ring_page_index]:x} 0x1000 "
        f"{hmp_filename(ring_capture)}",
    )
    descriptor = descriptor_for_consumed_index(ring_capture.read_bytes(), observed_index)
    if not descriptor["descriptor_valid"]:
        raise RuntimeError("consumed response entry is not a valid DMA descriptor")
    target_address = int(descriptor["target_address"])
    if target_address == 0:
        raise RuntimeError("consumed response descriptor has a null target")
    target_capture = output / "response-target.bin"
    hmp_command(
        monitor_host,
        monitor_port,
        f"pmemsave 0x{target_address:x} 0x1000 {hmp_filename(target_capture)}",
    )
    return {
        "schema": 1,
        "requested_minimum_response_read_index": target_index,
        "response_read_index": observed_index,
        "qemu_status": "paused",
        "response_ring_page_addresses": [f"0x{address:016x}" for address in ring_pages],
        "descriptor": {
            **descriptor,
            "target_address": f"0x{target_address:016x}",
        },
        "ring_page": {
            "path": str(ring_capture),
            "sha256": sha256(ring_capture),
        },
        "target_page": {
            "path": str(target_capture),
            "sha256": sha256(target_capture),
            "all_zero": not any(target_capture.read_bytes()),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--target-index",
        type=lambda value: int(value, 0),
        required=True,
        help="minimum DSP0 response read index that triggers the pause",
    )
    parser.add_argument("--monitor-host", default="127.0.0.1")
    parser.add_argument("--monitor-port", type=int, default=4444)
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args()
    if not 0 < args.target_index < 1024:
        raise SystemExit("refusing: target index must be between 1 and 1023")
    try:
        result = capture(
            args.trace,
            args.output,
            args.target_index,
            args.monitor_host,
            args.monitor_port,
            args.timeout,
        )
    except (OSError, RuntimeError, TimeoutError, ValueError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
