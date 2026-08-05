# DSP boot and reset control

## Ready polling

The symbolized macOS driver names its boot wait routine
`CPcieDSP::_waitFor469ToStart`, which supports the ADSP-21469-family
identification. It reads the per-DSP MMIO bank at `+0x1a4`, treats bit zero as
ready, and contains special handling for value `0xa8caed0f`. DSP0 receives up
to 100 polls and the remaining DSPs receive up to 10 polls, with 300 ms waits
in the observed implementation.

The exact OCTO offsets and cold values are:

| DSP | Offset | Cold value |
|---:|---:|---:|
| 0 | `0x01a4` | `0x00000a03` |
| 1 | `0x09a4` | `0x00000003` |
| 2 | `0x11a4` | `0x00000003` |
| 3 | `0x19a4` | `0x00000003` |
| 4 | `0x41a4` | `0x00000003` |
| 5 | `0x49a4` | `0x00000003` |
| 6 | `0x51a4` | `0x00000003` |
| 7 | `0x59a4` | `0x00000003` |

The two values are stable across repeated reads and every value has ready bit
zero set.

## DMA engine and per-DSP reset bits

BAR `0x2200` combines global, enable, and reset controls:

```text
bit 0       global DMA enable
bits 1..8   DSP0..DSP7 engine enables
bits 9..16  DSP0..DSP7 reset pulses
```

The official `CPcieDSP::ResetDSP` calls
`CPcieIntrManager::ResetDMAEngine(dsp)`. That routine clears enable bit
`dsp + 1`, sets reset bit `dsp + 9` for one write, then writes the same shadow
with the reset bit clear. It leaves that engine disabled. The normal start path
later adds its enable bit after both rings are initialized.

The global startup reset sequence uses `0x0001fe01` then `0x00000001`. The
fully enabled OCTO shadow is `0x000001ff`. The cold post-reset observation is
`0x0001fe00`.

## Firmware-aware hard reset

The exact UAD 11.0.1 Windows PCIe driver and the hash-identified symbolized
macOS driver independently expose a second reset decision above the per-engine
DMA controls.

`LoadFirmware` sets host object field `+0x0c38` before it sends command
`0x00120000`. The later hard-reset routine tests that field:

- when clear, the Windows implementation samples each reported DSP at per-DSP
  `+0x1a4`, writes one to BAR `+0x221c`, holds it for `0x2710` timer ticks,
  then writes zero. The macOS implementation independently confirms the one,
  then zero pulse at `+0x221c`;
- when set, both implementations skip the ordinary pulse and write
  `0x0be0deaf` to DSP0 MMIO `+0x1a8`.

The repeated cross-platform constant and branch establish the host behavior.
They do not establish the device-side meaning of `0x0be0deaf`. It may select a
firmware activation, commit, reboot, or recovery path. The updater's restart
requirement makes a persistent interpretation plausible, so this write is
outside the current experiment boundary.

The flag is set before the block helper returns. A failed or timed-out load is
therefore not evidence that the magic write is safe or useful. The next lawful
trace must record whether a successful official load is followed by this hard
reset during driver stop or system restart, and what the device reports after
the subsequent cold initialization.

## Hardware validation

Experiment 017 started all 16 empty rings, then applied the official per-DSP
sequence to one engine at a time. After each pulse it verified the disabled
shadow and all eight ready bits, then restored only the tested enable bit. All
eight passed, producing enable-bit pass mask `0x000001fe`. No ring page changed,
the final live shadow was `0x000001ff`, explicit cleanup succeeded, and a
separate read-only probe confirmed the cold state.

This validates FPGA DMA-engine isolation and recovery. It does not yet prove
isolation of arbitrary SHARC programs or a core-local recovery from a hung
program. Those require a known-safe runtime image and heartbeat first.
