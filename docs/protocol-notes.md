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
pulse reset, retain the global bit, and enable only DSP0.

The official driver assigns five interrupt vectors per DSP. DSP0 uses command
vector 0, response vector 1, and callback vectors 2 through 4. The low interrupt
enable shadow is written at `0x2204`; selected vectors are armed at `0x2208`.
Experiment 010 reproduced mask `0x1f` but did not receive a response.

## Query 026

- Inline command: `0x00260001`
- Expected response header: `0x800c0005`
- Response size: five dwords, header plus four payload words
- Response ring entry: `0x80000005`, zero, IOVA low, IOVA high
- Timeout in the official helper: 2000 ms

The command hardware read index advanced in the executed experiment. The
response index stayed at zero and the payload canary was untouched. This is a
transport observation, not evidence that the command was successfully decoded.

## Known initialization gap

The official per-DSP start path publishes four pages for both rings before it
enables the DSP DMA bit. It is reached after additional device-level setup.
Experiments 009 and 010 exposed only response page zero and used a different
ordering. Since the complete earlier setup is not bounded yet, a simple retry
with more pages is intentionally deferred.
