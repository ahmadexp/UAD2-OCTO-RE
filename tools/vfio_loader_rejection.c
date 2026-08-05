#define _GNU_SOURCE
// SPDX-License-Identifier: GPL-2.0-only
/* Experiment 018: send a deliberately invalid, one-word firmware block. */

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
#define RESPONSE_PAGE 64
#define HEADER_PAGE 65
#define PAYLOAD_PAGE 66
#define PAGE_COUNT 67
#define DMA_CONTROL 0x2200
#define INTERRUPT_ENABLE 0x2204
#define INTERRUPT_ACK 0x2208
#define NOTIFICATION_CONTROL 0x2220
#define DMA_COLD_RESET 0x0001fe00
#define DMA_RESET_GLOBAL 0x0001fe01
#define DMA_GLOBAL_ONLY 0x00000001
#define CALLBACK_SHADOW 0xcccccccc
#define CONNECT_COMMAND 0x00230002
#define CLOCK_COMMAND 0x00100002
#define LOADER_COMMAND_BASE 0x00120000
#define LOADER_RESPONSE_CLASS 0x80040000
#define DESCRIPTOR_ONE_DWORD 0x80000001
#define DESCRIPTOR_FOUR_DWORDS 0x80000004
#define CANARY_BYTE 0xa5
#define POLL_COUNT 3000

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

	for (dsp = 0; dsp < DSP_COUNT; dsp++)
		if (!(read32(bar, boot_offsets[dsp]) & 1))
			return false;
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

static void set_descriptor(uint32_t *entry, uint32_t dwords, uint64_t iova)
{
	entry[0] = 0x80000000U | dwords;
	entry[1] = 0;
	entry[2] = (uint32_t)iova;
	entry[3] = (uint32_t)(iova >> 32);
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
	uint32_t indexes[DSP_COUNT][2] = {{0}};
	uint32_t response[4] = {0};
	uint32_t loader_command = 0;
	uint32_t *entry, *response_buffer = NULL, *header_buffer, *payload_buffer = NULL;
	uint32_t command_position = 0, response_position = 0;
	uint32_t interrupt_shadow = CALLBACK_SHADOW;
	unsigned char *memory = MAP_FAILED, *snapshot = NULL;
	void *bar = MAP_FAILED;
	int container = -1, group = -1, device = -1;
	bool container_set = false, dma_mapped = false, startup = false;
	bool connects_consumed = false, loader_consumed = false;
	bool response_consumed = false, response_written = false;
	bool response_class_matches = false, write_bounded = false;
	bool ready_after = false, restored = false, reset_recovered = false;
	bool single_buffer = false;
	unsigned int dsp, ring, poll = 0, word;
	size_t payload_size = sizeof(uint32_t), payload_done = 0;
	const char *payload_path = NULL;
	int payload_fd = -1;
	int result = EXIT_FAILURE;

	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	if (argc >= 2 && strcmp(argv[1], "--single-buffer") == 0) {
		single_buffer = true;
		if (argc == 3)
			payload_path = argv[2];
		else if (argc > 3) {
			fprintf(stderr,
				"usage: vfio_loader_rejection [--single-buffer] [payload-file]\n");
			goto cleanup;
		}
	} else if (argc == 2) {
		payload_path = argv[1];
	} else if (argc > 2) {
		fprintf(stderr,
			"usage: vfio_loader_rejection [--single-buffer] [payload-file]\n");
		goto cleanup;
	}
	if (payload_path) {
		struct stat payload_stat;

		payload_fd = open(payload_path, O_RDONLY | O_CLOEXEC);
		if (payload_fd < 0 || fstat(payload_fd, &payload_stat) < 0) {
			perror("experiment-018: open payload file");
			goto cleanup;
		}
		if (payload_stat.st_size < 4 ||
		    payload_stat.st_size > (off_t)PAGE_SIZE_4K ||
		    (payload_stat.st_size % 4) != 0) {
			fprintf(stderr,
				"experiment-018: payload size %jd must be 4..4096 bytes and dword aligned\n",
				(intmax_t)payload_stat.st_size);
			goto cleanup;
		}
		payload_size = (size_t)payload_stat.st_size;
	}
	loader_command = LOADER_COMMAND_BASE | ((uint32_t)payload_size / 4 + 1);
	if (sysconf(_SC_PAGESIZE) != PAGE_SIZE_4K) {
		fprintf(stderr, "experiment-018: requires 4096-byte host pages\n");
		goto cleanup;
	}

	container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (container < 0 || ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
	    ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1) {
		perror("experiment-018: initialize VFIO");
		goto cleanup;
	}
	group = open(GROUP_PATH, O_RDWR | O_CLOEXEC);
	if (group < 0 || ioctl(group, VFIO_GROUP_GET_STATUS, &group_status) < 0 ||
	    !(group_status.flags & VFIO_GROUP_FLAGS_VIABLE) ||
	    ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0) {
		perror("experiment-018: open viable group");
		goto cleanup;
	}
	container_set = true;
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0 ||
	    ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info) < 0 ||
	    !(iommu_info.iova_pgsizes & PAGE_SIZE_4K)) {
		perror("experiment-018: configure IOMMU");
		goto cleanup;
	}

	memory = mmap(NULL, memory_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED || mlock(memory, memory_size) < 0) {
		perror("experiment-018: allocate locked pages");
		goto cleanup;
	}
	memset(memory, 0, RING_PAGE_COUNT * PAGE_SIZE_4K);
	memset(memory + RESPONSE_PAGE * PAGE_SIZE_4K, CANARY_BYTE,
	       (PAGE_COUNT - RESPONSE_PAGE) * PAGE_SIZE_4K);
	response_buffer = (uint32_t *)(memory + RESPONSE_PAGE * PAGE_SIZE_4K);
	header_buffer = (uint32_t *)(memory + HEADER_PAGE * PAGE_SIZE_4K);
	payload_buffer = (uint32_t *)(memory + PAYLOAD_PAGE * PAGE_SIZE_4K);
	*header_buffer = loader_command;
	*payload_buffer = 0;
	while (payload_fd >= 0 && payload_done < payload_size) {
		ssize_t count = read(payload_fd, (unsigned char *)payload_buffer + payload_done,
				     payload_size - payload_done);

		if (count <= 0) {
			if (count < 0)
				perror("experiment-018: read payload file");
			else
				fprintf(stderr, "experiment-018: short payload read\n");
			goto cleanup;
		}
		payload_done += (size_t)count;
	}
	if (payload_fd >= 0) {
		close(payload_fd);
		payload_fd = -1;
	}
	if (single_buffer) {
		if (payload_size > PAGE_SIZE_4K - 8) {
			fprintf(stderr,
				"experiment-018: single-buffer payload cannot exceed 4088 bytes\n");
			goto cleanup;
		}
		header_buffer[1] = LOADER_RESPONSE_CLASS;
		memcpy(&header_buffer[2], payload_buffer, payload_size);
	}

	dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	dma_map.vaddr = (uintptr_t)memory;
	dma_map.iova = TEST_IOVA;
	dma_map.size = memory_size;
	if (ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
		perror("experiment-018: map IOVA pages");
		goto cleanup;
	}
	dma_mapped = true;
	device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, DEVICE_NAME);
	if (device < 0 || ioctl(device, VFIO_DEVICE_GET_INFO, &device_info) < 0 ||
	    !(device_info.flags & VFIO_DEVICE_FLAGS_RESET) ||
	    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &bar_info) < 0 ||
	    bar_info.size != BAR0_SIZE || !(bar_info.flags & VFIO_REGION_INFO_FLAG_WRITE) ||
	    !(bar_info.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
		perror("experiment-018: validate device");
		goto cleanup;
	}
	bar = mmap(NULL, BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		   device, bar_info.offset);
	if (bar == MAP_FAILED) {
		perror("experiment-018: map BAR0");
		goto cleanup;
	}
	if (!all_rings_zero(bar) || !all_ready(bar) ||
	    read32(bar, DMA_CONTROL) != DMA_COLD_RESET ||
	    read32(bar, INTERRUPT_ENABLE) != 0) {
		fprintf(stderr, "experiment-018: baseline precondition changed\n");
		goto cleanup;
	}

	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		for (ring = 0; ring < 2; ring++) {
			uint32_t raw = read32(bar, dsp_banks[dsp] + ring * 0x40 + 0x28);

			indexes[dsp][ring] = raw < 1024 ? raw : 0;
			if (indexes[dsp][ring] != 0) {
				fprintf(stderr, "experiment-018: requires zero ring indexes\n");
				goto cleanup;
			}
		}
		entry = ring_entry(memory, dsp, 0, 0);
		entry[0] = CONNECT_COMMAND;
		entry[1] = 1;
	}
	entry = ring_entry(memory, 0, 0, 1);
	entry[0] = CLOCK_COMMAND;
	entry[1] = day_and_time();
	set_descriptor(ring_entry(memory, 0, 1, 0), 4,
		       TEST_IOVA + RESPONSE_PAGE * (uint64_t)PAGE_SIZE_4K);
	set_descriptor(ring_entry(memory, 0, 0, 2),
		       single_buffer ? (uint32_t)payload_size / 4 + 2 : 1,
		       TEST_IOVA + HEADER_PAGE * (uint64_t)PAGE_SIZE_4K);
	if (!single_buffer)
		set_descriptor(ring_entry(memory, 0, 0, 3),
			       (uint32_t)payload_size / 4,
			       TEST_IOVA + PAYLOAD_PAGE * (uint64_t)PAGE_SIZE_4K);
	snapshot = malloc(memory_size);
	if (!snapshot) {
		perror("experiment-018: allocate snapshot");
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

			initialize_ring(bar, base, iova, 0);
		}
		write32(bar, DMA_CONTROL, (1U << (dsp + 2)) - 1);
	}
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
	for (poll = 0; poll < POLL_COUNT && !interrupted; poll++) {
		connects_consumed = true;
		for (dsp = 0; dsp < DSP_COUNT; dsp++) {
			uint32_t expected = dsp == 0 ? 2 : 1;

			if (read32(bar, dsp_banks[dsp] + 0x28) != expected)
				connects_consumed = false;
		}
		if (connects_consumed)
			break;
		nanosleep(&delay, NULL);
	}
	if (!connects_consumed)
		goto cleanup;

	response_position = 1;
	write32(bar, dsp_banks[0] + 0x40 + 0x24, response_position);
	write32(bar, dsp_banks[0] + 0x40 + 0x20, response_position);
	interrupt_shadow |= 2;
	write32(bar, INTERRUPT_ENABLE, interrupt_shadow);
	command_position = single_buffer ? 3 : 4;
	write32(bar, dsp_banks[0] + 0x24, command_position);
	write32(bar, dsp_banks[0] + 0x20, command_position);
	interrupt_shadow |= 1;
	write32(bar, INTERRUPT_ENABLE, interrupt_shadow);

	for (poll = 0; poll < POLL_COUNT && !interrupted; poll++) {
		uint32_t command_read = read32(bar, dsp_banks[0] + 0x28);
		uint32_t response_read = read32(bar, dsp_banks[0] + 0x40 + 0x28);

		__sync_synchronize();
		if (command_read == command_position &&
		    response_read == response_position &&
		    response_buffer[0] != 0xa5a5a5a5)
			break;
		nanosleep(&delay, NULL);
	}
	__sync_synchronize();
	for (word = 0; word < 4; word++)
		response[word] = response_buffer[word];
	loader_consumed = read32(bar, dsp_banks[0] + 0x28) == command_position;
	response_consumed = read32(bar, dsp_banks[0] + 0x40 + 0x28) == response_position;
	response_written = response_buffer[0] != 0xa5a5a5a5;
	response_class_matches = (response[0] & 0xffff0000U) == LOADER_RESPONSE_CLASS;
	ready_after = all_ready(bar);
	write_bounded =
		memcmp(memory, snapshot, RESPONSE_PAGE * PAGE_SIZE_4K) == 0 &&
		memcmp(memory + RESPONSE_PAGE * PAGE_SIZE_4K + sizeof(response),
		       snapshot + RESPONSE_PAGE * PAGE_SIZE_4K + sizeof(response),
		       PAGE_SIZE_4K - sizeof(response)) == 0 &&
		memcmp(memory + HEADER_PAGE * PAGE_SIZE_4K,
		       snapshot + HEADER_PAGE * PAGE_SIZE_4K,
		       2 * PAGE_SIZE_4K) == 0;
	if (!interrupted && connects_consumed && loader_consumed && response_consumed &&
	    response_written && response_class_matches && ready_after && write_bounded)
		result = EXIT_SUCCESS;

cleanup:
	if (startup && bar != MAP_FAILED) {
		write32(bar, INTERRUPT_ENABLE, 0);
		write32(bar, INTERRUPT_ACK, 0xffffffff);
		write32(bar, DMA_CONTROL, DMA_RESET_GLOBAL);
		write32(bar, DMA_CONTROL, DMA_GLOBAL_ONLY);
		clear_rings(bar);
		write32(bar, DMA_CONTROL, DMA_COLD_RESET);
		restored = all_rings_zero(bar) &&
			read32(bar, DMA_CONTROL) == DMA_COLD_RESET;
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
	printf("  \"experiment\": \"018-loader-response-probe\",\n");
	printf("  \"framing\": \"%s\",\n",
	       single_buffer ? "alternate-single-buffer-with-response-class" :
	       "official-chained-send-block");
	printf("  \"payload_bytes\": %zu,\n", payload_size);
	printf("  \"payload_first_word\": \"0x%08x\",\n",
	       payload_buffer ? payload_buffer[0] : 0);
	printf("  \"loader_command\": \"0x%08x\",\n", loader_command);
	printf("  \"expected_response_class\": \"0x%08x\",\n", LOADER_RESPONSE_CLASS);
	printf("  \"connect_commands_consumed\": %s,\n",
	       connects_consumed ? "true" : "false");
	printf("  \"loader_descriptors_consumed\": %s,\n",
	       loader_consumed ? "true" : "false");
	printf("  \"response_descriptor_consumed\": %s,\n",
	       response_consumed ? "true" : "false");
	printf("  \"response_written\": %s,\n", response_written ? "true" : "false");
	printf("  \"response_words\": [");
	for (word = 0; word < 4; word++)
		printf("\"0x%08x\"%s", response[word], word == 3 ? "" : ", ");
	printf("],\n");
	printf("  \"response_class_matches\": %s,\n",
	       response_class_matches ? "true" : "false");
	printf("  \"writes_confined_to_response_prefix\": %s,\n",
	       write_bounded ? "true" : "false");
	printf("  \"all_dsps_ready_after_probe\": %s,\n", ready_after ? "true" : "false");
	printf("  \"explicit_restore_succeeded\": %s,\n", restored ? "true" : "false");
	printf("  \"vfio_reset_recovered\": %s,\n", reset_recovered ? "true" : "false");
	printf("  \"polls_ms\": %u,\n", poll < POLL_COUNT ? poll : POLL_COUNT);
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
	if (payload_fd >= 0)
		close(payload_fd);
	return result;
}
