#!/usr/bin/env python3
"""Capture and correlate every DSP0 command/response boundary from traced QEMU."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import socket
import struct
import time
from dataclasses import dataclass, field
from pathlib import Path


REGION_ACCESS = re.compile(
    r"^vfio_region_(?P<kind>read|write)\s+"
    r"\(.+:region0\+0x(?P<offset>[0-9a-f]+), "
    r"(?:(?:0x(?P<write_value>[0-9a-f]+), 4\)|"
    r"4\) = 0x(?P<read_value>[0-9a-f]+)))$",
    re.IGNORECASE,
)
COMMAND_BASE = 0x2000
RESPONSE_BASE = 0x2040
RING_PAGES = 4
RING_ENTRIES = 1024
PAGE_BYTES = 4096
MAX_CAPTURE_BYTES = 256 * 1024
# UAD2System.sys waits 600 ms for an ordinary resource completion.  Every
# diagnostic pause must remain comfortably below that deadline or the capture
# changes the loader path it is trying to observe.
RESOURCE_COMPLETION_WAIT_MS = 600
RESPONSE_SAMPLE_DELAYS_MS = (0, 5, 100)


def ring_page_registers(base: int) -> tuple[int, ...]:
    return tuple(base + index * 4 for index in range(RING_PAGES * 2))


COMMAND_PAGE_REGISTERS = ring_page_registers(COMMAND_BASE)
RESPONSE_PAGE_REGISTERS = ring_page_registers(RESPONSE_BASE)


@dataclass
class TraceState:
    command_page_words: dict[int, int] = field(default_factory=dict)
    response_page_words: dict[int, int] = field(default_factory=dict)
    command_host_index: int | None = None
    command_read_index: int | None = None
    response_host_index: int | None = None
    response_read_index: int | None = None
    line_number: int = 0


def update_trace_state(
    line: str, state: TraceState
) -> tuple[str, int, int] | None:
    """Update traced register state and return a protocol-boundary transition."""
    state.line_number += 1
    match = REGION_ACCESS.match(line.rstrip("\r\n"))
    if match is None:
        return None
    offset = int(match.group("offset"), 16)
    raw_value = match.group("write_value") or match.group("read_value")
    value = int(raw_value, 16)
    kind = match.group("kind").lower()

    if kind == "write":
        if offset in COMMAND_PAGE_REGISTERS:
            state.command_page_words[offset] = value
        elif offset in RESPONSE_PAGE_REGISTERS:
            state.response_page_words[offset] = value
        elif offset == COMMAND_BASE + 0x24:
            previous = state.command_host_index
            state.command_host_index = value
            if previous is not None and previous != value:
                return "command-host", previous, value
        elif offset == RESPONSE_BASE + 0x24:
            state.response_host_index = value
        return None

    if offset == COMMAND_BASE + 0x28:
        state.command_read_index = value
    elif offset == RESPONSE_BASE + 0x28:
        previous = state.response_read_index
        state.response_read_index = value
        if previous is not None and previous != value:
            return "response-read", previous, value
    return None


def page_addresses(words: dict[int, int], base: int) -> list[int]:
    registers = ring_page_registers(base)
    missing = [offset for offset in registers if offset not in words]
    if missing:
        raise ValueError(
            f"ring at 0x{base:04x} is incomplete; missing "
            + ", ".join(f"0x{offset:04x}" for offset in missing)
        )
    return [
        words[base + page * 8] | (words[base + page * 8 + 4] << 32)
        for page in range(RING_PAGES)
    ]


def advanced_indices(previous: int, current: int) -> list[int]:
    if not 0 <= previous < RING_ENTRIES or not 0 <= current < RING_ENTRIES:
        raise ValueError("ring index is outside 0 through 1023")
    count = (current - previous) % RING_ENTRIES
    return [(previous + step) % RING_ENTRIES for step in range(count)]


def descriptor_words(pages: list[bytes], index: int) -> tuple[int, int, int, int]:
    if len(pages) != RING_PAGES or any(len(page) != PAGE_BYTES for page in pages):
        raise ValueError("four exact 4096-byte ring pages are required")
    if not 0 <= index < RING_ENTRIES:
        raise ValueError("descriptor index is outside 0 through 1023")
    page = pages[index // 256]
    return struct.unpack_from("<4I", page, (index % 256) * 16)


def dma_descriptor(words: tuple[int, int, int, int]) -> dict[str, int | bool]:
    length_dwords = words[0] & 0x7FFFFFFF
    return {
        "valid": bool(words[0] & 0x80000000),
        "length_dwords": length_dwords,
        "length_bytes": length_dwords * 4,
        "address": words[2] | (words[3] << 32),
    }


def classify_response(data: bytes) -> dict[str, object]:
    if len(data) != PAGE_BYTES:
        raise ValueError("response target must be exactly 4096 bytes")
    if not any(data):
        return {"kind": "all-zero", "header_words": ["0x00000000"] * 4}
    words = struct.unpack_from("<4I", data)
    header = words[0]
    declared = header & 0xFFFF
    if header == 0x80070004 and words[1] == 0:
        kind = "bill-intermediate-success"
    elif header == 0x80020044 and words[1] == 0:
        kind = "bill-final"
    elif declared == 770 and header & 0xFFFF0000 == 0x80030000:
        kind = "authorization-table-candidate"
    else:
        kind = "unclassified"
    return {
        "kind": kind,
        "declared_dwords": declared,
        "header_words": [f"0x{word:08x}" for word in words],
    }


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def hmp_filename(path: Path) -> str:
    value = str(path)
    if not path.is_absolute():
        raise ValueError("QEMU output path must be absolute")
    if '"' in value or "\n" in value or "\r" in value:
        raise ValueError("QEMU output path contains an unsupported character")
    return f'"{value}"'


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
        'try "help',
        "could not open",
        "failed",
    )
    if any(marker in response.lower() for marker in failure_markers):
        raise RuntimeError(f"QEMU monitor rejected {command!r}: {response.strip()}")
    return response


def save_memory(
    host: str, port: int, address: int, size: int, path: Path
) -> bytes:
    if address == 0:
        raise ValueError("refusing to capture a null guest address")
    if not 0 < size <= MAX_CAPTURE_BYTES:
        raise ValueError("capture size is outside the bounded 256 KiB limit")
    hmp_command(
        host,
        port,
        f"pmemsave 0x{address:x} 0x{size:x} {hmp_filename(path)}",
    )
    data = path.read_bytes()
    if len(data) != size:
        raise RuntimeError(f"short pmemsave for {path}: {len(data)} != {size}")
    return data


def dump_ring(
    host: str,
    port: int,
    addresses: list[int],
    output: Path,
    name: str,
) -> list[bytes]:
    pages = []
    for page_index, address in enumerate(addresses):
        path = output / f"{name}-ring-page{page_index}.bin"
        pages.append(save_memory(host, port, address, PAGE_BYTES, path))
    return pages


def command_metadata(
    pages: list[bytes], indices: list[int], output: Path, host: str, port: int
) -> list[dict[str, object]]:
    result = []
    for index in indices:
        words = descriptor_words(pages, index)
        descriptor = dma_descriptor(words)
        record: dict[str, object] = {
            "index": index,
            "words": [f"0x{word:08x}" for word in words],
            "kind": "dma-descriptor" if descriptor["valid"] else "inline-command",
        }
        if descriptor["valid"]:
            address = int(descriptor["address"])
            length = int(descriptor["length_bytes"])
            record["address"] = f"0x{address:016x}"
            record["length_dwords"] = descriptor["length_dwords"]
            record["length_bytes"] = length
            if address and 0 < length <= MAX_CAPTURE_BYTES:
                path = output / f"command-{index:04d}-target.bin"
                payload = save_memory(host, port, address, length, path)
                record["target_path"] = str(path)
                record["target_sha256"] = sha256_bytes(payload)
                bill_offset = next(
                    (offset for offset in (0, 8) if payload[offset : offset + 4] == b"Bill"),
                    None,
                )
                record["bill_outer_header"] = bill_offset is not None
                if bill_offset is not None and len(payload) >= bill_offset + 20:
                    bill = struct.unpack_from("<4s4I", payload, bill_offset)
                    record["bill_metadata"] = {
                        "offset_bytes": bill_offset,
                        "resource_id": f"0x{bill[1]:08x}",
                        "format_word": f"0x{bill[2]:08x}",
                        "resource_type": bill[2] & 0xFFFF,
                        "payload_form": (bill[2] >> 16) & 0xFF,
                        "dsp_generation": (bill[2] >> 24) & 0xFF,
                        "declared_body_bytes": bill[3],
                        "replacement_dwords": bill[4],
                    }
                    if bill_offset == 8:
                        envelope = struct.unpack_from("<2I", payload)
                        record["resource_envelope"] = {
                            "command_word": f"0x{envelope[0]:08x}",
                            "allocation_offset_dwords": f"0x{envelope[1]:08x}",
                        }
        result.append(record)
    return result


def capture_boundary(
    state: TraceState,
    previous_response: int,
    current_response: int,
    previous_command: int,
    output: Path,
    monitor_host: str,
    monitor_port: int,
) -> dict[str, object]:
    command_addresses = page_addresses(state.command_page_words, COMMAND_BASE)
    response_addresses = page_addresses(state.response_page_words, RESPONSE_BASE)
    output.mkdir(parents=True, exist_ok=False)
    hmp_command(monitor_host, monitor_port, "stop")
    try:
        # Locate and snapshot the response target before dumping unrelated
        # command pages.  The official driver can release and reuse the target
        # shortly after the device advances the response read index.
        response_pages = dump_ring(
            monitor_host, monitor_port, response_addresses, output, "response"
        )
        responses = []
        for descriptor_index in advanced_indices(previous_response, current_response):
            words = descriptor_words(response_pages, descriptor_index)
            descriptor = dma_descriptor(words)
            record: dict[str, object] = {
                "index": descriptor_index,
                "words": [f"0x{word:08x}" for word in words],
                "valid": descriptor["valid"],
                "target_address": f"0x{int(descriptor['address']):016x}",
                "length_dwords": descriptor["length_dwords"],
            }
            address = int(descriptor["address"])
            if descriptor["valid"] and address:
                samples = []
                sample_start = time.monotonic()
                for requested_delay_ms in RESPONSE_SAMPLE_DELAYS_MS:
                    remaining = (
                        requested_delay_ms / 1000
                        - (time.monotonic() - sample_start)
                    )
                    if remaining > 0:
                        time.sleep(remaining)
                    path = output / (
                        f"response-{descriptor_index:04d}-"
                        f"t{requested_delay_ms:04d}ms.bin"
                    )
                    payload = save_memory(
                        monitor_host, monitor_port, address, PAGE_BYTES, path
                    )
                    samples.append(
                        {
                            "requested_delay_ms": requested_delay_ms,
                            "observed_elapsed_ms": round(
                                (time.monotonic() - sample_start) * 1000, 3
                            ),
                            "target_path": str(path),
                            "target_sha256": sha256_bytes(payload),
                            "classification": classify_response(payload),
                        }
                    )
                record["samples"] = samples
                record["classification"] = samples[-1]["classification"]
                record["first_nonzero_sample"] = next(
                    (
                        sample
                        for sample in samples
                        if sample["classification"]["kind"] != "all-zero"
                    ),
                    None,
                )
            responses.append(record)
        command_pages = dump_ring(
            monitor_host, monitor_port, command_addresses, output, "command"
        )
        command_current = state.command_host_index
        if command_current is None:
            raise ValueError("command host index has not been observed")
        commands = command_metadata(
            command_pages,
            advanced_indices(previous_command, command_current),
            output,
            monitor_host,
            monitor_port,
        )
        result = {
            "schema": 1,
            "trace_line": state.line_number,
            "indices": {
                "command_host": command_current,
                "command_read": state.command_read_index,
                "response_host": state.response_host_index,
                "response_read_previous": previous_response,
                "response_read_current": current_response,
            },
            "ring_addresses": {
                "command": [f"0x{value:016x}" for value in command_addresses],
                "response": [f"0x{value:016x}" for value in response_addresses],
            },
            "command_ring_sha256": [sha256_bytes(page) for page in command_pages],
            "response_ring_sha256": [sha256_bytes(page) for page in response_pages],
            "commands_since_previous_boundary": commands,
            "responses": responses,
        }
        (output / "metadata.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        return result
    except Exception:
        (output / "CAPTURE_FAILED").write_text(
            "QEMU was deliberately left paused after a capture failure.\n",
            encoding="utf-8",
        )
        raise


def capture_command_boundary(
    state: TraceState,
    previous_command: int,
    current_command: int,
    output: Path,
    monitor_host: str,
    monitor_port: int,
) -> dict[str, object]:
    command_addresses = page_addresses(state.command_page_words, COMMAND_BASE)
    output.mkdir(parents=True, exist_ok=False)
    hmp_command(monitor_host, monitor_port, "stop")
    try:
        command_pages = dump_ring(
            monitor_host, monitor_port, command_addresses, output, "command"
        )
        commands = command_metadata(
            command_pages,
            advanced_indices(previous_command, current_command),
            output,
            monitor_host,
            monitor_port,
        )
        result = {
            "schema": 1,
            "event": "command-host",
            "trace_line": state.line_number,
            "indices": {
                "command_host_previous": previous_command,
                "command_host_current": current_command,
                "command_read": state.command_read_index,
                "response_host": state.response_host_index,
                "response_read": state.response_read_index,
            },
            "ring_addresses": {
                "command": [f"0x{value:016x}" for value in command_addresses]
            },
            "command_ring_sha256": [sha256_bytes(page) for page in command_pages],
            "commands": commands,
            "responses": [],
        }
        (output / "metadata.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        return result
    except Exception:
        (output / "CAPTURE_FAILED").write_text(
            "QEMU was deliberately left paused after a capture failure.\n",
            encoding="utf-8",
        )
        raise


def capture_sequence(
    trace: Path,
    output: Path,
    monitor_host: str,
    monitor_port: int,
    timeout: float,
    max_boundaries: int,
    leave_paused: bool,
) -> dict[str, object]:
    if not output.is_absolute():
        raise ValueError("output directory must be absolute")
    if output.exists():
        raise ValueError("output directory already exists")
    output.mkdir(parents=True)
    state = TraceState()
    boundaries = []
    with trace.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            update_trace_state(line, state)
        if state.response_read_index is None or state.command_host_index is None:
            raise ValueError("initial command/response indices were not observed")
        deadline = time.monotonic() + timeout
        while len(boundaries) < max_boundaries and time.monotonic() < deadline:
            line = stream.readline()
            if not line:
                time.sleep(0.005)
                continue
            transition = update_trace_state(line, state)
            if transition is None:
                continue
            event, observed_previous, observed_current = transition
            boundary_dir = output / f"boundary-{len(boundaries):04d}"
            if event == "command-host":
                result = capture_command_boundary(
                    state,
                    observed_previous,
                    observed_current,
                    boundary_dir,
                    monitor_host,
                    monitor_port,
                )
            else:
                result = capture_boundary(
                    state,
                    observed_previous,
                    observed_current,
                    state.command_host_index,
                    boundary_dir,
                    monitor_host,
                    monitor_port,
                )
                result["event"] = "response-read"
                (boundary_dir / "metadata.json").write_text(
                    json.dumps(result, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8",
                )
            boundaries.append(result)
            if len(boundaries) < max_boundaries or not leave_paused:
                hmp_command(monitor_host, monitor_port, "cont")
            deadline = time.monotonic() + timeout
    summary = {
        "schema": 1,
        "trace": str(trace),
        "boundary_count": len(boundaries),
        "final_qemu_status": (
            "paused" if boundaries and leave_paused else "running-or-unavailable"
        ),
        "boundaries": [
            {
                "directory": f"boundary-{index:04d}",
                "event": item["event"],
                "trace_line": item["trace_line"],
                "indices": item["indices"],
                "response_kinds": [
                    response.get("classification", {}).get("kind", "not-captured")
                    for response in item["responses"]
                ],
            }
            for index, item in enumerate(boundaries)
        ],
    }
    (output / "sequence.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--monitor-host", default="127.0.0.1")
    parser.add_argument("--monitor-port", type=int, default=4444)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--max-boundaries", type=int, default=64)
    parser.add_argument("--leave-paused", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.max_boundaries <= 1024:
        raise SystemExit("refusing: max boundaries must be between 1 and 1024")
    try:
        result = capture_sequence(
            args.trace,
            args.output,
            args.monitor_host,
            args.monitor_port,
            args.timeout,
            args.max_boundaries,
            args.leave_paused,
        )
    except (OSError, RuntimeError, TimeoutError, ValueError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
