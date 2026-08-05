# Official 168-byte system-information record

The UAD 11.0.1 updater repeatedly requests a 168-byte record before comparing
device firmware versions. This note separates fields whose meaning is tied to
the updater's own report labels from fields that are only known structurally.

The analyzed `UADPerfMon` has SHA-256
`18417006f5dab4e9f149d6f57e618e2c543f3ad1b9966b7be7052222086b9359`.
The analyzed `UAD2DriverClient` has SHA-256
`da5c7925d89a9e43574abcfebc4b2e03dd3b0059bf08d5f423ee51d5df8f89da`.
The analyzed `UAD2System.sys` has SHA-256
`d09a451cc5843266aa304770c03d936eff967f0e2ffcc9ee556a44c04c58106e`.
The exact analyzed `UAD2Pcie.sys` has SHA-256
`d6f980bd3ae94f9206e71302f7ac6f6579e8d2778e92d5e7c34eb5b60d1d7f0a`.
Neither binary is distributed here.

## Request path

`UAD2DriverClient` constructs a 176-byte request for operation `0x6f`, receives
a separate four-byte status, and copies no more than 168 response bytes to the
caller. `UADPerfMon` reaches it through vtable offset `0x68`.

The recovered kernel path closes the call chain:

1. `UAD2System.sys` at `0x14000e520` selects the requested device and invokes
   vtable slot `0x60` with a 168-byte local record.
2. The PCIe interface vtable at `0x1400147f8` identifies
   `UAD2Pcie.sys:0x1400060f0` as the device implementation.
3. That implementation calls the local base assembler at `0x140005c9c`, adds
   cached object fields and BAR reads, then performs a bounded copy.
4. `UAD2System.sys` returns the result in its 176-byte outer envelope.

There is no command-ring submission in either PCIe record-building function.
Operation `0x6f` is therefore a host control operation whose result is assembled
from cached driver state and BAR MMIO. It is not a DSP response and cannot be
used as the missing resident-firmware response oracle.

Status `-0x5c`, decimal `-92`, is now tied to one exact branch. The PCIe
implementation returns it when the 32-bit device object field at offset
`0x0c40` is zero. Constructor and initialization paths set that field, the
device-start path sets it again, and the stop path clears it before disabling
the device. The updater classifies this host lifecycle condition as
still-booting, retries up to five times, and advances its deadline by
approximately 200 ms per retry. That friendly label must not be generalized
into evidence that a SHARC runtime has started.

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

## Exact source map for the labeled fields

The PCIe implementation's stack layout makes the source of every
label-correlated field explicit:

| Record offset | PCIe source |
|---:|---|
| `0x20` | constant `0x0b000003`, the packed UAD 11.0.0.3 driver version |
| `0x24` | BAR `0x2218`, with BAR `0x2234` used by the base assembler as an identity fallback |
| `0x28` | BAR `0x0008` |
| `0x2c` | BAR `0x0004` |
| `0x58..0x67` | four dwords read from BAR `0x20`, `0x24`, `0x28`, and `0x2c` |
| `0x84` | BAR `0x01ac` for the supported device-family mask, otherwise `0xffffffff` |
| `0xa0` | BAR `0x2238` |

This matches the updater's driver, FPGA, framework, bootloader, serial, and
auxiliary-FPGA labels without requiring a live capture. Other known source
ranges are record `0x30..0x57`, copied from a cached 40-byte subrecord, and
record `0x10`, `0x18`, `0x80`, and `0x98`, populated from PCIe device-object
state. Their cross-family meanings are not yet named.

The four version words are formatted as packed version values. The exact
display path varies for some FPGA encodings, including a special branch based
on bits `0x60` of the low byte. The table therefore names each field without
claiming one universal numeric encoding.

The firmware-version comparison uses record offsets `0x24`, `0x28`, `0x2c`,
and `0x84`. The first three now have report-correlated names. Offset `0x84`
comes from BAR `0x1ac` only for selected device families, but is not yet safely
given one global semantic name because the same report supports non-OCTO
devices.

## Partially classified fields

Other accessors and predicates read offsets `0x00`, `0x04`, `0x0c`, `0x5f`,
`0x84`, `0x94`, and `0xa0`. The updater uses them for status, device-family
classification, feature tests, and version comparison. Their exact semantics
can depend on the device type. Assigning one global C structure name to every
offset would therefore overstate the evidence.

The record source is now recovered without a capture. A live official call
would validate values and timing, but would not advance the missing DSP
response, executable format, relocation, or authentication work.

## Reproduction

Run the hash-locked verifier against locally obtained copies:

```bash
python3 tools/inspect_updater_state.py \
  /path/to/UADPerfMon /path/to/UAD2DriverClient

python3 tools/inspect_system_info_path.py \
  /path/to/UAD2System.sys /path/to/UAD2Pcie.sys
```

Both JSON tools refuse binaries with different hashes. The second verifier
checks the complete kernel call path, source registers, lifecycle gate, and
absence of a DSP-ring dependency.
