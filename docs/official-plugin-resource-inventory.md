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

Resource `0x0000012b`, generation 2, is a 452-byte form-zero object with SHA-256
`ff60952444ae492aa1a8b85dae686ad8445b9971260174202ad1ce13c73ba02d`.
It is byte-identical to the related prior capture, but that prior work labels
it as a talkback or monitor component. It is therefore useful as a transport
fixture after a valid framework exists, not as a harmless general-purpose
heartbeat.

## Reproduce the inventory

After extracting a user-owned copy of the 64-bit plug-in cabinet:

```bash
python3 tools/scan_bill_resources.py --recursive /path/to/extracted-cabinet \
  > /tmp/uad2-bill-inventory.json

python3 tools/analyze_bill_resources.py --recursive /path/to/extracted-cabinet \
  > /tmp/uad2-bill-analysis.json
```

The scanner caps untrusted declared sizes at 1 MiB, rejects malformed objects,
and never extracts payload bytes. The aggregate output used for this note is
preserved in [`bill-structure-11.0.1.json`](bill-structure-11.0.1.json).
