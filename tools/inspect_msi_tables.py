#!/usr/bin/env python3
"""Inspect selected tables extracted from an MSI compound document.

Use 7-Zip to extract the compound-document streams first:

    7zz x -tCompound -o/tmp/msi-tables package.msi
    python3 tools/inspect_msi_tables.py /tmp/msi-tables --table File

The tool is intentionally read-only and has no dependency on Windows Installer.
"""

from __future__ import annotations

import argparse
import json
import struct
from dataclasses import dataclass
from pathlib import Path


COL_FIELD_SIZE_MASK = 0x00FF
COL_STRING_BIT = 0x0800


@dataclass(frozen=True)
class Column:
    name: str
    number: int
    attributes: int

    @property
    def width(self) -> int:
        if self.attributes & COL_STRING_BIT:
            return 2
        width = self.attributes & COL_FIELD_SIZE_MASK
        if width not in (2, 4):
            raise ValueError(f"unsupported column width {width} for {self.name}")
        return width

    @property
    def is_string(self) -> bool:
        return bool(self.attributes & COL_STRING_BIT)


def load_string_pool(directory: Path) -> list[str]:
    pool = (directory / "!_StringPool").read_bytes()
    data = (directory / "!_StringData").read_bytes()
    if len(pool) < 4 or len(pool) % 4:
        raise ValueError("invalid _StringPool size")

    codepage, flags = struct.unpack_from("<HH", pool)
    if flags & 0x8000:
        raise ValueError("three-byte MSI string references are not supported")
    codec = "utf-8" if codepage == 65001 else f"cp{codepage}"

    strings = [""]
    data_offset = 0
    pool_offset = 4
    while pool_offset < len(pool):
        length, references = struct.unpack_from("<HH", pool, pool_offset)
        pool_offset += 4
        if length == 0 and references:
            if pool_offset + 4 > len(pool):
                raise ValueError("truncated long-string length")
            length = struct.unpack_from("<I", pool, pool_offset)[0]
            pool_offset += 4
        end = data_offset + length
        if end > len(data):
            raise ValueError("string extends past _StringData")
        strings.append(data[data_offset:end].decode(codec, "replace"))
        data_offset = end

    if data_offset != len(data):
        raise ValueError("unconsumed bytes in _StringData")
    return strings


def decode_integer(raw: int, width: int) -> int | None:
    if raw == 0:
        return None
    return raw ^ (0x8000 if width == 2 else 0x80000000)


def read_columns(directory: Path, strings: list[str]) -> dict[str, list[Column]]:
    raw = (directory / "!_Columns").read_bytes()
    row_width = 8
    if len(raw) % row_width:
        raise ValueError("invalid _Columns size")
    rows = len(raw) // row_width
    values: list[tuple[int, ...]] = []
    offset = 0
    for _ in range(4):
        values.append(struct.unpack_from("<" + "H" * rows, raw, offset))
        offset += 2 * rows

    tables: dict[str, list[Column]] = {}
    for index in range(rows):
        table_ref, number_raw, name_ref, type_raw = (column[index] for column in values)
        table = strings[table_ref]
        number = decode_integer(number_raw, 2)
        attributes = decode_integer(type_raw, 2)
        if number is None or attributes is None:
            raise ValueError("null value in _Columns")
        tables.setdefault(table, []).append(Column(strings[name_ref], number, attributes))
    for columns in tables.values():
        columns.sort(key=lambda column: column.number)
    return tables


def read_table(
    directory: Path, table: str, columns: list[Column], strings: list[str]
) -> list[dict[str, object]]:
    raw = (directory / f"!{table}").read_bytes()
    row_width = sum(column.width for column in columns)
    if not row_width or len(raw) % row_width:
        raise ValueError(f"invalid {table} table size")
    rows = len(raw) // row_width

    decoded_columns: list[list[object]] = []
    offset = 0
    for column in columns:
        fmt = "H" if column.width == 2 else "I"
        values = struct.unpack_from("<" + fmt * rows, raw, offset)
        offset += column.width * rows
        if column.is_string:
            decoded_columns.append([strings[value] for value in values])
        else:
            decoded_columns.append(
                [decode_integer(value, column.width) for value in values]
            )

    return [
        {column.name: decoded_columns[col_index][row_index] for col_index, column in enumerate(columns)}
        for row_index in range(rows)
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--table", default="File")
    parser.add_argument("--match", help="case-insensitive substring match on JSON rows")
    args = parser.parse_args()

    strings = load_string_pool(args.directory)
    schemas = read_columns(args.directory, strings)
    if args.table not in schemas:
        raise SystemExit(f"table not found: {args.table}")
    rows = read_table(args.directory, args.table, schemas[args.table], strings)
    if args.match:
        needle = args.match.casefold()
        rows = [row for row in rows if needle in json.dumps(row).casefold()]
    print(json.dumps(rows, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
