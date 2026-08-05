#define _GNU_SOURCE
// SPDX-License-Identifier: GPL-2.0-only
/* Experiment 014: query resident firmware after the exact OCTO startup. */

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
#define DSP_COUNT 8
#define RING_PAGE_COUNT 64
#define PAGE_COUNT 65
#define RESPONSE_PAGE 64
#define DMA_CONTROL 0x2200
#define INTERRUPT_ENABLE 0x2204
#define INTERRUPT_ACK 0x2208
#define NOTIFICATION_CONTROL 0x2220
#define DMA_COLD_RESET 0x0001fe00
#define DMA_RESET_GLOBAL 0x0001fe01
#define DMA_GLOBAL_ONLY 0x00000001
#define DMA_ALL_DSPS 0x000001ff
#define CALLBACK_SHADOW 0xcccccccc
#define RESPONSE_SHADOW 0xccccccce
#define QUERY_SHADOW 0xcccccccf
#define QUERY_COMMAND 0x00260001
#define QUERY_RESPONSE_HEADER 0x800c0005
#define QUERY_RESPONSE_WORDS 5
#define POLL_COUNT 2000
#define CONNECT_COMMAND 0x00230002
#define CLOCK_COMMAND 0x00100002
#define SEQUENCE_COMMAND 0x00270001
#define SEQUENCE_RESPONSE_HEADER 0x800d0002

static const uint32_t dsp_banks[DSP_COUNT] = {
	0x2000, 0x2080, 0x2100, 0x2180,
	0x6000, 0x6080, 0x6100, 0x6180,
};
static const uint32_t boot_offsets[DSP_COUNT] = {
	0x01a4, 0x09a4, 0x11a4, 0x19a4,
	0x41a4, 0x49a4, 0x51a4, 0x59a4,
};

static volatile sig_atomic_t interrupted;

static void handle_signal(int signal_number)
{
	(void)signal_number;
	interrupted = 1;
}

static uint32_t read32(const void *bar, uint32_t offset)
{
	const volatile uint32_t *word =
		(const volatile uint32_t *)((const char *)bar + offset);
	uint32_t value = *word;

	__sync_synchronize();
	return value;
}

static void write32(void *bar, uint32_t offset, uint32_t value)
{
	volatile uint32_t *word = (volatile uint32_t *)((char *)bar + offset);

	*word = value;
	__sync_synchronize();
}

static bool all_ready(const void *bar)
{
	unsigned int dsp;

	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		if (!(read32(bar, boot_offsets[dsp]) & 1))
			return false;
	}
	return true;
}

static bool all_rings_zero(const void *bar)
{
	unsigned int dsp, ring, word;

	for (dsp = 0; dsp < DSP_COUNT; dsp++)
		for (ring = 0; ring < 2; ring++)
			for (word = 0; word < 16; word++)
				if (read32(bar, dsp_banks[dsp] + ring * 0x40 + word * 4))
					return false;
	return true;
}

static void initialize_ring(void *bar, uint32_t base, uint64_t iova,
			    uint32_t index)
{
	unsigned int page;

	write32(bar, base + 0x24, index);
	write32(bar, base + 0x20, index);
	for (page = 0; page < 4; page++) {
		uint64_t address = iova + page * (uint64_t)PAGE_SIZE_4K;

		write32(bar, base + page * 8, (uint32_t)address);
		write32(bar, base + page * 8 + 4, (uint32_t)(address >> 32));
	}
}

static void clear_rings(void *bar)
{
	unsigned int dsp, ring, word;

	for (dsp = 0; dsp < DSP_COUNT; dsp++)
		for (ring = 0; ring < 2; ring++)
			for (word = 0; word < 10; word++)
				write32(bar, dsp_banks[dsp] + ring * 0x40 + word * 4, 0);
}

static uint32_t *ring_entry(unsigned char *memory, unsigned int dsp,
			    unsigned int ring, uint32_t index)
{
	size_t page = dsp * 8 + ring * 4;
	return (uint32_t *)(memory + page * PAGE_SIZE_4K + index * 16);
}

static uint32_t day_and_time(void)
{
	uint64_t seconds = (uint64_t)time(NULL);
	uint32_t days = (uint32_t)(seconds / 86400);
	uint32_t minutes = (uint32_t)((seconds % 86400) / 60);

	return (days << 12) + minutes;
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
		.argsz = sizeof(bar_info), .index = VFIO_PCI_BAR0_REGION_INDEX,
	};
	const size_t memory_size = PAGE_COUNT * PAGE_SIZE_4K;
	struct timespec delay = { .tv_sec = 0, .tv_nsec = 1000000 };
	uint32_t indexes[DSP_COUNT][2] = {{0}}, raw;
	uint32_t response[QUERY_RESPONSE_WORDS] = {0};
	uint32_t selected_command = QUERY_COMMAND;
	uint32_t selected_header = QUERY_RESPONSE_HEADER;
	unsigned int selected_response_words = QUERY_RESPONSE_WORDS;
	uint32_t command_position = 0, response_position = 0;
	uint32_t *command_entry = NULL, *response_entry = NULL, *response_buffer;
	unsigned char *memory = MAP_FAILED, *snapshot = NULL;
	void *bar = MAP_FAILED;
	int container = -1, group = -1, device = -1;
	bool container_set = false, dma_mapped = false, startup = false;
	bool command_consumed = false, response_consumed = false;
	bool connect_first = false, connect_consumed = false;
	bool header_matches = false, memory_bounded = false, ready_after = false;
	bool restored = false, reset_recovered = false;
	unsigned int dsp, ring, poll = 0, connect_poll = 0, word;
	uint32_t interrupt_shadow = CALLBACK_SHADOW;
	uint32_t query_command_index = 0, query_response_index = 0;
	int result = EXIT_FAILURE;

	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	if (argc == 2 && strcmp(argv[1], "--connect") == 0)
		connect_first = true;
	else if (argc == 2 && strcmp(argv[1], "--connect-query-027") == 0) {
		connect_first = true;
		selected_command = SEQUENCE_COMMAND;
		selected_header = SEQUENCE_RESPONSE_HEADER;
		selected_response_words = 2;
	}
	else if (argc != 1) {
		fprintf(stderr, "usage: vfio_full_query [--connect|--connect-query-027]\n");
		return EXIT_FAILURE;
	}
	if (sysconf(_SC_PAGESIZE) != PAGE_SIZE_4K) {
		fprintf(stderr, "experiment-014: requires 4096-byte host pages\n");
		goto cleanup;
	}
	container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (container < 0 || ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
	    ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1) {
		perror("experiment-014: initialize VFIO");
		goto cleanup;
	}
	group = open(GROUP_PATH, O_RDWR | O_CLOEXEC);
	if (group < 0 || ioctl(group, VFIO_GROUP_GET_STATUS, &group_status) < 0 ||
	    !(group_status.flags & VFIO_GROUP_FLAGS_VIABLE) ||
	    ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0) {
		perror("experiment-014: open viable group");
		goto cleanup;
	}
	container_set = true;
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0 ||
	    ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info) < 0 ||
	    !(iommu_info.iova_pgsizes & PAGE_SIZE_4K)) {
		perror("experiment-014: configure IOMMU");
		goto cleanup;
	}

	memory = mmap(NULL, memory_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED || mlock(memory, memory_size) < 0) {
		perror("experiment-014: allocate locked pages");
		goto cleanup;
	}
	memset(memory, 0, RING_PAGE_COUNT * PAGE_SIZE_4K);
	memset(memory + RESPONSE_PAGE * PAGE_SIZE_4K, 0xa5, PAGE_SIZE_4K);
	response_buffer = (uint32_t *)(memory + RESPONSE_PAGE * PAGE_SIZE_4K);

	dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	dma_map.vaddr = (uintptr_t)memory;
	dma_map.iova = TEST_IOVA;
	dma_map.size = memory_size;
	if (ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
		perror("experiment-014: map 65 IOVA pages");
		goto cleanup;
	}
	dma_mapped = true;
	device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, DEVICE_NAME);
	if (device < 0 || ioctl(device, VFIO_DEVICE_GET_INFO, &device_info) < 0 ||
	    !(device_info.flags & VFIO_DEVICE_FLAGS_RESET) ||
	    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &bar_info) < 0 ||
	    bar_info.size != BAR0_SIZE || !(bar_info.flags & VFIO_REGION_INFO_FLAG_WRITE) ||
	    !(bar_info.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
		perror("experiment-014: validate device");
		goto cleanup;
	}
	bar = mmap(NULL, BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		   device, bar_info.offset);
	if (bar == MAP_FAILED) {
		perror("experiment-014: map BAR0");
		goto cleanup;
	}
	if (!all_rings_zero(bar) || !all_ready(bar) ||
	    read32(bar, DMA_CONTROL) != DMA_COLD_RESET ||
	    read32(bar, INTERRUPT_ENABLE) != 0) {
		fprintf(stderr, "experiment-014: baseline precondition changed\n");
		goto cleanup;
	}

	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		for (ring = 0; ring < 2; ring++) {
			raw = read32(bar, dsp_banks[dsp] + ring * 0x40 + 0x28);
			indexes[dsp][ring] = raw < 1024 ? raw : 0;
		}
	}
	if (connect_first) {
		for (dsp = 0; dsp < DSP_COUNT; dsp++) {
			if (indexes[dsp][0] != 0 || indexes[dsp][1] != 0) {
				fprintf(stderr, "experiment-015: connect mode requires zero indexes\n");
				goto cleanup;
			}
			command_entry = ring_entry(memory, dsp, 0, 0);
			command_entry[0] = CONNECT_COMMAND;
			command_entry[1] = 1;
		}
		command_entry = ring_entry(memory, 0, 0, 1);
		command_entry[0] = CLOCK_COMMAND;
		command_entry[1] = day_and_time();
		query_command_index = 2;
	} else {
		query_command_index = indexes[0][0];
	}
	query_response_index = indexes[0][1];
	command_entry = ring_entry(memory, 0, 0, query_command_index);
	response_entry = ring_entry(memory, 0, 1, query_response_index);
	command_entry[0] = selected_command;
	response_entry[0] = 0x80000000 | selected_response_words;
	response_entry[2] = (uint32_t)(TEST_IOVA + RESPONSE_PAGE * (uint64_t)PAGE_SIZE_4K);
	response_entry[3] = (uint32_t)((TEST_IOVA +
		RESPONSE_PAGE * (uint64_t)PAGE_SIZE_4K) >> 32);
	snapshot = malloc(memory_size);
	if (!snapshot) {
		perror("experiment-014: allocate snapshot");
		goto cleanup;
	}
	memcpy(snapshot, memory, memory_size);
	__sync_synchronize();

	startup = true;
	write32(bar, NOTIFICATION_CONTROL, 0);
	write32(bar, INTERRUPT_ENABLE, 0);
	write32(bar, INTERRUPT_ACK, 0);
	write32(bar, DMA_CONTROL, DMA_RESET_GLOBAL);
	write32(bar, DMA_CONTROL, DMA_GLOBAL_ONLY);
	write32(bar, INTERRUPT_ACK, 0xffffffff);
	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		for (ring = 0; ring < 2; ring++) {
			uint32_t base = dsp_banks[dsp] + ring * 0x40;
			uint64_t iova = TEST_IOVA +
				(dsp * 8 + ring * 4) * (uint64_t)PAGE_SIZE_4K;
			initialize_ring(bar, base, iova, indexes[dsp][ring]);
		}
		write32(bar, DMA_CONTROL, (1U << (dsp + 2)) - 1);
	}
	if (connect_first) {
		for (dsp = 0; dsp < DSP_COUNT; dsp++) {
			uint32_t physical_command_bit = 1U << (dsp * 4);

			write32(bar, dsp_banks[dsp] + 0x24, 1);
			write32(bar, dsp_banks[dsp] + 0x20, 1);
			interrupt_shadow |= physical_command_bit;
			write32(bar, INTERRUPT_ENABLE, interrupt_shadow);
		}
		write32(bar, dsp_banks[0] + 0x24, 2);
		write32(bar, dsp_banks[0] + 0x20, 2);
		write32(bar, INTERRUPT_ENABLE, interrupt_shadow);
		for (connect_poll = 0; connect_poll < POLL_COUNT && !interrupted;
		     connect_poll++) {
			connect_consumed = true;
			for (dsp = 0; dsp < DSP_COUNT; dsp++) {
				uint32_t expected = dsp == 0 ? 2 : 1;

				if (read32(bar, dsp_banks[dsp] + 0x28) != expected)
					connect_consumed = false;
			}
			if (connect_consumed)
				break;
			nanosleep(&delay, NULL);
		}
		if (!connect_consumed)
			goto cleanup;
	}

	response_position = (query_response_index + 1) % 1024;
	write32(bar, dsp_banks[0] + 0x40 + 0x24, response_position);
	write32(bar, dsp_banks[0] + 0x40 + 0x20, response_position);
	interrupt_shadow |= 2;
	write32(bar, INTERRUPT_ENABLE,
		connect_first ? interrupt_shadow : RESPONSE_SHADOW);
	command_position = (query_command_index + 1) % 1024;
	write32(bar, dsp_banks[0] + 0x24, command_position);
	write32(bar, dsp_banks[0] + 0x20, command_position);
	interrupt_shadow |= 1;
	write32(bar, INTERRUPT_ENABLE,
		connect_first ? interrupt_shadow : QUERY_SHADOW);

	for (poll = 0; poll < POLL_COUNT && !interrupted; poll++) {
		uint32_t command_read = read32(bar, dsp_banks[0] + 0x28);
		uint32_t response_read = read32(bar, dsp_banks[0] + 0x40 + 0x28);

		__sync_synchronize();
		if (command_read == command_position &&
		    response_read == response_position && response_buffer[0] != 0xa5a5a5a5)
			break;
		nanosleep(&delay, NULL);
	}
	__sync_synchronize();
	for (word = 0; word < selected_response_words; word++)
		response[word] = response_buffer[word];
	command_consumed = read32(bar, dsp_banks[0] + 0x28) == command_position;
	response_consumed = read32(bar, dsp_banks[0] + 0x40 + 0x28) == response_position;
	header_matches = response[0] == selected_header;
	ready_after = all_ready(bar);
	memory_bounded = memcmp(memory, snapshot, RESPONSE_PAGE * PAGE_SIZE_4K) == 0 &&
		memcmp(memory + RESPONSE_PAGE * PAGE_SIZE_4K + selected_response_words * 4,
		       snapshot + RESPONSE_PAGE * PAGE_SIZE_4K + selected_response_words * 4,
		       PAGE_SIZE_4K - selected_response_words * 4) == 0;
	if (!interrupted && command_consumed && response_consumed && header_matches &&
	    ready_after && memory_bounded)
		result = EXIT_SUCCESS;

cleanup:
	if (startup && bar != MAP_FAILED) {
		write32(bar, INTERRUPT_ENABLE, 0);
		write32(bar, INTERRUPT_ACK, 0xffffffff);
		write32(bar, DMA_CONTROL, DMA_RESET_GLOBAL);
		write32(bar, DMA_CONTROL, DMA_GLOBAL_ONLY);
		clear_rings(bar);
		write32(bar, DMA_CONTROL, DMA_COLD_RESET);
		restored = all_rings_zero(bar) && read32(bar, DMA_CONTROL) == DMA_COLD_RESET;
		if (!restored)
			result = EXIT_FAILURE;
	}
	if (device >= 0 && bar != MAP_FAILED) {
		if (ioctl(device, VFIO_DEVICE_RESET) == 0)
			reset_recovered = all_rings_zero(bar) &&
				read32(bar, DMA_CONTROL) == DMA_COLD_RESET && all_ready(bar);
		if (!reset_recovered)
			result = EXIT_FAILURE;
	}

	printf("{\n");
	printf("  \"experiment\": \"%s\",\n",
	       selected_command == SEQUENCE_COMMAND ? "016-connect-then-query-027" :
	       connect_first ? "015-connect-then-query-026" :
	       "014-full-start-query-026");
	printf("  \"command\": \"0x%08x\",\n", selected_command);
	printf("  \"expected_response_header\": \"0x%08x\",\n", selected_header);
	printf("  \"callback_shadow\": \"0x%08x\",\n", CALLBACK_SHADOW);
	printf("  \"query_shadow\": \"0x%08x\",\n", QUERY_SHADOW);
	printf("  \"connect_sequence_requested\": %s,\n",
	       connect_first ? "true" : "false");
	printf("  \"connect_commands_consumed\": %s,\n",
	       connect_consumed ? "true" : "false");
	printf("  \"connect_polls_ms\": %u,\n", connect_poll);
	printf("  \"polls_ms\": %u,\n", poll < POLL_COUNT ? poll : POLL_COUNT);
	printf("  \"command_consumed\": %s,\n", command_consumed ? "true" : "false");
	printf("  \"response_consumed\": %s,\n", response_consumed ? "true" : "false");
	printf("  \"response_words\": [");
	for (word = 0; word < selected_response_words; word++)
		printf("\"0x%08x\"%s", response[word],
		       word + 1 == selected_response_words ? "" : ", ");
	printf("],\n");
	printf("  \"reply_header_matches\": %s,\n", header_matches ? "true" : "false");
	printf("  \"writes_confined_to_response_prefix\": %s,\n", memory_bounded ? "true" : "false");
	printf("  \"all_dsps_ready\": %s,\n", ready_after ? "true" : "false");
	printf("  \"explicit_restore_succeeded\": %s,\n", restored ? "true" : "false");
	printf("  \"vfio_reset_recovered\": %s,\n", reset_recovered ? "true" : "false");
	printf("  \"success\": %s\n", result == EXIT_SUCCESS ? "true" : "false");
	printf("}\n");

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
	return result;
}
