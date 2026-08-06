#define _GNU_SOURCE
// SPDX-License-Identifier: GPL-2.0-only
/* Experiment 012: read-only snapshot of startup and optional-audio registers. */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/vfio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GROUP_PATH "/dev/vfio/16"
#define DEVICE_NAME "0000:03:00.0"
#define EXPECTED_BAR0_SIZE UINT64_C(0x10000)

struct named_register {
	uint32_t offset;
	const char *name;
};

static const struct named_register registers[] = {
	{ 0x0030, "firmware_base_low" },
	{ 0x0034, "firmware_base_high" },
	{ 0x01a0, "dsp0_property_7" },
	{ 0x09a0, "dsp1_property_7" },
	{ 0x11a0, "dsp2_property_7" },
	{ 0x19a0, "dsp3_property_7" },
	{ 0x41a0, "dsp4_property_7" },
	{ 0x49a0, "dsp5_property_7" },
	{ 0x51a0, "dsp6_property_7" },
	{ 0x59a0, "dsp7_property_7" },
	{ 0x2200, "dma_control" },
	{ 0x2204, "interrupt_enable_low" },
	{ 0x2218, "fpga_revision" },
	{ 0x2220, "notification_control" },
	{ 0x2234, "extended_capabilities" },
	{ 0x2240, "audio_buffer_frames_minus_one" },
	{ 0x2244, "audio_dma_position" },
	{ 0x2248, "audio_transport_control" },
	{ 0x2250, "audio_playback_channels" },
	{ 0x2258, "audio_irq_period" },
	{ 0x225c, "audio_record_channels" },
	{ 0x2260, "audio_stream_enable" },
	{ 0x2268, "interrupt_enable_high" },
	{ 0x226c, "audio_buffer_size_kib" },
	{ 0x2270, "audio_periodic_timer" },
	{ 0x22c0, "unknown_22c0" },
	{ 0x22c4, "unknown_22c4" },
};

static void die(const char *message)
{
	fprintf(stderr, "vfio capability snapshot: %s: %s\n", message,
		strerror(errno));
	exit(EXIT_FAILURE);
}

static uint32_t mmio_read32(const void *bar, uint32_t offset)
{
	const volatile uint32_t *word =
		(const volatile uint32_t *)((const char *)bar + offset);
	uint32_t value = *word;

	__sync_synchronize();
	return value;
}

int main(void)
{
	struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
	struct vfio_region_info region = {
		.argsz = sizeof(region),
		.index = VFIO_PCI_BAR0_REGION_INDEX,
	};
	uint32_t values[sizeof(registers) / sizeof(registers[0])];
	unsigned int sg_zero_words = 0;
	int container = -1, group = -1, device = -1;
	void *bar = MAP_FAILED;
	int container_set = 0;
	unsigned int i;

	container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (container < 0)
		die("open /dev/vfio/vfio");
	if (ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
		errno = EPROTO;
		die("VFIO API version mismatch");
	}
	if (ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1) {
		errno = ENOTSUP;
		die("VFIO Type 1 IOMMU unavailable");
	}

	group = open(GROUP_PATH, O_RDWR | O_CLOEXEC);
	if (group < 0)
		die("open isolated IOMMU group 16");
	if (ioctl(group, VFIO_GROUP_GET_STATUS, &group_status) < 0)
		die("VFIO_GROUP_GET_STATUS");
	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		errno = EBUSY;
		die("IOMMU group is not viable");
	}
	if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0)
		die("VFIO_GROUP_SET_CONTAINER");
	container_set = 1;
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0)
		die("VFIO_SET_IOMMU");

	device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, DEVICE_NAME);
	if (device < 0)
		die("VFIO_GROUP_GET_DEVICE_FD");
	if (ioctl(device, VFIO_DEVICE_GET_INFO, &device_info) < 0)
		die("VFIO_DEVICE_GET_INFO");
	if (ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &region) < 0)
		die("VFIO_DEVICE_GET_REGION_INFO BAR0");
	if (region.size != EXPECTED_BAR0_SIZE ||
	    !(region.flags & VFIO_REGION_INFO_FLAG_READ) ||
	    !(region.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
		errno = EINVAL;
		die("BAR0 does not match snapshot contract");
	}

	bar = mmap(NULL, region.size, PROT_READ, MAP_SHARED, device, region.offset);
	if (bar == MAP_FAILED)
		die("mmap BAR0 read-only");

	for (i = 0; i < sizeof(registers) / sizeof(registers[0]); i++)
		values[i] = mmio_read32(bar, registers[i].offset);
	for (i = 0x8000; i < 0xc000; i += 4) {
		if (mmio_read32(bar, i) == 0)
			sg_zero_words++;
	}

	printf("{\n");
	printf("  \"experiment\": \"012-capability-and-audio-path-snapshot\",\n");
	printf("  \"transport\": \"vfio-pci\",\n");
	printf("  \"dma_mappings\": 0,\n");
	printf("  \"mmio_writes\": 0,\n");
	printf("  \"registers\": {\n");
	for (i = 0; i < sizeof(registers) / sizeof(registers[0]); i++) {
		printf("    \"%s\": {\"offset\": \"0x%04x\", \"value\": \"0x%08x\"}%s\n",
		       registers[i].name, registers[i].offset, values[i],
		       i + 1 == sizeof(registers) / sizeof(registers[0]) ? "" : ",");
	}
	printf("  },\n");
	printf("  \"shared_sg_table_words_read\": 4096,\n");
	printf("  \"shared_sg_table_zero_words\": %u,\n", sg_zero_words);
	printf("  \"shared_sg_tables_all_zero\": %s\n",
	       sg_zero_words == 4096 ? "true" : "false");
	printf("}\n");

	munmap(bar, region.size);
	close(device);
	if (container_set)
		ioctl(group, VFIO_GROUP_UNSET_CONTAINER);
	close(group);
	close(container);
	return EXIT_SUCCESS;
}
