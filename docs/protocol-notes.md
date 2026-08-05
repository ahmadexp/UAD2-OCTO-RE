# Host protocol notes

This is the smallest model consistent with the public drivers, exact official
driver analysis, and experiments on subsystem `1a00:0005`.

## Data path

```text
host queue object
  -> 16-byte command ring entry
  -> four IOMMU-backed 4 KiB ring pages
  -> FPGA per-DSP DMA engine
  -> resident DSP command processor
  -> response ring DMA reference
  -> host completion object and interrupt
```

Each command and response ring contains 1024 entries of 16 bytes, spread over
four 4 KiB pages. A short command is stored inline in the first dword. Larger
transfers use a four-dword DMA-reference entry.

## Ring registers

For each ring window:

| Offset | Current interpretation |
|---:|---|
| `+0x00..+0x1c` | Four 64-bit ring-page IOVAs |
| `+0x20` | Pending or notify index |
| `+0x24` | Host write index |
| `+0x28` | Hardware read index |

The official initializer reads `+0x28`, bounds it to 1024 entries, synchronizes
both host indexes to it, then publishes all four page addresses. The flush path
copies 16-byte entries, advances the host index modulo 1024, writes `+0x24`,
then writes `+0x20` while host and hardware indexes differ.

## Per-DSP ring banks

```text
DSP 0..3: 0x2000 + dsp_index * 0x80
DSP 4..7: 0x5e00 + dsp_index * 0x80
response ring: command ring + 0x40
```

This produces observed command banks `0x2000`, `0x2080`, `0x2100`, `0x2180`,
`0x6000`, `0x6080`, `0x6100`, and `0x6180`.

## DMA and interrupts

BAR0 `0x2200` is the DMA-control shadow. The cold value is `0x0001fe00`.
Bounded experiments used `0x0001fe01`, `0x00000001`, and `0x00000003` to
pulse reset, retain the global bit, and enable only DSP0. Static analysis shows
that the official shadow begins at `0x00000001`; each DSP adds bit
`1 << (dsp_index + 1)`, reaching `0x000001ff` for eight initialized DSPs.

The official driver assigns five logical interrupt vectors per DSP. On this
eight-DSP capability profile, logical offsets zero through three compress into
four physical bits per DSP and logical offset four is unmapped. All callback
vectors produce mask `0xcccccccc`; adding DSP0 response and command vectors
produces `0xcccccccf`. The low interrupt enable shadow is written at `0x2204`;
selected vectors are armed at `0x2208`.

## Query 026

- Inline command: `0x00260001`
- Expected response header: `0x800c0005`
- Response size: five dwords, header plus four payload words
- Response ring entry: `0x80000005`, zero, IOVA low, IOVA high
- Timeout in the official helper: 2000 ms

After full all-eight startup, the command hardware read index advanced. The
response index stayed at zero and the payload canary was untouched. The same
occurred after the official connect sequence and for query 027. This proves
host command completion, but not DSP-side service dispatch.

## Device startup

The official startup path is now ordered statically: clear interrupt state,
pulse and retain DMA state, acknowledge low interrupts, then initialize every
DSP's command and response rings before adding its DMA bit. Optional Apollo
audio profiles then publish two shared 4 MiB DMA tables and enable shared
interrupt vector 40. The OCTO capability branch skips that extension. See
[`device-startup-sequence.md`](device-startup-sequence.md).

Experiments 009 and 010 exposed only response page zero and enabled DSP0 DMA
before ring publication completed. Experiment 011 confirmed the corrected
four-page ring initialization and official ordering while DMA remained cold,
all pages remained unchanged, and no command was submitted. Experiment 013
then completed the startup across all eight DSPs. Experiments 014 through 016
established command completion but did not receive a runtime response.
