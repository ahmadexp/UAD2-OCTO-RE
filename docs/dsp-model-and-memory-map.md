# DSP identification and memory map

## Identification

The high-resolution photograph optically confirms `ADSP-21469` and `KBCZ-00`
on all eight SHARC packages on the tested Rev. 5 board. This resolves the
processor model and package variant independently of the software evidence.
The symbolized Universal Audio macOS driver also contains the method name
`CPcieDSP::_waitFor469ToStart`, providing a separate consistency check.

Universal Audio's [official UAD-2 PCIe product page](https://www.uaudio.com/products/uad2-pcie)
confirms that the OCTO configuration contains eight SHARC processors. The
repository's 4032 by 3024 photograph resolves all eight package markings. The
visible `KBCZ-00` text establishes the KBCZ package variant and device suffix,
but the image does not show a `-3` or `-4` speed ordering suffix. The board's
exact processor clock therefore remains a runtime or clock-measurement
question rather than an optical claim.

The [Analog Devices product page](https://www.analog.com/en/products/adsp-21469.html)
describes an up-to-450 MHz 32/40-bit floating-point SHARC processor with 5 Mbits of
on-chip RAM, DDR2 support, two link ports, 48-bit instructions, and 16/32-bit
VISA instructions. The address ranges below are transcribed from the
[ADSP-21467/ADSP-21469 data sheet](https://www.analog.com/media/en/technical-documentation/data-sheets/ADSP-21467_21469.pdf).

The same photograph optically confirms the central programmable logic as a
Xilinx Spartan-6 `XC6SLX75T` in the `FGG676` package. AMD's
[Spartan-6 packaging guide](https://docs.amd.com/v/u/en-US/ug385) lists that
device/package combination. Eight adjacent packages carry Nanya
`NT5TU64M16`-family markings, consistent with one 16-bit DDR2 device per SHARC;
their complete suffixes are not read confidently and are not asserted here.

## Internal memory

The same physical memory has different address ranges for 64-, 48-, 32-, and
16-bit accesses.

| Block | Width | ROM | Reserved | SRAM | Trailing reserved |
|---:|---:|---|---|---|---|
| 0 | 64 | `0x40000..0x47fff` | `0x48000..0x48fff` | `0x49000..0x4efff` | none |
| 0 | 48 | `0x80000..0x8aaa9` | `0x8aaaa..0x8bfff` | `0x8c000..0x93fff` | `0x94000..0x9ffff` |
| 0 | 32 | `0x80000..0x8ffff` | `0x90000..0x91fff` | `0x92000..0x9dfff` | `0x9e000..0x9ffff` |
| 0 | 16 | `0x100000..0x11ffff` | `0x120000..0x123fff` | `0x124000..0x13bfff` | `0x13c000..0x13ffff` |
| 1 | 64 | `0x50000..0x57fff` | `0x58000..0x58fff` | `0x59000..0x5efff` | none |
| 1 | 48 | `0xa0000..0xaaaa9` | `0xaaaaa..0xabfff` | `0xac000..0xb3fff` | `0xb4000..0xbffff` |
| 1 | 32 | `0xa0000..0xaffff` | `0xb0000..0xb1fff` | `0xb2000..0xbdfff` | `0xbe000..0xbffff` |
| 1 | 16 | `0x140000..0x15ffff` | `0x160000..0x163fff` | `0x164000..0x17bfff` | `0x17c000..0x17ffff` |

Blocks 2 and 3 are SRAM-only:

| Block | 64-bit | 48-bit | 32-bit | 16-bit |
|---:|---|---|---|---|
| 2 | `0x60000..0x63fff` | `0xc0000..0xc5554` | `0xc0000..0xc7fff` | `0x180000..0x18ffff` |
| 3 | `0x70000..0x73fff` | `0xe0000..0xe5554` | `0xe0000..0xe7fff` | `0x1c0000..0x1cffff` |

The I/O processor register range is `0x00000000..0x0003ffff`.

## External memory

Without DDR2, external banks occupy:

| Bank | Range |
|---:|---|
| 0 | `0x00200000..0x003fffff` |
| 1 | `0x04000000..0x043fffff` |
| 2 | `0x08000000..0x083fffff` |
| 3 | `0x0c000000..0x0c3fffff` |

With DDR2, the corresponding ranges are:

| Bank | Range |
|---:|---|
| 0 | `0x00200000..0x03ffffff` |
| 1 | `0x04000000..0x07ffffff` |
| 2 | `0x08000000..0x0bffffff` |
| 3 | `0x0c000000..0x0fffffff` |

Bank 0 instruction fetch aliases are `0x00200000..0x005fffff` for normal-word
ISA fetches and `0x00600000..0x00ffffff` for short-word VISA fetches.

## Remaining board-specific work

The processor data sheet is not the UAD loader map. The project still needs to
recover which regions the resident framework reserves, the plug-in overlay
layout, entry-point conventions, inter-core sharing, and the FPGA-visible host
window. No heartbeat should be linked until those board-specific reservations
are derived from a real loader transaction or decoded executable container.
