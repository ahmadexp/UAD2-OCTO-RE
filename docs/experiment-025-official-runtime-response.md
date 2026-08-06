# Experiment 025: official runtime and plug-in responses

Status: executed on 2026-08-05. The signed Windows runtime produced the first
repeatable nonzero OCTO response and two exact `Bill` loader success forms. An
official RealVerb-Pro VST3 instance opened, but the host then disabled it with
error `-38` after an all-zero resource response. A complete running DSP program
is therefore not claimed.

## Purpose

Experiments 023 and 024 proved descriptor consumption but captured no card
response content. This experiment used the official UAD 11.0.1 runtime and an
unmodified plug-in host to answer three narrower questions:

1. Can the OCTO return a valid, nonzero response through the four-page ring?
2. Does a target-compatible form-zero `Bill` resource receive a loader success?
3. Does that success extend to a complete, running audio plug-in?

The Windows 11 reference guest retained Secure Boot, the emulated TPM, the
WHQL-attested UAD driver, VFIO IOMMU containment, and QEMU MMIO tracing from
Experiment 023. The card began after an RTC-backed cold power cycle. No vendor
binary or plug-in was patched, and no time-limited demo was started.

## Authorization-table response

Starting REAPER and activating the UAD host produced four equivalent nonzero
responses. Each response descriptor declared 770 dwords, or 3,080 bytes. The
response layout was:

| Dword | Observed meaning |
|---:|---|
| 0 | Header `0x80030302` |
| 1 | Request token copied from the preceding `0x00110002` command |
| 2 through 769 | 768-entry state table |

The 768 body dwords had one invariant SHA-256 across all four captures and only
four values:

| Value | Entries |
|---:|---:|
| `0x80000000` | 203 |
| `0x81000000` | 2 |
| `0x82000000` | 24 |
| `0x83000000` | 539 |

The remaining 1,016 bytes in each 4 KiB target page were zero. The table's
exact semantic labels remain unknown, so the public analyzer calls it an
authorization-table candidate rather than assigning license meanings to the
four states.

This is a valid card-written response, not merely descriptor consumption. Its
request token changes with the paired command while its body remains stable.

## Live `Bill` resource successes

The plug-in scan and RealVerb-Pro instantiation submitted ordinary resource
envelopes through DSP0. Two response targets later contained the exact
intermediate success form recovered statically from `UAD2System.sys`:

| Resource | Size | Allocation offset | Command word | Response |
|---:|---:|---:|---:|---|
| `0x00000120` | 604 bytes | `0x000e023a` dwords | `0x00010099` | `80070004 00000000 00000120 00010099` |
| `0x000000d0` | 424 bytes | `0x000e02fa` dwords | `0x0001006c` | `80070004 00000000 000000d0 0001006c` |

For both transactions, the response resource ID equals the form-zero `Bill`
header and response word three equals the submitted envelope command word.
The host transmitted each object byte for byte, as predicted by the recovered
payload-form-zero branch. These observations validate the ordinary envelope,
pool offset, resource ID, and intermediate-success parser on this exact OCTO.

They do not decode the opaque inner cores. No clear segment table, relocation,
entry point, or DSP-side authentication decision is exposed by the response.

## RealVerb-Pro outcome

REAPER completed enough of the scan to instantiate the official
`UAD RealVerb-Pro` VST3 user interface. The plug-in reported:

```text
One or more UAD plug-ins have been disabled.
A UAD device is not responding (code -38).
```

The recovered host parser maps an all-zero four-dword resource response to
`-38`, and several posted four-dword targets remained all zero during this
load. RealVerb-Pro displayed `DISABLED`; no audio execution or DSP completion
was observed. The experiment therefore establishes partial target-compatible
resource acceptance, not a complete program load.

This failure is useful localization. The transport, authorization response,
form-zero copy, resource allocation, and at least two DSP-side resource
acceptances all work. The remaining fault lies later in the multi-resource
program transaction or its completion ordering.

## Reproduction tools

The exact-boundary watcher pauses traced QEMU when the official driver advances
the DSP0 response consumer index and saves the ring plus target page:

```bash
python3 lab/windows/capture_response_boundary.py \
  /path/to/qemu-vfio.trace /path/to/output \
  --target-index 2 --monitor-port 4444
```

The response analyzer emits only hashes and derived metadata:

```bash
python3 tools/analyze_uad2_response.py /path/to/response-target.bin
```

It recognizes the 770-dword table, `Bill` intermediate and final forms, and the
all-zero `-38` failure input. Synthetic tests cover each classifier.

The sanitized result is
[`data/experiment-025-official-runtime-response/result.json`](data/experiment-025-official-runtime-response/result.json).
Captured guest pages, traces, installer media, and resource bytes remain out of
the repository.

## Recovery

The guest received an ACPI shutdown. QEMU exited, VFIO released the device, and
the endpoint was left unbound with PCI command word `0x0002`. Memory decoding
was enabled, bus mastering was disabled, and no kernel PCIe or VFIO recovery
error was recorded.

## Conclusion

The “valid response” blocker is closed, and the ordinary `Bill` path is now
validated dynamically through DSP-side intermediate success. The complete
program-load blocker remains open because RealVerb-Pro ended in the official
all-zero-response error path. Arbitrary DSP code, public compute jobs, and
eight-engine program isolation remain gated on decoding and fixing that later
resource completion boundary.
