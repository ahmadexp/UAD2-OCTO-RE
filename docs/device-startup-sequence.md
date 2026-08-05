# Official device-start sequence

This document reduces the startup path in the exact analyzed Windows driver to
an ordered hardware state machine. The source binary is not distributed here.
Its SHA-256 is
`20a11d5b51a4c5093f0c6c3cb80ba132959dcae3362547f4a41ea59f0dd99a6b`.
All facts in this document are static findings unless an experiment is cited.

The hash-locked verifier checks the supporting instruction bytes:

```bash
python3 tools/inspect_official_driver.py /path/to/UAD2Pcie.sys
```

## Ordered state machine

The resume/start routine at image address `0x140007bc8` performs these stages:

| Stage | Operation | Static source |
|---:|---|---|
| 1 | Verify BAR `0x2218` still matches the stored FPGA revision, then write zero to `0x2220` | `0x140007bc8` |
| 2 | Clear low interrupt state at `0x2204` and `0x2208`, plus `0x2264` and `0x2268` when extended interrupts are present | `0x14000bc64` |
| 3 | Reset or quiesce DMA at `0x2200`, then restore the retained software shadow | `0x14000bea0` |
| 4 | Write `0xffffffff` to low interrupt arm or acknowledgement register `0x2208` | `0x140007bc8` |
| 5 | Start every reported DSP in index order | `0x14000a6c8` |
| 6 | If the optional audio-extension object exists, publish two shared 4 MiB DMA page tables at `0x8000` and `0xa000` | `0x140002984` |
| 7 | If that object exists, enable and arm shared DMA interrupt vector 40 | `0x140002984` |

The interrupt-manager constructor initializes its DMA-control software shadow
to `0x00000001`. On this modern FPGA, stage 3 derives its reset pulse as:

```text
(shadow & 0xfffffe01) | 0x0001fe00 = 0x0001fe01
retained shadow                         = 0x00000001
```

That explains the reset-to-global transition independently reproduced in the
earlier DMA experiments. Each successfully initialized DSP then adds bit
`1 << (dsp_index + 1)` to the shadow. The expected sequence begins at
`0x00000001`, becomes `0x00000003` after DSP 0, and reaches `0x000001ff`
after all eight DSPs.

## Per-DSP initialization

The per-DSP routine follows this order for each core:

1. Select its command-ring bank. DSP 0 through 3 use
   `0x2000 + dsp_index * 0x80`; DSP 4 through 7 use
   `0x5e00 + dsp_index * 0x80`.
2. Build board information from identity and capability reads.
3. When required by board state, poll the DSP boot word at core offset
   `+0x1a4`. The final value must have ready bit 0 set.
4. Initialize the command ring using four 4 KiB pages.
5. Initialize the response ring at command base `+0x40`, also using four
   4 KiB pages.
6. Add the core's DMA-enable bit to BAR `0x2200`.

For either ring, the initializer reads hardware index `+0x28`, replaces an
out-of-range value of 1024 or greater with zero, writes the selected index to
`+0x24` and then `+0x20`, and publishes four 64-bit page IOVAs at
`+0x00..+0x1c`.

## OCTO interrupt mapping

The observed capability word reports eight DSPs. The driver therefore maps
each five-vector logical DSP group into four physical interrupt bits:

```text
logical 5n + 0 -> physical 4n + 0, command ring
logical 5n + 1 -> physical 4n + 1, response ring
logical 5n + 2 -> physical 4n + 2, callback A
logical 5n + 3 -> physical 4n + 3, callback B
logical 5n + 4 -> unmapped
```

Registering the two usable callbacks for every DSP produces software shadow
`0xcccccccc`. Flushing a DSP 0 response and command raises that shadow to
`0xcccccccf`. `tools/decode_startup_profile.py` reproduces the mapping and DMA
shadow sequence from the observed capability words.

## Optional 4 MiB audio DMA setup

The driver allocates this object only when the 32-bit capability word is
negative and capability family bits 25:20 are not six. The observed OCTO word
is `0x00300811`, which is nonnegative, so the object is not allocated and the
resume path skips both 4 MiB tables and vector 40.

When present on another device, each table covers 4 MiB in 4 KiB units, so
each direction has 1024 64-bit entries. Their BAR windows begin at `0x8000`
and `0xa000`. The routine also reads shared-range words at `0x30` and `0x34`,
interacts with status words `0x2244` and `0x2248`, and enables and arms vector
40. Static class behavior and register use identify it as playback/capture
audio transport, not the per-DSP firmware command path.

Experiment 012 independently observed every optional-audio control word as
zero and all 4,096 dwords in `0x8000..0xbfff` as zero. It used no DMA mapping
and no MMIO writes. The shared 4 MiB transport is therefore not an OCTO
startup requirement and has no role in its query 026 path.

## Established versus unresolved

The ordering above is a static reconstruction of the official full-card
startup path. It does not yet prove which stages are necessary for a benign
resident-firmware query on this OCTO board.

The following points remain unresolved:

- The exact semantic name of BAR `0x2220`.
- The state-machine meanings of `0x2244` and `0x2248` on audio-capable boards.
- The exact meaning of the observed nonzero word at `0x30` on this card.
- Which device state selects the optional DSP boot-poll branch.
- Which runtime image installs the query 026 and 027 services.

Experiment 013 reproduced the complete applicable startup with 64 per-DSP ring
pages and all eight DMA bits while leaving the inapplicable audio tables
untouched. Experiments 014 through 016 then proved command consumption after
full startup, including the official connect sequence, but received no runtime
response.
