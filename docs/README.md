# Documentation index

This directory separates confirmed hardware observations, static-analysis
findings, experimental procedures, and future hypotheses. A successful result
means the stated bounded observation matched its acceptance criteria and the
card recovered. It does not imply that later or broader operations are safe.

## Project overview

- [`hardware-profile.md`](hardware-profile.md): exact tested endpoint and board
- [`architecture.md`](architecture.md): working architecture and proposed API
- [`protocol-notes.md`](protocol-notes.md): current ring, DMA, and interrupt model
- [`experiment-methodology.md`](experiment-methodology.md): experiment gates and recovery rules
- [`register-hypotheses.md`](register-hypotheses.md): register map with confidence labels
- [`official-driver-static-analysis.md`](official-driver-static-analysis.md): reproducible static findings
- [`framework-property-map.md`](framework-property-map.md): hash-locked boot and host-side DSP property dispatch
- [`device-startup-sequence.md`](device-startup-sequence.md): ordered official startup state machine
- [`dsp-model-and-memory-map.md`](dsp-model-and-memory-map.md): ADSP-21469-family evidence and data-sheet map
- [`dsp-boot-and-reset-control.md`](dsp-boot-and-reset-control.md): boot polling and all-eight reset isolation
- [`firmware-container-analysis.md`](firmware-container-analysis.md): exact OCTO artifact and unresolved loader rules
- [`firmware-family-inventory.md`](firmware-family-inventory.md): all 47 firmware-update wrappers and statistical comparisons
- [`runtime-response-state.md`](runtime-response-state.md): why dequeue works without replies and the required next evidence
- [`system-information-record.md`](system-information-record.md): official 168-byte host-built record, lifecycle gate, and exact BAR sources
- [`official-plugin-resource-inventory.md`](official-plugin-resource-inventory.md): metadata-only inventory of official `Bill` resources
- [`bill-resource-analysis.md`](bill-resource-analysis.md): ordinary DSP resource format, conditional transform, and runtime allocator
- [`authorization-states.md`](authorization-states.md): exact four-wire-value decoder and official display meanings
- [`driver-api.md`](driver-api.md): Linux transport module, userspace API, and capability boundary
- [`program-execution-gates.md`](program-execution-gates.md): harmless-program, API, recovery, and all-eight isolation criteria
- [`roadmap.md`](roadmap.md): milestones toward general-purpose DSP work
- [`reverse-engineering-status.md`](reverse-engineering-status.md): completion matrix for every requested outcome

## Captured baselines

- [`baseline-2026-08-04.json`](baseline-2026-08-04.json): passive PCI and host profile
- [`identity-2026-08-04.json`](identity-2026-08-04.json): allowlisted identity words
- [`dsp-status-2026-08-04.json`](dsp-status-2026-08-04.json): eight stable DSP-ready values
- [`vfio-2026-08-04.json`](vfio-2026-08-04.json): VFIO and IOMMU capability capture
- [`passive-target-2026-08-05.json`](passive-target-2026-08-05.json): read-only live endpoint reconfirmation
- [`bill-structure-11.0.1.json`](bill-structure-11.0.1.json): aggregate entropy, pairing, compression, block, checksum, and digest tests for 87 resources
- [`public-bill-audit-2026-08-05.json`](public-bill-audit-2026-08-05.json): exact five-resource comparison with a hash-locked public capture

## Experiments

| ID | Procedure | Result | Outcome |
|---:|---|---|---|
| 001 | [`experiment-001-ring-descriptors.md`](experiment-001-ring-descriptors.md) | [`experiment-001-result.json`](experiment-001-result.json) | Five ring pages published and restored |
| 002 | [`experiment-002-dma-engine.md`](experiment-002-dma-engine.md) | [`experiment-002-result.json`](experiment-002-result.json) | DSP0 DMA enable isolated and restored |
| 003 | [`experiment-003-reset-recovery.md`](experiment-003-reset-recovery.md) | [`experiment-003-result.json`](experiment-003-result.json) | Cold state recovered by VFIO reset |
| 004 | [`experiment-004-extended-ring-snapshot.md`](experiment-004-extended-ring-snapshot.md) | [`experiment-004-result.json`](experiment-004-result.json) | All 256 ring-window words cold-zero |
| 005 | [`experiment-005-ring-control-write.md`](experiment-005-ring-control-write.md) | [`experiment-005-result.json`](experiment-005-result.json) | `+0x20` retained and restored |
| 006 | [`experiment-006-ring-doorbell-write.md`](experiment-006-ring-doorbell-write.md) | [`experiment-006-result.json`](experiment-006-result.json) | `+0x24` retained and restored |
| 007 | [`experiment-007-empty-ring-activation.md`](experiment-007-empty-ring-activation.md) | [`experiment-007-result.json`](experiment-007-result.json) | Empty activation caused no page change |
| 008 | [`experiment-008-zero-entry-fetch.md`](experiment-008-zero-entry-fetch.md) | [`experiment-008-result.json`](experiment-008-result.json) | Hardware read index advanced |
| 009 | [`experiment-009-official-query.md`](experiment-009-official-query.md) | Included in procedure | Command dequeued, no response |
| 010 | [`experiment-010-official-query-interrupt-gates.md`](experiment-010-official-query-interrupt-gates.md) | [`experiment-010-result.json`](experiment-010-result.json) | Interrupt mask accepted, no response |
| 011 | [`experiment-011-official-ring-initializer.md`](experiment-011-official-ring-initializer.md) | [`experiment-011-result.json`](experiment-011-result.json) | Official four-page ring order accepted, DMA disabled |
| 012 | [`experiment-012-capability-and-audio-snapshot.md`](experiment-012-capability-and-audio-snapshot.md) | [`experiment-012-result.json`](experiment-012-result.json) | Optional 4 MiB audio path absent on OCTO |
| 013 | [`experiment-013-full-octo-start.md`](experiment-013-full-octo-start.md) | [`experiment-013-result.json`](experiment-013-result.json) | All 16 rings and eight DMA engines started and recovered |
| 014 | [`experiment-014-016-full-start-queries.md`](experiment-014-016-full-start-queries.md) | [`experiment-014-result.json`](experiment-014-result.json) | Query 026 consumed after full startup, no response |
| 015 | [`experiment-014-016-full-start-queries.md`](experiment-014-016-full-start-queries.md) | [`experiment-015-result.json`](experiment-015-result.json) | Connect sequence and query 026 consumed, no response |
| 016 | [`experiment-014-016-full-start-queries.md`](experiment-014-016-full-start-queries.md) | [`experiment-016-result.json`](experiment-016-result.json) | Connect sequence and query 027 consumed, no response |
| 017 | [`experiment-017-per-dsp-reset-isolation.md`](experiment-017-per-dsp-reset-isolation.md) | [`experiment-017-result.json`](experiment-017-result.json) | Every per-DSP reset pulse isolated, re-enabled, and recovered |
| 018 | [`experiment-018-loader-response-probes.md`](experiment-018-loader-response-probes.md) | [`experiment-018-result.json`](experiment-018-result.json) | Three incomplete loader objects consumed, no response, exact recovery |
| 019 | [`experiment-019-kernel-transport.md`](experiment-019-kernel-transport.md) | [`experiment-019-result.json`](experiment-019-result.json) | Signed kernel transport passed start, all-eight reset, stop, unload, and recovery |
| 020 | [`experiment-020-resource-pools.md`](experiment-020-resource-pools.md) | [`experiment-020-result.json`](experiment-020-result.json) | All four resource pools recovered identically across eight DSPs, zero writes |
| 021 | [`experiment-021-runtime-load.md`](experiment-021-runtime-load.md) | [`experiment-021-result.json`](experiment-021-result.json) | Exact compatible HBUT chain timed out safely; no response write and full recovery |
| 023 | [`experiment-023-official-windows-reference.md`](experiment-023-official-windows-reference.md) | [`sanitized result`](data/experiment-023-official-windows-reference/result.json) | Official full HBUT chain consumed, RTC cold recovery passed, all 16 rings published, response targets stayed zero |
| 024 | [`experiment-024-post-update-query.md`](experiment-024-post-update-query.md) | [`experiment-024-result.json`](experiment-024-result.json) | Post-update connect and query consumed, no response, exact recovery |
| 025 | [`experiment-025-official-runtime-response.md`](experiment-025-official-runtime-response.md) | [`sanitized result`](data/experiment-025-official-runtime-response/result.json) | First nonzero OCTO responses and two `Bill` successes; RealVerb later disabled on `-38` |
| 026 | [`experiment-026-realverb-resource-sequence.md`](experiment-026-realverb-resource-sequence.md) | [`sanitized result`](data/experiment-026-realverb-resource-sequence/result.json) | First zero RealVerb target identified as exact resource `0x12b` |
| 027 | [`experiment-027-post-official-linux-response.md`](experiment-027-post-official-linux-response.md) | [`sanitized result`](data/experiment-027-post-official-linux-response/result.json) | Linux adopted the resident framework and received query 026 response in 1 ms |
| 028 | [`experiment-028-post-official-bill-12b.md`](experiment-028-post-official-bill-12b.md) | [`sanitized result`](data/experiment-028-post-official-bill-12b/result.json) | Exact first RealVerb object accepted from Linux in 1 ms |
| 029 | [`experiment-029-bill-integrity.md`](experiment-029-bill-integrity.md) | [`sanitized result`](data/experiment-029-bill-integrity/result.json) | One-bit prefix and core changes rejected; device-side integrity proven |
| 030 | [`experiment-030-eight-dsp-bill-isolation.md`](experiment-030-eight-dsp-bill-isolation.md) | [`sanitized result`](data/experiment-030-eight-dsp-bill-isolation/result.json) | Exact authenticated resource accepted independently on all eight DSPs with non-target rings unchanged |

Experiment 010 has a separate
[`independent recovery capture`](experiment-010-independent-recovery.json).
Experiment 011 also has a separate
[`independent recovery capture`](experiment-011-independent-recovery.json).
Experiment 013 has a separate
[`independent recovery capture`](experiment-013-independent-recovery.json).
Experiment 017 has a separate
[`independent recovery capture`](experiment-017-independent-recovery.json).

## Evidence labels

- **Observed:** read directly from the exact OCTO target.
- **Executed:** a bounded write experiment completed with its cleanup path.
- **Static:** derived from an exact, hash-identified driver binary.
- **Hypothesis:** suggested by related public work or inference and not yet
  established on this card.
- **Blocked:** intentionally deferred until a safer prerequisite is established.
