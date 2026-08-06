#!/usr/bin/env python3
"""Inspect ADI SHARC ELF, Flat-V6 DLM, and standard loader images.

The tool emits metadata only.  It never writes section contents, decoded
payloads, or executable bytes.  Its strict mode is intended to reject an
incorrectly linked custom kernel before any hardware experiment.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
from typing import Any


ELF_MAGIC = b"\x7fELF"
ELFCLASS32 = 1
ELFDATA2LSB = 1
EM_SHARC = 0x85
ET_REL = 1
ET_EXEC = 2
ET_DYN = 3
PT_LOAD = 1
SHT_NOBITS = 8
SHT_REL = 9
SHT_RELA = 4
SHT_SYMTAB = 2
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4

ELF_TYPES = {ET_REL: "relocatable", ET_EXEC: "executable", ET_DYN: "dynamic"}
SECTION_TYPES = {
    0: "null",
    1: "progbits",
    SHT_SYMTAB: "symtab",
    3: "strtab",
    SHT_RELA: "rela",
    7: "note",
    SHT_NOBITS: "nobits",
    SHT_REL: "rel",
}

# ELF relocation identifiers emitted by the ADI SHARC toolchain.  A Flat-V6
# DLM maps these into a more specific relocation that also records the source
# and destination address-space widths.
SHARC_ELF_RELOCATIONS = {
    0x0B: "R_ADDR24_V3",
    0x0C: "R_ADDR32_V3",
    0x0D: "R_ADDR_VAR_V3",
    0x0E: "R_PCRSHORT_V3",
    0x0F: "R_PCRLONG_V3",
    0x10: "R_DATA6_V3",
    0x11: "R_DATA16_V3",
    0x12: "R_DATA6_VISA_V3",
    0x13: "R_DATA7_VISA_V3",
    0x14: "R_DATA16_VISA_V3",
    0x17: "R_PCR6_VISA_V3",
    0x19: "R_ADDR_VAR16_V3",
}

SHARC_LDR_TAGS = {
    0x0: ("FINAL_INIT", 0),
    0x1: ("ZERO_LDATA", 0),
    0x2: ("ZERO_L48", 0),
    0x3: ("INIT_L16", 2),
    0x4: ("INIT_L32", 4),
    0x5: ("INIT_L48", 6),
    0x6: ("INIT_L64", 8),
    0x7: ("ZERO_EXT8", 0),
    0x8: ("ZERO_EXT16", 0),
    0x9: ("INIT_EXT8", 1),
    0xA: ("INIT_EXT16", 2),
}

DLM_MAGIC = b"bFLT"
DLM_REVISION = 6
DLM_FAMILY_SHARC = 2
DLM_SECTION_CODE = 1 << 0
DLM_SECTION_NOBITS = 1 << 1
DLM_SECTION_RELOCATED = 1 << 2


class ImageError(ValueError):
    """An image is malformed or violates a requested contract."""


def _bounded(data: bytes, offset: int, size: int, label: str) -> bytes:
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise ImageError(f"{label} exceeds the file")
    return data[offset : offset + size]


def _cstring(table: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(table):
        return f"<bad-string-offset-0x{offset:x}>"
    end = table.find(b"\0", offset)
    if end < 0:
        end = len(table)
    return table[offset:end].decode("utf-8", errors="replace")


def _hex(value: int) -> str:
    return f"0x{value:08x}"


def inspect_elf(data: bytes) -> dict[str, Any]:
    if len(data) < 52 or data[:4] != ELF_MAGIC:
        raise ImageError("not an ELF image")
    ident = data[:16]
    if ident[4] != ELFCLASS32:
        raise ImageError("only ELF32 is supported")
    if ident[5] != ELFDATA2LSB:
        raise ImageError("only little-endian SHARC ELF is supported")

    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data, 0)
    (
        _ident,
        file_type,
        machine,
        version,
        entry,
        phoff,
        shoff,
        flags,
        ehsize,
        phentsize,
        phnum,
        shentsize,
        shnum,
        shstrndx,
    ) = header
    if machine != EM_SHARC:
        raise ImageError(f"ELF machine is 0x{machine:04x}, not SHARC 0x0085")
    if version != 1 or ehsize != 52:
        raise ImageError("unsupported ELF header version or size")
    if phnum and phentsize != 32:
        raise ImageError("unexpected ELF32 program-header size")
    if shnum and shentsize != 40:
        raise ImageError("unexpected ELF32 section-header size")
    _bounded(data, phoff, phnum * phentsize, "program-header table")
    _bounded(data, shoff, shnum * shentsize, "section-header table")
    if shnum == 0 or shstrndx >= shnum:
        raise ImageError("missing section-name string table")

    raw_sections = [
        struct.unpack_from("<IIIIIIIIII", data, shoff + index * shentsize)
        for index in range(shnum)
    ]
    shstr_header = raw_sections[shstrndx]
    shstr = _bounded(data, shstr_header[4], shstr_header[5], "section names")
    names = [_cstring(shstr, section[0]) for section in raw_sections]

    symbols_by_table: dict[int, list[dict[str, Any]]] = {}
    undefined_symbols: set[str] = set()
    for index, section in enumerate(raw_sections):
        (_name, sec_type, _flags, _addr, offset, size, link, _info, _align, entsize) = section
        if sec_type != SHT_SYMTAB:
            continue
        if link >= shnum or raw_sections[link][1] != 3:
            raise ImageError(f"symbol table {names[index]} has no valid string table")
        if entsize != 16 or size % entsize:
            raise ImageError(f"symbol table {names[index]} has invalid entry size")
        sym_bytes = _bounded(data, offset, size, f"symbol table {names[index]}")
        str_header = raw_sections[link]
        strings = _bounded(data, str_header[4], str_header[5], "symbol names")
        symbols: list[dict[str, Any]] = []
        for sym_index in range(size // entsize):
            name_off, value, sym_size, info, other, shndx = struct.unpack_from(
                "<IIIBBH", sym_bytes, sym_index * entsize
            )
            name = _cstring(strings, name_off)
            item = {
                "index": sym_index,
                "name": name,
                "value": _hex(value),
                "size": sym_size,
                "binding": info >> 4,
                "type": info & 0xF,
                "visibility": other & 0x3,
                "section_index": shndx,
            }
            symbols.append(item)
            if shndx == 0 and name and (info >> 4) != 0:
                undefined_symbols.add(name)
        symbols_by_table[index] = symbols

    relocation_counts: dict[str, int] = {}
    relocation_types: dict[str, int] = {}
    relocations: list[dict[str, Any]] = []
    for index, section in enumerate(raw_sections):
        (_name, sec_type, _flags, _addr, offset, size, link, info, _align, entsize) = section
        if sec_type not in (SHT_REL, SHT_RELA):
            continue
        expected = 12 if sec_type == SHT_RELA else 8
        if entsize != expected or size % entsize:
            raise ImageError(f"relocation table {names[index]} has invalid entry size")
        if link not in symbols_by_table or info >= shnum:
            raise ImageError(f"relocation table {names[index]} has invalid links")
        rel_bytes = _bounded(data, offset, size, f"relocation table {names[index]}")
        relocation_counts[names[index]] = size // entsize
        symbols = symbols_by_table[link]
        for rel_index in range(size // entsize):
            if sec_type == SHT_RELA:
                rel_offset, rel_info, addend = struct.unpack_from(
                    "<IIi", rel_bytes, rel_index * entsize
                )
            else:
                rel_offset, rel_info = struct.unpack_from(
                    "<II", rel_bytes, rel_index * entsize
                )
                addend = None
            sym_index = rel_info >> 8
            rel_type = rel_info & 0xFF
            rel_name = SHARC_ELF_RELOCATIONS.get(rel_type, f"UNKNOWN_0x{rel_type:02x}")
            relocation_types[rel_name] = relocation_types.get(rel_name, 0) + 1
            relocations.append(
                {
                    "table": names[index],
                    "target_section": names[info],
                    "offset": _hex(rel_offset),
                    "type": rel_name,
                    "symbol": symbols[sym_index]["name"] if sym_index < len(symbols) else "<bad-symbol-index>",
                    "addend": addend,
                }
            )

    sections = []
    for index, section in enumerate(raw_sections):
        (_name, sec_type, sec_flags, addr, offset, size, link, info, align, entsize) = section
        if sec_type != SHT_NOBITS:
            _bounded(data, offset, size, f"section {names[index]}")
        sections.append(
            {
                "index": index,
                "name": names[index],
                "type": SECTION_TYPES.get(sec_type, f"type-{sec_type}"),
                "address": _hex(addr),
                "bytes": size,
                "alignment": align,
                "entry_bytes": entsize,
                "alloc": bool(sec_flags & SHF_ALLOC),
                "write": bool(sec_flags & SHF_WRITE),
                "execute": bool(sec_flags & SHF_EXECINSTR),
                "link": link,
                "info": info,
            }
        )

    segments = []
    for index in range(phnum):
        ph = struct.unpack_from("<IIIIIIII", data, phoff + index * phentsize)
        p_type, p_offset, vaddr, paddr, filesz, memsz, p_flags, align = ph
        _bounded(data, p_offset, filesz, f"program segment {index}")
        segments.append(
            {
                "index": index,
                "type": "load" if p_type == PT_LOAD else f"type-{p_type}",
                "virtual_address": _hex(vaddr),
                "physical_address": _hex(paddr),
                "file_bytes": filesz,
                "memory_bytes": memsz,
                "flags": p_flags,
                "alignment": align,
            }
        )

    symbol_names = sorted(
        {symbol["name"] for symbols in symbols_by_table.values() for symbol in symbols if symbol["name"]}
    )
    defined_symbols = sorted(
        {
            symbol["name"]
            for symbols in symbols_by_table.values()
            for symbol in symbols
            if symbol["name"] and symbol["section_index"] != 0
        }
    )
    global_function_symbols = sorted(
        {
            symbol["name"]
            for symbols in symbols_by_table.values()
            for symbol in symbols
            if symbol["name"]
            and symbol["section_index"] != 0
            and symbol["binding"] in (1, 2)
            and symbol["type"] == 2
        }
    )
    return {
        "format": "elf32-sharc",
        "sha256": hashlib.sha256(data).hexdigest(),
        "file_bytes": len(data),
        "elf_type": ELF_TYPES.get(file_type, f"type-{file_type}"),
        "machine": "SHARC",
        "entry": _hex(entry),
        "flags": _hex(flags),
        "segments": segments,
        "sections": sections,
        "symbol_names": symbol_names,
        "defined_symbols": defined_symbols,
        "global_function_symbols": global_function_symbols,
        "undefined_symbols": sorted(undefined_symbols),
        "relocation_count": len(relocations),
        "relocation_counts_by_table": relocation_counts,
        "relocation_counts_by_type": dict(sorted(relocation_types.items())),
        "relocations": relocations,
    }


def _dlm_endian(data: bytes) -> str:
    if len(data) < 56 or data[:4] != DLM_MAGIC:
        raise ImageError("not an ADI Flat-V6 DLM")
    for endian in (">", "<"):
        rev, family = struct.unpack_from(f"{endian}II", data, 4)
        if rev == DLM_REVISION and family == DLM_FAMILY_SHARC:
            return endian
    raise ImageError("DLM revision/family is not Flat-V6 SHARC")


def inspect_dlm(data: bytes) -> dict[str, Any]:
    endian = _dlm_endian(data)
    words = struct.unpack_from(f"{endian}13I", data, 4)
    rev, family, sectab_start, sectab_count, reloc_start, reloc_count, build_date, *filler = words
    if any(filler):
        raise ImageError("DLM reserved header words are nonzero")
    _bounded(data, sectab_start, sectab_count * 28, "DLM section table")
    _bounded(data, reloc_start, reloc_count * 12, "DLM relocation table")

    sections = []
    raw_sections = []
    for index in range(sectab_count):
        values = struct.unpack_from(f"{endian}7I", data, sectab_start + index * 28)
        name_offset, start, byte_count, alignment, byte_width, flags, mapped_addr = values
        if byte_width not in (1, 2, 4, 5, 6, 8):
            raise ImageError(f"DLM section {index} has invalid byte width")
        name = _cstring(data, name_offset)
        if not (flags & DLM_SECTION_NOBITS):
            _bounded(data, start, byte_count, f"DLM section {name}")
        raw_sections.append(values)
        sections.append(
            {
                "index": index,
                "name": name,
                "file_offset": _hex(start),
                "bytes": byte_count,
                "alignment": alignment,
                "byte_width": byte_width,
                "code": bool(flags & DLM_SECTION_CODE),
                "nobits": bool(flags & DLM_SECTION_NOBITS),
                "pre_relocated": bool(flags & DLM_SECTION_RELOCATED),
                "mapped_address": _hex(mapped_addr),
            }
        )

    relocation_types: dict[str, int] = {}
    relocations = []
    for index in range(reloc_count):
        ref_offset, result, packed = struct.unpack_from(
            f"{endian}III", data, reloc_start + index * 12
        )
        reserved = packed & 0xFF
        rel_type = (packed >> 8) & 0xFF
        sym_section = (packed >> 16) & 0xFF
        ref_section = (packed >> 24) & 0xFF
        if reserved or ref_section >= sectab_count or sym_section >= sectab_count:
            raise ImageError(f"DLM relocation {index} has invalid section fields")
        if rel_type < 0x01 or rel_type > 0x3A:
            raise ImageError(f"DLM relocation {index} has unknown SHARC type 0x{rel_type:02x}")
        key = f"0x{rel_type:02x}"
        relocation_types[key] = relocation_types.get(key, 0) + 1
        relocations.append(
            {
                "index": index,
                "reference_section": sections[ref_section]["name"],
                "symbol_section": sections[sym_section]["name"],
                "reference_offset": _hex(ref_offset),
                "partial_result": _hex(result),
                "dynamic_type": key,
            }
        )

    export_section_counts = {
        name: sum(item["name"] == name for item in sections)
        for name in (".expstr", ".expsym")
    }
    exported_sections = {
        item["name"]: item["index"]
        for item in sections
        if item["name"] in (".expstr", ".expsym")
    }
    exported_names: list[str] = []
    if export_section_counts == {".expstr": 1, ".expsym": 1}:
        str_section = sections[exported_sections[".expstr"]]
        sym_section = sections[exported_sections[".expsym"]]
        if str_section["nobits"] or str_section["byte_width"] != 4:
            raise ImageError("SHARC DLM .expstr must be a stored DM32 section")
        if sym_section["nobits"] or sym_section["byte_width"] != 4:
            raise ImageError("SHARC DLM .expsym must be a stored DM32 section")
        if str_section["bytes"] % 4 or sym_section["bytes"] % 8:
            raise ImageError("SHARC DLM export tables have invalid sizes")
        raw_strings = _bounded(
            data,
            int(str_section["file_offset"], 16),
            str_section["bytes"],
            "DLM exported string table",
        )
        target_chars = bytes(raw_strings[index + 3] for index in range(0, len(raw_strings), 4))
        if target_chars and target_chars[-1] != 0:
            raise ImageError("SHARC DLM exported string table is not terminated")
        exported_names = [
            item.decode("utf-8", errors="replace")
            for item in target_chars.split(b"\0")
            if item
        ]
        if len(exported_names) != sym_section["bytes"] // 8:
            raise ImageError("SHARC DLM export name and symbol counts differ")
    return {
        "format": "adi-flat-v6-sharc",
        "sha256": hashlib.sha256(data).hexdigest(),
        "file_bytes": len(data),
        "byte_order": "big" if endian == ">" else "little",
        "revision": rev,
        "family": family,
        "build_date": build_date,
        "sections": sections,
        "export_sections": exported_sections,
        "export_section_counts": export_section_counts,
        "exported_names": exported_names,
        "relocation_count": reloc_count,
        "relocation_counts_by_dynamic_type": dict(sorted(relocation_types.items())),
        "relocations": relocations,
    }


def _align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def _parse_ldr(data: bytes, endian: str) -> list[dict[str, Any]]:
    blocks = []
    offset = 0
    while offset + 12 <= len(data):
        tag, count, target = struct.unpack_from(f"{endian}III", data, offset)
        definition = SHARC_LDR_TAGS.get(tag)
        if definition is None:
            raise ImageError(f"unknown loader tag 0x{tag:x} at byte 0x{offset:x}")
        name, bytes_per_word = definition
        payload_bytes = count * bytes_per_word
        if tag == 0 and count != 0:
            raise ImageError("FINAL_INIT block has a nonzero count")
        _bounded(data, offset + 12, payload_bytes, f"loader block {len(blocks)}")
        blocks.append(
            {
                "index": len(blocks),
                "file_offset": _hex(offset),
                "tag": name,
                "count": count,
                "target": _hex(target),
                "payload_bytes": payload_bytes,
            }
        )
        offset = _align_up(offset + 12 + payload_bytes, 12)
        if tag == 0:
            if any(data[offset:]):
                raise ImageError("nonzero bytes follow FINAL_INIT")
            break
    if not blocks or blocks[-1]["tag"] != "FINAL_INIT":
        raise ImageError("loader stream has no FINAL_INIT block")
    return blocks


def inspect_ldr(data: bytes) -> dict[str, Any]:
    errors = []
    for endian, name in ((">", "big"), ("<", "little")):
        try:
            blocks = _parse_ldr(data, endian)
            return {
                "format": "sharc-loader-stream",
                "sha256": hashlib.sha256(data).hexdigest(),
                "file_bytes": len(data),
                "byte_order": name,
                "blocks": blocks,
            }
        except ImageError as error:
            errors.append(str(error))
    raise ImageError("not a standard SHARC loader stream: " + "; ".join(errors))


def inspect(data: bytes, format_hint: str = "auto") -> dict[str, Any]:
    if format_hint == "elf" or (format_hint == "auto" and data.startswith(ELF_MAGIC)):
        return inspect_elf(data)
    if format_hint == "dlm" or (format_hint == "auto" and data.startswith(DLM_MAGIC)):
        return inspect_dlm(data)
    if format_hint in ("ldr", "auto"):
        return inspect_ldr(data)
    raise ImageError(f"unsupported format hint {format_hint}")


def validate_contract(
    result: dict[str, Any],
    *,
    require_symbol: str | None = None,
    require_export_name: str | None = None,
    require_no_undefined: bool = False,
    require_no_relocations: bool = False,
    max_code_bytes: int | None = None,
) -> None:
    if require_symbol:
        if result["format"] != "elf32-sharc":
            raise ImageError("symbol validation currently requires SHARC ELF")
        if require_symbol not in result["global_function_symbols"]:
            raise ImageError(f"required global function {require_symbol!r} is absent")
    if require_export_name:
        if result["format"] != "adi-flat-v6-sharc":
            raise ImageError("export validation currently requires a SHARC DLM")
        if result["export_section_counts"] != {".expstr": 1, ".expsym": 1}:
            raise ImageError("DLM does not contain exactly one export string and symbol table")
        if require_export_name not in result["exported_names"]:
            raise ImageError(f"required DLM export {require_export_name!r} is absent")
    if require_no_undefined:
        if result["format"] != "elf32-sharc":
            raise ImageError("undefined-symbol validation currently requires SHARC ELF")
        if result["undefined_symbols"]:
            raise ImageError("undefined symbols remain: " + ", ".join(result["undefined_symbols"]))
    if require_no_relocations and result.get("relocation_count", 0):
        raise ImageError("relocations remain in the image")
    if max_code_bytes is not None:
        if result["format"] == "elf32-sharc":
            code_bytes = sum(
                section["bytes"]
                for section in result["sections"]
                if section["alloc"] and section["execute"]
            )
        elif result["format"] == "adi-flat-v6-sharc":
            code_bytes = sum(section["bytes"] for section in result["sections"] if section["code"])
        else:
            code_bytes = sum(
                block["payload_bytes"]
                for block in result["blocks"]
                if block["tag"] in ("INIT_L48", "INIT_L16")
            )
        if code_bytes > max_code_bytes:
            raise ImageError(f"code is {code_bytes} bytes, limit is {max_code_bytes}")
        result["validated_code_bytes"] = code_bytes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--format", choices=("auto", "elf", "dlm", "ldr"), default="auto")
    parser.add_argument("--require-symbol")
    parser.add_argument("--require-export-name")
    parser.add_argument("--require-no-undefined", action="store_true")
    parser.add_argument("--require-no-relocations", action="store_true")
    parser.add_argument("--max-code-bytes", type=lambda value: int(value, 0))
    args = parser.parse_args()
    try:
        result = inspect(args.image.read_bytes(), args.format)
        validate_contract(
            result,
            require_symbol=args.require_symbol,
            require_export_name=args.require_export_name,
            require_no_undefined=args.require_no_undefined,
            require_no_relocations=args.require_no_relocations,
            max_code_bytes=args.max_code_bytes,
        )
    except (OSError, ImageError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
