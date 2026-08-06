# Experiment 028: exact Bill acceptance from Linux

Linux submitted the exact first RealVerb resource captured from the official
OCTO path. DSP0 consumed it and returned the exact resource-success form in one
millisecond.

## Exact-object gate

The wrapper accepts only the private 460-byte target whose SHA-256 is
`0c353512fb27ed961b4e0746de7f1bbc462447f6e2c0263bc6209cda7b7718d0`.
The C probe independently checks the envelope command, allocation offset,
`Bill` magic, resource ID, attributes, declared body length, and trailing
length before enabling DMA.

Raw vendor bytes are not included in the repository.

## Result

The response was:

```text
80070004 00000000 0000012b 00010073
```

The header, zero status, resource ID, and command echo match the exact success
branch in both analyzed host drivers. The command and response descriptors were
consumed in 1 ms. No mapped byte outside the four-word response changed, all
eight DSPs remained ready, and restore plus VFIO reset passed.

This supersedes interpreting the all-zero `0x12b` target in Experiment 026 as
intrinsic rejection of that object. The invasive VM capture and the bounded
Linux replay used different observation and framework-state contexts, so the
earlier zero cannot identify the object as invalid. This experiment proves
that Linux reproduces the ordinary resource transport and completion rules.

Resource acceptance is not entry-point execution. No module header, activation
command, audio route, or arbitrary output buffer was supplied.

See [`result.json`](data/experiment-028-post-official-bill-12b/result.json).
