#define _GNU_SOURCE
// SPDX-License-Identifier: GPL-2.0-only
/* Read-only snapshot of the four DSP resource-pool layouts. */

#include <errno.h>
#include <fcntl.h>
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
#define DSP_COUNT 8

struct resource_layout {
	uint32_t base[4];
	uint32_t size[4];
	uint32_t scratch[4];
};

static const uint32_t base_offsets[4] = { 0x10, 0x18, 0x14, 0x1c };
static const uint32_t size_offsets[4] = { 0x184, 0x18c, 0x188, 0x190 };
static const uint32_t scratch_offsets[4] = { 0, 0x198, 0x194, 0x19c };

static void die(const char *message)
{
	fprintf(stderr, "vfio resource layout: %s: %s\n", message,
		strerror(errno));
	exit(EXIT_FAILURE);
}

static uint32_t dsp_base(unsigned int dsp)
{
	return (dsp > 3 ? 0x2000 : 0) + dsp * 0x800;
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
	struct resource_layout layouts[DSP_COUNT] = { 0 };
	int container = -1, group = -1, device = -1;
	int container_set = 0;
	void *bar = MAP_FAILED;
	unsigned int dsp, pool;

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

	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		for (pool = 0; pool < 4; pool++) {
			layouts[dsp].base[pool] =
				mmio_read32(bar, dsp_base(dsp) + base_offsets[pool]);
			layouts[dsp].size[pool] =
				mmio_read32(bar, dsp_base(dsp) + size_offsets[pool]);
			layouts[dsp].scratch[pool] = scratch_offsets[pool] ?
				mmio_read32(bar, dsp_base(dsp) + scratch_offsets[pool]) : 0;
		}
	}

	printf("{\n");
	printf("  \"experiment\": \"020-resource-layout-snapshot\",\n");
	printf("  \"transport\": \"vfio-pci\",\n");
	printf("  \"dma_mappings\": 0,\n");
	printf("  \"mmio_writes\": 0,\n");
	printf("  \"dsps\": [\n");
	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		printf("    {\"dsp\": %u, \"register_base\": \"0x%04x\", \"pools\": [\n",
		       dsp, dsp_base(dsp));
		for (pool = 0; pool < 4; pool++) {
			printf("      {\"type\": %u, \"base\": \"0x%08x\", \"size\": \"0x%08x\", \"scratch\": \"0x%08x\"}%s\n",
			       pool, layouts[dsp].base[pool], layouts[dsp].size[pool],
			       layouts[dsp].scratch[pool], pool == 3 ? "" : ",");
		}
		printf("    ]}%s\n", dsp == DSP_COUNT - 1 ? "" : ",");
	}
	printf("  ]\n");
	printf("}\n");

	munmap(bar, region.size);
	close(device);
	if (container_set)
		ioctl(group, VFIO_GROUP_UNSET_CONTAINER);
	close(group);
	close(container);
	return EXIT_SUCCESS;
}
