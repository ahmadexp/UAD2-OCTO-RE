// SPDX-License-Identifier: GPL-2.0-only
/* Experiments 005/006: bounded DSP0 ring-control write/read/restore. */

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
#define EXPECTED_FPGA_REVISION 0xa012dc0dU
#define EXPECTED_CAPABILITIES 0x00300811U
#define EXPECTED_DMA_CONTROL 0x0001fe00U
#define DSP0_CMD_POINTER 0x2020U
#define DSP0_CMD_DOORBELL 0x2024U

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
	fprintf(stderr, "vfio ring control: %s: %s\n", message,
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

static void mmio_write32(void *bar, uint32_t offset, uint32_t value)
{
	volatile uint32_t *word =
		(volatile uint32_t *)((char *)bar + offset);

	*word = value;
	__sync_synchronize();
}

static unsigned int count_nonzero_ring_words(const void *bar,
					     uint32_t target_offset,
					     uint32_t *unexpected_offset)
{
	unsigned int dsp, ring, field, nonzero = 0;

	*unexpected_offset = 0;
	for (dsp = 0; dsp < 8; dsp++) {
		for (ring = 0; ring < 2; ring++) {
			uint32_t base = dsp_banks[dsp] + ring * 0x40;

			for (field = 0; field < 16; field++) {
				uint32_t offset = base + field * 4;
				uint32_t value = mmio_read32(bar, offset);

				if (value != 0) {
					nonzero++;
					if (offset != target_offset)
						*unexpected_offset = offset;
				}
			}
		}
	}
	return nonzero;
}

static bool identity_and_dsps_match(const void *bar)
{
	unsigned int i;

	if (mmio_read32(bar, 0x2218) != EXPECTED_FPGA_REVISION ||
	    mmio_read32(bar, 0x2234) != EXPECTED_CAPABILITIES ||
	    mmio_read32(bar, 0x2200) != EXPECTED_DMA_CONTROL)
		return false;
	for (i = 0; i < 8; i++) {
		if (!(mmio_read32(bar, boot_offsets[i]) & 1))
			return false;
	}
	return true;
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
	uint32_t unexpected_offset = 0;
	unsigned int nonzero_before, nonzero_after_write, nonzero_after_wait;
	unsigned int nonzero_after_restore, nonzero_after_reset;
	uint32_t write_readback, wait_readback, restore_readback;
	bool after_write_exact, after_wait_exact, restored, reset_recovered;
	int container = -1, group = -1, device = -1;
	void *bar = MAP_FAILED;
	bool container_set = false;
	uint32_t target_offset = DSP0_CMD_POINTER;
	const char *experiment = "005-ring-control-write";
	int rc = EXIT_FAILURE;

	if (argc == 2 && strcmp(argv[1], "--experiment-006") == 0) {
		target_offset = DSP0_CMD_DOORBELL;
		experiment = "006-ring-doorbell-write";
	} else if (argc != 1) {
		fprintf(stderr, "usage: %s [--experiment-006]\n", argv[0]);
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
	if (!(device_info.flags & VFIO_DEVICE_FLAGS_PCI) ||
	    !(device_info.flags & VFIO_DEVICE_FLAGS_RESET)) {
		errno = ENOTSUP;
		die("VFIO PCI reset support unavailable");
	}
	if (ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &region) < 0)
		die("VFIO_DEVICE_GET_REGION_INFO BAR0");
	if (region.size != EXPECTED_BAR0_SIZE ||
	    !(region.flags & VFIO_REGION_INFO_FLAG_READ) ||
	    !(region.flags & VFIO_REGION_INFO_FLAG_WRITE) ||
	    !(region.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
		errno = EINVAL;
		die("BAR0 does not match the Experiment 005 contract");
	}

	bar = mmap(NULL, region.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   device, region.offset);
	if (bar == MAP_FAILED)
		die("mmap VFIO BAR0");

	nonzero_before = count_nonzero_ring_words(bar, target_offset,
					  &unexpected_offset);
	if (nonzero_before != 0 || !identity_and_dsps_match(bar)) {
		errno = EPROTO;
		die("cold baseline mismatch, write refused");
	}

	mmio_write32(bar, target_offset, 1);
	write_readback = mmio_read32(bar, target_offset);
	nonzero_after_write = count_nonzero_ring_words(bar, target_offset,
					       &unexpected_offset);
	after_write_exact = write_readback == 1 && nonzero_after_write == 1 &&
		unexpected_offset == 0 && identity_and_dsps_match(bar);

	usleep(250000);
	wait_readback = mmio_read32(bar, target_offset);
	nonzero_after_wait = count_nonzero_ring_words(bar, target_offset,
					      &unexpected_offset);
	after_wait_exact = wait_readback == 1 && nonzero_after_wait == 1 &&
		unexpected_offset == 0 && identity_and_dsps_match(bar);

	mmio_write32(bar, target_offset, 0);
	restore_readback = mmio_read32(bar, target_offset);
	nonzero_after_restore = count_nonzero_ring_words(bar, target_offset,
						 &unexpected_offset);
	restored = restore_readback == 0 && nonzero_after_restore == 0 &&
		identity_and_dsps_match(bar);

	if (ioctl(device, VFIO_DEVICE_RESET) < 0)
		die("VFIO_DEVICE_RESET");
	nonzero_after_reset = count_nonzero_ring_words(bar, target_offset,
					       &unexpected_offset);
	reset_recovered = nonzero_after_reset == 0 && identity_and_dsps_match(bar);

	printf("{\n");
	printf("  \"experiment\": \"%s\",\n", experiment);
	printf("  \"target_offset\": \"0x%04x\",\n", target_offset);
	printf("  \"dma_mappings\": 0,\n");
	printf("  \"dma_control_writes\": 0,\n");
	printf("  \"ring_control_writes\": 2,\n");
	printf("  \"vfio_device_reset_calls\": 1,\n");
	printf("  \"nonzero_before\": %u,\n", nonzero_before);
	printf("  \"write_one_readback\": \"0x%08x\",\n", write_readback);
	printf("  \"nonzero_after_write\": %u,\n", nonzero_after_write);
	printf("  \"after_write_exact\": %s,\n",
	       after_write_exact ? "true" : "false");
	printf("  \"wait_readback\": \"0x%08x\",\n", wait_readback);
	printf("  \"nonzero_after_wait\": %u,\n", nonzero_after_wait);
	printf("  \"after_wait_exact\": %s,\n",
	       after_wait_exact ? "true" : "false");
	printf("  \"restore_readback\": \"0x%08x\",\n", restore_readback);
	printf("  \"nonzero_after_restore\": %u,\n", nonzero_after_restore);
	printf("  \"restored\": %s,\n", restored ? "true" : "false");
	printf("  \"nonzero_after_reset\": %u,\n", nonzero_after_reset);
	printf("  \"reset_recovered\": %s,\n",
	       reset_recovered ? "true" : "false");
	printf("  \"success\": %s\n",
	       after_write_exact && after_wait_exact && restored && reset_recovered
		? "true" : "false");
	printf("}\n");
	fflush(stdout);
	rc = after_write_exact && after_wait_exact && restored && reset_recovered
		? EXIT_SUCCESS : EXIT_FAILURE;

	munmap(bar, region.size);
	close(device);
	if (container_set)
		ioctl(group, VFIO_GROUP_UNSET_CONTAINER);
	close(group);
	close(container);
	return rc;
}
