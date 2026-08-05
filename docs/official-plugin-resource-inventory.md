# Official plug-in resource inventory

## Scope

The separate 64-bit plug-in cabinet in the official UAD 11.0.1 installer was
scanned for structurally valid embedded `Bill` objects. No vendor resource is
stored in this repository. `tools/scan_bill_resources.py` reports file hashes,
resource offsets, headers, sizes, and resource hashes only.

Six plug-in modules contained valid resources:

| Plug-in module | Module SHA-256 | Resources |
|---|---|---:|
| UAD Millennia NSEQ-2 | `25cc5df9fb4282078b1af3ab4021363409c5ebfce8c621b2bcd9f53763bbed5b` | 2 |
| UAD Oxford Limiter | `fb0237a240e6cdd16818a31dbce09c7b74f9aa32a8de8e6ff2c54edb78ec3053` | 2 |
| UAD SSL E Channel Strip Legacy | `c85cef255fe518d6edc9aa46d246edd82626bd4713cd49226d265c73b9c38e81` | 29 |
| UAD API Vision Channel Strip Legacy | `70d5796b8bf88ecb4cf0a360a211bbfcab4412839b6355ca22e04f605719c36b` | 38 |
| UAD ENGL Savage 120 | `6560447d19f6ef23d8349cc00528e56f38440b63de2e84e6fcf96ba88db450a8` | 2 |
| UAD Softube Vintage Amp Room | `4d1d1a0f7c0f2f1d77e976ca7fa20c9f245347b3ecaf898276a8163c444ba676` | 14 |

The combined inventory contains 87 resource instances and 69 unique resource
hashes. Eighteen repeated instances have byte-identical hashes for the same
generation, type, and ID. This shows that resources are deterministic and
reused across plug-in modules, rather than re-encrypted uniquely for every DLL.

| Field | Distribution |
|---|---|
| DSP generation | 39 generation 1, 39 generation 2, 9 generation 0 |
| Resource type | 78 type 0, 3 type 1, 5 type 2, 1 type 3 |
| Payload form | All 87 are form 0 |

Because every observed official resource uses payload form 0, the host sends
all of these objects byte for byte. The recovered deterministic tail-
replacement branch is valid driver behavior, but it is not exercised by this
inventory.

The bytes after each 20-byte outer header remain opaque. The prefix before the
declared trailing-dword region is not one fixed size:

| Opaque prefix after outer header | Instances |
|---:|---:|
| 32 bytes | 56 |
| 48 bytes | 29 |
| 96 bytes | 2 |

The trailing region is not uniformly 16-byte aligned: its byte length modulo
16 is 0 for 36 instances, 4 for 20, 8 for 20, and 12 for 11. Across the 69
unique resources, every one of the first eight prefix dword positions has 69
distinct values.

Four direct SHA-256 candidates were tested across all 87 instances: a digest
of the trailing region in the first or last 32 prefix bytes, a digest of outer
header plus trailing region, and a digest of the earlier prefix plus trailing
region. Every candidate produced zero matches. No visible segment address,
relocation table, entry point, or recognizable public-key signature structure
is exposed at this layer. The negative digest result does not prove encryption
or the absence of DSP-side authentication.

The expanded metadata-only analysis also finds no standard digest of the core
or `outer_header || core` anywhere in the eligible prefixes, no CRC-32 or
Adler-32 match in any prefix dword, no common compression magic, no aligned
16-byte block repetition, and no aligned block shared by two unique cores.
The 69 unique cores have mean entropy 7.608898 bits per byte and do not become
smaller under maximum-level zlib compression. All 39 adjacent generation
pairs have the same prefix size, but their core byte equality averages only
0.4227 percent. These are strong negative format tests, not proof of a
particular cipher or authentication design. Full measurements are recorded in
[`bill-structure-11.0.1.json`](bill-structure-11.0.1.json).

## Exact public-corpus counterparts

The public [Open Apollo header](https://github.com/rolotrealanis98/open-apollo/blob/29a22b1254393488c5a5f55eb11db77c4d0251f9/driver/ua_dsp_programs.h)
at commit `29a22b1254393488c5a5f55eb11db77c4d0251f9` contains five related captures.
All five have exact UAD 11.0.1 cabinet counterparts when matched by the low 24
bits of resource ID, DSP generation, and size:

| Official ID | Public ID | Size | Public label |
|---:|---:|---:|---|
| `0x000000a5` | `0x020000a5` | 1940 | mixer core |
| `0x000000c2` | `0x020000c2` | 184 | output routing |
| `0x000000db` | `0x020000db` | 728 | capture routing |
| `0x000000eb` | `0x020000eb` | 716 | input routing |
| `0x0000012b` | `0x0200012b` | 452 | talkback or monitor |

In each pair, byte offset seven is the only difference: the public resource ID
has high byte `0x02`, while the cabinet ID has high byte zero. Every byte from
offset eight through end of file is identical, including attributes, prefix,
and complete inner core. This corrects the earlier overbroad statement that
the complete `0x12b` objects were byte-identical.

The public labels and related Apollo x4 runtime addresses are useful semantic
correlations, not a decode of the inner core and not OCTO memory-map proof. The
source of the resource-ID high-byte normalization is also not established for
the Windows OCTO path. The hash-locked, metadata-only audit is recorded in
[`public-bill-audit-2026-08-05.json`](public-bill-audit-2026-08-05.json).

## Reproduce the inventory

After extracting a user-owned copy of the 64-bit plug-in cabinet:

```bash
python3 tools/scan_bill_resources.py --recursive /path/to/extracted-cabinet \
  > /tmp/uad2-bill-inventory.json

python3 tools/analyze_bill_resources.py --recursive /path/to/extracted-cabinet \
  > /tmp/uad2-bill-analysis.json

python3 tools/audit_public_bill_corpus.py \
  /path/to/open-apollo/driver/ua_dsp_programs.h \
  /path/to/extracted-cabinet
```

The scanner caps untrusted declared sizes at 1 MiB, rejects malformed objects,
and never extracts payload bytes. The aggregate output used for this note is
preserved in [`bill-structure-11.0.1.json`](bill-structure-11.0.1.json).
