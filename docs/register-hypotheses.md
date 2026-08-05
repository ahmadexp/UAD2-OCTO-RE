# Register hypotheses for the 64 KiB endpoint

These offsets come from public reverse engineering of related `1a00:0002`
Apollo endpoints. They are unverified on subsystem `1a00:0005` until marked as
observed. An address appearing here does not authorize writing it.

| Offset or range | Candidate meaning | Initial access | OCTO status |
|---:|---|---|---|
| `0x0020..0x002c` | Serial or identity string | Read | Observed, 14 ASCII digits plus NUL padding |
| `0x0030..0x0034` | Firmware or shared-range base | Read | Observed as `0x000000002b89b176`; exact meaning unresolved |
| `0x2000` | DSP 0 command ring window | Read, then bounded VFIO writes | Observed |
| `0x2200` | DMA master control | Bounded VFIO writes only | Observed cold as `0x0001fe00` |
| `0x2204` | Interrupt-enable bitmask | Bounded VFIO writes only | DSP 0 vectors accepted as `0x0000001f` |
| `0x2208` | Interrupt arm or acknowledge | Bounded VFIO writes only | Official DSP 0 initialization sequence tested |
| `0x2218` | FPGA revision | Read | Observed as `0xa012dc0d`, v2 bit set |
| `0x2234` | Extended capabilities | Read | Observed as `0x00300811`, DSP count 8 |
| `0x3800` | Mixer or shared SRAM window | Do not read yet | Unobserved |
| `0x6000` | DSP 4 command ring window | Bounded VFIO reads and writes | Observed, published and restored in Experiment 013 |
| `0x8000..0xbfff` | Optional audio playback/capture scatter-gather tables | Read only on OCTO | All 4,096 dwords observed zero; static branch proves inapplicable to subsystem `0005` |
| `0xc000..0xcfff` | Firmware mailbox and descriptors | Do not read yet | Unobserved |

Candidate DSP ring formula from related endpoints:

```text
DSP 0..3: 0x2000 + dsp_index * 0x80
DSP 4..7: 0x5e00 + dsp_index * 0x80
```

The unusual high-bank formula must be independently verified before use.

The eight boot-poll offsets have now been observed and are stable. Every slot
has ready bit 0 set:

| DSP | Ring bank | Boot-status offset | Observed status |
|---:|---:|---:|---:|
| 0 | `0x2000` | `0x01a4` | `0x00000a03` |
| 1 | `0x2080` | `0x09a4` | `0x00000003` |
| 2 | `0x2100` | `0x11a4` | `0x00000003` |
| 3 | `0x2180` | `0x19a4` | `0x00000003` |
| 4 | `0x6000` | `0x41a4` | `0x00000003` |
| 5 | `0x6080` | `0x49a4` | `0x00000003` |
| 6 | `0x6100` | `0x51a4` | `0x00000003` |
| 7 | `0x6180` | `0x59a4` | `0x00000003` |

Command and response hardware read indexes were zero for all eight slots and
did not change over 250 ms. This is consistent with ready resident DSP
firmware but no host rings having been programmed.

A subsequent capture read all eleven documented words in each of sixteen rings,
176 ring words total. Every ring word was zero. Thus page addresses, the two
control/index fields, and the hardware field at `+0x28` are all clear in the
cold host state.

Static analysis of the official Windows ring class establishes these roles:

| Ring offset | Driver use | Working name |
|---:|---|---|
| `+0x20` | Written with the new host index when work is pending | Pending or notify index |
| `+0x24` | Written with the new host producer index | Host write index |
| `+0x28` | Read by the host and compared with its producer index | Hardware read index |

On initialization, the driver reads `+0x28`, bounds it to the 1024-entry ring,
and writes that value to `+0x24` and `+0x20`. When flushing entries it copies
16-byte records into one of four DMA pages, advances the host index modulo
1024, writes `+0x24`, and writes `+0x20` if the hardware and host indexes
differ. These roles supersede the earlier provisional pointer and doorbell
labels used in experiment output keys.

For the observed v2 capability word:

```text
extended_capabilities = 0x00300811
DSP count, bits 15:8  = 8
family, bits 25:20    = 3
low feature bits      = 0x11, meaning not yet decoded
```
