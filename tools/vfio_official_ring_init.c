// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experiment 011: reproduce the official DSP 0 ring-initializer order.
 *
 * This probe maps four command and four response pages, synchronizes the two
 * host indexes to the bounded hardware index, and publishes every descriptor.
 * It never enables DMA, writes an interrupt register, or submits a command.
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
#define PAGE_SIZE_4K 4096
#define PAGE_COUNT 8
#define RING_PAGES 4
#define DSP0_CMD_BASE 0x2000
#define DSP0_RESP_BASE 0x2040
#define DMA_CONTROL 0x2200
#define DMA_COLD_RESET 0x0001fe00

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

static uint64_t read_descriptor(const void *bar, uint32_t offset)
{
	uint64_t value = mmio_read32(bar, offset);

	value |= (uint64_t)mmio_read32(bar, offset + 4) << 32;
	return value;
}

static uint32_t bounded_hardware_index(const void *bar, uint32_t base,
				       uint32_t *raw)
{
	*raw = mmio_read32(bar, base + 0x28);
	return *raw < 1024 ? *raw : 0;
}

static void initialize_ring(void *bar, uint32_t base, uint64_t first_iova,
			    uint32_t index)
{
	unsigned int page;

	/* This order matches the hash-identified official ring initializer. */
	mmio_write32(bar, base + 0x24, index);
	mmio_write32(bar, base + 0x20, index);
	for (page = 0; page < RING_PAGES; page++)
		write_descriptor(bar, base + page * 8,
				 first_iova + page * (uint64_t)PAGE_SIZE_4K);
}

static bool ring_matches(const void *bar, uint32_t base, uint64_t first_iova,
			 uint32_t index)
{
	unsigned int page;

	if (mmio_read32(bar, base + 0x20) != index ||
	    mmio_read32(bar, base + 0x24) != index)
		return false;
	for (page = 0; page < RING_PAGES; page++) {
		uint64_t expected = first_iova +
			page * (uint64_t)PAGE_SIZE_4K;

		if (read_descriptor(bar, base + page * 8) != expected)
			return false;
	}
	return true;
}

static bool only_dsp0_rings_published(const void *bar,
				      uint32_t command_index,
				      uint32_t response_index)
{
	unsigned int dsp, ring, field;

	for (dsp = 0; dsp < 8; dsp++) {
		for (ring = 0; ring < 2; ring++) {
			uint32_t base = dsp_banks[dsp] + ring * 0x40;

			for (field = 0; field < 16; field++) {
				uint32_t expected = 0;

				if (dsp == 0 && field < 8) {
					unsigned int page = field / 2;
					unsigned int memory_page = ring * 4 + page;
					uint64_t iova = TEST_IOVA +
						memory_page * (uint64_t)PAGE_SIZE_4K;

					expected = field % 2 ? (uint32_t)(iova >> 32) :
						(uint32_t)iova;
				} else if (dsp == 0 && field == 8) {
					expected = ring ? response_index : command_index;
				} else if (dsp == 0 && field == 9) {
					expected = ring ? response_index : command_index;
				} else if (dsp == 0 && field == 10) {
					/* Hardware index is observed, never written. */
					continue;
				}
				if (mmio_read32(bar, base + field * 4) != expected)
					return false;
			}
		}
	}
	return true;
}

static void fill_canaries(unsigned char *memory)
{
	unsigned int page;

	for (page = 0; page < PAGE_COUNT; page++)
		memset(memory + page * PAGE_SIZE_4K, 0x40 + page,
		       PAGE_SIZE_4K);
}

static bool pages_unchanged(const unsigned char *memory)
{
	unsigned int page;
	size_t byte;

	for (page = 0; page < PAGE_COUNT; page++) {
		for (byte = 0; byte < PAGE_SIZE_4K; byte++) {
			if (memory[page * PAGE_SIZE_4K + byte] !=
			    (unsigned char)(0x40 + page))
				return false;
		}
	}
	return true;
}

static bool clear_dsp0_rings(void *bar)
{
	unsigned int page;

	for (page = 0; page < RING_PAGES; page++) {
		write_descriptor(bar, DSP0_CMD_BASE + page * 8, 0);
		write_descriptor(bar, DSP0_RESP_BASE + page * 8, 0);
	}
	mmio_write32(bar, DSP0_CMD_BASE + 0x24, 0);
	mmio_write32(bar, DSP0_CMD_BASE + 0x20, 0);
	mmio_write32(bar, DSP0_RESP_BASE + 0x24, 0);
	mmio_write32(bar, DSP0_RESP_BASE + 0x20, 0);
	return all_ring_words_zero(bar);
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
	const size_t memory_size = PAGE_COUNT * PAGE_SIZE_4K;
	struct timespec wait_time = { .tv_sec = 0, .tv_nsec = 250000000 };
	unsigned char *memory = MAP_FAILED;
	void *bar = MAP_FAILED;
	int container = -1, group = -1, device = -1;
	bool container_set = false, dma_mapped = false;
	bool rings_published = false, publication_ok = false;
	bool pages_ok = false, ready_ok = false, dma_unchanged = false;
	bool restore_ok = false, reset_called = false, reset_recovered = false;
	uint32_t command_index_raw = 0, response_index_raw = 0;
	uint32_t command_index = 0, response_index = 0;
	int result = EXIT_FAILURE;

	(void)argv;
	if (argc != 1) {
		fprintf(stderr, "usage: vfio_official_ring_init\n");
		return EXIT_FAILURE;
	}
	if (sysconf(_SC_PAGESIZE) != PAGE_SIZE_4K) {
		fprintf(stderr, "experiment-011: requires 4096-byte host pages\n");
		goto cleanup;
	}

	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);

	container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (container < 0 ||
	    ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
	    ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1) {
		perror("experiment-011: initialize VFIO container");
		goto cleanup;
	}
	group = open(GROUP_PATH, O_RDWR | O_CLOEXEC);
	if (group < 0 ||
	    ioctl(group, VFIO_GROUP_GET_STATUS, &group_status) < 0 ||
	    !(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		perror("experiment-011: open viable group 16");
		goto cleanup;
	}
	if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0) {
		perror("experiment-011: set container");
		goto cleanup;
	}
	container_set = true;
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0 ||
	    ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info) < 0 ||
	    !(iommu_info.iova_pgsizes & PAGE_SIZE_4K)) {
		perror("experiment-011: configure Type 1 IOMMU");
		goto cleanup;
	}

	memory = mmap(NULL, memory_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED || mlock(memory, memory_size) < 0) {
		perror("experiment-011: allocate locked ring pages");
		goto cleanup;
	}
	fill_canaries(memory);

	dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	dma_map.vaddr = (uintptr_t)memory;
	dma_map.iova = TEST_IOVA;
	dma_map.size = memory_size;
	if (ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
		perror("experiment-011: map eight IOVA pages");
		goto cleanup;
	}
	dma_mapped = true;

	device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, DEVICE_NAME);
	if (device < 0 ||
	    ioctl(device, VFIO_DEVICE_GET_INFO, &device_info) < 0 ||
	    !(device_info.flags & VFIO_DEVICE_FLAGS_PCI) ||
	    !(device_info.flags & VFIO_DEVICE_FLAGS_RESET) ||
	    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &bar_info) < 0 ||
	    bar_info.size != BAR0_SIZE ||
	    !(bar_info.flags & VFIO_REGION_INFO_FLAG_READ) ||
	    !(bar_info.flags & VFIO_REGION_INFO_FLAG_WRITE) ||
	    !(bar_info.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
		perror("experiment-011: validate VFIO device and BAR0");
		goto cleanup;
	}
	bar = mmap(NULL, bar_info.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   device, bar_info.offset);
	if (bar == MAP_FAILED) {
		perror("experiment-011: map BAR0");
		goto cleanup;
	}

	if (!all_ring_words_zero(bar) || !all_dsps_ready(bar) ||
	    mmio_read32(bar, DMA_CONTROL) != DMA_COLD_RESET) {
		fprintf(stderr, "experiment-011: baseline precondition changed\n");
		goto cleanup;
	}

	command_index = bounded_hardware_index(bar, DSP0_CMD_BASE,
					       &command_index_raw);
	response_index = bounded_hardware_index(bar, DSP0_RESP_BASE,
						&response_index_raw);
	initialize_ring(bar, DSP0_CMD_BASE, TEST_IOVA, command_index);
	initialize_ring(bar, DSP0_RESP_BASE,
			TEST_IOVA + 4 * (uint64_t)PAGE_SIZE_4K, response_index);
	rings_published = true;

	publication_ok = ring_matches(bar, DSP0_CMD_BASE, TEST_IOVA,
				      command_index) &&
		ring_matches(bar, DSP0_RESP_BASE,
			     TEST_IOVA + 4 * (uint64_t)PAGE_SIZE_4K,
			     response_index) &&
		only_dsp0_rings_published(bar, command_index, response_index);
	if (!publication_ok)
		goto cleanup;

	nanosleep(&wait_time, NULL);
	pages_ok = pages_unchanged(memory);
	ready_ok = all_dsps_ready(bar);
	dma_unchanged = mmio_read32(bar, DMA_CONTROL) == DMA_COLD_RESET;
	if (interrupted || !pages_ok || !ready_ok || !dma_unchanged ||
	    !only_dsp0_rings_published(bar, command_index, response_index))
		goto cleanup;

	result = EXIT_SUCCESS;

cleanup:
	if (rings_published && bar != MAP_FAILED) {
		restore_ok = clear_dsp0_rings(bar);
		if (!restore_ok)
			result = EXIT_FAILURE;
	}
	if (device >= 0 && bar != MAP_FAILED) {
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
	printf("  \"experiment\": \"011-official-ring-initializer\",\n");
	printf("  \"mapped_pages\": %d,\n", PAGE_COUNT);
	printf("  \"command_hardware_index_raw\": \"0x%08" PRIx32 "\",\n",
	       command_index_raw);
	printf("  \"command_synchronized_index\": %" PRIu32 ",\n",
	       command_index);
	printf("  \"response_hardware_index_raw\": \"0x%08" PRIx32 "\",\n",
	       response_index_raw);
	printf("  \"response_synchronized_index\": %" PRIu32 ",\n",
	       response_index);
	printf("  \"rings_published\": %s,\n",
	       rings_published ? "true" : "false");
	printf("  \"descriptor_words_written\": %d,\n",
	       rings_published ? 16 : 0);
	printf("  \"ring_control_writes\": %d,\n",
	       rings_published ? 4 : 0);
	printf("  \"dma_control_writes\": 0,\n");
	printf("  \"interrupt_register_writes\": 0,\n");
	printf("  \"command_entries_submitted\": 0,\n");
	printf("  \"publication_readback_ok\": %s,\n",
	       publication_ok ? "true" : "false");
	printf("  \"pages_unchanged\": %s,\n", pages_ok ? "true" : "false");
	printf("  \"all_dsps_ready\": %s,\n", ready_ok ? "true" : "false");
	printf("  \"dma_control_unchanged\": %s,\n",
	       dma_unchanged ? "true" : "false");
	printf("  \"rings_restored_to_zero\": %s,\n",
	       restore_ok ? "true" : "false");
	printf("  \"vfio_device_reset_called\": %s,\n",
	       reset_called ? "true" : "false");
	printf("  \"reset_recovered\": %s,\n",
	       reset_recovered ? "true" : "false");
	printf("  \"interrupted\": %s,\n", interrupted ? "true" : "false");
	printf("  \"success\": %s\n", result == EXIT_SUCCESS ? "true" : "false");
	printf("}\n");
	return result;
}
