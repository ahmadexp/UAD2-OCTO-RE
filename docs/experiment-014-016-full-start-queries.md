# Experiments 014 through 016: full-start command completion

Status: executed safely on 2026-08-05. Host commands were consumed in every
case, but no DSP response was produced.

## Shared setup

Each run reproduced Experiment 013's full eight-DSP startup, mapped one
additional bounded response buffer, registered the compressed callback mask,
and queued the official four-word response descriptor before the inline query.
Cleanup restored every descriptor, index, interrupt, and DMA word before VFIO
reset.

## Experiment 014

Query 026 (`0x00260001`, GetAuthReceipt) was consumed. The response-ring index
did not advance, the five-dword canary remained intact, and the expected
`0x800c0005` header did not appear.

## Experiment 015

The official connect sequence was submitted first: `0x00230002` with argument
one to every DSP, then DSP0 clock command `0x00100002`. Every connect command
was consumed within 1 ms. Query 026 was then consumed, again with no response.

## Experiment 016

After the same successful connect sequence, query 027 (`0x00270001`,
GetAuthSequenceNumber) was consumed. Its expected `0x800d0002` response header
did not appear and its response canary remained intact.

## Interpretation

These results prove host-to-FPGA command delivery and hardware completion
across the fully initialized transport. Repeating additional service queries
in the same boot state is not justified. The likely missing prerequisite is a
loaded DSP runtime/framework service, but that remains an inference until the
runtime loader is recovered.

See [`experiment-014-result.json`](experiment-014-result.json),
[`experiment-015-result.json`](experiment-015-result.json), and
[`experiment-016-result.json`](experiment-016-result.json).
