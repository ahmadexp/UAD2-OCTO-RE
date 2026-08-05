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
and one payload word. It remains untested.

Separate block-send callers use command DMA references. Commands observed near
firmware-management paths include `0x000d0000`, `0x00120000`, and `0x000e0000`.
They are outside the current safety boundary and must not be issued merely
because their framing is known.

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

The independently analyzed official Windows implementation of this device
logic has SHA-256
`3e62923ca25fa9c987eddf4ff7d81edfd4245973752bf16ccf61ef18d9c01ed8`.
Its `LoadFirmware` method at image address `0x1400102d0` calls `_sendBlock` at
`0x1400108a8` with command `0x00120000`, response class `0x80040000`, and a
150,000 ms timeout. The descriptor initializer at `0x1400104c0` accepts fewer
than `0x10000` dwords per page reference and sets bit 31. This independently
confirms the extended header plus page-bounded chain used in Experiment 021.

Separate static analysis of UAD 11.0.1 `UADPerfMon` shows that `FBUT`, `GBUT`,
and `HBUT` select the firmware-update interface. The exact OCTO `HBUT` artifact
matches BAR revision `0xa012dc0d` and is treated as potentially persistent. See
[`firmware-container-analysis.md`](firmware-container-analysis.md).
