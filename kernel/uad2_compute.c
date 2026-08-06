// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experimental transport driver for the exact UAD-2 OCTO 1a00:0002/0005
 * profile. The ABI intentionally has no raw MMIO, physical-address, arbitrary
 * command, or arbitrary program-image operation. Program loading is limited to
 * the exact authenticated RealVerb resource bundle validated below.
 */

#include <linux/capability.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kref.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "uad2_compute.h"

#define UAD2_VENDOR_ID 0x1a00
#define UAD2_DEVICE_ID 0x0002
#define UAD2_SUBVENDOR_ID 0x1a00
#define UAD2_SUBDEVICE_OCTO 0x0005
#define UAD2_BAR0_SIZE 0x10000

#define UAD2_DSP_COUNT 8
#define UAD2_RING_COUNT 2
#define UAD2_RING_PAGES 4
#define UAD2_RING_PAGE_BYTES 4096
#define UAD2_RING_ENTRIES 1024
#define UAD2_MAX_BUFFERS 16
#define UAD2_MAX_BUFFER_BYTES UAD2_RING_PAGE_BYTES

#define UAD2_PROGRAM_CHUNKS 16
#define UAD2_PROGRAM_RESOURCES 13
#define UAD2_PROGRAM_IMAGE_PAGES 17
#define UAD2_PROGRAM_WORKSPACE_PAGES 34
#define UAD2_PROGRAM_MEMSPEC_PAGE 16
#define UAD2_PROGRAM_RESPONSE_PAGE 17
#define UAD2_PROGRAM_INPUT_PAGE 30
#define UAD2_PROGRAM_OUTPUT_PAGE 32
#define UAD2_PROGRAM_WORKSPACE_BYTES \
	(UAD2_PROGRAM_WORKSPACE_PAGES * UAD2_RING_PAGE_BYTES)
#define UAD2_PROGRAM_IMAGE_BYTES \
	(UAD2_PROGRAM_IMAGE_PAGES * UAD2_RING_PAGE_BYTES)
#define UAD2_BILL_MAGIC 0x6c6c6942
#define UAD2_BILL_RESPONSE 0x80070004
#define UAD2_PROCESS_RESPONSE 0x80020044
#define UAD2_PROCESS_MARKER 0xf001000e
#define UAD2_PROCESS_COMMAND 0x000b0004
#define UAD2_PROCESS_FLAGS 0x00400000
#define UAD2_PROCESS_ADDRESS 0x0009d00a
#define UAD2_PROCESS_PROPERTY_7 0x000b2000
#define UAD2_PROCESS_INPUT_DWORDS 0x42
#define UAD2_PROCESS_OUTPUT_DWORDS 0x44
#define UAD2_INTERRUPT_SHADOW 0xcccccccc
#define UAD2_WAIT_POLLS 600

#define UAD2_DMA_CONTROL 0x2200
#define UAD2_INTERRUPT_ENABLE 0x2204
#define UAD2_INTERRUPT_ACK 0x2208
#define UAD2_FPGA_REVISION 0x2218
#define UAD2_NOTIFICATION_CONTROL 0x2220
#define UAD2_EXTENDED_CAPABILITIES 0x2234

#define UAD2_EXPECTED_FPGA_REVISION 0xa012dc0d
#define UAD2_EXPECTED_CAPABILITIES 0x00300811
#define UAD2_DMA_COLD_RESET 0x0001fe00
#define UAD2_DMA_RESET_GLOBAL 0x0001fe01
#define UAD2_DMA_GLOBAL_ONLY 0x00000001
#define UAD2_DMA_ALL_DSPS 0x000001ff

static const u32 uad2_dsp_banks[UAD2_DSP_COUNT] = {
	0x2000, 0x2080, 0x2100, 0x2180,
	0x6000, 0x6080, 0x6100, 0x6180,
};

static const u32 uad2_ready_offsets[UAD2_DSP_COUNT] = {
	0x01a4, 0x09a4, 0x11a4, 0x19a4,
	0x41a4, 0x49a4, 0x51a4, 0x59a4,
};

static const u32 uad2_property_7_offsets[UAD2_DSP_COUNT] = {
	0x01a0, 0x09a0, 0x11a0, 0x19a0,
	0x41a0, 0x49a0, 0x51a0, 0x59a0,
};

struct uad2_resource_definition {
	u32 id;
	u32 command;
	u32 allocation;
	u32 body_bytes;
	u32 replacement_dwords;
	u32 total_bytes;
	u8 first_chunk;
	u8 chunk_count;
	u16 chunk_bytes[2];
};

static const struct uad2_resource_definition uad2_realverb_resources[] = {
	{0x12b, 0x00010073, 0x000e0000, 0x01b0, 0x0060, 460,  0, 1, {460, 0}},
	{0x0eb, 0x000100b5, 0x000e0040, 0x02b8, 0x00a2, 724,  1, 1, {724, 0}},
	{0x0c1, 0x0001009c, 0x000e00ac, 0x0254, 0x008d, 624,  2, 1, {624, 0}},
	{0x0a5, 0x000101e7, 0x000e010a, 0x0780, 0x01c8, 1948, 3, 1, {1948, 0}},
	{0x120, 0x00010099, 0x000e023a, 0x0248, 0x008a, 612,  4, 1, {612, 0}},
	{0x0bd, 0x00010057, 0x000e0296, 0x0140, 0x0048, 348,  5, 1, {348, 0}},
	{0x11f, 0x0001005d, 0x000e02c6, 0x0158, 0x004e, 372,  6, 1, {372, 0}},
	{0x0d0, 0x0001006c, 0x000e02fa, 0x0194, 0x005d, 432,  7, 1, {432, 0}},
	{0x0f9, 0x0001055e, 0x000e0338, 0x155c, 0x054f, 5496, 8, 2, {4096, 1400}},
	{0x0bf, 0x00010600, 0x000e06c2, 0x17e4, 0x05f1, 6144, 10, 2, {4096, 2048}},
	{0x11e, 0x0001003f, 0x000e0ab8, 0x00e0, 0x0030, 252, 12, 1, {252, 0}},
	{0x11d, 0x00010081, 0x000e0ad8, 0x01e8, 0x0072, 516, 13, 1, {516, 0}},
	{0x0d1, 0x000104a8, 0x000e0b24, 0x1284, 0x0495, 4768, 14, 2, {4096, 672}},
};

static const u32 uad2_zero_commands[][4] = {
	{0x00080004, 0x0009d00a, 0, 0x01ae}, {0x00080004, 0x0009cf74, 0, 0x0096},
	{0x00080004, 0x0009cede, 0, 0x0096}, {0x00080004, 0x0009cea6, 0, 0x0038},
	{0x00080004, 0x0009cea4, 0, 0x0002}, {0x00080004, 0x0009cea2, 0, 0x0002},
	{0x00080004, 0x0009cea0, 0, 0x0002}, {0x00080004, 0x0009ce9e, 0, 0x0002},
	{0x00080004, 0x0009ce7c, 0, 0x0022}, {0x00080004, 0x0009ce5a, 0, 0x0022},
	{0x00080004, 0x0009ce38, 0, 0x0022}, {0x00080004, 0x0009ce16, 0, 0x0022},
	{0x00080004, 0x0009cdf4, 0, 0x0022}, {0x00080004, 0x0009cdd2, 0, 0x0022},
	{0x00080004, 0x0009cdb0, 0, 0x0022}, {0x00080004, 0x0009cd8e, 0, 0x0022},
	{0x00080004, 0x0009cd6c, 0, 0x0022}, {0x00080004, 0x0009cd4a, 0, 0x0022},
	{0x00080004, 0x0009cd28, 0, 0x0022}, {0x00080004, 0x0009cd24, 0, 0x0004},
	{0x00080004, 0x0009cd20, 0, 0x0004}, {0x00080004, 0x0009cd1c, 0, 0x0004},
	{0x00080004, 0x0009ccfc, 0, 0x0020}, {0x00080004, 0x0009ccec, 0, 0x0010},
	{0x00080004, 0x0009ccdc, 0, 0x0010}, {0x00080004, 0x0009cccc, 0, 0x0010},
	{0x00080004, 0x0009ccbc, 0, 0x0010}, {0x00080004, 0x0009ccb0, 0, 0x000c},
	{0x00080004, 0x0009cca0, 0, 0x0010}, {0x00080004, 0x0009cbf6, 0, 0x00aa},
	{0x00080004, 0x0009cb4c, 0, 0x00aa}, {0x00080004, 0x0009cb0c, 0, 0x0040},
	{0x00080004, 0x08fee380, 0, 0xfc80},
};

static const u32 uad2_memspec_expected[65] = {
	0x00150041,
	0x0009d0b2, 0x0009cf74, 0x0009d0b3, 0x0009cede,
	0x0009d0b4, 0x0009cea6, 0x0009d0bc, 0x0009cea4,
	0x0009d0bd, 0x0009cea2, 0x0009d0be, 0x0009cea0,
	0x0009d0bf, 0x0009ce9e, 0x0009d0c0, 0x0009ce7c,
	0x0009d0c1, 0x0009ce5a, 0x0009d0c2, 0x0009ce38,
	0x0009d0c3, 0x0009ce16, 0x0009d0c4, 0x0009cdf4,
	0x0009d0c5, 0x0009cdd2, 0x0009d0c6, 0x0009cdb0,
	0x0009d0c7, 0x0009cd8e, 0x0009d0c8, 0x0009cd6c,
	0x0009d0c9, 0x0009cd4a, 0x0009d0ca, 0x0009cd28,
	0x0009d0ce, 0x0009cd24, 0x0009d0cf, 0x0009cd20,
	0x0009d0d0, 0x0009cd1c, 0x0009d0e6, 0x0009ccfc,
	0x0009d0f1, 0x0009ccec, 0x0009d0f2, 0x0009ccdc,
	0x0009d0f3, 0x0009cccc, 0x0009d0f4, 0x0009ccbc,
	0x0009d0f5, 0x0009ccb0, 0x0009d0f6, 0x0009cca0,
	0x0009d1aa, 0x0009cbf6, 0x0009d1ab, 0x0009cb4c,
	0x0009d1ac, 0x0009cb0c, 0x0009d1b4, 0x08fee380,
};

struct uad2_ring_page {
	void *cpu;
	dma_addr_t dma;
};

struct uad2_user_buffer {
	void *cpu;
	dma_addr_t dma;
	size_t bytes;
	u64 id;
	u32 flags;
	atomic_t map_count;
};

struct uad2_loaded_program {
	void *cpu;
	dma_addr_t dma;
	u64 id;
	u32 dsp;
	u32 command_position;
	u32 response_position;
	u32 next_request_id;
	bool loaded;
};

struct uad2_completed_job {
	u64 id;
	s32 status;
	u32 request_id;
	u32 response_marker;
	bool valid;
};

struct uad2_compute_device {
	struct pci_dev *pdev;
	void __iomem *bar;
	struct miscdevice misc;
	struct mutex lock;
	struct kref refcount;
	struct uad2_ring_page pages[UAD2_DSP_COUNT][UAD2_RING_COUNT]
				    [UAD2_RING_PAGES];
	struct uad2_user_buffer buffers[UAD2_MAX_BUFFERS];
	struct uad2_loaded_program program;
	struct uad2_completed_job completed_job;
	u64 next_object_id;
	bool started;
	bool opened;
	bool removing;
	unsigned int index;
	struct address_space *mapping;
};

static atomic_t uad2_next_index = ATOMIC_INIT(0);

static void uad2_free_program(struct uad2_compute_device *device);

static u32 uad2_read(struct uad2_compute_device *device, u32 offset)
{
	return readl(device->bar + offset);
}

static void uad2_write(struct uad2_compute_device *device, u32 offset,
		       u32 value)
{
	writel(value, device->bar + offset);
}

static bool uad2_all_dsps_ready(struct uad2_compute_device *device)
{
	unsigned int dsp;

	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++)
		if (!(uad2_read(device, uad2_ready_offsets[dsp]) & 1))
			return false;
	return true;
}

static bool uad2_all_ring_words_zero(struct uad2_compute_device *device)
{
	unsigned int dsp, ring, word;

	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++)
		for (ring = 0; ring < UAD2_RING_COUNT; ring++)
			for (word = 0; word < 16; word++)
				if (uad2_read(device, uad2_dsp_banks[dsp] +
					      ring * 0x40 + word * 4))
					return false;
	return true;
}

static void uad2_clear_all_rings(struct uad2_compute_device *device)
{
	unsigned int dsp, ring, word;

	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++)
		for (ring = 0; ring < UAD2_RING_COUNT; ring++)
			for (word = 0; word < 10; word++)
				uad2_write(device, uad2_dsp_banks[dsp] +
					   ring * 0x40 + word * 4, 0);
}

static bool uad2_cold_precondition(struct uad2_compute_device *device)
{
	return uad2_read(device, UAD2_DMA_CONTROL) == UAD2_DMA_COLD_RESET &&
		uad2_read(device, UAD2_INTERRUPT_ENABLE) == 0 &&
		uad2_read(device, UAD2_FPGA_REVISION) ==
			UAD2_EXPECTED_FPGA_REVISION &&
		uad2_read(device, UAD2_EXTENDED_CAPABILITIES) ==
			UAD2_EXPECTED_CAPABILITIES &&
		uad2_all_ring_words_zero(device) &&
		uad2_all_dsps_ready(device);
}

static void uad2_initialize_ring(struct uad2_compute_device *device,
				 unsigned int dsp, unsigned int ring)
{
	u32 base = uad2_dsp_banks[dsp] + ring * 0x40;
	u32 index = uad2_read(device, base + 0x28);
	unsigned int page;

	if (index >= UAD2_RING_ENTRIES)
		index = 0;
	uad2_write(device, base + 0x24, index);
	uad2_write(device, base + 0x20, index);
	for (page = 0; page < UAD2_RING_PAGES; page++) {
		dma_addr_t address = device->pages[dsp][ring][page].dma;

		uad2_write(device, base + page * 8, lower_32_bits(address));
		uad2_write(device, base + page * 8 + 4,
			   upper_32_bits(address));
	}
}

static bool uad2_rings_published(struct uad2_compute_device *device)
{
	unsigned int dsp, ring, page;

	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++) {
		for (ring = 0; ring < UAD2_RING_COUNT; ring++) {
			u32 base = uad2_dsp_banks[dsp] + ring * 0x40;

			for (page = 0; page < UAD2_RING_PAGES; page++) {
				u64 observed = uad2_read(device, base + page * 8);

				observed |= (u64)uad2_read(device,
					base + page * 8 + 4) << 32;
				if (observed != device->pages[dsp][ring][page].dma)
					return false;
			}
		}
	}
	return true;
}

static int uad2_start_locked(struct uad2_compute_device *device)
{
	unsigned int dsp, ring, page;

	if (device->started)
		return 0;
	if (!uad2_cold_precondition(device))
		return -EBUSY;

	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++)
		for (ring = 0; ring < UAD2_RING_COUNT; ring++)
			for (page = 0; page < UAD2_RING_PAGES; page++)
				memset(device->pages[dsp][ring][page].cpu, 0,
				       UAD2_RING_PAGE_BYTES);
	dma_wmb();
	pci_set_master(device->pdev);
	uad2_write(device, UAD2_NOTIFICATION_CONTROL, 0);
	uad2_write(device, UAD2_INTERRUPT_ENABLE, 0);
	uad2_write(device, UAD2_INTERRUPT_ACK, 0);
	uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_RESET_GLOBAL);
	uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_GLOBAL_ONLY);
	uad2_write(device, UAD2_INTERRUPT_ACK, 0xffffffff);
	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++) {
		for (ring = 0; ring < UAD2_RING_COUNT; ring++)
			uad2_initialize_ring(device, dsp, ring);
		uad2_write(device, UAD2_DMA_CONTROL, (1U << (dsp + 2)) - 1);
	}
	uad2_read(device, UAD2_DMA_CONTROL);

	if (uad2_read(device, UAD2_DMA_CONTROL) != UAD2_DMA_ALL_DSPS ||
	    !uad2_rings_published(device) || !uad2_all_dsps_ready(device)) {
		uad2_write(device, UAD2_INTERRUPT_ENABLE, 0);
		uad2_write(device, UAD2_INTERRUPT_ACK, 0xffffffff);
		uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_RESET_GLOBAL);
		uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_GLOBAL_ONLY);
		uad2_clear_all_rings(device);
		uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_COLD_RESET);
		uad2_read(device, UAD2_DMA_CONTROL);
		pci_clear_master(device->pdev);
		return -EIO;
	}
	device->started = true;
	return 0;
}

static int uad2_stop_locked(struct uad2_compute_device *device)
{
	bool recovered;

	if (!device->started)
		return 0;
	uad2_write(device, UAD2_INTERRUPT_ENABLE, 0);
	uad2_write(device, UAD2_INTERRUPT_ACK, 0xffffffff);
	uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_RESET_GLOBAL);
	uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_GLOBAL_ONLY);
	uad2_clear_all_rings(device);
	uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_COLD_RESET);
	uad2_read(device, UAD2_DMA_CONTROL);
	pci_clear_master(device->pdev);
	device->started = false;
	recovered = uad2_cold_precondition(device);
	uad2_free_program(device);
	return recovered ? 0 : -EIO;
}

static int uad2_reset_dsp_locked(struct uad2_compute_device *device,
				 struct uad2_compute_reset *reset)
{
	u32 enable_bit, reset_bit, disabled, before;
	bool ready_after_pulse;

	if (!device->started)
		return -EPIPE;
	if (reset->dsp_index >= UAD2_DSP_COUNT || reset->flags)
		return -EINVAL;
	before = uad2_read(device, UAD2_DMA_CONTROL);
	if (before != UAD2_DMA_ALL_DSPS)
		return -EBUSY;
	enable_bit = 1U << (reset->dsp_index + 1);
	reset_bit = 1U << (reset->dsp_index + 9);
	disabled = before & ~enable_bit;

	reset->dma_control_before = before;
	uad2_write(device, UAD2_DMA_CONTROL, disabled | reset_bit);
	uad2_write(device, UAD2_DMA_CONTROL, disabled);
	msleep(10);
	ready_after_pulse = !!(uad2_read(device,
		uad2_ready_offsets[reset->dsp_index]) & 1);
	uad2_write(device, UAD2_DMA_CONTROL, before);
	uad2_read(device, UAD2_DMA_CONTROL);
	reset->dma_control_after = uad2_read(device, UAD2_DMA_CONTROL);
	reset->ready_after = !!(uad2_read(device,
		uad2_ready_offsets[reset->dsp_index]) & 1);
	if (!ready_after_pulse || reset->dma_control_after != before ||
	    !reset->ready_after)
		return -EIO;
	if (device->program.loaded &&
	    device->program.dsp == reset->dsp_index)
		uad2_free_program(device);
	return 0;
}

static u32 *uad2_ring_entry(struct uad2_compute_device *device, u32 dsp,
			    u32 ring, u32 index)
{
	u32 bounded = index % UAD2_RING_ENTRIES;
	u32 page = bounded / (UAD2_RING_PAGE_BYTES / 16);
	u32 offset = (bounded % (UAD2_RING_PAGE_BYTES / 16)) * 16;

	return device->pages[dsp][ring][page].cpu + offset;
}

static int uad2_wait_index(struct uad2_compute_device *device, u32 offset,
			   u32 expected)
{
	unsigned int poll;

	for (poll = 0; poll < UAD2_WAIT_POLLS; poll++) {
		if (uad2_read(device, offset) == expected)
			return 0;
		usleep_range(900, 1100);
	}
	return -ETIMEDOUT;
}

static bool uad2_zero_padding(const u8 *page, size_t used)
{
	size_t byte;

	for (byte = used; byte < UAD2_RING_PAGE_BYTES; byte++)
		if (page[byte])
			return false;
	return true;
}

static bool uad2_word_canary(const u32 *page, size_t used_dwords, u32 canary)
{
	size_t word;

	for (word = used_dwords;
	     word < UAD2_RING_PAGE_BYTES / sizeof(*page); word++)
		if (page[word] != canary)
			return false;
	return true;
}

static bool uad2_validate_program_image(const u8 *image)
{
	unsigned int resource, part;

	for (resource = 0; resource < ARRAY_SIZE(uad2_realverb_resources);
	     resource++) {
		const struct uad2_resource_definition *definition =
			&uad2_realverb_resources[resource];
		const u32 *header = (const u32 *)(image +
			definition->first_chunk * UAD2_RING_PAGE_BYTES);

		if (header[0] != definition->command ||
		    header[1] != definition->allocation ||
		    header[2] != UAD2_BILL_MAGIC || header[3] != definition->id ||
		    header[4] != 0 || header[5] != definition->body_bytes ||
		    header[6] != definition->replacement_dwords ||
		    definition->total_bytes != definition->body_bytes + 28 ||
		    (definition->command & 0xffff) != definition->total_bytes / 4)
			return false;
		for (part = 0; part < definition->chunk_count; part++) {
			const u8 *page = image +
				(definition->first_chunk + part) * UAD2_RING_PAGE_BYTES;

			if (!uad2_zero_padding(page, definition->chunk_bytes[part]))
				return false;
		}
	}
	if (memcmp(image + UAD2_PROGRAM_MEMSPEC_PAGE * UAD2_RING_PAGE_BYTES,
		   uad2_memspec_expected, sizeof(uad2_memspec_expected)))
		return false;
	return uad2_zero_padding(image +
		UAD2_PROGRAM_MEMSPEC_PAGE * UAD2_RING_PAGE_BYTES,
		sizeof(uad2_memspec_expected));
}

static void uad2_free_program(struct uad2_compute_device *device)
{
	if (device->program.cpu)
		dma_free_coherent(&device->pdev->dev,
			UAD2_PROGRAM_WORKSPACE_BYTES, device->program.cpu,
			device->program.dma);
	memset(&device->program, 0, sizeof(device->program));
	memset(&device->completed_job, 0, sizeof(device->completed_job));
}

static int uad2_recover_failed_program(struct uad2_compute_device *device,
				       int error)
{
	uad2_free_program(device);
	if (uad2_stop_locked(device))
		return -EIO;
	return error;
}

static int uad2_load_program_locked(struct uad2_compute_device *device,
				    struct uad2_compute_program_load *load)
{
	struct uad2_loaded_program *program = &device->program;
	u32 command_position = 0, response_position = 0;
	u32 command_base, response_base;
	unsigned int resource, part, zero;
	int result;

	if (!device->started)
		return -EPIPE;
	if (load->dsp_index >= UAD2_DSP_COUNT || load->flags ||
	    load->image_bytes != UAD2_PROGRAM_IMAGE_BYTES ||
	    !load->image_pointer || program->loaded)
		return -EINVAL;
	command_base = uad2_dsp_banks[load->dsp_index];
	response_base = command_base + 0x40;
	if (uad2_read(device, command_base + 0x28) ||
	    uad2_read(device, command_base + 0x24) ||
	    uad2_read(device, response_base + 0x28) ||
	    uad2_read(device, response_base + 0x24))
		return -EBUSY;

	program->cpu = dma_alloc_coherent(&device->pdev->dev,
		UAD2_PROGRAM_WORKSPACE_BYTES, &program->dma, GFP_KERNEL);
	if (!program->cpu)
		return -ENOMEM;
	memset(program->cpu, 0, UAD2_PROGRAM_WORKSPACE_BYTES);
	if (copy_from_user(program->cpu, u64_to_user_ptr(load->image_pointer),
			   UAD2_PROGRAM_IMAGE_BYTES))
		return uad2_recover_failed_program(device, -EFAULT);
	if (!uad2_validate_program_image(program->cpu))
		return uad2_recover_failed_program(device, -EINVAL);

	for (resource = 0; resource < ARRAY_SIZE(uad2_realverb_resources);
	     resource++) {
		const struct uad2_resource_definition *definition =
			&uad2_realverb_resources[resource];
		u32 *response = program->cpu +
			(UAD2_PROGRAM_RESPONSE_PAGE + resource) * UAD2_RING_PAGE_BYTES;
		u32 *descriptor = uad2_ring_entry(device, load->dsp_index, 1,
			response_position);
		dma_addr_t response_dma = program->dma +
			(UAD2_PROGRAM_RESPONSE_PAGE + resource) * UAD2_RING_PAGE_BYTES;
		u32 expected_command = (command_position + definition->chunk_count) %
			UAD2_RING_ENTRIES;
		u32 expected_response = (response_position + 1) % UAD2_RING_ENTRIES;

		memset(response, 0xa5, UAD2_RING_PAGE_BYTES);
		descriptor[0] = 0x80000004;
		descriptor[1] = 0;
		descriptor[2] = lower_32_bits(response_dma);
		descriptor[3] = upper_32_bits(response_dma);
		for (part = 0; part < definition->chunk_count; part++) {
			u32 *command = uad2_ring_entry(device, load->dsp_index, 0,
				(command_position + part) % UAD2_RING_ENTRIES);
			dma_addr_t command_dma = program->dma +
				(definition->first_chunk + part) * UAD2_RING_PAGE_BYTES;

			command[0] = 0x80000000 | definition->chunk_bytes[part] / 4;
			command[1] = 0;
			command[2] = lower_32_bits(command_dma);
			command[3] = upper_32_bits(command_dma);
		}
		dma_wmb();
		uad2_write(device, response_base + 0x24, expected_response);
		uad2_write(device, response_base + 0x20, expected_response);
		uad2_write(device, UAD2_INTERRUPT_ENABLE, UAD2_INTERRUPT_SHADOW);
		uad2_write(device, command_base + 0x24, expected_command);
		uad2_write(device, command_base + 0x20, expected_command);
		uad2_write(device, UAD2_INTERRUPT_ENABLE, UAD2_INTERRUPT_SHADOW);
		result = uad2_wait_index(device, command_base + 0x28,
			expected_command);
		if (!result)
			result = uad2_wait_index(device, response_base + 0x28,
				expected_response);
		dma_rmb();
		if (result || response[0] != UAD2_BILL_RESPONSE || response[1] ||
		    response[2] != definition->id ||
		    response[3] != definition->command ||
		    !uad2_word_canary(response, 4, 0xa5a5a5a5))
			return uad2_recover_failed_program(device,
				result ? result : -EKEYREJECTED);
		command_position = expected_command;
		response_position = expected_response;
	}

	for (zero = 0; zero < ARRAY_SIZE(uad2_zero_commands); zero++) {
		u32 expected = (command_position + 1) % UAD2_RING_ENTRIES;

		memcpy(uad2_ring_entry(device, load->dsp_index, 0,
			command_position), uad2_zero_commands[zero],
			sizeof(uad2_zero_commands[zero]));
		dma_wmb();
		uad2_write(device, command_base + 0x24, expected);
		uad2_write(device, command_base + 0x20, expected);
		uad2_write(device, UAD2_INTERRUPT_ENABLE, UAD2_INTERRUPT_SHADOW);
		result = uad2_wait_index(device, command_base + 0x28, expected);
		if (result)
			return uad2_recover_failed_program(device, result);
		command_position = expected;
	}
	{
		u32 *descriptor = uad2_ring_entry(device, load->dsp_index, 0,
			command_position);
		dma_addr_t memspec_dma = program->dma +
			UAD2_PROGRAM_MEMSPEC_PAGE * UAD2_RING_PAGE_BYTES;
		u32 expected = (command_position + 1) % UAD2_RING_ENTRIES;

		descriptor[0] = 0x80000041;
		descriptor[1] = 0;
		descriptor[2] = lower_32_bits(memspec_dma);
		descriptor[3] = upper_32_bits(memspec_dma);
		dma_wmb();
		uad2_write(device, command_base + 0x24, expected);
		uad2_write(device, command_base + 0x20, expected);
		uad2_write(device, UAD2_INTERRUPT_ENABLE, UAD2_INTERRUPT_SHADOW);
		result = uad2_wait_index(device, command_base + 0x28, expected);
		if (result)
			return uad2_recover_failed_program(device, result);
		command_position = expected;
	}
	if (uad2_read(device, uad2_property_7_offsets[load->dsp_index]) !=
	    UAD2_PROCESS_PROPERTY_7)
		return uad2_recover_failed_program(device, -EPROTO);

	program->id = ++device->next_object_id;
	program->dsp = load->dsp_index;
	program->command_position = command_position;
	program->response_position = response_position;
	program->next_request_id = 1;
	program->loaded = true;
	load->program_id = program->id;
	return 0;
}

static struct uad2_user_buffer *uad2_find_buffer(
	struct uad2_compute_device *device, u64 id)
{
	unsigned int index;

	for (index = 0; index < UAD2_MAX_BUFFERS; index++)
		if (device->buffers[index].cpu && device->buffers[index].id == id)
			return &device->buffers[index];
	return NULL;
}

static void uad2_free_all_buffers(struct uad2_compute_device *device)
{
	unsigned int index;

	for (index = 0; index < UAD2_MAX_BUFFERS; index++) {
		struct uad2_user_buffer *buffer = &device->buffers[index];

		if (!buffer->cpu)
			continue;
		dma_free_coherent(&device->pdev->dev, PAGE_ALIGN(buffer->bytes),
			buffer->cpu, buffer->dma);
		memset(buffer, 0, sizeof(*buffer));
	}
}

static int uad2_alloc_buffer_locked(struct uad2_compute_device *device,
				    struct uad2_compute_buffer_alloc *allocation)
{
	struct uad2_user_buffer *buffer = NULL;
	unsigned int index;

	if (!allocation->bytes || allocation->bytes > UAD2_MAX_BUFFER_BYTES ||
	    (allocation->flags & ~(UAD2_BUFFER_INPUT | UAD2_BUFFER_OUTPUT)) ||
	    !(allocation->flags & (UAD2_BUFFER_INPUT | UAD2_BUFFER_OUTPUT)))
		return -EINVAL;
	for (index = 0; index < UAD2_MAX_BUFFERS; index++)
		if (!device->buffers[index].cpu) {
			buffer = &device->buffers[index];
			break;
		}
	if (!buffer)
		return -ENOSPC;
	buffer->bytes = allocation->bytes;
	buffer->flags = allocation->flags;
	buffer->cpu = dma_alloc_coherent(&device->pdev->dev,
		PAGE_ALIGN(buffer->bytes), &buffer->dma, GFP_KERNEL);
	if (!buffer->cpu) {
		memset(buffer, 0, sizeof(*buffer));
		return -ENOMEM;
	}
	memset(buffer->cpu, 0, PAGE_ALIGN(buffer->bytes));
	buffer->id = ++device->next_object_id;
	atomic_set(&buffer->map_count, 0);
	allocation->buffer_id = buffer->id;
	allocation->mmap_offset = buffer->id * UAD2_RING_PAGE_BYTES;
	return 0;
}

static int uad2_free_buffer_locked(struct uad2_compute_device *device,
				   const struct uad2_compute_buffer_free *release)
{
	struct uad2_user_buffer *buffer;

	if (release->flags)
		return -EINVAL;
	buffer = uad2_find_buffer(device, release->buffer_id);
	if (!buffer)
		return -ENOENT;
	if (atomic_read(&buffer->map_count))
		return -EBUSY;
	dma_free_coherent(&device->pdev->dev, PAGE_ALIGN(buffer->bytes),
		buffer->cpu, buffer->dma);
	memset(buffer, 0, sizeof(*buffer));
	return 0;
}

static int uad2_submit_job_locked(struct uad2_compute_device *device,
				  struct uad2_compute_job_submit *submit)
{
	struct uad2_loaded_program *program = &device->program;
	struct uad2_user_buffer *input, *output;
	u32 command_base, response_base, expected_command, expected_response;
	u32 before_indices[UAD2_DSP_COUNT][2];
	u32 request_id;
	unsigned int channel, dsp;
	int result = 0;

	if (!device->started || !program->loaded)
		return -EPIPE;
	if (submit->flags || submit->dsp_index != program->dsp ||
	    submit->program_id != program->id)
		return -EINVAL;
	input = uad2_find_buffer(device, submit->input_buffer_id);
	output = uad2_find_buffer(device, submit->output_buffer_id);
	if (!input || !output || input->bytes < UAD2_COMPUTE_FRAME_BYTES ||
	    output->bytes < UAD2_COMPUTE_FRAME_BYTES ||
	    !(input->flags & UAD2_BUFFER_INPUT) ||
	    !(output->flags & UAD2_BUFFER_OUTPUT))
		return -EINVAL;
	command_base = uad2_dsp_banks[program->dsp];
	response_base = command_base + 0x40;
	request_id = program->next_request_id++;
	if (!request_id)
		request_id = program->next_request_id++;
	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++) {
		before_indices[dsp][0] = uad2_read(device,
			uad2_dsp_banks[dsp] + 0x28);
		before_indices[dsp][1] = uad2_read(device,
			uad2_dsp_banks[dsp] + 0x40 + 0x28);
	}
	for (channel = 0; channel < UAD2_COMPUTE_CHANNELS; channel++) {
		u32 *input_page = program->cpu +
			(UAD2_PROGRAM_INPUT_PAGE + channel) * UAD2_RING_PAGE_BYTES;
		u32 *output_page = program->cpu +
			(UAD2_PROGRAM_OUTPUT_PAGE + channel) * UAD2_RING_PAGE_BYTES;
		u32 *command = uad2_ring_entry(device, program->dsp, 0,
			(program->command_position + channel) % UAD2_RING_ENTRIES);
		u32 *response = uad2_ring_entry(device, program->dsp, 1,
			(program->response_position + channel) % UAD2_RING_ENTRIES);
		dma_addr_t input_dma = program->dma +
			(UAD2_PROGRAM_INPUT_PAGE + channel) * UAD2_RING_PAGE_BYTES;
		dma_addr_t output_dma = program->dma +
			(UAD2_PROGRAM_OUTPUT_PAGE + channel) * UAD2_RING_PAGE_BYTES;

		memset(input_page, 0, UAD2_RING_PAGE_BYTES);
		input_page[0] = 0x00070042;
		input_page[1] = UAD2_PROCESS_PROPERTY_7 + channel * 0x40;
		memcpy(input_page + 2,
			input->cpu + channel * UAD2_COMPUTE_TICK_SAMPLES * sizeof(u32),
			UAD2_COMPUTE_TICK_SAMPLES * sizeof(u32));
		memset(output_page, 0xa5, UAD2_RING_PAGE_BYTES);
		memset(output_page, 0xcc, 4 * sizeof(u32));
		output_page[UAD2_PROCESS_OUTPUT_DWORDS - 1] = 0xffffdead;
		command[0] = 0x80000000 | UAD2_PROCESS_INPUT_DWORDS;
		command[1] = 0;
		command[2] = lower_32_bits(input_dma);
		command[3] = upper_32_bits(input_dma);
		response[0] = 0x80000000 | UAD2_PROCESS_OUTPUT_DWORDS;
		response[1] = 0;
		response[2] = lower_32_bits(output_dma);
		response[3] = upper_32_bits(output_dma);
	}
	{
		u32 *command = uad2_ring_entry(device, program->dsp, 0,
			(program->command_position + UAD2_COMPUTE_CHANNELS) %
			UAD2_RING_ENTRIES);

		command[0] = UAD2_PROCESS_COMMAND;
		command[1] = UAD2_PROCESS_FLAGS;
		command[2] = request_id;
		command[3] = UAD2_PROCESS_ADDRESS;
	}
	expected_command = (program->command_position +
		UAD2_COMPUTE_CHANNELS + 1) % UAD2_RING_ENTRIES;
	expected_response = (program->response_position +
		UAD2_COMPUTE_CHANNELS) % UAD2_RING_ENTRIES;
	dma_wmb();
	uad2_write(device, response_base + 0x24, expected_response);
	uad2_write(device, response_base + 0x20, expected_response);
	uad2_write(device, UAD2_INTERRUPT_ENABLE, UAD2_INTERRUPT_SHADOW);
	uad2_write(device, command_base + 0x24, expected_command);
	uad2_write(device, command_base + 0x20, expected_command);
	uad2_write(device, UAD2_INTERRUPT_ENABLE, UAD2_INTERRUPT_SHADOW);
	result = uad2_wait_index(device, command_base + 0x28, expected_command);
	if (!result)
		result = uad2_wait_index(device, response_base + 0x28,
			expected_response);
	dma_rmb();
	for (channel = 0; !result && channel < UAD2_COMPUTE_CHANNELS; channel++) {
		const u32 *input_page = program->cpu +
			(UAD2_PROGRAM_INPUT_PAGE + channel) * UAD2_RING_PAGE_BYTES;
		const u32 *output_page = program->cpu +
			(UAD2_PROGRAM_OUTPUT_PAGE + channel) * UAD2_RING_PAGE_BYTES;

		if (output_page[0] != UAD2_PROCESS_RESPONSE ||
		    output_page[1] != request_id || output_page[2] != channel ||
		    output_page[3] != UAD2_PROCESS_MARKER ||
		    input_page[0] != 0x00070042 ||
		    input_page[1] != UAD2_PROCESS_PROPERTY_7 + channel * 0x40 ||
		    memcmp(input_page + 2,
			   input->cpu + channel * UAD2_COMPUTE_TICK_SAMPLES * sizeof(u32),
			   UAD2_COMPUTE_TICK_SAMPLES * sizeof(u32)) ||
		    !uad2_zero_padding((const u8 *)input_page,
			UAD2_PROCESS_INPUT_DWORDS * sizeof(u32)) ||
		    !uad2_word_canary(output_page, UAD2_PROCESS_OUTPUT_DWORDS,
			0xa5a5a5a5))
			result = -EPROTO;
		else
			memcpy(output->cpu +
				channel * UAD2_COMPUTE_TICK_SAMPLES * sizeof(u32),
			       output_page + 4,
			       UAD2_COMPUTE_TICK_SAMPLES * sizeof(u32));
	}
	for (dsp = 0; !result && dsp < UAD2_DSP_COUNT; dsp++) {
		if (dsp == program->dsp)
			continue;
		if (uad2_read(device, uad2_dsp_banks[dsp] + 0x28) !=
		    before_indices[dsp][0] ||
		    uad2_read(device, uad2_dsp_banks[dsp] + 0x40 + 0x28) !=
		    before_indices[dsp][1])
			result = -EIO;
	}
	program->command_position = expected_command;
	program->response_position = expected_response;
	device->completed_job.id = ++device->next_object_id;
	device->completed_job.status = result;
	device->completed_job.request_id = request_id;
	device->completed_job.response_marker = UAD2_PROCESS_MARKER;
	device->completed_job.valid = true;
	submit->request_id = request_id;
	submit->job_id = device->completed_job.id;
	submit->status = result;
	submit->response_marker = UAD2_PROCESS_MARKER;
	if (result)
		return uad2_recover_failed_program(device, result);
	return 0;
}

static long uad2_ioctl(struct file *file, unsigned int command,
		       unsigned long argument)
{
	struct uad2_compute_device *device = file->private_data;
	void __user *pointer = (void __user *)argument;
	long result = 0;

	if (_IOC_TYPE(command) != UAD2_COMPUTE_IOC_MAGIC)
		return -ENOTTY;
	mutex_lock(&device->lock);
	if (device->removing) {
		result = -ENODEV;
		goto unlock;
	}

	switch (command) {
	case UAD2_COMPUTE_IOC_GET_INFO: {
		struct uad2_compute_info info = {
			.size = sizeof(info),
			.abi_version = UAD2_COMPUTE_ABI_VERSION,
			.vendor_id = device->pdev->vendor,
			.device_id = device->pdev->device,
			.subsystem_vendor_id = device->pdev->subsystem_vendor,
			.subsystem_device_id = device->pdev->subsystem_device,
			.fpga_revision = uad2_read(device, UAD2_FPGA_REVISION),
			.extended_capabilities =
				uad2_read(device, UAD2_EXTENDED_CAPABILITIES),
			.dsp_count = UAD2_DSP_COUNT,
			.transport_started = device->started,
			.capabilities = UAD2_CAP_RING_TRANSPORT |
					UAD2_CAP_PER_DSP_RESET |
					UAD2_CAP_PROGRAM_LOAD |
					UAD2_CAP_DMA_BUFFERS |
					UAD2_CAP_JOB_COMPLETION |
					UAD2_CAP_PROGRAM_ISOLATION,
		};

		if (copy_to_user(pointer, &info, sizeof(info)))
			result = -EFAULT;
		break;
	}
	case UAD2_COMPUTE_IOC_GET_DSP_STATUS: {
		struct uad2_compute_dsp_status status;
		u32 command_base, response_base, dma_control;

		if (copy_from_user(&status, pointer, sizeof(status))) {
			result = -EFAULT;
			break;
		}
		if (status.size != sizeof(status) ||
		    status.dsp_index >= UAD2_DSP_COUNT) {
			result = -EINVAL;
			break;
		}
		command_base = uad2_dsp_banks[status.dsp_index];
		response_base = command_base + 0x40;
		dma_control = uad2_read(device, UAD2_DMA_CONTROL);
		status.raw_ready_word = uad2_read(device,
			uad2_ready_offsets[status.dsp_index]);
		status.ready = !!(status.raw_ready_word & 1);
		status.dma_enabled = !!(dma_control &
			(1U << (status.dsp_index + 1)));
		status.command_read_index = uad2_read(device,
			command_base + 0x28);
		status.command_write_index = uad2_read(device,
			command_base + 0x24);
		status.response_read_index = uad2_read(device,
			response_base + 0x28);
		status.response_write_index = uad2_read(device,
			response_base + 0x24);
		memset(status.reserved, 0, sizeof(status.reserved));
		if (copy_to_user(pointer, &status, sizeof(status)))
			result = -EFAULT;
		break;
	}
	case UAD2_COMPUTE_IOC_START_TRANSPORT:
		if (!capable(CAP_SYS_RAWIO))
			result = -EPERM;
		else
			result = uad2_start_locked(device);
		break;
	case UAD2_COMPUTE_IOC_STOP_TRANSPORT:
		if (!capable(CAP_SYS_RAWIO))
			result = -EPERM;
		else
			result = uad2_stop_locked(device);
		break;
	case UAD2_COMPUTE_IOC_RESET_DSP: {
		struct uad2_compute_reset reset;

		if (!capable(CAP_SYS_RAWIO)) {
			result = -EPERM;
			break;
		}
		if (copy_from_user(&reset, pointer, sizeof(reset))) {
			result = -EFAULT;
			break;
		}
		if (reset.size != sizeof(reset)) {
			result = -EINVAL;
			break;
		}
		memset(reset.reserved, 0, sizeof(reset.reserved));
		result = uad2_reset_dsp_locked(device, &reset);
		if (!result && copy_to_user(pointer, &reset, sizeof(reset)))
			result = -EFAULT;
		break;
	}
	case UAD2_COMPUTE_IOC_ALLOC_BUFFER: {
		struct uad2_compute_buffer_alloc allocation;

		if (!capable(CAP_SYS_RAWIO)) {
			result = -EPERM;
			break;
		}
		if (copy_from_user(&allocation, pointer, sizeof(allocation))) {
			result = -EFAULT;
			break;
		}
		if (allocation.size != sizeof(allocation)) {
			result = -EINVAL;
			break;
		}
		memset(allocation.reserved, 0, sizeof(allocation.reserved));
		allocation.reserved0 = 0;
		result = uad2_alloc_buffer_locked(device, &allocation);
		if (!result && copy_to_user(pointer, &allocation, sizeof(allocation)))
			result = -EFAULT;
		break;
	}
	case UAD2_COMPUTE_IOC_FREE_BUFFER: {
		struct uad2_compute_buffer_free release;

		if (!capable(CAP_SYS_RAWIO)) {
			result = -EPERM;
			break;
		}
		if (copy_from_user(&release, pointer, sizeof(release))) {
			result = -EFAULT;
			break;
		}
		if (release.size != sizeof(release)) {
			result = -EINVAL;
			break;
		}
		result = uad2_free_buffer_locked(device, &release);
		break;
	}
	case UAD2_COMPUTE_IOC_LOAD_PROGRAM: {
		struct uad2_compute_program_load load;

		if (!capable(CAP_SYS_RAWIO)) {
			result = -EPERM;
			break;
		}
		if (copy_from_user(&load, pointer, sizeof(load))) {
			result = -EFAULT;
			break;
		}
		if (load.size != sizeof(load)) {
			result = -EINVAL;
			break;
		}
		memset(load.reserved, 0, sizeof(load.reserved));
		load.reserved0 = 0;
		result = uad2_load_program_locked(device, &load);
		if (!result && copy_to_user(pointer, &load, sizeof(load)))
			result = -EFAULT;
		break;
	}
	case UAD2_COMPUTE_IOC_SUBMIT_JOB: {
		struct uad2_compute_job_submit submit;

		if (!capable(CAP_SYS_RAWIO)) {
			result = -EPERM;
			break;
		}
		if (copy_from_user(&submit, pointer, sizeof(submit))) {
			result = -EFAULT;
			break;
		}
		if (submit.size != sizeof(submit)) {
			result = -EINVAL;
			break;
		}
		memset(submit.reserved, 0, sizeof(submit.reserved));
		result = uad2_submit_job_locked(device, &submit);
		if (copy_to_user(pointer, &submit, sizeof(submit)))
			result = -EFAULT;
		break;
	}
	case UAD2_COMPUTE_IOC_WAIT_JOB: {
		struct uad2_compute_job_wait wait;

		if (copy_from_user(&wait, pointer, sizeof(wait))) {
			result = -EFAULT;
			break;
		}
		if (wait.size != sizeof(wait) || wait.timeout_ms < 0 ||
		    !device->completed_job.valid ||
		    wait.job_id != device->completed_job.id) {
			result = -EINVAL;
			break;
		}
		wait.status = device->completed_job.status;
		wait.completed = 1;
		wait.request_id = device->completed_job.request_id;
		wait.response_marker = device->completed_job.response_marker;
		memset(wait.reserved, 0, sizeof(wait.reserved));
		if (copy_to_user(pointer, &wait, sizeof(wait)))
			result = -EFAULT;
		break;
	}
	default:
		result = -ENOTTY;
	}

unlock:
	mutex_unlock(&device->lock);
	return result;
}

static void uad2_release_device(struct kref *refcount)
{
	struct uad2_compute_device *device = container_of(refcount,
		struct uad2_compute_device, refcount);

	kfree(device->misc.name);
	kfree(device);
}

static int uad2_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct uad2_compute_device *device = container_of(misc,
		struct uad2_compute_device, misc);
	int result = 0;

	mutex_lock(&device->lock);
	if (device->removing || !kref_get_unless_zero(&device->refcount))
		result = -ENODEV;
	else if (device->opened) {
		kref_put(&device->refcount, uad2_release_device);
		result = -EBUSY;
	}
	else {
		device->opened = true;
		device->mapping = file->f_mapping;
		file->private_data = device;
	}
	mutex_unlock(&device->lock);
	if (result)
		return result;
	return nonseekable_open(inode, file);
}

static int uad2_release(struct inode *inode, struct file *file)
{
	struct uad2_compute_device *device = file->private_data;

	(void)inode;
	mutex_lock(&device->lock);
	uad2_stop_locked(device);
	uad2_free_program(device);
	uad2_free_all_buffers(device);
	device->opened = false;
	device->mapping = NULL;
	mutex_unlock(&device->lock);
	kref_put(&device->refcount, uad2_release_device);
	return 0;
}

static void uad2_vma_open(struct vm_area_struct *vma)
{
	struct uad2_user_buffer *buffer = vma->vm_private_data;

	atomic_inc(&buffer->map_count);
}

static void uad2_vma_close(struct vm_area_struct *vma)
{
	struct uad2_user_buffer *buffer = vma->vm_private_data;

	atomic_dec_if_positive(&buffer->map_count);
}

static const struct vm_operations_struct uad2_vm_ops = {
	.open = uad2_vma_open,
	.close = uad2_vma_close,
};

static int uad2_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct uad2_compute_device *device = file->private_data;
	struct uad2_user_buffer *buffer;
	u64 id = vma->vm_pgoff;
	size_t requested = vma->vm_end - vma->vm_start;
	int result;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	mutex_lock(&device->lock);
	buffer = uad2_find_buffer(device, id);
	if (!buffer || requested != PAGE_ALIGN(buffer->bytes)) {
		result = -EINVAL;
		goto unlock;
	}
	vma->vm_pgoff = 0;
	result = dma_mmap_coherent(&device->pdev->dev, vma, buffer->cpu,
		buffer->dma, PAGE_ALIGN(buffer->bytes));
	if (!result) {
		vma->vm_ops = &uad2_vm_ops;
		vma->vm_private_data = buffer;
		uad2_vma_open(vma);
	}
unlock:
	mutex_unlock(&device->lock);
	return result;
}

static const struct file_operations uad2_fops = {
	.owner = THIS_MODULE,
	.open = uad2_open,
	.release = uad2_release,
	.unlocked_ioctl = uad2_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = uad2_ioctl,
#endif
	.mmap = uad2_mmap,
	.llseek = noop_llseek,
};

static void uad2_free_pages(struct uad2_compute_device *device)
{
	unsigned int dsp, ring, page;

	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++)
		for (ring = 0; ring < UAD2_RING_COUNT; ring++)
			for (page = 0; page < UAD2_RING_PAGES; page++) {
				struct uad2_ring_page *ring_page =
					&device->pages[dsp][ring][page];

				if (!ring_page->cpu)
					continue;
				dma_free_coherent(&device->pdev->dev,
					UAD2_RING_PAGE_BYTES, ring_page->cpu,
					ring_page->dma);
				ring_page->cpu = NULL;
			}
}

static int uad2_allocate_pages(struct uad2_compute_device *device)
{
	unsigned int dsp, ring, page;

	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++)
		for (ring = 0; ring < UAD2_RING_COUNT; ring++)
			for (page = 0; page < UAD2_RING_PAGES; page++) {
				struct uad2_ring_page *ring_page =
					&device->pages[dsp][ring][page];

				ring_page->cpu = dma_alloc_coherent(&device->pdev->dev,
					UAD2_RING_PAGE_BYTES, &ring_page->dma,
					GFP_KERNEL);
				if (!ring_page->cpu)
					return -ENOMEM;
				memset(ring_page->cpu, 0, UAD2_RING_PAGE_BYTES);
			}
	return 0;
}

static int uad2_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct uad2_compute_device *device;
	int result;

	(void)id;
	if (PAGE_SIZE != UAD2_RING_PAGE_BYTES)
		return dev_err_probe(&pdev->dev, -EOPNOTSUPP,
			"requires 4096-byte host pages\n");
	if (pci_resource_len(pdev, 0) != UAD2_BAR0_SIZE)
		return dev_err_probe(&pdev->dev, -EINVAL,
			"refusing BAR0 length %pa, expected 0x10000\n",
			&pdev->resource[0].end);

	device = kzalloc(sizeof(*device), GFP_KERNEL);
	if (!device)
		return -ENOMEM;
	device->pdev = pdev;
	device->index = atomic_fetch_inc(&uad2_next_index);
	mutex_init(&device->lock);
	kref_init(&device->refcount);
	pci_set_drvdata(pdev, device);

	result = pci_enable_device_mem(pdev);
	if (result)
		goto put_device;
	result = pci_request_region(pdev, 0, "uad2_compute");
	if (result)
		goto disable_device;
	result = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (result)
		goto release_region;
	device->bar = pci_iomap(pdev, 0, UAD2_BAR0_SIZE);
	if (!device->bar) {
		result = -ENOMEM;
		goto release_region;
	}
	if (!uad2_cold_precondition(device)) {
		result = dev_err_probe(&pdev->dev, -EBUSY,
			"cold-state precondition failed\n");
		goto unmap_bar;
	}
	result = uad2_allocate_pages(device);
	if (result)
		goto free_pages;

	device->misc.minor = MISC_DYNAMIC_MINOR;
	device->misc.name = kasprintf(GFP_KERNEL, "uad2_compute%u",
		device->index);
	device->misc.fops = &uad2_fops;
	device->misc.parent = &pdev->dev;
	device->misc.mode = 0600;
	if (!device->misc.name) {
		result = -ENOMEM;
		goto free_pages;
	}
	result = misc_register(&device->misc);
	if (result)
		goto free_pages;

	dev_info(&pdev->dev,
		 "registered /dev/%s with 64 coherent ring pages; transport stopped\n",
		 device->misc.name);
	return 0;

free_pages:
	uad2_free_pages(device);
unmap_bar:
	pci_iounmap(pdev, device->bar);
release_region:
	pci_release_region(pdev, 0);
disable_device:
	pci_disable_device(pdev);
put_device:
	pci_set_drvdata(pdev, NULL);
	kref_put(&device->refcount, uad2_release_device);
	return result;
}

static void uad2_remove(struct pci_dev *pdev)
{
	struct uad2_compute_device *device = pci_get_drvdata(pdev);
	int stop_result;

	misc_deregister(&device->misc);
	mutex_lock(&device->lock);
	device->removing = true;
	if (device->mapping)
		unmap_mapping_range(device->mapping, 0, 0, 1);
	stop_result = uad2_stop_locked(device);
	if (stop_result)
		dev_err(&pdev->dev, "transport recovery failed during removal\n");
	uad2_free_program(device);
	uad2_free_all_buffers(device);
	uad2_free_pages(device);
	pci_iounmap(pdev, device->bar);
	pci_release_region(pdev, 0);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	mutex_unlock(&device->lock);
	kref_put(&device->refcount, uad2_release_device);
}

static const struct pci_device_id uad2_ids[] = {
	{
		PCI_DEVICE_SUB(UAD2_VENDOR_ID, UAD2_DEVICE_ID,
			       UAD2_SUBVENDOR_ID, UAD2_SUBDEVICE_OCTO)
	},
	{ }
};
MODULE_DEVICE_TABLE(pci, uad2_ids);

static struct pci_driver uad2_driver = {
	.name = "uad2_compute",
	.id_table = uad2_ids,
	.probe = uad2_probe,
	.remove = uad2_remove,
};
module_pci_driver(uad2_driver);

MODULE_AUTHOR("UAD-2 OCTO reverse-engineering project");
MODULE_DESCRIPTION("Experimental bounded UAD-2 OCTO ring transport");
MODULE_LICENSE("GPL");
