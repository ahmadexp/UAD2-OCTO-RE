# Experiment 032: complete RealVerb resource pass

Status: executed on 2026-08-06. Linux replayed the complete first 13-resource
RealVerb-Pro pass captured from the unmodified official runtime. Every resource
returned the exact intermediate-success response. A following fixed readback
was consumed but again produced no response.

## Scope and safeguards

This experiment advances only the resource loader. It does not submit the
later captured move, unload, process, or synchronous-control commands. The
wrapper accepts exactly one private Experiment 029 capture directory and
hash-locks all 16 page-bounded command targets. The C probe independently
checks every size, envelope command, allocation offset, `Bill` magic, resource
ID, form, declared body length, and replacement length.

The IOMMU mapping contains 64 ring pages, 16 read-only resource-target pages,
13 response pages, and one six-dword readback response page. Each resource is
submitted sequentially. The probe stops at the first response that is absent
or differs from the exact form:

```text
80070004 00000000 resource_id envelope_command
```

The per-resource deadline is 600 ms. Only after all 13 exact completions does
the probe issue the already bounded four-dword readback at `0xe0000`. Recovery
disables interrupts, resets global DMA, clears all published ring state,
returns DMA control to `0x0001fe00`, and performs a VFIO device reset.

## Exact sequence

| Order | ID | Allocation | Command | Bytes | DMA descriptors | Completion |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | `0x12b` | `0xe0000` | `0x00010073` | 460 | 1 | 1 ms |
| 2 | `0x0eb` | `0xe0040` | `0x000100b5` | 724 | 1 | 1 ms |
| 3 | `0x0c1` | `0xe00ac` | `0x0001009c` | 624 | 1 | 1 ms |
| 4 | `0x0a5` | `0xe010a` | `0x000101e7` | 1,948 | 1 | 2 ms |
| 5 | `0x120` | `0xe023a` | `0x00010099` | 612 | 1 | 1 ms |
| 6 | `0x0bd` | `0xe0296` | `0x00010057` | 348 | 1 | 1 ms |
| 7 | `0x11f` | `0xe02c6` | `0x0001005d` | 372 | 1 | 1 ms |
| 8 | `0x0d0` | `0xe02fa` | `0x0001006c` | 432 | 1 | 1 ms |
| 9 | `0x0f9` | `0xe0338` | `0x0001055e` | 5,496 | 2 | 5 ms |
| 10 | `0x0bf` | `0xe06c2` | `0x00010600` | 6,144 | 2 | 5 ms |
| 11 | `0x11e` | `0xe0ab8` | `0x0001003f` | 252 | 1 | 1 ms |
| 12 | `0x11d` | `0xe0ad8` | `0x00010081` | 516 | 1 | 1 ms |
| 13 | `0x0d1` | `0xe0b24` | `0x000104a8` | 4,768 | 2 | 4 ms |

Every response echoed the expected resource ID and envelope command. This
closes the original RealVerb first-failure localization as a framework-state
problem: the same `0x12b` object that was unanswered in the official failing
run is accepted under the resident framework, and so are all 12 objects after
it.

## Private target hashes

Raw target bytes remain private. The reproducibility boundary consists of
these SHA-256 values in submission order:

```text
0c353512fb27ed961b4e0746de7f1bbc462447f6e2c0263bc6209cda7b7718d0
6d91985233b00edfad9c3e93c17bb75c7e51922d11076fec03d3ab23f3c99e21
87512f74b674462241144deca658f59b165cee7e3d5635b1e0864ffecd284fb1
0c3561eb68e83169d38e58cd70d6bc9464afbd4efc2b3ac34bc339e19cffe79a
e2f918628e4c4882e142113a86f3ee9fcf8034a5ec48c35b663bf0f839f60ed0
bd62be051299bfea36e54119643fea6089423aea6a0b060b8269d4f572c2c78e
203be752a4d3fd8a11739484d3ad0af9ac26359fd1bec5850ef38dd0e1fd45ac
f4c041ab7b0aa19ab9f32235e1b51f4db5b3f7f88fb3f526254b2a906ff96123
07177bd6b15ba0dceafad3ba3d12147f0f87040cdd0b291abb1bca9cfdeb5783
e813ef001a733114e9d775e4b8a5f4f0a36e889088ed6cf9162ee12675ff69de
884f3682122e2dad85895eda6df3545c657c8b0dc2c65d71ddd68a3ae130e7d5
09a0f5d881656044220c0dfe9e8568411ea5e41c192e357aa49872bd37efda75
8e6f5a294bde687a7eeeab2d5a447666d289080d666c63f5e72724531caa6106
ea339b46f1a8d871ac8122d01ce151289a415aa0fd23e6ff0d0840411338667b
17d79a7db9af334375b2f8b68f9640413f08becb04f52f59e4b8f3bf2b702914
91a2ef99d9afd44c3001c68b6a7b396cfb18410ed6e0c13ac2730afe5f623da9
```

## Readback and remaining boundary

After the thirteenth success, command

```text
000c0004 000e0000 00000004 00000000
```

was consumed. Its response descriptor was not consumed, all six canary dwords
remained `0xa5a5a5a5`, and no out-of-prefix DMA write occurred during the
six-second deadline. Complete resource loading is therefore still insufficient
for readback. The next justified target is the official plug-in allocation and
`Process` metadata that precedes readback, not arbitrary address exploration.

A later preflight found an important recovery limitation. After this complete
pass and unanswered readback, a second `0x12b` submission and query 026 were
consumed without responses. Exact official unload commands and the proven
per-DSP reset sequence did not restore the service. Ready bits and transport
recovery therefore do not guarantee application-level runtime recovery. A
fresh official plug-in activation is required before another full pass. See
[`experiment-033-034-resource-lifecycle-and-allocation.md`](experiment-033-034-resource-lifecycle-and-allocation.md).

All seven non-target DSP ring-index sets remained unchanged and all eight DSP
ready bits remained set. The endpoint was left unbound with `enable=0`, PCI
command `0x0002`, and IOMMU group 16.

The public reproducer is split between
[`vfio_realverb_sequence.c`](../tools/vfio_realverb_sequence.c) and
[`uad2-vfio-realverb-sequence.sh`](../tools/uad2-vfio-realverb-sequence.sh).
The sanitized machine-readable result is
[`result.json`](data/experiment-032-complete-realverb-resource-pass/result.json).
