# Bill DSP resource format and host transform

The ordinary DSP-program path is separate from the persistent `FBUT`, `GBUT`,
and `HBUT` firmware-update path. Program resources captured by prior GPL
research begin with ASCII `Bill`. The official Windows `UAD2System.sys`
contains the corresponding parser and host-side payload transform.

The analyzed 64-bit driver has SHA-256
`d09a451cc5843266aa304770c03d936eff967f0e2ffcc9ee556a44c04c58106e`.
No vendor binary or captured program is distributed here.

## Outer header

The parser at image address `0x140002688` constructs a resource object from a
20-byte little-endian header:

| Offset | Size | Current interpretation |
|---:|---:|---|
| `0x00` | 4 | ASCII `Bill` |
| `0x04` | 4 | Resource ID |
| `0x08` | 4 | Packed attributes |
| `0x0c` | 4 | Body size in bytes, excluding the header |
| `0x10` | 4 | Number of trailing dwords replaced before transmission |

The packed attributes contain a 16-bit resource type, an 8-bit payload form,
and an 8-bit DSP-generation field. The official validator at `0x140016468`
enforces these bounds:

- The declared body plus 20 bytes must equal the input size.
- Body size and replacement count must both be nonzero.
- Resource type must be at most four.
- DSP generation must be at most two.
- A nonzero payload form requires a nonzero resource type.
- The replacement region may not exceed `total_dwords - 13`.

These checks are structural. No signature or cryptographic verification call
occurs in this parser.

## Deterministic tail replacement

The transmit transform at `0x1400163b8` copies the resource except for its
last `replacement_dwords * 4` bytes. It discards that input tail and writes a
deterministic sequence in its place:

```text
state = bitwise_not(resource_id) as an unsigned 32-bit integer
repeat replacement_dwords times:
    emit state as one big-endian dword
    state = (state * 0xbc8f) mod 0x7fffffff
```

The first emitted bytes for resource ID `0x020000c2` are `fd ff ff 3d`.
This is obfuscation or deterministic filler, not a cryptographic signature.
The driver does not compare the discarded input tail with the generated
stream.

`tools/inspect_bill_container.py` reproduces the parser bounds and exact tail
transform without hardware access:

```bash
python3 tools/inspect_bill_container.py program.bill
python3 tools/inspect_bill_container.py program.bill \
  --write-transformed /tmp/program.transformed.bill
```

## Transport envelope

`CResourcePool::transmitResource`, beginning at `0x14000cafc`, allocates a
command DMA object large enough for two leading dwords plus the complete
transformed resource. The resource transform writes at command-buffer offset
eight. The command is split into page-sized DMA descriptors and is paired with
a response object. The completion parser recognizes response header
`0x80020044` and status class `0xf0060000`.

This resource path is distinct from `_sendBlock(0x00120000, 0x80040000, ...)`,
which carries the DSP framework or firmware payload and expects response class
`0x8004xxxx`.

## Relocations and executable core

No relocation records are interpreted by the outer `Bill` parser. Bytes before
the replaced tail are copied unchanged. Any segment table, relocation data,
entry point, or executable authentication therefore belongs to the preserved
inner core or to the DSP-side consumer.

Prior captures for related Apollo hardware include resource IDs
`0x020000a5`, `0x020000c2`, `0x020000db`, `0x020000eb`, and `0x0200012b`.
They demonstrate the format but do not establish that those programs or their
memory reservations are compatible with this OCTO revision. Loading them on
the OCTO is not justified until a valid framework response and target-specific
resource inventory are available.

## Proven and unresolved

Confirmed statically:

- Exact 20-byte outer header and field bounds.
- Exact deterministic tail replacement algorithm.
- Absence of host-side cryptographic verification in this parser.
- Resource transport response header and status class.

Still unresolved:

- The preserved inner-core encoding.
- Segment and relocation records inside that core, if any.
- DSP-side validation or authentication.
- OCTO-specific resource IDs, entry points, and memory reservations.
- The command-envelope fields preceding the transformed resource.
