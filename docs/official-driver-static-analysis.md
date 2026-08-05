# Official UAD2 PCIe driver static analysis

This note records only the protocol facts needed to reproduce bounded local
experiments. The analyzed `UAD2Pcie.sys` has SHA-256
`20a11d5b51a4c5093f0c6c3cb80ba132959dcae3362547f4a41ea59f0dd99a6b`.
No vendor binary is included in this repository.

Run the hash-locked signature verifier against a locally extracted copy:

```bash
python3 tools/inspect_official_driver.py /path/to/UAD2Pcie.sys
```

It refuses any other hash and verifies the PE instruction bytes supporting the
offset, ring-size, startup, interrupt, and query constants below. The full
ordered startup reconstruction is in
[`device-startup-sequence.md`](device-startup-sequence.md).

## Ring class

The ring initializer at image address `0x14000c47c` reads BAR ring offset
`+0x28`, rejects values at or above 1024 by replacing them with zero, copies
the value into its hardware-read and host-write fields, then writes it to
`+0x24` and `+0x20`. It programs four 4 KiB page addresses into offsets
`+0x00` through `+0x1c`.

The flush routine at `0x14000c764` rereads `+0x28`, copies queued 16-byte
entries into the four DMA pages, advances the host write index modulo 1024,
writes the new index to `+0x24`, and, while hardware read differs from host
write, also writes it to `+0x20` and enables the ring's interrupt vector.

## DSP 0 ring and interrupt assignment

The DSP object constructor at `0x14000a4b4` assigns command vector
`dsp_index * 5`, response vector `dsp_index * 5 + 1`, and three callback
vectors at offsets 2, 3, and 4. Thus DSP 0 uses vectors 0 through 4.

The interrupt-enable routine at `0x14000b810` accumulates the mapped bit in a
shadow mask and writes its lower word to BAR0 `0x2204`. When its arm argument
is true, it also writes the bit to `0x2208`. Ring flush calls this routine with
arm false. Extended interrupt bits, when present, use `0x2264` and `0x2268`.

The per-DSP start routine at `0x14000a6c8` initializes both command and
response through the generic ring initializer, so each ring receives all four
page descriptors. Only after both ring initializations succeed does it call
`0x14000b6a8`, which adds the DSP engine bit to the DMA-control shadow and
writes BAR0 `0x2200`. This differs from Experiments 009 and 010 in two ways:
their response ring exposed only its first page, and their DSP 0 DMA bit was
enabled before ring publication completed. Entry zero still resides in the
published response page, but exact reproduction requires all four pages and
the official ordering.

The resume/start path at `0x140007bc8` clears interrupt state, pulses DMA reset,
acknowledges low interrupt bits, and starts every reported DSP. It publishes
two shared 4 MiB DMA tables and enables vector 40 only when an optional audio
extension was created during device mapping. The observed OCTO capability word
`0x00300811` makes that predicate false. The interrupt manager's DMA shadow
begins at one. Each per-DSP start adds bit
`1 << (dsp_index + 1)`, producing `0x00000003` after DSP 0 and
`0x000001ff` after all eight cores.

The complete order is statically bounded, but not every stage is safe to
execute as a single experiment yet. Experiment 011 therefore reproduces only
the official command and response ring initializer with DMA disabled and no
command submission.

The macOS `CPcieDSP::ResetDSP` path calls
`CPcieIntrManager::ResetDMAEngine(dsp)`. It clears enable bit `dsp + 1`, pulses
reset bit `dsp + 9`, then clears the reset bit while leaving that DSP engine
disabled. Experiment 017 executed that sequence independently for all eight
engines, re-enabled only the tested engine, and recovered every case. See
[`dsp-boot-and-reset-control.md`](dsp-boot-and-reset-control.md).

For eight-DSP devices the interrupt manager compresses five logical vectors
per DSP into four physical bits. Logical offsets zero through three map to the
corresponding four-bit group and logical offset four is unmapped. The callback
shadow for all eight DSPs is `0xcccccccc`; a queued DSP0 response and command
produce `0xcccccccf`.

## Query 026 ABI

The response-backed send helper is at `0x14001047c`. The query wrapper supplies
command base `0x00260000`; the helper ORs in one, producing inline ring command
`0x00260001`. It expects response header `0x800c0005` followed by four payload
dwords. The response ring entry is a DMA reference with words
`0x80000005`, `0`, address low, and address high. The response object is queued
and flushed before the command object.

A neighboring query uses command `0x00270001`, expected header `0x800d0002`,
and one payload word. Experiment 016 executed it after the recovered connect
sequence; the command dequeued without a response.

Separate block-send callers use command DMA references. Commands observed near
firmware-management paths include `0x000d0000`, `0x00120000`, and `0x000e0000`.
They are outside the current safety boundary and must not be issued merely
because their framing is known.

The exact PCIe driver contains three adjacent wrappers, but adjacency is not a
call sequence:

| Wrapper address | Command | Expected response | Timeout |
|---:|---:|---:|---:|
| `0x14000e340` | `0x000d0000` | `0x80050000` | 5,000 ms |
| `0x14000e420` | `0x00120000` | `0x80040000` | 150,000 ms |
| `0x14000e460` | `0x000e0000` | `0x80060000` | 5,000 ms |

The updater's firmware operation reaches the middle wrapper. No recovered
caller proves that the first and third wrappers surround an initial cold-boot
HBUT update. Public code that describes these as an unconditional three-phase
sequence therefore exceeds the available evidence.

The exact UAD 11.0.1 `UAD2System.sys` dispatch layer sharpens this result. Four
public wrappers select operations `0x67`, `0x68`, `0x69`, and `0x6a` before
entering one common dispatcher. At the target object, operations `0x67`,
`0x68`, and `0x69` select distinct vtable offsets `0x30`, `0x38`, and `0x40`.
The common path invokes only the selected method. Operation `0x69` therefore
does not automatically call the `0x67` and `0x68` methods at this dispatch
layer. Operation `0x6a`, already identified as `LoadFPGAImage`, remains a
separate operation. This does not prove that another higher-level caller never
sequences the operations, but it rejects automatic bracketing inside
`_loadBlock`.

The symbolized macOS implementation labels its runtime wrapper `LoadFirmware`.
It calls the block helper with command base `0x00120000`, expected response
class `0x80040000`, and timeout `0x249f0` (150,000 ms). For a short payload the
helper emits one DMA descriptor for a header dword containing
`0x00120000 | (payload_dwords + 1)`, followed by DMA descriptors for the
payload pages. It queues a four-dword response descriptor first and accepts a
reply whose first dword has class `0x8004xxxx`. This proves that a command-ring
block loader exists, but does not establish whether its caller ultimately
changes persistent state or identify its accepted inner image format.

For payloads of at least `0x3fffc` bytes, `_sendBlock` uses its recovered
extended-length form instead: the two header dwords are `command_base |
0x40000000` and `payload_dwords + 2`. The 2,558,096-byte OCTO HBUT therefore
uses `0x40120000, 0x0009c226`, followed by 625 page-bounded DMA descriptors.

The independently analyzed related official Windows implementation of this
device logic has SHA-256
`3e62923cb084ffb7fd4f7e5c06c8b65ff521b21e84c6d5f66ea9b64fc6f01ed8`.
Its `LoadFirmware` method at image address `0x1400102d0` calls `_sendBlock` at
`0x1400108a8` with command `0x00120000`, response class `0x80040000`, and a
150,000 ms timeout. The descriptor initializer at `0x1400104c0` accepts fewer
than `0x10000` dwords per page reference and sets bit 31. This independently
confirms the extended header plus page-bounded chain used in Experiment 021.

More importantly, the exact UAD 11.0.1 PCIe installer driver has SHA-256
`d6f980bd3ae94f9206e71302f7ac6f6579e8d2778e92d5e7c34eb5b60d1d7f0a`.
Its embedded PDB name identifies `UAD2Pcie`, and the accompanying INF binds the
tested OCTO identity. `PcieDevice::sendBlock` is at `0x14000e978` and its
`LoadFirmware` wrapper is at `0x14000e420`. It independently establishes:

- extended framing above `0xfffe` payload dwords;
- `command | 0x40000000` plus `payload_dwords + 2`;
- one payload descriptor per physical 4 KiB boundary;
- descriptor length below `0x10000` dwords, valid bit 31, zero word one, and
  low/high DMA address words;
- a four-dword response descriptor;
- command `0x00120000`, response class `0x80040000`, and 150-second timeout.

Run the separate exact-driver verifier with:

```bash
python3 tools/inspect_pcie_loader.py /path/to/UAD2Pcie.sys
```

All 19 signatures are hash-locked. This removes large-chain framing as the
likely explanation for Experiment 021 stopping at its first payload
descriptor. It does not explain the missing state transition or make another
HBUT submission safe.

Separate static analysis of UAD 11.0.1 `UADPerfMon` shows that `FBUT`, `GBUT`,
and `HBUT` select the firmware-update interface. The exact OCTO `HBUT` artifact
matches BAR revision `0xa012dc0d` and is treated as potentially persistent. See
[`firmware-container-analysis.md`](firmware-container-analysis.md).

## Updater system-state query

The UAD 11.0.1 updater's version-comparison path requests a 168-byte system
record through vtable slot `0x68`. Driver status `-0x5c` is classified
separately and converted to a retry result. The caller permits five retries,
with a deadline advanced by approximately 200 ms between reads, before it
compares several cached firmware and framework version fields.

The matching `UAD2DriverClient` implementation issues operation `0x6f`, uses a
176-byte request record, receives a four-byte status, and copies at most 168
bytes to the caller. The matching `UAD2System.sys` wrapper selects the device
and calls its vtable slot `0x60`. The exact `UAD2Pcie.sys` implementation then
assembles the record from cached object fields and BAR MMIO. No DSP command
ring is involved.

The not-ready result is an exact host lifecycle gate: PCIe object field
`+0x0c40` clear returns `-0x5c`; constructor, initialization, and start paths
set it, while stop clears it. This operation therefore does not reveal the
device-side transition that makes runtime queries responsive, and is not a
candidate for the missing valid DSP response.

These two binaries can be checked without redistributing them:

```bash
python3 tools/inspect_updater_state.py \
  /path/to/UADPerfMon /path/to/UAD2DriverClient

python3 tools/inspect_system_info_path.py \
  /path/to/UAD2System.sys /path/to/UAD2Pcie.sys
```

Six report-correlated fields are now assigned: driver version at `+0x20`, FPGA
version at `+0x24`, DSP framework version at `+0x28`, DSP bootloader version at
`+0x2c`, serial-number storage or reference at `+0x58`, and auxiliary FPGA
version at `+0xa0`. Their exact sources are now BAR `0x2218`, `0x8`, `0x4`,
`0x20..0x2c`, and `0x2238`, plus the packed driver-version constant. Several
family-dependent bytes remain unassigned. See
[`system-information-record.md`](system-information-record.md).

## Ordinary resource completion path

The exact `UAD2System.sys` resource loader is verified separately:

```bash
python3 tools/inspect_bill_loader.py /path/to/UAD2System.sys
```

It confirms the two-dword pool envelope, 4 KiB copy chunks, ten ordinary
600 ms completion waits, the resource-ID-specific `0x80070004` success form,
and the final `0x80020044` plus `0xf0060000` status form. It also records the
exact low-code to host-error mapping. These findings define completion
handling for a future Linux program API, but cannot be exercised until the DSP
framework responds. See [`bill-resource-analysis.md`](bill-resource-analysis.md).
