# Official 168-byte system-information record

The UAD 11.0.1 updater repeatedly requests a 168-byte record before comparing
device firmware versions. This note separates fields whose meaning is tied to
the updater's own report labels from fields that are only known structurally.

The analyzed `UADPerfMon` has SHA-256
`18417006f5dab4e9f149d6f57e618e2c543f3ad1b9966b7be7052222086b9359`.
The analyzed `UAD2DriverClient` has SHA-256
`da5c7925d89a9e43574abcfebc4b2e03dd3b0059bf08d5f423ee51d5df8f89da`.
Neither binary is distributed here.

## Request path

`UAD2DriverClient` constructs a 176-byte request for operation `0x6f`, receives
a separate four-byte status, and copies no more than 168 response bytes to the
caller. `UADPerfMon` reaches it through vtable offset `0x68`.

Status `-0x5c` is not treated as a generic failure. The version comparator
classifies it as a still-booting result, retries up to five times, and advances
its deadline by approximately 200 ms per retry. This is evidence for an
explicit boot-to-ready state in the official software stack. It is not proof
that the Linux ring experiments currently receive `-0x5c`, because they have
not reproduced operation `0x6f` at this interface layer.

## Label-correlated fields

The updater builds a human-readable device report. Each accessor below first
fetches the same 168-byte record, formats one field, and is immediately paired
with the listed report label in the report builder.

| Record offset | Size | Report meaning | Evidence status |
|---:|---:|---|---|
| `0x20` | 4 | UAD-2 driver version | Static, label-correlated |
| `0x24` | 4 | FPGA version | Static, label-correlated |
| `0x28` | 4 | DSP framework version | Static, label-correlated |
| `0x2c` | 4 | DSP bootloader version | Static, label-correlated |
| `0x58` | unknown | serial-number storage or reference | Static, passed to the serial formatter |
| `0xa0` | 4 | auxiliary FPGA version | Static, label-correlated and device-conditional |

The four version words are formatted as packed version values. The exact
display path varies for some FPGA encodings, including a special branch based
on bits `0x60` of the low byte. The table therefore names each field without
claiming one universal numeric encoding.

The firmware-version comparison uses record offsets `0x24`, `0x28`, `0x2c`,
and `0x84`. The first three now have report-correlated names. Offset `0x84`
participates in comparison but is not yet safely named because the same report
supports device families other than PCIe OCTO.

## Partially classified fields

Other accessors and predicates read offsets `0x00`, `0x04`, `0x0c`, `0x5f`,
`0x84`, `0x94`, and `0xa0`. The updater uses them for status, device-family
classification, feature tests, and version comparison. Their exact semantics
can depend on the device type. Assigning one global C structure name to every
offset would therefore overstate the evidence.

No captured OCTO operation-`0x6f` response exists yet. A lawful capture from
the official stack would allow these static offsets to be correlated with the
known OCTO identity and BAR revision without guessing.

## Reproduction

Run the hash-locked verifier against locally obtained copies:

```bash
python3 tools/inspect_updater_state.py \
  /path/to/UADPerfMon /path/to/UAD2DriverClient
```

The JSON output includes the recovered field table and refuses binaries with
different hashes.
