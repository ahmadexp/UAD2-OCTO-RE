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

The payload copy loop uses at most `0x400` dwords per DMA object, so every copy
is bounded to 4 KiB. After queuing the response and command objects, the
ordinary wait mode allows ten attempts with a 600 ms interval. During that
loop the driver also calls `CPcieDSP::GetProperty` with ID 13 and a four-byte
output. The hash-locked public driver dispatches only property IDs 0 through
12, so ID 13 takes the unsupported cleanup branch and emits no DSP command.
It is a host-side wait-loop poll, not a hidden resource-status transaction.

Two response forms can terminate the wait path:

- `0x80070004`, word one zero, and word two equal to the resource ID is
  accepted as an intermediate resource-specific success;
- final completion uses header `0x80020044`, zero in words one and two, and
  status class `0xf0060000` in the upper 16 bits of word three.

The final low status-code mapping is exact:

| Low code | Host result |
|---:|---:|
| `0x0001` | `-91` |
| `0x0002` | `-109` |
| `0x0003` | `-93` |
| `0x0004` | `-97` |
| `0x0005` | `-98` |
| `0x0008` | `-122` |
| `0x0009` | `-123` |

An all-zero four-dword response maps to `-38`; any other malformed response
maps to `-50`. A separate earlier response branch recognizes status class
`0xf0010000` in word one. The public host maps low codes `0x0005` and
`0x000d` to host results `-55` and `-60`; their DSP-side semantic names remain
unknown.

Experiment 025 received the intermediate success form twice from this OCTO:

| Resource | Allocation offset | Command word | Response words |
|---:|---:|---:|---|
| `0x00000120` | `0x000e023a` | `0x00010099` | `80070004 00000000 00000120 00010099` |
| `0x000000d0` | `0x000e02fa` | `0x0001006c` | `80070004 00000000 000000d0 0001006c` |

The resource ID and envelope command word are echoed exactly. Both resources
use payload form zero, so these captures dynamically confirm the byte-for-byte
copy and the loader's intermediate-success parser. The enclosing RealVerb-Pro
load later failed with host result `-38`, so they do not prove complete program
execution.

Experiment 028 added a Linux-controlled success for the exact first RealVerb
object, resource `0x12b` at offset `0xe0000`. Experiment 030 repeated that
exact success independently on all eight DSPs.

Experiment 032 replayed the complete first RealVerb pass under Linux. All 13
resources returned the exact intermediate-success form, including resources
`0xf9`, `0xbf`, and `0xd1`, whose command targets each required two page-bounded
DMA descriptors. The response for every object echoed both its resource ID
and envelope command. Completion times ranged from 1 to 5 ms. This proves the
full captured resource sequence is accepted under the resident framework; it
does not prove allocation finalization, activation, or execution.

The complete pass also revealed that device and transport reset are not an
application-level unload oracle. A later `0x12b` reload and query 026 were
consumed without responses. The exact 13-command pool-zero cleanup list was
then consumed, but a second reload still received no response. The public
driver confirms each cleanup as `0x00030002`, resource ID, zero, and zero, yet
consumption does not prove the DSP-side registry was cleared. A fresh official
activation is now required before replaying allocation metadata.

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

The fixed public driver audit closes a tempting host-side shortcut. Its
`CResource` and `CResourcePool` paths validate outer metadata, allocate a pool
range, copy or replace bytes, and wait for the four-dword completion. They do
not return a decoded body to the host. The related public Linux source likewise
submits captured `Bill` byte arrays as opaque resources. Its human-readable
program labels, module-activation words, and SRAM addresses came from separate
runtime captures, not from an inner-resource decoder. Finally, the operation-13
wait-loop property call is unsupported by the public PCIe property dispatcher
and sends no DSP command. It cannot be repurposed as a decoded-memory readback.

There is a genuine resource-readback builder, but it is inside
`CPluginInstance::Process`, not the property path. It emits `0x000c0004`, a
mapped resource address, a requested dword count, and the original resource
spec, then allocates a response of `requested_dwords + 2`. Experiment 031
queued the fixed four-dword form immediately after exact `0x12b` acceptance.
The command was consumed, but its response descriptor and canary-filled target
were unchanged. The readback therefore requires process or activation context
that isolated resource acceptance does not establish.

Experiment 032 repeated the same readback only after all 13 RealVerb resources
had returned exact successes. Its command was again consumed while its
response descriptor and six canary dwords remained unchanged. Incomplete
resource loading is therefore ruled out as the missing prerequisite.

The same driver recovers one genuine outer relocation layer. Native plug-in
allocation metadata stores 16-byte memory specs at offset `0x188` and 8-byte
readback specs at offset `0x98c`. Command class `0x00150000` splits each memory
spec into an 8-bit resource selector and a 24-bit offset, resolves the selected
mapped resource address, and pairs it with a mapped private-resource
destination. This proves host runtime address patching, but it does not reveal
the authenticated inner body's segments, DSP-side relocations, reservations,
or entry point.

This rules out recovering segments, relocations, or entry points by calling an
unnoticed host parser. The remaining evidence path is the DSP-side consumer:
a lawful post-decode memory trace, a documented debug interface, or a clear
vendor or development artifact for the exact ADSP-21469 framework.

Prior captures for related Apollo hardware include resource IDs
`0x020000a5`, `0x020000c2`, `0x020000db`, `0x020000eb`, and `0x0200012b`.
They demonstrate the format but do not establish that those programs or their
memory reservations are compatible with this OCTO revision. Experiment 025
instead used resources selected and submitted by the unmodified official OCTO
runtime. Replaying the related Apollo captures is still not justified.

The official 11.0.1 installer also contains valid embedded `Bill` resources in
OCTO-capable 64-bit plug-in modules. `tools/scan_bill_resources.py` inventories
their offsets, headers, and hashes without extracting or redistributing vendor
payloads. The inventory confirms DSP-generation-two resources and resource
types 0 through 3, but the inner bodies remain opaque. See
[`official-plugin-resource-inventory.md`](official-plugin-resource-inventory.md).

Across the 87 official instances, the bytes preserved before the declared
trailing region are not one fixed length. Subtracting the 20-byte outer header
leaves opaque prefixes of 32 bytes in 56 instances, 48 bytes in 29 instances,
and 96 bytes in two instances. The declared trailing region is not uniformly
16-byte aligned. Its size modulo 16 is 0 for 36 instances, 4 for 20, 8 for 20,
and 12 for 11.

Four direct SHA-256 layouts were tested for every instance and matched none:

- first 32 prefix bytes equal `SHA256(trailing_region)`;
- last 32 prefix bytes equal `SHA256(trailing_region)`;
- last 32 prefix bytes equal `SHA256(outer_header || trailing_region)`;
- last 32 prefix bytes equal
  `SHA256(prefix_before_last_32 || trailing_region)`.

For the 69 unique resource hashes, every one of the first eight prefix dword
positions also has 69 distinct values. These results reject simple cleartext
digest layouts. They do not distinguish a signature from encrypted metadata,
a keyed authenticator, compressed state, or ordinary high-entropy program
data.

## Corpus-wide inner-core tests

The second analysis pass treats the bytes after the preserved prefix as the
unknown inner core and reports aggregate measurements only. It does not emit
vendor bytes. Across the 69 unique resources:

- inner-core entropy ranges from 6.160432 to 7.997328 bits per byte, with a
  mean of 7.608898;
- maximum-level zlib output ranges from 1.000455 to 1.130952 times the input,
  with a mean of 1.022522, so none of the cores becomes smaller;
- none starts with ELF, gzip, xz, bzip2, ZIP, zlib, LZ4-frame, or Zstandard
  magic, and no standard decompressor candidate succeeds;
- no resource repeats an aligned 16-byte block internally;
- no aligned 16-byte core block is shared by two different unique resources;
- CRC-32 and Adler-32 of either the core or complete body match no dword in
  the corresponding prefix;
- resource ID, attributes, core size, and file size likewise match no prefix
  dword;
- MD5, SHA-1, SHA-224, SHA-256, SHA-384, SHA-512, BLAKE2s, and BLAKE2b of the
  core or `outer_header || core` occur nowhere in any eligible prefix.

The cabinet also provides exactly 39 adjacent generation-1/generation-2
pairs. All 39 pairs use the same prefix size, 21 have equal total size, and
eight retain the same resource ID. Their overlapping cores share only 0 to
1.5152 percent of bytes, with a mean of 0.4227 percent, close to chance for
unrelated byte streams. XOR entropy averages 7.584978 bits per byte.

Together, these results strongly reject an uncompressed clear SHARC image,
ECB-like repeated-block encoding, and several simple embedded checksum or
digest layouts. They are consistent with encryption, high-entropy compression,
or another keyed encoding, but do not identify which. In particular, they do
not reveal a key, authentication rule, relocation table, segment table, or
entry point. Those rules remain on the DSP-side consumer path.

Experiment 029 adds a dynamic constraint. For resource `0x12b`, the 432-byte
body is a 48-byte preserved prefix plus a 384-byte trailing core. One-bit
changes at the first and last prefix bytes and at the first, last, and two
interior core positions were all rejected in 1 ms. The unchanged object was
accepted before and after. Device-side integrity or authentication therefore
covers both regions, even though the algorithm and cleartext remain unknown.

## Proven and unresolved

Confirmed statically and, where noted, dynamically:

- Exact 20-byte outer header and field bounds.
- Exact payload-form branch and deterministic tail replacement algorithm.
- Exact two-dword resource envelope and free-list allocation direction.
- Exact structural checks for the four-dword completion.
- Exact 4 KiB resource-copy limit, bounded wait, intermediate success form,
  final status mapping, and malformed-response errors.
- Absence of host-side cryptographic verification in this parser.
- Resource transport response header and status class.
- Exact official form-zero successes on the physical OCTO, including Linux
  submission of `0x12b` independently to all eight DSPs.
- Dynamic integrity or authentication coverage over both regions of the
  `0x12b` body.

Still unresolved:

- The preserved inner-core encoding.
- Segment and relocation records inside that core, if any.
- Integrity or authentication algorithm and key source.
- Entry points and inner memory reservations for a complete OCTO program.

The loader findings are independently checked by the hash-locked verifier:

```bash
python3 tools/inspect_bill_loader.py /path/to/UAD2System.sys
```
