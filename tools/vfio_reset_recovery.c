// SPDX-License-Identifier: GPL-2.0-only
/* Experiment 003: one VFIO reset followed by read-only recovery checks. */

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
#include <time.h>
#include <unistd.h>

#define GROUP_PATH "/dev/vfio/16"
#define DEVICE_NAME "0000:03:00.0"
#define EXPECTED_BAR0_SIZE 0x10000
#define EXPECTED_FPGA_REVISION 0xa012dc0dU
#define EXPECTED_CAPABILITIES 0x00300811U
#define EXPECTED_DMA_CONTROL 0x0001fe00U
#define RECOVERY_TIMEOUT_MS 10000U
#define POLL_INTERVAL_US 100000U

static const uint32_t boot_offsets[8] = {
	0x01a4, 0x09a4, 0x11a4, 0x19a4,
	0x41a4, 0x49a4, 0x51a4, 0x59a4,
};

static const uint32_t dsp_banks[8] = {
	0x2000, 0x2080, 0x2100, 0x2180,
	0x6000, 0x6080, 0x6100, 0x6180,
};

struct snapshot {
	uint32_t fpga_revision;
	uint32_t capabilities;
	uint32_t dma_control;
	uint32_t dsp_status[8];
	unsigned int zero_ring_words;
};

static void die(const char *message)
{
	fprintf(stderr, "vfio reset recovery: %s: %s\n", message,
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

static void take_snapshot(const void *bar, struct snapshot *snapshot)
{
	unsigned int dsp, ring, field;

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->fpga_revision = mmio_read32(bar, 0x2218);
	snapshot->capabilities = mmio_read32(bar, 0x2234);
	snapshot->dma_control = mmio_read32(bar, 0x2200);

	for (dsp = 0; dsp < 8; dsp++) {
		snapshot->dsp_status[dsp] = mmio_read32(bar, boot_offsets[dsp]);
		for (ring = 0; ring < 2; ring++) {
			uint32_t base = dsp_banks[dsp] + ring * 0x40;

			for (field = 0; field < 11; field++) {
				if (mmio_read32(bar, base + field * 4) == 0)
					snapshot->zero_ring_words++;
			}
		}
	}
}

static bool snapshot_matches_cold(const struct snapshot *snapshot)
{
	unsigned int i;

	if (snapshot->fpga_revision != EXPECTED_FPGA_REVISION ||
	    snapshot->capabilities != EXPECTED_CAPABILITIES ||
	    snapshot->dma_control != EXPECTED_DMA_CONTROL ||
	    snapshot->zero_ring_words != 176)
		return false;

	for (i = 0; i < 8; i++) {
		if (!(snapshot->dsp_status[i] & 1))
			return false;
	}
	return true;
}

static uint64_t monotonic_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		die("clock_gettime");
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

int main(void)
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
	struct snapshot before, after;
	uint64_t started, elapsed = 0;
	unsigned int attempts = 0, i;
	int container = -1, group = -1, device = -1;
	void *bar = MAP_FAILED;
	bool container_set = false, recovered = false;
	int rc = EXIT_FAILURE;

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
		die("VFIO device does not advertise PCI reset support");
	}
	if (ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &region) < 0)
		die("VFIO_DEVICE_GET_REGION_INFO BAR0");
	if (region.size != EXPECTED_BAR0_SIZE ||
	    !(region.flags & VFIO_REGION_INFO_FLAG_READ) ||
	    !(region.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
		errno = EINVAL;
		die("BAR0 does not match the Experiment 003 contract");
	}

	bar = mmap(NULL, region.size, PROT_READ, MAP_SHARED, device,
		   region.offset);
	if (bar == MAP_FAILED)
		die("mmap VFIO BAR0 read-only");

	take_snapshot(bar, &before);
	if (!snapshot_matches_cold(&before)) {
		errno = EPROTO;
		die("cold baseline mismatch, reset refused");
	}

	if (ioctl(device, VFIO_DEVICE_RESET) < 0)
		die("VFIO_DEVICE_RESET");

	started = monotonic_ms();
	do {
		attempts++;
		take_snapshot(bar, &after);
		if (snapshot_matches_cold(&after)) {
			recovered = true;
			break;
		}
		usleep(POLL_INTERVAL_US);
		elapsed = monotonic_ms() - started;
	} while (elapsed < RECOVERY_TIMEOUT_MS);
	elapsed = monotonic_ms() - started;

	printf("{\n");
	printf("  \"experiment\": \"003-reset-recovery\",\n");
	printf("  \"vfio_device_reset_calls\": 1,\n");
	printf("  \"dma_mappings\": 0,\n");
	printf("  \"mmio_writes\": 0,\n");
	printf("  \"before_cold_baseline\": true,\n");
	printf("  \"recovered\": %s,\n", recovered ? "true" : "false");
	printf("  \"recovery_attempts\": %u,\n", attempts);
	printf("  \"recovery_elapsed_ms\": %" PRIu64 ",\n", elapsed);
	printf("  \"after\": {\n");
	printf("    \"fpga_revision\": \"0x%08x\",\n", after.fpga_revision);
	printf("    \"capabilities\": \"0x%08x\",\n", after.capabilities);
	printf("    \"dma_control\": \"0x%08x\",\n", after.dma_control);
	printf("    \"zero_ring_words\": %u,\n", after.zero_ring_words);
	printf("    \"dsps\": [\n");
	for (i = 0; i < 8; i++) {
		printf("      {\"index\": %u, \"status\": \"0x%08x\", "
		       "\"ready\": %s}%s\n", i, after.dsp_status[i],
		       (after.dsp_status[i] & 1) ? "true" : "false",
		       i == 7 ? "" : ",");
	}
	printf("    ]\n");
	printf("  },\n");
	printf("  \"success\": %s\n", recovered ? "true" : "false");
	printf("}\n");
	fflush(stdout);
	rc = recovered ? EXIT_SUCCESS : EXIT_FAILURE;

	munmap(bar, region.size);
	close(device);
	if (container_set)
		ioctl(group, VFIO_GROUP_UNSET_CONTAINER);
	close(group);
	close(container);
	return rc;
}
