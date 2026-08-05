// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experiments 001, 002, 007, and 008 for bounded DSP 0 ring behavior.
 * No valid command, interrupt, DSP-core reset, or firmware write.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/vfio.h>
#include <signal.h>
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
#define BAR0_SIZE UINT64_C(0x10000)
#define TEST_IOVA UINT64_C(0x10000000)
#define PAGE_COUNT 5
#define RESPONSE_CANARY 0x5a
#define DSP0_CMD_BASE 0x2000
#define DSP0_RESP_BASE 0x2040
#define DMA_CONTROL 0x2200
#define DMA_COLD_RESET 0x0001fe00
#define DMA_RESET_GLOBAL 0x0001fe01
#define DMA_GLOBAL_ONLY 0x00000001
#define DMA_GLOBAL_DSP0 0x00000003

static const uint32_t dsp_banks[8] = {
	0x2000, 0x2080, 0x2100, 0x2180,
	0x6000, 0x6080, 0x6100, 0x6180,
};

static const uint32_t boot_offsets[8] = {
	0x01a4, 0x09a4, 0x11a4, 0x19a4,
	0x41a4, 0x49a4, 0x51a4, 0x59a4,
};

static volatile sig_atomic_t interrupted;

static void handle_signal(int signal_number)
{
	(void)signal_number;
	interrupted = 1;
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

static bool all_ring_words_zero(const void *bar)
{
	unsigned int dsp, ring, field;

	for (dsp = 0; dsp < 8; dsp++) {
		for (ring = 0; ring < 2; ring++) {
			uint32_t base = dsp_banks[dsp] + ring * 0x40;

			for (field = 0; field < 16; field++) {
				if (mmio_read32(bar, base + field * 4) != 0)
					return false;
			}
		}
	}
	return true;
}

static bool all_dsps_ready(const void *bar)
{
	unsigned int dsp;

	for (dsp = 0; dsp < 8; dsp++) {
		if (!(mmio_read32(bar, boot_offsets[dsp]) & 1))
			return false;
	}
	return true;
}

static void write_descriptor(void *bar, uint32_t offset, uint64_t iova)
{
	mmio_write32(bar, offset, (uint32_t)iova);
	mmio_write32(bar, offset + 4, (uint32_t)(iova >> 32));
}

static bool descriptor_matches(const void *bar, uint32_t offset, uint64_t iova)
{
	uint64_t observed = mmio_read32(bar, offset);

	observed |= (uint64_t)mmio_read32(bar, offset + 4) << 32;
	return observed == iova;
}

static bool pages_unchanged(const unsigned char *memory, size_t page_size)
{
	size_t page, byte;

	for (page = 0; page < 4; page++) {
		for (byte = 0; byte < page_size; byte++) {
			if (memory[page * page_size + byte] != 0)
				return false;
		}
	}
	for (byte = 0; byte < page_size; byte++) {
		if (memory[4 * page_size + byte] != RESPONSE_CANARY)
			return false;
	}
	return true;
}

static bool published_ring_state_matches(const void *bar, size_t page_size)
{
	unsigned int dsp, ring, field;

	for (dsp = 0; dsp < 8; dsp++) {
		for (ring = 0; ring < 2; ring++) {
			uint32_t base = dsp_banks[dsp] + ring * 0x40;

			for (field = 0; field < 16; field++) {
				uint64_t expected = 0;

				if (dsp == 0 && ring == 0 && field < 8) {
					unsigned int page = field / 2;
					uint64_t iova = TEST_IOVA +
						page * (uint64_t)page_size;

					expected = field % 2 ? iova >> 32 :
						(uint32_t)iova;
				} else if (dsp == 0 && ring == 1 && field < 2) {
					uint64_t iova = TEST_IOVA +
						4 * (uint64_t)page_size;

					expected = field ? iova >> 32 :
						(uint32_t)iova;
				}
				if (mmio_read32(bar, base + field * 4) !=
				    (uint32_t)expected)
					return false;
			}
		}
	}
	return true;
}

static bool triggered_ring_state_matches(const void *bar, size_t page_size)
{
	unsigned int dsp, ring, field;

	for (dsp = 0; dsp < 8; dsp++) {
		for (ring = 0; ring < 2; ring++) {
			uint32_t base = dsp_banks[dsp] + ring * 0x40;

			for (field = 0; field < 16; field++) {
				uint64_t expected = 0;

				if (dsp == 0 && ring == 0 && field < 8) {
					unsigned int page = field / 2;
					uint64_t iova = TEST_IOVA +
						page * (uint64_t)page_size;

					expected = field % 2 ? iova >> 32 :
						(uint32_t)iova;
				} else if (dsp == 0 && ring == 1 && field < 2) {
					uint64_t iova = TEST_IOVA +
						4 * (uint64_t)page_size;

					expected = field ? iova >> 32 :
						(uint32_t)iova;
				} else if (dsp == 0 && ring == 0 &&
					   field >= 8 && field <= 10) {
					continue;
				}
				if (mmio_read32(bar, base + field * 4) !=
				    (uint32_t)expected)
					return false;
			}
		}
	}
	return true;
}

int main(int argc, char **argv)
{
	struct sigaction action = { .sa_handler = handle_signal };
	struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
	struct vfio_iommu_type1_info iommu_info = { .argsz = sizeof(iommu_info) };
	struct vfio_iommu_type1_dma_map dma_map = { .argsz = sizeof(dma_map) };
	struct vfio_iommu_type1_dma_unmap dma_unmap = { .argsz = sizeof(dma_unmap) };
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
	struct vfio_region_info bar_info = {
		.argsz = sizeof(bar_info),
		.index = VFIO_PCI_BAR0_REGION_INDEX,
	};
	long page_size = sysconf(_SC_PAGESIZE);
	size_t memory_size;
	unsigned char *memory = MAP_FAILED;
	void *bar = MAP_FAILED;
	int container = -1, group = -1, device = -1;
	bool container_set = false, dma_mapped = false, descriptors_published = false;
	bool readback_ok = false, ready_ok = false, canary_ok = false;
	bool ring_state_ok = false, restore_ok = false;
	bool dma_modified = false, dma_restore_ok = false;
	bool experiment002 = argc == 2 &&
		strcmp(argv[1], "--experiment-002") == 0;
	bool experiment007 = argc == 2 &&
		strcmp(argv[1], "--experiment-007") == 0;
	bool experiment008 = argc == 2 &&
		strcmp(argv[1], "--experiment-008") == 0;
	bool dma_experiment = experiment002 || experiment007 || experiment008;
	uint32_t dma_readbacks[3] = { 0, 0, 0 };
	unsigned int dma_write_count = 0;
	unsigned int ring_pointer_writes = 0, doorbell_writes = 0;
	uint32_t pointer_readback = 0, doorbell_readback = 0;
	uint32_t pointer_after_wait = 0, doorbell_after_wait = 0;
	uint32_t position_before = 0, position_after = 0;
	bool controls_modified = false, reset_called = false;
	bool reset_recovered = false;
	const char *experiment = experiment002 ? "002-dma-engine" :
		(experiment007 ? "007-empty-ring-activation" :
		 (experiment008 ? "008-zero-entry-fetch" :
		  "001-ring-descriptors"));
	unsigned int page;
	int result = EXIT_FAILURE;
	struct timespec wait_time = { .tv_sec = 0, .tv_nsec = 250000000 };

	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	if (argc > 2 || (argc == 2 && !experiment002 && !experiment007 &&
			       !experiment008)) {
		fprintf(stderr,
			"usage: %s [--experiment-002|--experiment-007|"
			"--experiment-008]\n",
			argv[0]);
		return EXIT_FAILURE;
	}

	if (page_size <= 0 || (TEST_IOVA % (uint64_t)page_size) != 0) {
		fprintf(stderr, "%s: unsupported page size\n", experiment);
		goto cleanup;
	}
	memory_size = PAGE_COUNT * (size_t)page_size;

	container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (container < 0 ||
	    ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
	    ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1) {
		perror("experiment: initialize VFIO container");
		goto cleanup;
	}
	group = open(GROUP_PATH, O_RDWR | O_CLOEXEC);
	if (group < 0 ||
	    ioctl(group, VFIO_GROUP_GET_STATUS, &group_status) < 0 ||
	    !(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		perror("experiment: open viable group 16");
		goto cleanup;
	}
	if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0) {
		perror("experiment: set container");
		goto cleanup;
	}
	container_set = true;
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0 ||
	    ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info) < 0 ||
	    !(iommu_info.iova_pgsizes & (uint64_t)page_size)) {
		perror("experiment: configure Type 1 IOMMU");
		goto cleanup;
	}

	memory = mmap(NULL, memory_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED || mlock(memory, memory_size) < 0) {
		perror("experiment: allocate locked ring pages");
		goto cleanup;
	}
	memset(memory, 0, 4 * (size_t)page_size);
	memset(memory + 4 * page_size, RESPONSE_CANARY, (size_t)page_size);

	dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	dma_map.vaddr = (uintptr_t)memory;
	dma_map.iova = TEST_IOVA;
	dma_map.size = memory_size;
	if (ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
		perror("experiment: map five IOVA pages");
		goto cleanup;
	}
	dma_mapped = true;

	device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, DEVICE_NAME);
	if (device < 0 ||
	    ioctl(device, VFIO_DEVICE_GET_INFO, &device_info) < 0 ||
	    !(device_info.flags & VFIO_DEVICE_FLAGS_PCI) ||
	    (experiment008 && !(device_info.flags & VFIO_DEVICE_FLAGS_RESET)) ||
	    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &bar_info) < 0 ||
	    bar_info.size != BAR0_SIZE ||
	    !(bar_info.flags & VFIO_REGION_INFO_FLAG_READ) ||
	    !(bar_info.flags & VFIO_REGION_INFO_FLAG_WRITE) ||
	    !(bar_info.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
		perror("experiment: validate VFIO device and BAR0");
		goto cleanup;
	}
	bar = mmap(NULL, bar_info.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   device, bar_info.offset);
	if (bar == MAP_FAILED) {
		perror("experiment: map BAR0");
		goto cleanup;
	}

	if (!all_ring_words_zero(bar) || !all_dsps_ready(bar) ||
	    (dma_experiment && mmio_read32(bar, DMA_CONTROL) != DMA_COLD_RESET)) {
		fprintf(stderr, "%s: baseline precondition changed\n", experiment);
		goto cleanup;
	}

	for (page = 0; page < 4; page++)
		write_descriptor(bar, DSP0_CMD_BASE + page * 8,
				 TEST_IOVA + page * (uint64_t)page_size);
	write_descriptor(bar, DSP0_RESP_BASE,
			 TEST_IOVA + 4 * (uint64_t)page_size);
	descriptors_published = true;

	readback_ok = true;
	for (page = 0; page < 4; page++) {
		if (!descriptor_matches(bar, DSP0_CMD_BASE + page * 8,
					TEST_IOVA + page * (uint64_t)page_size))
			readback_ok = false;
	}
	if (!descriptor_matches(bar, DSP0_RESP_BASE,
				TEST_IOVA + 4 * (uint64_t)page_size))
		readback_ok = false;
	if (!readback_ok)
		goto cleanup;
	ring_state_ok = published_ring_state_matches(bar, (size_t)page_size);
	if (!ring_state_ok)
		goto cleanup;

	if (dma_experiment) {
		mmio_write32(bar, DMA_CONTROL, DMA_RESET_GLOBAL);
		dma_modified = true;
		dma_write_count++;
		dma_readbacks[0] = mmio_read32(bar, DMA_CONTROL);
		if (dma_readbacks[0] != DMA_RESET_GLOBAL)
			goto cleanup;

		mmio_write32(bar, DMA_CONTROL, DMA_GLOBAL_ONLY);
		dma_write_count++;
		dma_readbacks[1] = mmio_read32(bar, DMA_CONTROL);
		if (dma_readbacks[1] != DMA_GLOBAL_ONLY)
			goto cleanup;

		mmio_write32(bar, DMA_CONTROL, DMA_GLOBAL_DSP0);
		dma_write_count++;
		dma_readbacks[2] = mmio_read32(bar, DMA_CONTROL);
		if (dma_readbacks[2] != DMA_GLOBAL_DSP0)
			goto cleanup;
	}
	if (experiment007) {
		mmio_write32(bar, DSP0_CMD_BASE + 0x24, 0);
		doorbell_writes++;
		if (mmio_read32(bar, DSP0_CMD_BASE + 0x24) != 0)
			goto cleanup;
		mmio_write32(bar, DSP0_CMD_BASE + 0x20, 0);
		ring_pointer_writes++;
		if (mmio_read32(bar, DSP0_CMD_BASE + 0x20) != 0)
			goto cleanup;
	}
	if (experiment008) {
		position_before = mmio_read32(bar, DSP0_CMD_BASE + 0x28);
		mmio_write32(bar, DSP0_CMD_BASE + 0x24, 1);
		controls_modified = true;
		doorbell_writes++;
		doorbell_readback = mmio_read32(bar, DSP0_CMD_BASE + 0x24);
		mmio_write32(bar, DSP0_CMD_BASE + 0x20, 1);
		ring_pointer_writes++;
		pointer_readback = mmio_read32(bar, DSP0_CMD_BASE + 0x20);
	}

	nanosleep(&wait_time, NULL);
	ready_ok = all_dsps_ready(bar);
	canary_ok = pages_unchanged(memory, (size_t)page_size);
	if (experiment008) {
		pointer_after_wait = mmio_read32(bar, DSP0_CMD_BASE + 0x20);
		doorbell_after_wait = mmio_read32(bar, DSP0_CMD_BASE + 0x24);
		position_after = mmio_read32(bar, DSP0_CMD_BASE + 0x28);
		ring_state_ok = triggered_ring_state_matches(bar,
						      (size_t)page_size);
	} else {
		ring_state_ok = published_ring_state_matches(bar,
						     (size_t)page_size);
	}
	if (interrupted || !ready_ok || !canary_ok || !ring_state_ok)
		goto cleanup;

	result = EXIT_SUCCESS;

cleanup:
	if (dma_modified && bar != MAP_FAILED) {
		mmio_write32(bar, DMA_CONTROL, DMA_COLD_RESET);
		dma_write_count++;
		dma_restore_ok = mmio_read32(bar, DMA_CONTROL) == DMA_COLD_RESET;
		if (!dma_restore_ok)
			result = EXIT_FAILURE;
	}
	if (controls_modified && bar != MAP_FAILED) {
		mmio_write32(bar, DSP0_CMD_BASE + 0x24, 0);
		doorbell_writes++;
		mmio_write32(bar, DSP0_CMD_BASE + 0x20, 0);
		ring_pointer_writes++;
	}
	if (descriptors_published && bar != MAP_FAILED) {
		for (page = 0; page < 4; page++)
			write_descriptor(bar, DSP0_CMD_BASE + page * 8, 0);
		write_descriptor(bar, DSP0_RESP_BASE, 0);
		restore_ok = true;
		for (page = 0; page < 4; page++) {
			if (!descriptor_matches(bar, DSP0_CMD_BASE + page * 8, 0))
				restore_ok = false;
		}
		if (!descriptor_matches(bar, DSP0_RESP_BASE, 0))
			restore_ok = false;
		if (!restore_ok)
			result = EXIT_FAILURE;
	}
	if (experiment008 && device >= 0 && bar != MAP_FAILED) {
		reset_called = true;
		if (ioctl(device, VFIO_DEVICE_RESET) == 0) {
			reset_recovered = all_ring_words_zero(bar) &&
				mmio_read32(bar, DMA_CONTROL) == DMA_COLD_RESET &&
				all_dsps_ready(bar);
		}
		if (!reset_recovered)
			result = EXIT_FAILURE;
	}

	if (bar != MAP_FAILED)
		munmap(bar, BAR0_SIZE);
	if (device >= 0)
		close(device);
	if (dma_mapped) {
		dma_unmap.iova = TEST_IOVA;
		dma_unmap.size = memory_size;
		if (ioctl(container, VFIO_IOMMU_UNMAP_DMA, &dma_unmap) < 0 ||
		    dma_unmap.size != memory_size)
			result = EXIT_FAILURE;
	}
	if (memory != MAP_FAILED) {
		munlock(memory, memory_size);
		munmap(memory, memory_size);
	}
	if (container_set)
		ioctl(group, VFIO_GROUP_UNSET_CONTAINER);
	if (group >= 0)
		close(group);
	if (container >= 0)
		close(container);

	printf("{\n");
	printf("  \"experiment\": \"%s\",\n", experiment);
	printf("  \"mapped_pages\": %d,\n", PAGE_COUNT);
	printf("  \"descriptors_published\": %s,\n",
	       descriptors_published ? "true" : "false");
	printf("  \"descriptor_words_written\": %d,\n",
	       descriptors_published ? 10 : 0);
	printf("  \"ring_pointer_writes\": %u,\n", ring_pointer_writes);
	printf("  \"doorbell_writes\": %u,\n", doorbell_writes);
	printf("  \"pointer_readback\": \"0x%08x\",\n", pointer_readback);
	printf("  \"doorbell_readback\": \"0x%08x\",\n", doorbell_readback);
	printf("  \"pointer_after_wait\": \"0x%08x\",\n",
	       pointer_after_wait);
	printf("  \"doorbell_after_wait\": \"0x%08x\",\n",
	       doorbell_after_wait);
	printf("  \"position_before\": \"0x%08x\",\n", position_before);
	printf("  \"position_after\": \"0x%08x\",\n", position_after);
	printf("  \"dma_control_writes\": %u,\n", dma_write_count);
	printf("  \"dma_readback_reset_global\": \"0x%08x\",\n",
	       dma_readbacks[0]);
	printf("  \"dma_readback_global_only\": \"0x%08x\",\n",
	       dma_readbacks[1]);
	printf("  \"dma_readback_global_dsp0\": \"0x%08x\",\n",
	       dma_readbacks[2]);
	printf("  \"dma_restored_to_cold_reset\": %s,\n",
	       dma_experiment ? (dma_restore_ok ? "true" : "false") : "null");
	printf("  \"descriptor_readback_ok\": %s,\n",
	       readback_ok ? "true" : "false");
	printf("  \"all_dsps_ready\": %s,\n", ready_ok ? "true" : "false");
	printf("  \"pages_unchanged\": %s,\n", canary_ok ? "true" : "false");
	printf("  \"published_ring_state_unchanged\": %s,\n",
	       ring_state_ok ? "true" : "false");
	printf("  \"descriptors_restored_to_zero\": %s,\n",
	       restore_ok ? "true" : "false");
	printf("  \"vfio_device_reset_called\": %s,\n",
	       reset_called ? "true" : "false");
	printf("  \"reset_recovered\": %s,\n",
	       experiment008 ? (reset_recovered ? "true" : "false") : "null");
	printf("  \"interrupted\": %s,\n", interrupted ? "true" : "false");
	printf("  \"success\": %s\n", result == EXIT_SUCCESS ? "true" : "false");
	printf("}\n");
	return result;
}
