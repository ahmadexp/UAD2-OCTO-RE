# UAD-2 OCTO reverse engineering

[![CI](https://github.com/ahmadexp/uad2-octo-reverse-engineering/actions/workflows/ci.yml/badge.svg)](https://github.com/ahmadexp/uad2-octo-reverse-engineering/actions/workflows/ci.yml)
[![License: GPL-2.0-only](https://img.shields.io/badge/license-GPL--2.0--only-blue.svg)](LICENSE)

![Universal Audio UAD-2 OCTO PCIe card](docs/images/uad2-octo-rev5.jpg)

An experimental, evidence-driven effort to understand the Universal Audio
UAD-2 PCIe OCTO host protocol and evaluate whether its eight SHARC DSPs can be
used for general-purpose computation.

> [!WARNING]
> This project performs low-level PCIe, MMIO, DMA, interrupt, and reset
> experiments on physical hardware. It is not a production driver. Running an
> experiment on a different board revision or without an isolated IOMMU group
> can crash the host, corrupt memory, or damage the card.

## Current result

The PCIe transport, register layout, bounded VFIO DMA, ring descriptors, DSP0
DMA enable, interrupt masks, and reliable endpoint recovery are established on
one UAD-2 OCTO Rev 5 card. The FPGA dequeues a correctly framed official query,
but the resident DSP firmware has not returned a response under the reproduced
partial initialization sequence.

General-purpose DSP execution is therefore **not yet achieved**. The official
device and per-DSP startup order has now been recovered statically. The next
technical milestone is validating that order in small, DMA-contained stages
before any loader or firmware command is attempted.

| Area | Status | Evidence |
|---|---|---|
| PCI identity and 64 KiB BAR | Confirmed | [`docs/hardware-profile.md`](docs/hardware-profile.md) |
| Eight DSP-ready locations | Confirmed | [`docs/dsp-status-2026-08-04.json`](docs/dsp-status-2026-08-04.json) |
| VFIO Type 1 IOMMU containment | Confirmed | [`docs/vfio-2026-08-04.json`](docs/vfio-2026-08-04.json) |
| Ring descriptors and index registers | Confirmed | [`docs/protocol-notes.md`](docs/protocol-notes.md) |
| DSP0 DMA and endpoint reset | Confirmed | Experiments 002 and 003 |
| Command dequeue | Observed | Experiments 008 through 010 |
| Response delivery | Not yet observed | [`docs/experiment-010-official-query-interrupt-gates.md`](docs/experiment-010-official-query-interrupt-gates.md) |
| Official startup order | Recovered statically | [`docs/device-startup-sequence.md`](docs/device-startup-sequence.md) |
| DSP program loading | Not attempted | [`docs/roadmap.md`](docs/roadmap.md) |
| Generic compute API | Design only | [`docs/architecture.md`](docs/architecture.md) |

## Observed hardware

- PCI address used during research: `0000:03:00.0`
- PCI ID: `1a00:0002`
- Subsystem ID: `1a00:0005`
- Class: `0480`, multimedia controller
- BAR0: 64 KiB
- Link: PCIe Gen1 x1
- FPGA revision: `0xa012dc0d`
- Extended capabilities: `0x00300811`
- Reported DSP count: 8
- IOMMU group during experiments: 16, containing only the card

The board serial is intentionally excluded. The 64 KiB endpoint differs from
older UAD-2 PCIe reports using device ID `0001` and a 16 KiB BAR. Do not run
these tools on an older profile.

## Repository guide

- [`docs/README.md`](docs/README.md): documentation and experiment index
- [`docs/hardware-profile.md`](docs/hardware-profile.md): confirmed hardware profile
- [`docs/protocol-notes.md`](docs/protocol-notes.md): ring, DMA, and interrupt model
- [`docs/experiment-methodology.md`](docs/experiment-methodology.md): safety and evidence rules
- [`docs/official-driver-static-analysis.md`](docs/official-driver-static-analysis.md): hash-locked driver findings
- [`docs/device-startup-sequence.md`](docs/device-startup-sequence.md): official device-start state machine
- [`docs/roadmap.md`](docs/roadmap.md): path toward a general-purpose compute stack
- [`tools/`](tools): passive capture, VFIO probes, and experiment wrappers
- [`kernel/`](kernel): minimal read-only Linux identity probe
- [`tests/`](tests): tests for allowlists and static-analysis tooling

## Safe starting point

Start with PCI configuration and sysfs metadata. This step does not enable,
bind, reset, map, or write to the endpoint:

```bash
python3 tools/capture_baseline.py \
  --host user@uad2-host \
  --bdf 0000:03:00.0
```

The first MMIO identity profile reads only six allowlisted words. Review
[`docs/experiment-methodology.md`](docs/experiment-methodology.md) before using
any remote wrapper. Set the SSH target explicitly for scripts that copy and
compile a probe:

```bash
export UAD2_TARGET=user@uad2-host
```

Do not begin with the query experiment. The query tool exists to preserve the
completed research procedure, and the documented initialization gap must be
closed before another recognized command is justified.

## Experiment history

1. Publish empty DSP0 ring descriptors and restore them.
2. Enable only the DSP0 DMA engine and restore cold reset.
3. Prove VFIO endpoint reset recovers the exact cold state.
4. Capture all 16 command and response ring windows.
5. Isolate ring `+0x20`, later identified as pending or notify index.
6. Isolate ring `+0x24`, later identified as host write index.
7. Activate an empty ring at index zero.
8. Publish one zero entry and observe hardware read-index advancement.
9. Submit two candidate framings of official query 026, with no response.
10. Repeat the corrected query with official DSP0 interrupt gates, with no
    response, then independently verify full recovery.
11. Prepare a no-command probe that publishes four pages for both DSP0 rings in
    the official order while DMA remains disabled.

Every experiment has a Markdown procedure and, where executed, a JSON result
under [`docs/`](docs). Experiment 008's original interpretation was revised:
read-index advancement is consistent with dequeue, but unchanged zero-filled
memory does not directly prove a DMA fetch.

## Reproducing the driver findings

No proprietary driver, firmware, or installer content is stored here. If you
legally possess the analyzed `UAD2Pcie.sys`, verify it locally:

```bash
python3 tools/inspect_official_driver.py /path/to/UAD2Pcie.sys
```

The verifier accepts only SHA-256
`20a11d5b51a4c5093f0c6c3cb80ba132959dcae3362547f4a41ea59f0dd99a6b`
and checks 36 instruction signatures supporting the documented ring, startup,
interrupt, DMA, DSP-ready, and query constants.

## Prior work

- [Open Apollo](https://github.com/rolotrealanis98/open-apollo), a GPL-2.0
  Linux driver and reverse-engineering project for related Apollo endpoints
- [stepbrobd/uad2](https://github.com/stepbrobd/uad2), an experimental Linux
  driver based on analysis of the macOS UAD PCIe driver

Their maps provided hypotheses, but this repository records observations and
restore behavior from the OCTO subsystem `1a00:0005` separately.

## Contributing and legal notes

Read [`CONTRIBUTING.md`](CONTRIBUTING.md) before proposing a register write or
hardware experiment. Report unsafe behavior privately as described in
[`SECURITY.md`](SECURITY.md).

Source code is licensed under GPL-2.0-only. Universal Audio, UAD, UAD-2, and
SHARC are trademarks of their respective owners. This independent research
project is not affiliated with or endorsed by Universal Audio or Analog
Devices.
