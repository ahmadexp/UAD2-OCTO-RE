# Experiment 026: deadline-safe RealVerb resource sequence

## Historical result and correction

This capture identified resource `0x12b` as the first unanswered object in one
official RealVerb-Pro attempt. Its response target remained all zero at
40.997 ms, 82.261 ms, and 141.023 ms. Every sample is well inside the official
loader's 600 ms wait window, and the official host reported `-38`, the exact
host mapping for an all-zero resource response.

Later Linux experiments accepted `0x12b`, then accepted all 13 resources in the
same order with exact correlated responses. The allocation and Process path was
also completed. Therefore `0x12b` is not an intrinsically failing object. This
experiment localizes the first missing response in that historical host state;
it does not identify a bad resource in RealVerb's container.

The complete first pass contains 13 resources in this order:

```text
12b eb c1 a5 120 bd 11f d0 f9 bf 11e 11d d1
```

The official host repeated the same order once. All 26 paired resource
response targets remained zero through their 100 ms requested sample, with a
maximum actual sampling time of 143.449 ms. At the time, `0x12b` was the first
observable failure boundary. Experiments 032 and 043 supersede any stronger
interpretation.

## First object

| Field | Value |
|---|---:|
| resource ID | `0x0000012b` |
| declared body | 432 bytes |
| complete command target | 460 bytes |
| payload form | 0, byte-for-byte |
| pool allocation | `0x000e0000` dwords |
| envelope command | `0x00010073` |
| command-target SHA-256 | `0c353512fb27ed961b4e0746de7f1bbc462447f6e2c0263bc6209cda7b7718d0` |
| zero response-page SHA-256 | `ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7` |

A hash-locked public counterpart differs only in the high byte of its outer
resource ID and labels `0x12b` as a talkback or monitor resource for related
Apollo x4 hardware. That label is a semantic correlation only. It does not
prove the OCTO role or decode the opaque body.

## Timing method

The collector watches the response-ring descriptor-consumption boundary. At
each new boundary it saves the response ring and target before dumping the
larger command pages, then samples the same response target at requested
delays of zero, 5 ms, and 100 ms. The collector performs no target write.

An earlier exploratory capture included an 850 ms sample and delayed the
target snapshot until after command-page dumping. That run cannot localize the
first failure because 850 ms exceeds the official wait. It is excluded from
this conclusion. The revised collector caps its schedule at 100 ms and its
test suite asserts that every requested sample is below 600 ms.

## Relation to Experiment 025

Experiment 025 observed intermediate success for resources `0x120` and `0xd0`
during a broader official plug-in scan and load context. This experiment
records all-zero responses for those same IDs inside this exact two-pass
RealVerb sequence. These results are not contradictory: resource identity is
only one input to a stateful DSP loader, and Experiment 025 did not prove a
complete RealVerb program.

The historical localization is that this attempt begins with an unanswered
`0x12b` object. Later acceptance proves that the failure was stateful and not a
property of the object itself.

## Publication boundary and recovery

The public result contains resource IDs, lengths, command words, hashes, timing,
and classifications only. Vendor payload bytes, guest memory, and the raw VFIO
trace remain private. The private trace SHA-256 is
`30a634675a64af5cd2a4711d8ee0631822d50fdb81ff8c2d2ecfda5d62d35b75`.

After the reference run and the later read-only probe attempt, QEMU was stopped
and the physical endpoint was returned to an unbound state with `enable=0` and
PCI command `0x0002`.

The sanitized machine-readable record is
[`data/experiment-026-realverb-resource-sequence/result.json`](data/experiment-026-realverb-resource-sequence/result.json).
