// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experiment 009/010: issue one response-backed, read-only query recovered
 * from the official UAD2Pcie driver. Experiment 010 additionally mirrors the
 * driver's DSP0 interrupt-gate sequence. All DMA is confined by VFIO's IOMMU
 * mapping.
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
#define PAGE_COUNT 7
#define DSP0_CMD_BASE 0x2000
#define DSP0_RESP_BASE 0x2040
#define DMA_CONTROL 0x2200
#define INTERRUPT_ENABLE 0x2204
#define INTERRUPT_ARM 0x2208
#define DMA_COLD_RESET 0x0001fe00
#define DMA_RESET_GLOBAL 0x0001fe01
#define DMA_GLOBAL_ONLY 0x00000001
#define DMA_GLOBAL_DSP0 0x00000003
#define QUERY_COMMAND 0x00260001
#define QUERY_RESPONSE_HEADER 0x800c0005
#define QUERY_RESPONSE_WORDS 5
#define RESPONSE_CANARY 0xa5
#define POLL_COUNT 2000

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

static void write_page_address(void *bar, uint32_t offset, uint64_t iova)
{
	mmio_write32(bar, offset, (uint32_t)iova);
	mmio_write32(bar, offset + 4, (uint32_t)(iova >> 32));
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

static bool bytes_equal(const unsigned char *left,
			const unsigned char *right, size_t length)
{
	return memcmp(left, right, length) == 0;
}

int main(int argc, char **argv)
{
	struct sigaction action = { .sa_handler = handle_signal };
	struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
	struct vfio_iommu_type1_info iommu_info = { .argsz = sizeof(iommu_info) };
	struct vfio_iommu_type1_dma_map dma_map = { .argsz = sizeof(dma_map) };
	struct vfio_iommu_type1_dma_unmap dma_unmap = {
		.argsz = sizeof(dma_unmap),
	};
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
	struct vfio_region_info bar_info = {
		.argsz = sizeof(bar_info),
		.index = VFIO_PCI_BAR0_REGION_INDEX,
	};
	struct timespec poll_delay = { .tv_sec = 0, .tv_nsec = 1000000 };
	long page_size = sysconf(_SC_PAGESIZE);
	size_t memory_size = 0;
	unsigned char *memory = MAP_FAILED;
	unsigned char *snapshot = NULL;
	uint32_t *command_ring, *response_ring, *command_buffer;
	uint32_t *response_buffer = NULL;
	void *bar = MAP_FAILED;
	int container = -1, group = -1, device = -1;
	bool container_set = false, dma_mapped = false;
	bool descriptors_published = false, controls_modified = false;
	bool dma_modified = false, reset_called = false, reset_recovered = false;
	bool dma_restored = false, descriptors_restored = false;
	bool interrupt_gates_requested = false, interrupt_gates_modified = false;
	bool interrupt_gates_restored = false;
	bool command_consumed = false, response_consumed = false;
	bool command_memory_unchanged = false, response_tail_unchanged = false;
	bool reply_matches = false, ready_after = false;
	uint32_t command_position = 0, response_position = 0;
	uint32_t interrupt_enable_readback = 0;
	uint32_t response_words[QUERY_RESPONSE_WORDS] = { 0 };
	unsigned int poll = 0, page;
	int result = EXIT_FAILURE;
	const char *experiment = "009-official-query-026";

	if (argc == 2 && strcmp(argv[1], "--with-interrupt-gates") == 0) {
		interrupt_gates_requested = true;
		experiment = "010-official-query-026-with-interrupt-gates";
	} else if (argc != 1) {
		fprintf(stderr, "usage: %s [--with-interrupt-gates]\n", argv[0]);
		return EXIT_FAILURE;
	}

	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);

	if (page_size <= 0 || TEST_IOVA % (uint64_t)page_size != 0) {
		fprintf(stderr, "experiment-009: unsupported page size\n");
		goto cleanup;
	}
	memory_size = PAGE_COUNT * (size_t)page_size;

	container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (container < 0 ||
	    ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
	    ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1) {
		perror("experiment-009: initialize VFIO container");
		goto cleanup;
	}
	group = open(GROUP_PATH, O_RDWR | O_CLOEXEC);
	if (group < 0 ||
	    ioctl(group, VFIO_GROUP_GET_STATUS, &group_status) < 0 ||
	    !(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		perror("experiment-009: open viable group 16");
		goto cleanup;
	}
	if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0) {
		perror("experiment-009: set container");
		goto cleanup;
	}
	container_set = true;
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0 ||
	    ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info) < 0 ||
	    !(iommu_info.iova_pgsizes & (uint64_t)page_size)) {
		perror("experiment-009: configure Type 1 IOMMU");
		goto cleanup;
	}

	memory = mmap(NULL, memory_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED || mlock(memory, memory_size) < 0) {
		perror("experiment-009: allocate locked pages");
		goto cleanup;
	}
	snapshot = malloc(memory_size);
	if (!snapshot) {
		perror("experiment-009: allocate snapshot");
		goto cleanup;
	}
	memset(memory, 0, memory_size);
	command_ring = (uint32_t *)memory;
	response_ring = (uint32_t *)(memory + 4 * page_size);
	command_buffer = (uint32_t *)(memory + 5 * page_size);
	response_buffer = (uint32_t *)(memory + 6 * page_size);
	memset(response_buffer, RESPONSE_CANARY, (size_t)page_size);

	/* Short commands are inline; only the driver's block helper uses a ref. */
	command_ring[0] = QUERY_COMMAND;
	command_ring[1] = 0;
	command_ring[2] = 0;
	command_ring[3] = 0;
	response_ring[0] = 0x80000000 | QUERY_RESPONSE_WORDS;
	response_ring[1] = 0;
	response_ring[2] = (uint32_t)(TEST_IOVA + 6 * (uint64_t)page_size);
	response_ring[3] = (uint32_t)((TEST_IOVA +
		6 * (uint64_t)page_size) >> 32);
	command_buffer[0] = 0;
	memcpy(snapshot, memory, memory_size);
	__sync_synchronize();

	dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	dma_map.vaddr = (uintptr_t)memory;
	dma_map.iova = TEST_IOVA;
	dma_map.size = memory_size;
	if (ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
		perror("experiment-009: map seven IOVA pages");
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
		perror("experiment-009: validate VFIO device and BAR0");
		goto cleanup;
	}
	bar = mmap(NULL, bar_info.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   device, bar_info.offset);
	if (bar == MAP_FAILED) {
		perror("experiment-009: map BAR0");
		goto cleanup;
	}

	if (!all_ring_words_zero(bar) || !all_dsps_ready(bar) ||
	    mmio_read32(bar, DMA_CONTROL) != DMA_COLD_RESET ||
	    mmio_read32(bar, INTERRUPT_ENABLE) != 0) {
		fprintf(stderr, "experiment-009: baseline precondition changed\n");
		goto cleanup;
	}

	for (page = 0; page < 4; page++)
		write_page_address(bar, DSP0_CMD_BASE + page * 8,
				   TEST_IOVA + page * (uint64_t)page_size);
	write_page_address(bar, DSP0_RESP_BASE,
			   TEST_IOVA + 4 * (uint64_t)page_size);
	descriptors_published = true;

	mmio_write32(bar, DMA_CONTROL, DMA_RESET_GLOBAL);
	dma_modified = true;
	if (mmio_read32(bar, DMA_CONTROL) != DMA_RESET_GLOBAL)
		goto cleanup;
	mmio_write32(bar, DMA_CONTROL, DMA_GLOBAL_ONLY);
	if (mmio_read32(bar, DMA_CONTROL) != DMA_GLOBAL_ONLY)
		goto cleanup;
	mmio_write32(bar, DMA_CONTROL, DMA_GLOBAL_DSP0);
	if (mmio_read32(bar, DMA_CONTROL) != DMA_GLOBAL_DSP0)
		goto cleanup;

	if (interrupt_gates_requested) {
		/* Official initialization arms DSP0 vectors 2, 3, and 4. */
		mmio_write32(bar, INTERRUPT_ARM, 0x00000004);
		interrupt_gates_modified = true;
		mmio_write32(bar, INTERRUPT_ENABLE, 0x00000004);
		mmio_write32(bar, INTERRUPT_ARM, 0x00000008);
		mmio_write32(bar, INTERRUPT_ENABLE, 0x0000000c);
		mmio_write32(bar, INTERRUPT_ARM, 0x00000010);
		mmio_write32(bar, INTERRUPT_ENABLE, 0x0000001c);
		if (mmio_read32(bar, INTERRUPT_ENABLE) != 0x0000001c)
			goto cleanup;
	}

	/* The official driver queues the response DMA object before the command. */
	mmio_write32(bar, DSP0_RESP_BASE + 0x24, 1);
	controls_modified = true;
	mmio_write32(bar, DSP0_RESP_BASE + 0x20, 1);
	if (interrupt_gates_requested)
		mmio_write32(bar, INTERRUPT_ENABLE, 0x0000001e);
	mmio_write32(bar, DSP0_CMD_BASE + 0x24, 1);
	mmio_write32(bar, DSP0_CMD_BASE + 0x20, 1);
	if (interrupt_gates_requested)
		mmio_write32(bar, INTERRUPT_ENABLE, 0x0000001f);
	interrupt_enable_readback = mmio_read32(bar, INTERRUPT_ENABLE);

	for (poll = 0; poll < POLL_COUNT && !interrupted; poll++) {
		command_position = mmio_read32(bar, DSP0_CMD_BASE + 0x28);
		response_position = mmio_read32(bar, DSP0_RESP_BASE + 0x28);
		__sync_synchronize();
		if (command_position == 1 && response_position == 1 &&
		    response_buffer[0] != 0xa5a5a5a5)
			break;
		nanosleep(&poll_delay, NULL);
	}
	__sync_synchronize();
	for (page = 0; page < QUERY_RESPONSE_WORDS; page++)
		response_words[page] = response_buffer[page];
	command_consumed = command_position == 1;
	response_consumed = response_position == 1;
	reply_matches = response_words[0] == QUERY_RESPONSE_HEADER;
	ready_after = all_dsps_ready(bar);
	command_memory_unchanged =
		bytes_equal(memory, snapshot, 6 * (size_t)page_size);
	response_tail_unchanged = bytes_equal(
		memory + 6 * page_size + QUERY_RESPONSE_WORDS * sizeof(uint32_t),
		snapshot + 6 * page_size +
			QUERY_RESPONSE_WORDS * sizeof(uint32_t),
		(size_t)page_size - QUERY_RESPONSE_WORDS * sizeof(uint32_t));

	if (!interrupted && command_consumed && response_consumed &&
	    reply_matches && ready_after && command_memory_unchanged &&
	    response_tail_unchanged)
		result = EXIT_SUCCESS;

cleanup:
	if (dma_modified && bar != MAP_FAILED) {
		mmio_write32(bar, DMA_CONTROL, DMA_COLD_RESET);
		dma_restored = mmio_read32(bar, DMA_CONTROL) == DMA_COLD_RESET;
		if (!dma_restored)
			result = EXIT_FAILURE;
	}
	if (interrupt_gates_modified && bar != MAP_FAILED) {
		mmio_write32(bar, INTERRUPT_ENABLE, 0);
		mmio_write32(bar, INTERRUPT_ARM, 0);
		interrupt_gates_restored =
			mmio_read32(bar, INTERRUPT_ENABLE) == 0;
		if (!interrupt_gates_restored)
			result = EXIT_FAILURE;
	}
	if (controls_modified && bar != MAP_FAILED) {
		mmio_write32(bar, DSP0_CMD_BASE + 0x24, 0);
		mmio_write32(bar, DSP0_CMD_BASE + 0x20, 0);
		mmio_write32(bar, DSP0_RESP_BASE + 0x24, 0);
		mmio_write32(bar, DSP0_RESP_BASE + 0x20, 0);
	}
	if (descriptors_published && bar != MAP_FAILED) {
		for (page = 0; page < 4; page++)
			write_page_address(bar, DSP0_CMD_BASE + page * 8, 0);
		write_page_address(bar, DSP0_RESP_BASE, 0);
		descriptors_restored = true;
		for (page = 0; page < 4; page++) {
			if (mmio_read32(bar, DSP0_CMD_BASE + page * 8) != 0 ||
			    mmio_read32(bar, DSP0_CMD_BASE + page * 8 + 4) != 0)
				descriptors_restored = false;
		}
		if (mmio_read32(bar, DSP0_RESP_BASE) != 0 ||
		    mmio_read32(bar, DSP0_RESP_BASE + 4) != 0)
			descriptors_restored = false;
		if (!descriptors_restored)
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
	free(snapshot);
	if (container_set)
		ioctl(group, VFIO_GROUP_UNSET_CONTAINER);
	if (group >= 0)
		close(group);
	if (container >= 0)
		close(container);

	printf("{\n");
	printf("  \"experiment\": \"%s\",\n", experiment);
	printf("  \"command\": \"0x%08x\",\n", QUERY_COMMAND);
	printf("  \"expected_response_header\": \"0x%08x\",\n",
	       QUERY_RESPONSE_HEADER);
	printf("  \"command_position\": %u,\n", command_position);
	printf("  \"response_position\": %u,\n", response_position);
	printf("  \"interrupt_gates_requested\": %s,\n",
	       interrupt_gates_requested ? "true" : "false");
	printf("  \"interrupt_enable_readback\": \"0x%08x\",\n",
	       interrupt_enable_readback);
	printf("  \"polls_ms\": %u,\n", poll < POLL_COUNT ? poll : POLL_COUNT);
	printf("  \"response_words\": [\n");
	for (page = 0; page < QUERY_RESPONSE_WORDS; page++)
		printf("    \"0x%08x\"%s\n", response_words[page],
		       page + 1 == QUERY_RESPONSE_WORDS ? "" : ",");
	printf("  ],\n");
	printf("  \"command_consumed\": %s,\n",
	       command_consumed ? "true" : "false");
	printf("  \"response_consumed\": %s,\n",
	       response_consumed ? "true" : "false");
	printf("  \"reply_header_matches\": %s,\n",
	       reply_matches ? "true" : "false");
	printf("  \"command_memory_unchanged\": %s,\n",
	       command_memory_unchanged ? "true" : "false");
	printf("  \"response_tail_unchanged\": %s,\n",
	       response_tail_unchanged ? "true" : "false");
	printf("  \"all_dsps_ready\": %s,\n", ready_after ? "true" : "false");
	printf("  \"dma_restored_to_cold_reset\": %s,\n",
	       dma_restored ? "true" : "false");
	printf("  \"descriptors_restored_to_zero\": %s,\n",
	       descriptors_restored ? "true" : "false");
	printf("  \"interrupt_gates_restored_to_zero\": %s,\n",
	       interrupt_gates_restored ? "true" : "false");
	printf("  \"vfio_device_reset_called\": %s,\n",
	       reset_called ? "true" : "false");
	printf("  \"reset_recovered\": %s,\n",
	       reset_recovered ? "true" : "false");
	printf("  \"interrupted\": %s,\n", interrupted ? "true" : "false");
	printf("  \"success\": %s\n", result == EXIT_SUCCESS ? "true" : "false");
	printf("}\n");
	return result;
}
