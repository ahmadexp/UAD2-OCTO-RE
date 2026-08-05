# Experiment 012: capability and optional-audio snapshot

Status: executed successfully on 2026-08-05.

## Objective

Test the static finding that the two shared 4 MiB tables belong to an optional
audio extension that is not instantiated by the OCTO capability branch.

## Method and result

The VFIO probe mapped BAR0 read-only, created no DMA mapping, and contained no
MMIO write helper. It read the capability and optional-audio control words plus
all 4,096 dwords in BAR windows `0x8000..0xbfff`.

The observed capability was `0x00300811`. All optional-audio control words and
all 4,096 shared-table dwords were zero. DMA control remained at cold value
`0x0001fe00`. This confirms that reproducing the 4 MiB tables is neither
applicable nor necessary on the tested OCTO.

See [`experiment-012-result.json`](experiment-012-result.json) for the complete
capture.

