# Framework property map

This note closes a host-versus-DSP ambiguity in the framework startup path.
The `GetProperty` calls used by `CDSPResourceManager::Initialize` are local
PCIe-driver operations. They do not submit a command-ring query and cannot
produce the missing valid DSP response.

## Reproducible source

The analyzed x86-64 macOS binary is available in the public
[stepbrobd/uad2](https://github.com/stepbrobd/uad2) repository at commit
`910a8f413d33bc3489d0da8fc613870153ccb4f2`. The file
`bin/x86_64-darwin/uad2.kext` has SHA-256
`7b664e8ad67b8104d9797defcc0c707fff55ae4981a725559ed9554f2f55cdf6`.
The binary itself is not copied into this repository.

Run the dependency-free verifier against a locally obtained copy:

```bash
python3 tools/inspect_framework_driver.py /path/to/uad2.kext
```

The tool refuses every other hash. It verifies six symbol addresses, 21
instruction sequences, the complete 13-entry `GetProperty` switch table, and
the property-size table.

## Property dispatch

`CPcieDSP::GetProperty` accepts property IDs 0 through 12. Its entire dispatch
is a local switch over cached object state, BAR reads, ring objects, and a
host-side device-family predicate.

| ID | Recovered source | Static size table |
|---:|---|---:|
| 0 | Per-DSP MMIO `+0x0c` | 4 |
| 1 | Cached object field `+0x14` | 0 |
| 2 | Cached object field `+0x0c` | 4 |
| 3 | Per-DSP MMIO `+0x00`, masked with `0x7f` | 4 |
| 4 | 64-bit command-ring object field | 0 |
| 5 | 64-bit response-ring object field | 0 |
| 6 | Eleven-word resource-pool BAR map | 44 |
| 7 | Per-DSP MMIO `+0x1a0` | 0 |
| 8 | Cached DSP index at object `+0x10` | 0 |
| 9 | Constant zero | 4 |
| 10 | Diagnostic register and state dump | 4 |
| 11 | Unsupported switch case | 0 |
| 12 | Host device-family predicate | 0 |

A zero in the separate size table does not mean that the implementation
returns no value. Several internal callers supply explicit sizes, and the
common method requires an output buffer of at least four bytes before entering
the switch.

## Resource-manager initialization

`CDSPResourceManager::Initialize` makes three property calls in order:

1. property 7 into a four-byte manager field;
2. property 6 into a 44-byte local record;
3. property 8 into a four-byte local field.

Property 6 reads these offsets relative to the current DSP window, in this
exact order:

```text
0x010, 0x018, 0x014, 0x01c,
0x184, 0x18c, 0x188, 0x190,
0x198, 0x194, 0x19c
```

The manager rearranges those eleven words into four `CResourcePool` objects.
This exactly explains the register set captured without writes in Experiment
020. The live capture remains the authority for the OCTO values; the static
path explains how the official host code interprets them.

## Consequences

- Properties 6, 7, and 8 are not resident-framework service calls.
- Replaying them through a DSP command ring would invent a protocol that the
  recovered driver does not use.
- The four pool bases, sizes, and scratch reservations are now tied to both a
  live all-eight capture and the exact host consumer.
- The first unresolved response milestone remains a real card-written response
  to a command-ring operation, not a host-assembled property record.

This mapping does not decode allocations inside an opaque `Bill` program. It
therefore narrows the runtime-reservation problem without supplying segment,
relocation, entry-point, stack, or overlay semantics.
