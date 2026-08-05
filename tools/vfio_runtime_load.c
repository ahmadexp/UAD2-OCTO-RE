#define _GNU_SOURCE
// SPDX-License-Identifier: GPL-2.0-only
/* Experiment 021: guarded submission of the exact OCTO firmware-update block. */

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
#define PAGE_SIZE_4K 4096U
#define DSP_COUNT 8U
#define RING_PAGE_COUNT 64U
#define RESPONSE_PAGE 64U
#define HEADER_PAGE 65U
#define PAYLOAD_FIRST_PAGE 66U
#define EXPECTED_FILE_SIZE 2558096U
#define EXPECTED_PAYLOAD_DWORDS 0x0009c214U
#define PAYLOAD_PAGE_COUNT ((EXPECTED_FILE_SIZE + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K)
#define PAGE_COUNT (PAYLOAD_FIRST_PAGE + PAYLOAD_PAGE_COUNT)
#define DMA_CONTROL 0x2200U
#define INTERRUPT_ENABLE 0x2204U
#define INTERRUPT_ACK 0x2208U
#define NOTIFICATION_CONTROL 0x2220U
#define FPGA_REVISION 0x2218U
#define DMA_COLD_RESET 0x0001fe00U
#define DMA_RESET_GLOBAL 0x0001fe01U
#define DMA_GLOBAL_ONLY 0x00000001U
#define CALLBACK_SHADOW 0xccccccccU
#define CONNECT_COMMAND 0x00230002U
#define CLOCK_COMMAND 0x00100002U
#define LOADER_COMMAND_BASE 0x00120000U
#define LOADER_EXTENDED_FLAG 0x40000000U
#define LOADER_RESPONSE_CLASS 0x80040000U
#define EXPECTED_FPGA_REVISION 0xa012dc0dU
#define HBUT_MAGIC 0x54554248U
#define CANARY_BYTE 0xa5
#define RING_CAPACITY 1024U
#define POLL_LIMIT_MS 150000U
#define NO_PROGRESS_LIMIT_MS 5000U

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
	volatile uint32_t *word =
		(volatile uint32_t *)((char *)bar + offset);

	*word = value;
	__sync_synchronize();
}

static bool all_ready(const void *bar)
{
	unsigned int dsp;

	for (dsp = 0; dsp < DSP_COUNT; dsp++)
		if (!(read32(bar, boot_offsets[dsp]) & 1U))
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

static void clear_rings(void *bar)
{
	unsigned int dsp, ring, word;

	for (dsp = 0; dsp < DSP_COUNT; dsp++)
		for (ring = 0; ring < 2; ring++)
			for (word = 0; word < 10; word++)
				write32(bar, dsp_banks[dsp] + ring * 0x40 + word * 4, 0);
}

static void initialize_ring(void *bar, uint32_t base, uint64_t iova)
{
	unsigned int page;

	write32(bar, base + 0x24, 0);
	write32(bar, base + 0x20, 0);
	for (page = 0; page < 4; page++) {
		uint64_t address = iova + page * (uint64_t)PAGE_SIZE_4K;

		write32(bar, base + page * 8, (uint32_t)address);
		write32(bar, base + page * 8 + 4, (uint32_t)(address >> 32));
	}
}

static uint32_t *ring_entry(unsigned char *memory, unsigned int dsp,
			    unsigned int ring, uint32_t index)
{
	size_t page = dsp * 8U + ring * 4U;

	return (uint32_t *)(memory + page * PAGE_SIZE_4K + index * 16U);
}

static void set_descriptor(uint32_t *entry, uint32_t dwords, uint64_t iova)
{
	entry[0] = 0x80000000U | dwords;
	entry[1] = 0;
	entry[2] = (uint32_t)iova;
	entry[3] = (uint32_t)(iova >> 32);
}

static uint32_t day_and_time(void)
{
	uint64_t seconds = (uint64_t)time(NULL);
	uint32_t days = (uint32_t)(seconds / 86400U);
	uint32_t minutes = (uint32_t)((seconds % 86400U) / 60U);

	return (days << 12) + minutes;
}

static bool exact_hbut(const uint32_t *words, size_t size)
{
	return size == EXPECTED_FILE_SIZE && words[0] == HBUT_MAGIC &&
		words[3] == EXPECTED_FPGA_REVISION &&
		words[6] == EXPECTED_PAYLOAD_DWORDS &&
		((uint64_t)words[6] + 16U) * 4U == size;
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
	const size_t memory_size = (size_t)PAGE_COUNT * PAGE_SIZE_4K;
	struct timespec delay = { .tv_sec = 0, .tv_nsec = 1000000 };
	uint32_t response[4] = { 0 };
	uint32_t loader_command[2] = {
		LOADER_COMMAND_BASE | LOADER_EXTENDED_FLAG,
		EXPECTED_FILE_SIZE / 4U + 2U,
	};
	uint32_t command_position = 0, response_position = 0;
	uint32_t final_command_read = 0, final_response_read = 0;
	uint32_t interrupt_shadow = CALLBACK_SHADOW;
	uint32_t *entry, *response_buffer = NULL, *header_buffer = NULL;
	unsigned char *payload = NULL, *memory = MAP_FAILED, *snapshot = NULL;
	void *bar = MAP_FAILED;
	int firmware_fd = -1, container = -1, group = -1, device = -1;
	bool container_set = false, dma_mapped = false, startup = false;
	bool exact_file = false, live_compatible = false, connects_consumed = false;
	bool loader_consumed = false, response_consumed = false;
	bool command_progress_seen = false;
	bool response_written = false, response_class_matches = false;
	bool memory_bounded = false, ready_after = false, restored = false;
	bool reset_recovered = false, iommu_unmapped = false;
	unsigned int dsp, ring, page, poll = 0, word;
	size_t done = 0;
	int result = EXIT_FAILURE;

	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	if (argc != 2) {
		fprintf(stderr, "usage: vfio_runtime_load exact-FirmwareUpdateOcto.bin\n");
		goto cleanup;
	}
	if (sysconf(_SC_PAGESIZE) != PAGE_SIZE_4K) {
		fprintf(stderr, "experiment-021: requires 4096-byte host pages\n");
		goto cleanup;
	}
	firmware_fd = open(argv[1], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (firmware_fd < 0) {
		perror("experiment-021: open exact HBUT");
		goto cleanup;
	}
	{
		struct stat firmware_stat;

		if (fstat(firmware_fd, &firmware_stat) < 0 ||
		    !S_ISREG(firmware_stat.st_mode) ||
		    firmware_stat.st_size != EXPECTED_FILE_SIZE) {
			fprintf(stderr, "experiment-021: refusing unexpected HBUT file\n");
			goto cleanup;
		}
	}
	if (PAYLOAD_PAGE_COUNT + 3U >= RING_CAPACITY) {
		fprintf(stderr, "experiment-021: descriptor chain exceeds one ring\n");
		goto cleanup;
	}

	memory = mmap(NULL, memory_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED || mlock(memory, memory_size) < 0) {
		perror("experiment-021: allocate locked bounded pages");
		goto cleanup;
	}
	memset(memory, 0, RING_PAGE_COUNT * PAGE_SIZE_4K);
	memset(memory + RESPONSE_PAGE * PAGE_SIZE_4K, CANARY_BYTE,
	       2U * PAGE_SIZE_4K);
	payload = memory + PAYLOAD_FIRST_PAGE * PAGE_SIZE_4K;
	memset(payload, 0, (size_t)PAYLOAD_PAGE_COUNT * PAGE_SIZE_4K);
	while (done < EXPECTED_FILE_SIZE) {
		ssize_t count = read(firmware_fd, payload + done,
				     EXPECTED_FILE_SIZE - done);

		if (count <= 0) {
			if (count < 0)
				perror("experiment-021: read HBUT");
			else
				fprintf(stderr, "experiment-021: short HBUT read\n");
			goto cleanup;
		}
		done += (size_t)count;
	}
	close(firmware_fd);
	firmware_fd = -1;
	exact_file = exact_hbut((const uint32_t *)payload, EXPECTED_FILE_SIZE);
	if (!exact_file) {
		fprintf(stderr, "experiment-021: HBUT structure mismatch\n");
		goto cleanup;
	}

	response_buffer = (uint32_t *)(memory + RESPONSE_PAGE * PAGE_SIZE_4K);
	header_buffer = (uint32_t *)(memory + HEADER_PAGE * PAGE_SIZE_4K);
	header_buffer[0] = loader_command[0];
	header_buffer[1] = loader_command[1];
	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		entry = ring_entry(memory, dsp, 0, 0);
		entry[0] = CONNECT_COMMAND;
		entry[1] = 1;
	}
	entry = ring_entry(memory, 0, 0, 1);
	entry[0] = CLOCK_COMMAND;
	entry[1] = day_and_time();
	set_descriptor(ring_entry(memory, 0, 1, 0), 4,
		       TEST_IOVA + RESPONSE_PAGE * (uint64_t)PAGE_SIZE_4K);
	set_descriptor(ring_entry(memory, 0, 0, 2), 2,
		       TEST_IOVA + HEADER_PAGE * (uint64_t)PAGE_SIZE_4K);
	for (page = 0; page < PAYLOAD_PAGE_COUNT; page++) {
		size_t remaining = EXPECTED_FILE_SIZE - (size_t)page * PAGE_SIZE_4K;
		uint32_t bytes = remaining > PAGE_SIZE_4K ? PAGE_SIZE_4K :
			(uint32_t)remaining;

		set_descriptor(ring_entry(memory, 0, 0, 3U + page), bytes / 4U,
			       TEST_IOVA + (PAYLOAD_FIRST_PAGE + page) *
			       (uint64_t)PAGE_SIZE_4K);
	}
	command_position = 3U + PAYLOAD_PAGE_COUNT;
	response_position = 1;
	snapshot = malloc(memory_size);
	if (!snapshot) {
		perror("experiment-021: allocate snapshot");
		goto cleanup;
	}
	memcpy(snapshot, memory, memory_size);
	__sync_synchronize();

	container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (container < 0 ||
	    ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
	    ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1) {
		perror("experiment-021: initialize VFIO");
		goto cleanup;
	}
	group = open(GROUP_PATH, O_RDWR | O_CLOEXEC);
	if (group < 0 || ioctl(group, VFIO_GROUP_GET_STATUS, &group_status) < 0 ||
	    !(group_status.flags & VFIO_GROUP_FLAGS_VIABLE) ||
	    ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0) {
		perror("experiment-021: open viable isolated group");
		goto cleanup;
	}
	container_set = true;
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0 ||
	    ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info) < 0 ||
	    !(iommu_info.iova_pgsizes & PAGE_SIZE_4K)) {
		perror("experiment-021: configure IOMMU");
		goto cleanup;
	}
	dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	dma_map.vaddr = (uintptr_t)memory;
	dma_map.iova = TEST_IOVA;
	dma_map.size = memory_size;
	if (ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
		perror("experiment-021: map bounded IOVA range");
		goto cleanup;
	}
	dma_mapped = true;
	device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, DEVICE_NAME);
	if (device < 0 || ioctl(device, VFIO_DEVICE_GET_INFO, &device_info) < 0 ||
	    !(device_info.flags & VFIO_DEVICE_FLAGS_RESET) ||
	    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &bar_info) < 0 ||
	    bar_info.size != BAR0_SIZE ||
	    !(bar_info.flags & VFIO_REGION_INFO_FLAG_WRITE) ||
	    !(bar_info.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
		perror("experiment-021: validate device");
		goto cleanup;
	}
	bar = mmap(NULL, BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		   device, bar_info.offset);
	if (bar == MAP_FAILED) {
		perror("experiment-021: map BAR0");
		goto cleanup;
	}
	live_compatible = read32(bar, FPGA_REVISION) == EXPECTED_FPGA_REVISION;
	if (!live_compatible || !all_rings_zero(bar) || !all_ready(bar) ||
	    read32(bar, DMA_CONTROL) != DMA_COLD_RESET ||
	    read32(bar, INTERRUPT_ENABLE) != 0) {
		fprintf(stderr, "experiment-021: baseline or compatibility gate failed\n");
		goto cleanup;
	}

	startup = true;
	write32(bar, NOTIFICATION_CONTROL, 0);
	write32(bar, INTERRUPT_ENABLE, 0);
	write32(bar, INTERRUPT_ACK, 0);
	write32(bar, DMA_CONTROL, DMA_RESET_GLOBAL);
	write32(bar, DMA_CONTROL, DMA_GLOBAL_ONLY);
	write32(bar, INTERRUPT_ACK, 0xffffffffU);
	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		for (ring = 0; ring < 2; ring++) {
			uint32_t base = dsp_banks[dsp] + ring * 0x40U;
			uint64_t iova = TEST_IOVA +
				(dsp * 8U + ring * 4U) * (uint64_t)PAGE_SIZE_4K;

			initialize_ring(bar, base, iova);
		}
		write32(bar, DMA_CONTROL, (1U << (dsp + 2U)) - 1U);
	}
	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		uint32_t command_bit = 1U << (dsp * 4U);

		write32(bar, dsp_banks[dsp] + 0x24, 1);
		write32(bar, dsp_banks[dsp] + 0x20, 1);
		interrupt_shadow |= command_bit;
		write32(bar, INTERRUPT_ENABLE, interrupt_shadow);
	}
	write32(bar, dsp_banks[0] + 0x24, 2);
	write32(bar, dsp_banks[0] + 0x20, 2);
	for (poll = 0; poll < 3000U && !interrupted; poll++) {
		connects_consumed = true;
		for (dsp = 0; dsp < DSP_COUNT; dsp++) {
			uint32_t expected = dsp == 0 ? 2U : 1U;

			if (read32(bar, dsp_banks[dsp] + 0x28) != expected)
				connects_consumed = false;
		}
		if (connects_consumed)
			break;
		nanosleep(&delay, NULL);
	}
	if (!connects_consumed)
		goto cleanup;

	write32(bar, dsp_banks[0] + 0x40 + 0x24, response_position);
	write32(bar, dsp_banks[0] + 0x40 + 0x20, response_position);
	interrupt_shadow |= 2U;
	write32(bar, INTERRUPT_ENABLE, interrupt_shadow);
	write32(bar, dsp_banks[0] + 0x24, command_position);
	write32(bar, dsp_banks[0] + 0x20, command_position);
	interrupt_shadow |= 1U;
	write32(bar, INTERRUPT_ENABLE, interrupt_shadow);

	for (poll = 0; poll < POLL_LIMIT_MS && !interrupted; poll++) {
		uint32_t command_read = read32(bar, dsp_banks[0] + 0x28);
		uint32_t response_read = read32(bar, dsp_banks[0] + 0x40 + 0x28);

		final_command_read = command_read;
		final_response_read = response_read;
		if (command_read != 2U)
			command_progress_seen = true;
		__sync_synchronize();
		if (command_read == command_position &&
		    response_read == response_position &&
		    response_buffer[0] != 0xa5a5a5a5U)
			break;
		if (!command_progress_seen && poll + 1U >= NO_PROGRESS_LIMIT_MS)
			break;
		nanosleep(&delay, NULL);
	}
	__sync_synchronize();
	final_command_read = read32(bar, dsp_banks[0] + 0x28);
	final_response_read = read32(bar, dsp_banks[0] + 0x40 + 0x28);
	for (word = 0; word < 4; word++)
		response[word] = response_buffer[word];
	loader_consumed = read32(bar, dsp_banks[0] + 0x28) == command_position;
	response_consumed =
		read32(bar, dsp_banks[0] + 0x40 + 0x28) == response_position;
	response_written = response_buffer[0] != 0xa5a5a5a5U;
	response_class_matches =
		(response[0] & 0xffff0000U) == LOADER_RESPONSE_CLASS;
	ready_after = all_ready(bar);
	memory_bounded =
		memcmp(memory, snapshot, RESPONSE_PAGE * PAGE_SIZE_4K) == 0 &&
		memcmp(memory + RESPONSE_PAGE * PAGE_SIZE_4K + sizeof(response),
		       snapshot + RESPONSE_PAGE * PAGE_SIZE_4K + sizeof(response),
		       PAGE_SIZE_4K - sizeof(response)) == 0 &&
		memcmp(memory + HEADER_PAGE * PAGE_SIZE_4K,
		       snapshot + HEADER_PAGE * PAGE_SIZE_4K,
		       (size_t)(PAGE_COUNT - HEADER_PAGE) * PAGE_SIZE_4K) == 0;
	if (!interrupted && connects_consumed && loader_consumed &&
	    response_consumed && response_written && response_class_matches &&
	    ready_after && memory_bounded)
		result = EXIT_SUCCESS;

cleanup:
	if (startup && bar != MAP_FAILED) {
		write32(bar, INTERRUPT_ENABLE, 0);
		write32(bar, INTERRUPT_ACK, 0xffffffffU);
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
				read32(bar, DMA_CONTROL) == DMA_COLD_RESET &&
				all_ready(bar);
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
		if (ioctl(container, VFIO_IOMMU_UNMAP_DMA, &dma_unmap) == 0 &&
		    dma_unmap.size == memory_size)
			iommu_unmapped = true;
		else
			result = EXIT_FAILURE;
	}

	printf("{\n");
	printf("  \"experiment\": \"021-exact-octo-hbut-chain\",\n");
	printf("  \"expected_file_size\": %u,\n", EXPECTED_FILE_SIZE);
	printf("  \"exact_sha256_required_by_wrapper\": true,\n");
	printf("  \"hbut_structure_matches\": %s,\n", exact_file ? "true" : "false");
	printf("  \"live_fpga_compatible\": %s,\n", live_compatible ? "true" : "false");
	printf("  \"loader_command_words\": [\"0x%08x\", \"0x%08x\"],\n",
	       loader_command[0], loader_command[1]);
	printf("  \"payload_descriptors\": %u,\n", PAYLOAD_PAGE_COUNT);
	printf("  \"mapped_pages\": %u,\n", PAGE_COUNT);
	printf("  \"mapped_bytes\": %zu,\n", memory_size);
	printf("  \"connect_commands_consumed\": %s,\n", connects_consumed ? "true" : "false");
	printf("  \"loader_descriptors_consumed\": %s,\n", loader_consumed ? "true" : "false");
	printf("  \"command_progress_seen\": %s,\n", command_progress_seen ? "true" : "false");
	printf("  \"final_command_read_index\": %u,\n", final_command_read);
	printf("  \"response_descriptor_consumed\": %s,\n", response_consumed ? "true" : "false");
	printf("  \"final_response_read_index\": %u,\n", final_response_read);
	printf("  \"response_written\": %s,\n", response_written ? "true" : "false");
	printf("  \"response_words\": [");
	for (word = 0; word < 4; word++)
		printf("\"0x%08x\"%s", response[word], word == 3 ? "" : ", ");
	printf("],\n");
	printf("  \"response_class_matches\": %s,\n", response_class_matches ? "true" : "false");
	printf("  \"writes_confined_to_response_prefix\": %s,\n", memory_bounded ? "true" : "false");
	printf("  \"all_dsps_ready_after_load\": %s,\n", ready_after ? "true" : "false");
	printf("  \"explicit_restore_succeeded\": %s,\n", restored ? "true" : "false");
	printf("  \"vfio_reset_recovered\": %s,\n", reset_recovered ? "true" : "false");
	printf("  \"iommu_unmapped\": %s,\n", iommu_unmapped ? "true" : "false");
	printf("  \"polls_ms\": %u,\n", poll < POLL_LIMIT_MS ? poll : POLL_LIMIT_MS);
	printf("  \"success\": %s\n", result == EXIT_SUCCESS ? "true" : "false");
	printf("}\n");

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
	if (firmware_fd >= 0)
		close(firmware_fd);
	return result;
}
