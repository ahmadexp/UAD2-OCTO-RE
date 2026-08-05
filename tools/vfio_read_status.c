// SPDX-License-Identifier: GPL-2.0-only
/* Read-only UAD-2 OCTO status probe through the legacy VFIO group API. */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/vfio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GROUP_PATH "/dev/vfio/16"
#define DEVICE_NAME "0000:03:00.0"
#define EXPECTED_BAR0_SIZE 0x10000

static const uint32_t boot_offsets[8] = {
	0x01a4, 0x09a4, 0x11a4, 0x19a4,
	0x41a4, 0x49a4, 0x51a4, 0x59a4,
};

static const uint32_t dsp_banks[8] = {
	0x2000, 0x2080, 0x2100, 0x2180,
	0x6000, 0x6080, 0x6100, 0x6180,
};

static void die(const char *message)
{
	fprintf(stderr, "vfio status probe: %s: %s\n", message,
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

int main(int argc, char **argv)
{
	struct vfio_group_status group_status = {
		.argsz = sizeof(group_status),
	};
	struct vfio_device_info device_info = {
		.argsz = sizeof(device_info),
	};
	struct vfio_region_info region = {
		.argsz = sizeof(region),
		.index = VFIO_PCI_BAR0_REGION_INDEX,
	};
	uint32_t first[8], second[8];
	uint32_t extended_ring_words[8][2][5] = {{{0}}};
	unsigned int zero_ring_words = 0;
	unsigned int zero_extended_ring_words = 0;
	int container = -1, group = -1, device = -1;
	void *bar = MAP_FAILED;
	unsigned int i;
	bool container_set = false;
	bool extended_rings = false;
	int rc = EXIT_FAILURE;

	if (argc == 2 && strcmp(argv[1], "--extended-rings") == 0)
		extended_rings = true;
	else if (argc != 1) {
		fprintf(stderr, "usage: %s [--extended-rings]\n", argv[0]);
		return EXIT_FAILURE;
	}

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
	container_set = true;
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
		die("BAR0 does not match the read-only probe contract");
	}

	bar = mmap(NULL, region.size, PROT_READ, MAP_SHARED, device,
		   region.offset);
	if (bar == MAP_FAILED)
		die("mmap VFIO BAR0 read-only");

	for (i = 0; i < 8; i++)
		first[i] = mmio_read32(bar, boot_offsets[i]);
	usleep(250000);
	for (i = 0; i < 8; i++)
		second[i] = mmio_read32(bar, boot_offsets[i]);
	for (i = 0; i < 8; i++) {
		unsigned int ring, field;

		for (ring = 0; ring < 2; ring++) {
			uint32_t base = dsp_banks[i] + ring * 0x40;

			for (field = 0; field < 11; field++) {
				if (mmio_read32(bar, base + field * 4) == 0)
					zero_ring_words++;
			}
			if (extended_rings) {
				for (field = 0; field < 5; field++) {
					uint32_t value =
						mmio_read32(bar, base + 0x2c + field * 4);

					extended_ring_words[i][ring][field] = value;
					if (value == 0)
						zero_extended_ring_words++;
				}
			}
		}
	}

	printf("{\n");
	printf("  \"transport\": \"vfio-pci\",\n");
	printf("  \"iommu_type\": \"VFIO_TYPE1_IOMMU\",\n");
	printf("  \"dma_mappings\": 0,\n");
	printf("  \"mmio_writes\": 0,\n");
	printf("  \"bar0_size\": %" PRIu64 ",\n", (uint64_t)region.size);
	printf("  \"device_flags\": %u,\n", device_info.flags);
	printf("  \"dma_control\": \"0x%08x\",\n",
	       mmio_read32(bar, 0x2200));
	printf("  \"ring_words_read\": 176,\n");
	printf("  \"zero_ring_words\": %u,\n", zero_ring_words);
	if (extended_rings) {
		printf("  \"extended_ring_words_read\": 80,\n");
		printf("  \"zero_extended_ring_words\": %u,\n",
		       zero_extended_ring_words);
		printf("  \"extended_ring_words\": [\n");
		for (i = 0; i < 8; i++) {
			unsigned int ring, field;

			printf("    {\"dsp\": %u, \"rings\": [", i);
			for (ring = 0; ring < 2; ring++) {
				printf("[");
				for (field = 0; field < 5; field++) {
					printf("\"0x%08x\"%s",
					       extended_ring_words[i][ring][field],
					       field == 4 ? "" : ", ");
				}
				printf("]%s", ring == 1 ? "" : ", ");
			}
			printf("]}%s\n", i == 7 ? "" : ",");
		}
		printf("  ],\n");
	}
	printf("  \"dsps\": [\n");
	for (i = 0; i < 8; i++) {
		printf("    {\"index\": %u, \"offset\": \"0x%04x\", "
		       "\"first\": \"0x%08x\", \"second\": \"0x%08x\", "
		       "\"ready\": %s}%s\n",
		       i, boot_offsets[i], first[i], second[i],
		       (second[i] & 1) ? "true" : "false",
		       i == 7 ? "" : ",");
	}
	printf("  ]\n");
	printf("}\n");
	rc = EXIT_SUCCESS;

	munmap(bar, region.size);
	bar = MAP_FAILED;
	close(device);
	device = -1;
	if (container_set)
		ioctl(group, VFIO_GROUP_UNSET_CONTAINER);
	close(group);
	close(container);
	return rc;
}
