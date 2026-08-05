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
- [`device-startup-sequence.md`](device-startup-sequence.md): ordered official startup state machine
- [`roadmap.md`](roadmap.md): milestones toward general-purpose DSP work

## Captured baselines

- [`baseline-2026-08-04.json`](baseline-2026-08-04.json): passive PCI and host profile
- [`identity-2026-08-04.json`](identity-2026-08-04.json): allowlisted identity words
- [`dsp-status-2026-08-04.json`](dsp-status-2026-08-04.json): eight stable DSP-ready values
- [`vfio-2026-08-04.json`](vfio-2026-08-04.json): VFIO and IOMMU capability capture

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
| 011 | [`experiment-011-official-ring-initializer.md`](experiment-011-official-ring-initializer.md) | Not executed | Prepared no-command official ring order |

Experiment 010 has a separate
[`independent recovery capture`](experiment-010-independent-recovery.json).

## Evidence labels

- **Observed:** read directly from the exact OCTO target.
- **Executed:** a bounded write experiment completed with its cleanup path.
- **Static:** derived from an exact, hash-identified driver binary.
- **Hypothesis:** suggested by related public work or inference and not yet
  established on this card.
- **Blocked:** intentionally deferred until a safer prerequisite is established.
