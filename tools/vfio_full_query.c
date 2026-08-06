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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GROUP_PATH "/dev/vfio/16"
#define DEVICE_NAME "0000:03:00.0"
#define BAR0_SIZE UINT64_C(0x10000)
#define TEST_IOVA UINT64_C(0x10000000)
#define PAGE_SIZE_4K 4096
#define DSP_COUNT 8
#define RING_PAGE_COUNT 64
#define PAGE_COUNT 66
#define RESPONSE_PAGE 64
#define RESOURCE_PAGE 65
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
#define BILL_RESPONSE_HEADER 0x80070004
#define BILL_TARGET_BYTES 460
#define BILL_TARGET_DWORDS 115
#define BILL_TARGET_COMMAND 0x00010073
#define BILL_TARGET_OFFSET 0x000e0000
#define BILL_MAGIC 0x6c6c6942
#define BILL_RESOURCE_ID 0x0000012b
#define BILL_BODY_BYTES 432
#define BILL_REPLACEMENT_DWORDS 96

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
	bool bill_accepted = false;
	bool restored = false, reset_recovered = false;
	bool post_official = false;
	bool bill_probe = false;
	bool bill_mutation = false;
	bool bill_isolation = false;
	unsigned long bill_mutation_offset = 0;
	unsigned int target_dsp = 0;
	bool non_target_indices_unchanged = false;
	unsigned int dsp, ring, poll = 0, connect_poll = 0, word;
	uint32_t interrupt_shadow = CALLBACK_SHADOW;
	uint32_t response_interrupt_shadow = 0, final_interrupt_shadow = 0;
	uint32_t query_command_index = 0, query_response_index = 0;
	int resource_fd = -1, result = EXIT_FAILURE;

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
	else if (argc == 2 && strcmp(argv[1], "--post-official") == 0)
		post_official = true;
	else if (argc == 3 && strcmp(argv[1], "--post-official-bill") == 0) {
		post_official = true;
		bill_probe = true;
		selected_header = BILL_RESPONSE_HEADER;
		selected_response_words = 4;
	}
	else if (argc == 4 && strcmp(argv[1], "--post-official-bill-flip") == 0) {
		char *end = NULL;

		post_official = true;
		bill_probe = true;
		bill_mutation = true;
		selected_header = BILL_RESPONSE_HEADER;
		selected_response_words = 4;
		errno = 0;
		bill_mutation_offset = strtoul(argv[3], &end, 10);
		if (errno || !end || *end != '\0' ||
		    (bill_mutation_offset != 28 && bill_mutation_offset != 75 &&
		     bill_mutation_offset != 76 && bill_mutation_offset != 123 &&
		     bill_mutation_offset != 124 && bill_mutation_offset != 459)) {
			fprintf(stderr, "experiment-029: mutation offset is not approved\n");
			return EXIT_FAILURE;
		}
	}
	else if (argc == 4 && strcmp(argv[1], "--post-official-bill-dsp") == 0) {
		char *end = NULL;
		unsigned long parsed;

		post_official = true;
		bill_probe = true;
		bill_isolation = true;
		selected_header = BILL_RESPONSE_HEADER;
		selected_response_words = 4;
		errno = 0;
		parsed = strtoul(argv[3], &end, 10);
		if (errno || !end || *end != '\0' || parsed >= DSP_COUNT) {
			fprintf(stderr, "experiment-030: target DSP is invalid\n");
			return EXIT_FAILURE;
		}
		target_dsp = (unsigned int)parsed;
	}
	else if (argc != 1) {
		fprintf(stderr, "usage: vfio_full_query [--connect|--connect-query-027|--post-official|--post-official-bill exact-command-target|--post-official-bill-flip exact-command-target {28|75|76|123|124|459}|--post-official-bill-dsp exact-command-target {0..7}]\n");
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
	memset(memory + RESPONSE_PAGE * PAGE_SIZE_4K,
	       bill_probe ? 0 : 0xa5, PAGE_SIZE_4K);
	response_buffer = (uint32_t *)(memory + RESPONSE_PAGE * PAGE_SIZE_4K);
	if (bill_probe) {
		struct stat resource_stat;
		uint32_t *resource = (uint32_t *)(memory +
			RESOURCE_PAGE * PAGE_SIZE_4K);
		size_t received = 0;

		resource_fd = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (resource_fd < 0 || fstat(resource_fd, &resource_stat) < 0 ||
		    !S_ISREG(resource_stat.st_mode) ||
		    resource_stat.st_size != BILL_TARGET_BYTES) {
			fprintf(stderr, "experiment-028: refusing unexpected Bill target\n");
			goto cleanup;
		}
		while (received < BILL_TARGET_BYTES) {
			ssize_t chunk = read(resource_fd,
				(unsigned char *)resource + received,
				BILL_TARGET_BYTES - received);

			if (chunk <= 0) {
				fprintf(stderr,
					"experiment-028: could not read exact Bill target\n");
				goto cleanup;
			}
			received += (size_t)chunk;
		}
		if (resource[0] != BILL_TARGET_COMMAND ||
		    resource[1] != BILL_TARGET_OFFSET ||
		    resource[2] != BILL_MAGIC || resource[3] != BILL_RESOURCE_ID ||
		    resource[4] != 0 || resource[5] != BILL_BODY_BYTES ||
		    resource[6] != BILL_REPLACEMENT_DWORDS) {
			fprintf(stderr, "experiment-028: Bill target structure mismatch\n");
			goto cleanup;
		}
		if (bill_mutation)
			((unsigned char *)resource)[bill_mutation_offset] ^= 1;
	}

	dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	dma_map.vaddr = (uintptr_t)memory;
	dma_map.iova = TEST_IOVA;
	dma_map.size = memory_size;
	if (ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
		perror("experiment-014: map IOVA pages");
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
	if (!all_ready(bar) || read32(bar, DMA_CONTROL) != DMA_COLD_RESET ||
	    read32(bar, INTERRUPT_ENABLE) != 0) {
		fprintf(stderr, "experiment-014: baseline precondition changed\n");
		goto cleanup;
	}
	if (!post_official && !all_rings_zero(bar)) {
		fprintf(stderr, "experiment-014: baseline rings are not empty\n");
		goto cleanup;
	}
	if (post_official) {
		/*
		 * The reference VM's DMA mappings no longer exist. With DMA held in
		 * cold reset, discard only its stale page addresses and host pointers.
		 * The global DMA reset below synchronizes hardware read indices.
		 */
		clear_rings(bar);
	}

	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		for (ring = 0; ring < 2; ring++) {
			raw = read32(bar, dsp_banks[dsp] + ring * 0x40 + 0x28);
			indexes[dsp][ring] = post_official ? 0 : (raw < 1024 ? raw : 0);
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
		query_command_index = indexes[target_dsp][0];
	}
	query_response_index = indexes[target_dsp][1];
	command_entry = ring_entry(memory, target_dsp, 0, query_command_index);
	response_entry = ring_entry(memory, target_dsp, 1, query_response_index);
	if (bill_probe) {
		uint64_t resource_iova = TEST_IOVA +
			RESOURCE_PAGE * (uint64_t)PAGE_SIZE_4K;

		command_entry[0] = 0x80000000U | BILL_TARGET_DWORDS;
		command_entry[2] = (uint32_t)resource_iova;
		command_entry[3] = (uint32_t)(resource_iova >> 32);
	} else {
		command_entry[0] = selected_command;
	}
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
	write32(bar, dsp_banks[target_dsp] + 0x40 + 0x24, response_position);
	write32(bar, dsp_banks[target_dsp] + 0x40 + 0x20, response_position);
	interrupt_shadow |= 2U << (target_dsp * 4);
	response_interrupt_shadow = interrupt_shadow;
	write32(bar, INTERRUPT_ENABLE, response_interrupt_shadow);
	command_position = (query_command_index + 1) % 1024;
	write32(bar, dsp_banks[target_dsp] + 0x24, command_position);
	write32(bar, dsp_banks[target_dsp] + 0x20, command_position);
	interrupt_shadow |= 1U << (target_dsp * 4);
	final_interrupt_shadow = interrupt_shadow;
	write32(bar, INTERRUPT_ENABLE, final_interrupt_shadow);

	for (poll = 0; poll < (bill_probe ? 6000U : POLL_COUNT) && !interrupted;
	     poll++) {
		uint32_t command_read = read32(bar, dsp_banks[target_dsp] + 0x28);
		uint32_t response_read = read32(bar,
			dsp_banks[target_dsp] + 0x40 + 0x28);

		__sync_synchronize();
		if (command_read == command_position &&
		    response_read == response_position &&
		    response_buffer[0] != (bill_probe ? 0 : 0xa5a5a5a5))
			break;
		nanosleep(&delay, NULL);
	}
	__sync_synchronize();
	for (word = 0; word < selected_response_words; word++)
		response[word] = response_buffer[word];
	command_consumed = read32(bar, dsp_banks[target_dsp] + 0x28) ==
		command_position;
	response_consumed = read32(bar,
		dsp_banks[target_dsp] + 0x40 + 0x28) == response_position;
	non_target_indices_unchanged = true;
	for (dsp = 0; bill_isolation && dsp < DSP_COUNT; dsp++) {
		uint32_t command_base, response_base;

		if (dsp == target_dsp)
			continue;
		command_base = dsp_banks[dsp];
		response_base = command_base + 0x40;
		if (read32(bar, command_base + 0x20) != indexes[dsp][0] ||
		    read32(bar, command_base + 0x24) != indexes[dsp][0] ||
		    read32(bar, command_base + 0x28) != indexes[dsp][0] ||
		    read32(bar, response_base + 0x20) != indexes[dsp][1] ||
		    read32(bar, response_base + 0x24) != indexes[dsp][1] ||
		    read32(bar, response_base + 0x28) != indexes[dsp][1])
			non_target_indices_unchanged = false;
	}
	header_matches = response[0] == selected_header;
	bill_accepted = bill_probe && header_matches && response[1] == 0;
	ready_after = all_ready(bar);
	memory_bounded = memcmp(memory, snapshot, RESPONSE_PAGE * PAGE_SIZE_4K) == 0 &&
		memcmp(memory + RESPONSE_PAGE * PAGE_SIZE_4K + selected_response_words * 4,
		       snapshot + RESPONSE_PAGE * PAGE_SIZE_4K + selected_response_words * 4,
		       PAGE_SIZE_4K - selected_response_words * 4) == 0 &&
		memcmp(memory + RESOURCE_PAGE * PAGE_SIZE_4K,
		       snapshot + RESOURCE_PAGE * PAGE_SIZE_4K, PAGE_SIZE_4K) == 0;
	if (!interrupted && command_consumed && response_consumed && header_matches &&
	    ready_after && memory_bounded &&
	    (!bill_probe || (bill_mutation ? !bill_accepted : bill_accepted)) &&
	    non_target_indices_unchanged)
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
	       bill_mutation ? "029-post-official-bill-integrity" :
	       bill_isolation ? "030-eight-dsp-bill-isolation" :
	       bill_probe ? "028-post-official-bill-12b" :
	       post_official ? "027-post-official-query-026" :
	       connect_first ? "015-connect-then-query-026" :
	       "014-full-start-query-026");
	printf("  \"post_official_state_adopted\": %s,\n",
	       post_official ? "true" : "false");
	printf("  \"bill_target_submitted\": %s,\n",
	       bill_probe ? "true" : "false");
	printf("  \"target_dsp\": %u,\n", target_dsp);
	printf("  \"bill_target_exact\": %s,\n",
	       bill_probe && !bill_mutation ? "true" : "false");
	if (bill_mutation)
		printf("  \"single_bit_mutation_offset\": %lu,\n",
		       bill_mutation_offset);
	printf("  \"bill_accepted\": %s,\n",
	       bill_accepted ? "true" : "false");
	printf("  \"integrity_rejection_observed\": %s,\n",
	       bill_mutation && header_matches && !bill_accepted ? "true" : "false");
	printf("  \"non_target_ring_indices_unchanged\": %s,\n",
	       non_target_indices_unchanged ? "true" : "false");
	printf("  \"command\": \"0x%08x\",\n",
	       bill_probe ? BILL_TARGET_COMMAND : selected_command);
	printf("  \"expected_response_header\": \"0x%08x\",\n", selected_header);
	printf("  \"callback_shadow\": \"0x%08x\",\n", CALLBACK_SHADOW);
	printf("  \"dsp0_response_shadow_constant\": \"0x%08x\",\n",
	       RESPONSE_SHADOW);
	printf("  \"dsp0_query_shadow_constant\": \"0x%08x\",\n",
	       QUERY_SHADOW);
	printf("  \"response_interrupt_shadow\": \"0x%08x\",\n",
	       response_interrupt_shadow);
	printf("  \"final_interrupt_shadow\": \"0x%08x\",\n",
	       final_interrupt_shadow);
	printf("  \"connect_sequence_requested\": %s,\n",
	       connect_first ? "true" : "false");
	printf("  \"connect_commands_consumed\": %s,\n",
	       connect_consumed ? "true" : "false");
	printf("  \"connect_polls_ms\": %u,\n", connect_poll);
	printf("  \"polls_ms\": %u,\n", poll);
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
	if (resource_fd >= 0)
		close(resource_fd);
	free(snapshot);
	if (container_set)
		ioctl(group, VFIO_GROUP_UNSET_CONTAINER);
	if (group >= 0)
		close(group);
	if (container >= 0)
		close(container);
	return result;
}
