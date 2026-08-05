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

## Conditional deterministic tail replacement

`CResourcePool::transmitResource` checks both the payload-form byte and the
resource type before calling the transform at `0x1400163b8`. Because the
validator already rejects a nonzero payload form with resource type zero, the
effective rule is:

- payload form zero: copy the complete `Bill` object unchanged;
- payload form nonzero: copy the resource except its last
  `replacement_dwords * 4` bytes, then replace that tail with the sequence
  below.

```text
state = bitwise_not(resource_id) as an unsigned 32-bit integer
repeat replacement_dwords times:
    emit state as one big-endian dword
    state = (state * 0xbc8f) mod 0x7fffffff
```

The first emitted bytes for resource ID `0x020000c2` are `fd ff ff 3d`.
This is obfuscation or deterministic filler, not a cryptographic signature.
The driver does not compare the discarded input tail with the generated
stream. The related captured resources listed below all have attributes
`0x02000000`, meaning DSP generation two, payload form zero, and resource type
zero. The official path copies those objects unchanged and does not apply the
tail transform.

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
transmit-form resource. The exact command buffer is:

| Dword | Meaning |
|---:|---|
| 0 | `0x00010000 | total_dwords` for a low-to-high pool, or `0x00040000 | total_dwords` for a high-to-low pool |
| 1 | Allocated runtime pool offset |
| 2 onward | Complete copy or conditionally transformed `Bill` resource |

`total_dwords` includes both envelope dwords. The command is split into
page-sized DMA descriptors and paired with a response object.

The allocator at `0x14000c098` keeps an ordered free-list whose entries contain
an offset and size in dwords. One pool takes space from the low end of a free
range; the other takes space from its high end. Allocation creates a 32-byte
record containing the resource pointer, assigned offset, size, and reference
count. Reusing the same resource increments that reference count. The pool's
exact OCTO registers and reservations were recovered in Experiment 020. Pool 0
starts at `0x4000`, has `0xe0000` dwords available, and allocates low to high.
Pools 1 through 3 allocate high to low after their aligned scratch
reservations. See
[`experiment-020-resource-pools.md`](experiment-020-resource-pools.md) for the
complete all-eight-DSP map. A first pool-0 allocation therefore receives
offset `0x4000` when the runtime pool is otherwise empty.

The completion parser recognizes a four-dword response with header
`0x80020044`, zero in dwords one and two, and status class `0xf0060000` in the
upper 16 bits of dword three. The low status code is mapped to a host error.
No such completion has yet been received from the OCTO.

The offline tool can construct the exact envelope when a known allocation
offset and pool direction are supplied:

```bash
python3 tools/inspect_bill_container.py program.bill \
  --write-command /tmp/program.command \
  --allocation-offset 0x1234 \
  --pool-direction low-to-high
```

This resource path is distinct from `_sendBlock(0x00120000, 0x80040000, ...)`,
which carries the DSP framework or firmware payload and expects response class
`0x8004xxxx`.

## Relocations and executable core

No relocation records are interpreted by the outer `Bill` parser. Form-zero
objects are copied in full; for other forms, bytes before the replaced tail are
copied unchanged. Any segment table, relocation data, entry point, or
executable authentication therefore belongs to the opaque body or to the
DSP-side consumer.

Prior captures for related Apollo hardware include resource IDs
`0x020000a5`, `0x020000c2`, `0x020000db`, `0x020000eb`, and `0x0200012b`.
They demonstrate the format but do not establish that those programs or their
memory reservations are compatible with this OCTO revision. Loading them on
the OCTO is not justified until a valid framework response and target-specific
resource inventory are available.

The official 11.0.1 installer also contains valid embedded `Bill` resources in
OCTO-capable 64-bit plug-in modules. `tools/scan_bill_resources.py` inventories
their offsets, headers, and hashes without extracting or redistributing vendor
payloads. The inventory confirms DSP-generation-two resources and resource
types 0 through 3, but the inner bodies remain opaque. See
[`official-plugin-resource-inventory.md`](official-plugin-resource-inventory.md).

## Proven and unresolved

Confirmed statically:

- Exact 20-byte outer header and field bounds.
- Exact payload-form branch and deterministic tail replacement algorithm.
- Exact two-dword resource envelope and free-list allocation direction.
- Exact structural checks for the four-dword completion.
- Absence of host-side cryptographic verification in this parser.
- Resource transport response header and status class.

Still unresolved:

- The preserved inner-core encoding.
- Segment and relocation records inside that core, if any.
- DSP-side validation or authentication.
- OCTO-specific resource IDs, entry points, and memory reservations.
