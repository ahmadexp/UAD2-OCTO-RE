# Experiment 023: official Windows reference lifecycle

Status: executed on 2026-08-05 and 2026-08-06. The exact official firmware
update transport completed its entire 625-descriptor payload chain. After an
RTC-backed cold power cycle, the signed Windows driver started the OCTO and
published all 16 rings. No card-written runtime response was captured.

Experiment 025 later captured nonzero runtime responses and two ordinary
resource-loader successes through an official plug-in host. This document
retains Experiment 023's narrower firmware-update result.

## Purpose

Earlier experiments reproduced the statically recovered host protocol but did
not know whether the card was in the same state as the official runtime. This
experiment observes the exact UAD 11.0.1 Windows software below the driver
boundary using QEMU/KVM VFIO passthrough, without patching the guest or vendor
binaries.

The guest used an official Windows 11 Enterprise evaluation image, UEFI Secure
Boot, an emulated TPM, and an isolated IOMMU group containing only the OCTO.
Direct VFIO BAR mapping and direct KVM interrupt injection were disabled so the
selected PCI configuration, MMIO, reset, interrupt, and IOMMU events passed
through QEMU's trace points.

## Signed driver result

Before installation, Windows identified the endpoint as an unconfigured
multimedia controller with Configuration Manager code 28. After installation
and reboot it reported:

| Field | Captured value |
|---|---|
| Device | Universal Audio UAD-2 OCTO |
| PnP status | OK, Configuration Manager code 0 |
| PCIe driver | `12.18.30.228`, dated 2023-11-30 |
| INF | `oem3.inf` |
| Signature | Microsoft Windows Hardware Compatibility Publisher |
| Driver-store score | WHQL |
| Driver-store status | Attested |

`UAD2Pcie`, `UAD2System`, and `UAD2WdmAudio` were all running and reported
status OK. Secure Boot and the emulated TPM were enabled and ready. The three
installer MSIs had valid Authenticode signatures from Universal Audio, Inc.
The exact media hashes are in the sanitized result file.

## Firmware update transaction

The official UAD-2 OCTO firmware dialog submitted the exact hash-identified
`HBUT` artifact. The DSP0 command ring moved from index 4 to 630:

1. Entry 4 was the recovered two-dword extended header descriptor.
2. Entries 5 through 628 were 624 page-sized `0x80000400` descriptors.
3. Entry 629 was a final `0x80000224` descriptor, or 2,192 bytes.

The response ring moved from index 2 to 3 using the expected four-dword
response descriptor. Hardware read index 630 proves consumption of the full
command chain, correcting Experiment 021's earlier stop at its first payload
descriptor. Hardware response read index 3 proves consumption of the posted
response descriptor. It does not prove a reply because the completion page
was still entirely zero at the capture point.

The updater requested a restart. Windows encountered
`SYSTEM_SERVICE_EXCEPTION (0x3B)` during the VFIO teardown and QEMU reported an
unrecoverable VFIO error. Linux recorded a non-fatal root-port ACS violation;
PCIe error recovery succeeded. An RTC-backed power-off then returned the card
to D0, unbound, with PCI command word zero. The updater did not offer the OCTO
firmware update on the next official boot. That is evidence consistent with a
persistent version change, but it is not a decoded flash protocol or a direct
readback of firmware storage.

## Post-update startup and exact response boundary

The signed driver then published four command and four response pages for each
of all eight DSPs and enabled all eight DMA bits. DSP0 submitted two command
pairs, one during initial service startup and one when the UAD Meter was
reconnected. The second pair was:

```text
00100002 01bf5214
00110002 a5521e80
```

It posted response descriptor `80000302 00000000 1782f000 00000001`.
A trace-triggered monitor paused QEMU at the exact transition of response
hardware read index from 1 to 2. The complete 4 KiB response target was zero at
that boundary, 5 ms later, and 100 ms later. A different nonzero page observed
afterward was proven to be Windows allocator reuse and is excluded from the
response evidence.

This establishes an important semantic correction: response hardware read
index advancement means the card consumed an available response descriptor.
It does not, by itself, mean the card wrote response content.

## Reproduction and public evidence boundary

The disposable lab is under [`lab/windows/`](../lab/windows). Analyze a QEMU
trace and a legally captured ring image with:

```bash
python3 tools/analyze_qemu_vfio_trace.py /path/to/qemu-vfio.trace
python3 tools/analyze_uad2_ring_dump.py /path/to/dsp0-command.bin \
  --layout command
```

The public result is
[`data/experiment-023-official-windows-reference/result.json`](data/experiment-023-official-windows-reference/result.json).
Only hashes, identities, protocol words, and derived facts are published.
Firmware, driver, installer, guest-memory, and plug-in bytes remain outside the
repository.

## Conclusion

The official path validates the complete large-block transport, all-eight
startup topology, and two descriptor-consumption boundaries. This experiment
does not decode the HBUT inner payload or establish an executable entry ABI.
The licensed plug-in follow-up and its partial loader success are documented in
[`experiment-025-official-runtime-response.md`](experiment-025-official-runtime-response.md).
