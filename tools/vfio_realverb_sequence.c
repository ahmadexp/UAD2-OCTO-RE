#define _GNU_SOURCE
// SPDX-License-Identifier: GPL-2.0-only
/* Experiments 032-034 and 040-042: resource lifecycle and bounded processing. */

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
#define RESOURCE_COUNT 13
#define CHUNK_COUNT 16
#define RESOURCE_PAGE_BASE 64
#define RESPONSE_PAGE_BASE (RESOURCE_PAGE_BASE + CHUNK_COUNT)
#define READBACK_PAGE (RESPONSE_PAGE_BASE + RESOURCE_COUNT)
#define MEMSPEC_PAGE (READBACK_PAGE + 1)
#define PROCESS_INPUT_PAGE_BASE (MEMSPEC_PAGE + 1)
#define PROCESS_TICKS_MAX 8
#define PROCESS_OUTPUT_PAGE_BASE \
	(PROCESS_INPUT_PAGE_BASE + PROCESS_CHANNELS * PROCESS_TICKS_MAX)
#define PAGE_COUNT (PROCESS_OUTPUT_PAGE_BASE + PROCESS_CHANNELS * PROCESS_TICKS_MAX)
#define DMA_CONTROL 0x2200
#define INTERRUPT_ENABLE 0x2204
#define INTERRUPT_ACK 0x2208
#define NOTIFICATION_CONTROL 0x2220
#define DMA_COLD_RESET 0x0001fe00
#define DMA_RESET_GLOBAL 0x0001fe01
#define DMA_GLOBAL_ONLY 0x00000001
#define CALLBACK_SHADOW 0xcccccccc
#define BILL_MAGIC 0x6c6c6942
#define BILL_RESPONSE_HEADER 0x80070004
#define BILL_RESPONSE_WORDS 4
#define READBACK_COMMAND 0x000c0004
#define READBACK_DWORDS 4
#define READBACK_RESPONSE_WORDS (READBACK_DWORDS + 2)
#define ZERO_COMMAND_COUNT 33
#define MEMSPEC_DWORDS 65
#define PROCESS_CHANNELS 2
#define PROCESS_INPUT_DWORDS 0x42
#define PROCESS_OUTPUT_DWORDS 0x44
#define PROCESS_COMMAND_BASE 0x000b0000
#define PROCESS_COMMAND_DWORDS 4
#define PROCESS_FLAGS 0x00400000
#define PROCESS_PLUGIN_ADDRESS 0x0009d00a
#define PROCESS_PROPERTY_7 0x000b2000
#define PROCESS_RESPONSE_MARKER 0xf001000e
#define RESOURCE_WAIT_MS 600
#define READBACK_WAIT_MS 6000
#define PROCESS_WAIT_MS 6000

struct resource_definition {
	uint32_t id;
	uint32_t command;
	uint32_t allocation;
	uint32_t body_bytes;
	uint32_t replacement_dwords;
	size_t total_bytes;
	unsigned int first_chunk;
	unsigned int chunk_count;
	size_t chunk_bytes[2];
};

struct resource_result {
	uint32_t response[BILL_RESPONSE_WORDS];
	unsigned int polls_ms;
	bool command_consumed;
	bool response_consumed;
	bool accepted;
};

static const uint32_t dsp_banks[DSP_COUNT] = {
	0x2000, 0x2080, 0x2100, 0x2180,
	0x6000, 0x6080, 0x6100, 0x6180,
};

static const uint32_t boot_offsets[DSP_COUNT] = {
	0x01a4, 0x09a4, 0x11a4, 0x19a4,
	0x41a4, 0x49a4, 0x51a4, 0x59a4,
};

static const uint32_t property_7_offsets[DSP_COUNT] = {
	0x01a0, 0x09a0, 0x11a0, 0x19a0,
	0x41a0, 0x49a0, 0x51a0, 0x59a0,
};

static const struct resource_definition resources[RESOURCE_COUNT] = {
	{0x12b, 0x00010073, 0x000e0000, 0x01b0, 0x0060, 460,  0, 1, {460, 0}},
	{0x0eb, 0x000100b5, 0x000e0040, 0x02b8, 0x00a2, 724,  1, 1, {724, 0}},
	{0x0c1, 0x0001009c, 0x000e00ac, 0x0254, 0x008d, 624,  2, 1, {624, 0}},
	{0x0a5, 0x000101e7, 0x000e010a, 0x0780, 0x01c8, 1948, 3, 1, {1948, 0}},
	{0x120, 0x00010099, 0x000e023a, 0x0248, 0x008a, 612,  4, 1, {612, 0}},
	{0x0bd, 0x00010057, 0x000e0296, 0x0140, 0x0048, 348,  5, 1, {348, 0}},
	{0x11f, 0x0001005d, 0x000e02c6, 0x0158, 0x004e, 372,  6, 1, {372, 0}},
	{0x0d0, 0x0001006c, 0x000e02fa, 0x0194, 0x005d, 432,  7, 1, {432, 0}},
	{0x0f9, 0x0001055e, 0x000e0338, 0x155c, 0x054f, 5496, 8, 2, {4096, 1400}},
	{0x0bf, 0x00010600, 0x000e06c2, 0x17e4, 0x05f1, 6144, 10, 2, {4096, 2048}},
	{0x11e, 0x0001003f, 0x000e0ab8, 0x00e0, 0x0030, 252, 12, 1, {252, 0}},
	{0x11d, 0x00010081, 0x000e0ad8, 0x01e8, 0x0072, 516, 13, 1, {516, 0}},
	{0x0d1, 0x000104a8, 0x000e0b24, 0x1284, 0x0495, 4768, 14, 2, {4096, 672}},
};

static const uint32_t zero_commands[ZERO_COMMAND_COUNT][4] = {
	{0x00080004, 0x0009d00a, 0, 0x01ae},
	{0x00080004, 0x0009cf74, 0, 0x0096},
	{0x00080004, 0x0009cede, 0, 0x0096},
	{0x00080004, 0x0009cea6, 0, 0x0038},
	{0x00080004, 0x0009cea4, 0, 0x0002},
	{0x00080004, 0x0009cea2, 0, 0x0002},
	{0x00080004, 0x0009cea0, 0, 0x0002},
	{0x00080004, 0x0009ce9e, 0, 0x0002},
	{0x00080004, 0x0009ce7c, 0, 0x0022},
	{0x00080004, 0x0009ce5a, 0, 0x0022},
	{0x00080004, 0x0009ce38, 0, 0x0022},
	{0x00080004, 0x0009ce16, 0, 0x0022},
	{0x00080004, 0x0009cdf4, 0, 0x0022},
	{0x00080004, 0x0009cdd2, 0, 0x0022},
	{0x00080004, 0x0009cdb0, 0, 0x0022},
	{0x00080004, 0x0009cd8e, 0, 0x0022},
	{0x00080004, 0x0009cd6c, 0, 0x0022},
	{0x00080004, 0x0009cd4a, 0, 0x0022},
	{0x00080004, 0x0009cd28, 0, 0x0022},
	{0x00080004, 0x0009cd24, 0, 0x0004},
	{0x00080004, 0x0009cd20, 0, 0x0004},
	{0x00080004, 0x0009cd1c, 0, 0x0004},
	{0x00080004, 0x0009ccfc, 0, 0x0020},
	{0x00080004, 0x0009ccec, 0, 0x0010},
	{0x00080004, 0x0009ccdc, 0, 0x0010},
	{0x00080004, 0x0009cccc, 0, 0x0010},
	{0x00080004, 0x0009ccbc, 0, 0x0010},
	{0x00080004, 0x0009ccb0, 0, 0x000c},
	{0x00080004, 0x0009cca0, 0, 0x0010},
	{0x00080004, 0x0009cbf6, 0, 0x00aa},
	{0x00080004, 0x0009cb4c, 0, 0x00aa},
	{0x00080004, 0x0009cb0c, 0, 0x0040},
	{0x00080004, 0x08fee380, 0, 0xfc80},
};

static const uint32_t memspec_expected[MEMSPEC_DWORDS] = {
	0x00150041,
	0x0009d0b2, 0x0009cf74, 0x0009d0b3, 0x0009cede,
	0x0009d0b4, 0x0009cea6, 0x0009d0bc, 0x0009cea4,
	0x0009d0bd, 0x0009cea2, 0x0009d0be, 0x0009cea0,
	0x0009d0bf, 0x0009ce9e, 0x0009d0c0, 0x0009ce7c,
	0x0009d0c1, 0x0009ce5a, 0x0009d0c2, 0x0009ce38,
	0x0009d0c3, 0x0009ce16, 0x0009d0c4, 0x0009cdf4,
	0x0009d0c5, 0x0009cdd2, 0x0009d0c6, 0x0009cdb0,
	0x0009d0c7, 0x0009cd8e, 0x0009d0c8, 0x0009cd6c,
	0x0009d0c9, 0x0009cd4a, 0x0009d0ca, 0x0009cd28,
	0x0009d0ce, 0x0009cd24, 0x0009d0cf, 0x0009cd20,
	0x0009d0d0, 0x0009cd1c, 0x0009d0e6, 0x0009ccfc,
	0x0009d0f1, 0x0009ccec, 0x0009d0f2, 0x0009ccdc,
	0x0009d0f3, 0x0009cccc, 0x0009d0f4, 0x0009ccbc,
	0x0009d0f5, 0x0009ccb0, 0x0009d0f6, 0x0009cca0,
	0x0009d1aa, 0x0009cbf6, 0x0009d1ab, 0x0009cb4c,
	0x0009d1ac, 0x0009cb0c, 0x0009d1b4, 0x08fee380,
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
	size_t page = dsp * 8 + ring * 4;
	return (uint32_t *)(memory + page * PAGE_SIZE_4K + index * 16);
}

static bool load_exact_chunks(unsigned char *memory, char **paths)
{
	unsigned int resource_index;

	for (resource_index = 0; resource_index < RESOURCE_COUNT; resource_index++) {
		const struct resource_definition *definition = &resources[resource_index];
		unsigned int part;

		for (part = 0; part < definition->chunk_count; part++) {
			unsigned int chunk_index = definition->first_chunk + part;
			unsigned char *destination = memory +
				(RESOURCE_PAGE_BASE + chunk_index) * PAGE_SIZE_4K;
			struct stat file_status;
			size_t received = 0;
			int descriptor = open(paths[chunk_index],
				O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

			if (descriptor < 0 || fstat(descriptor, &file_status) < 0 ||
			    !S_ISREG(file_status.st_mode) ||
			    file_status.st_size != (off_t)definition->chunk_bytes[part]) {
				fprintf(stderr, "experiment-032: refusing chunk %u\n",
					chunk_index);
				if (descriptor >= 0)
					close(descriptor);
				return false;
			}
			while (received < definition->chunk_bytes[part]) {
				ssize_t amount = read(descriptor, destination + received,
					definition->chunk_bytes[part] - received);

				if (amount <= 0) {
					fprintf(stderr, "experiment-032: short chunk %u\n",
						chunk_index);
					close(descriptor);
					return false;
				}
				received += (size_t)amount;
			}
			close(descriptor);
		}
		{
			const uint32_t *header = (const uint32_t *)(memory +
				(RESOURCE_PAGE_BASE + definition->first_chunk) * PAGE_SIZE_4K);

			if (header[0] != definition->command ||
			    header[1] != definition->allocation ||
			    header[2] != BILL_MAGIC || header[3] != definition->id ||
			    header[4] != 0 || header[5] != definition->body_bytes ||
			    header[6] != definition->replacement_dwords ||
			    definition->total_bytes != definition->body_bytes + 28U ||
			    (definition->command & 0xffffU) != definition->total_bytes / 4U) {
				fprintf(stderr, "experiment-032: structure mismatch at resource %u\n",
					resource_index);
				return false;
			}
		}
	}
	return true;
}

static bool load_exact_memspec(unsigned char *memory, const char *path)
{
	unsigned char *destination = memory + MEMSPEC_PAGE * PAGE_SIZE_4K;
	struct stat file_status;
	size_t expected_bytes = MEMSPEC_DWORDS * sizeof(uint32_t);
	size_t received = 0;
	int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	if (descriptor < 0 || fstat(descriptor, &file_status) < 0 ||
	    !S_ISREG(file_status.st_mode) ||
	    file_status.st_size != (off_t)expected_bytes) {
		fprintf(stderr, "experiment-034: refusing memory-spec target\n");
		if (descriptor >= 0)
			close(descriptor);
		return false;
	}
	while (received < expected_bytes) {
		ssize_t amount = read(descriptor, destination + received,
			expected_bytes - received);

		if (amount <= 0) {
			fprintf(stderr, "experiment-034: short memory-spec target\n");
			close(descriptor);
			return false;
		}
		received += (size_t)amount;
	}
	close(descriptor);
	if (memcmp(destination, memspec_expected, expected_bytes) != 0) {
		fprintf(stderr, "experiment-034: memory-spec structure mismatch\n");
		return false;
	}
	return true;
}

static bool memory_writes_bounded(const unsigned char *memory,
				  const unsigned char *snapshot,
				  bool process_probe,
				  unsigned int process_ticks)
{
	unsigned int page;

	if (memcmp(memory, snapshot, RESPONSE_PAGE_BASE * PAGE_SIZE_4K) != 0)
		return false;
	for (page = 0; page < RESOURCE_COUNT; page++) {
		size_t offset = (RESPONSE_PAGE_BASE + page) * PAGE_SIZE_4K;

		if (memcmp(memory + offset + BILL_RESPONSE_WORDS * 4,
			   snapshot + offset + BILL_RESPONSE_WORDS * 4,
			   PAGE_SIZE_4K - BILL_RESPONSE_WORDS * 4) != 0)
			return false;
	}
	{
		size_t offset = READBACK_PAGE * PAGE_SIZE_4K;

		if (memcmp(memory + offset + READBACK_RESPONSE_WORDS * 4,
			   snapshot + offset + READBACK_RESPONSE_WORDS * 4,
			   PAGE_SIZE_4K - READBACK_RESPONSE_WORDS * 4) != 0)
			return false;
	}
	if (memcmp(memory + MEMSPEC_PAGE * PAGE_SIZE_4K,
		   snapshot + MEMSPEC_PAGE * PAGE_SIZE_4K,
		   PAGE_SIZE_4K) != 0)
		return false;
	for (page = 0; page < PROCESS_CHANNELS * PROCESS_TICKS_MAX; page++) {
		size_t input_offset = (PROCESS_INPUT_PAGE_BASE + page) * PAGE_SIZE_4K;
		size_t output_offset = (PROCESS_OUTPUT_PAGE_BASE + page) * PAGE_SIZE_4K;

		if (memcmp(memory + input_offset, snapshot + input_offset,
			   PAGE_SIZE_4K) != 0)
			return false;
		if (process_probe && page < PROCESS_CHANNELS * process_ticks) {
			if (memcmp(memory + output_offset + PROCESS_OUTPUT_DWORDS * 4,
				   snapshot + output_offset + PROCESS_OUTPUT_DWORDS * 4,
				   PAGE_SIZE_4K - PROCESS_OUTPUT_DWORDS * 4) != 0)
				return false;
		} else if (memcmp(memory + output_offset, snapshot + output_offset,
				  PAGE_SIZE_4K) != 0) {
			return false;
		}
	}
	return true;
}

static uint32_t fnv1a32(const uint32_t *words, size_t count)
{
	const unsigned char *bytes = (const unsigned char *)words;
	uint32_t hash = 2166136261U;
	size_t byte;

	for (byte = 0; byte < count * sizeof(*words); byte++) {
		hash ^= bytes[byte];
		hash *= 16777619U;
	}
	return hash;
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
	struct resource_result results[RESOURCE_COUNT] = {0};
	unsigned char *memory = MAP_FAILED, *snapshot = NULL;
	void *bar = MAP_FAILED;
	int container = -1, group = -1, device = -1;
	bool container_set = false, dma_mapped = false, startup = false;
	bool all_accepted = true, ready_after = false, writes_bounded = false;
	bool non_target_indices_unchanged = true;
	bool cleanup_probe = false, allocation_probe = false, process_probe = false;
	bool isolation_trial = false, impulse_probe = false, stream_probe = false;
	bool allocation_commands_consumed = true;
	bool memspec_consumed = false;
	bool readback_requested = false, readback_command_consumed = false;
	bool readback_response_consumed = false, readback_observed = false;
	bool process_requested = false, process_commands_consumed = false;
	bool process_responses_consumed = false, process_outputs_observed = false;
	bool process_response_headers_valid = false;
	bool restored = false, reset_recovered = false;
	bool iommu_unmap_succeeded = false;
	uint32_t readback_response[READBACK_RESPONSE_WORDS] = {0};
	uint32_t command_position = 0, response_position = 0;
	uint32_t interrupt_shadow = CALLBACK_SHADOW;
	unsigned int target_dsp = 0;
	unsigned int resource_index, dsp, ring, part, word, path_offset = 1;
	unsigned int zero_polls[ZERO_COMMAND_COUNT] = {0};
	unsigned int zero_consumed_count = 0, memspec_polls = 0;
	unsigned int unload_polls[RESOURCE_COUNT] = {0}, unload_consumed_count = 0;
	unsigned int readback_polls = 0, accepted_count = 0;
	unsigned int process_polls = 0, process_ticks = 1;
	unsigned int process_changed_dwords[PROCESS_TICKS_MAX][PROCESS_CHANNELS] = {0};
	unsigned int process_nonzero_audio[PROCESS_TICKS_MAX][PROCESS_CHANNELS] = {0};
	uint32_t process_response_headers[PROCESS_TICKS_MAX][PROCESS_CHANNELS][4] = {0};
	uint32_t process_response_tails[PROCESS_TICKS_MAX][PROCESS_CHANNELS] = {0};
	uint32_t process_output_hashes[PROCESS_TICKS_MAX][PROCESS_CHANNELS] = {0};
	uint32_t process_output_preview[PROCESS_TICKS_MAX][PROCESS_CHANNELS][4] = {0};
	bool process_input_roundtrip_valid = false, process_tail_observed = false;
	int result = EXIT_FAILURE;

	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	if (argc == 2 && strcmp(argv[1], "--cleanup") == 0) {
		cleanup_probe = true;
	} else if (argc == CHUNK_COUNT + 3 && strcmp(argv[1], "--allocation") == 0) {
		allocation_probe = true;
		path_offset = 2;
	} else if (argc == CHUNK_COUNT + 3 && strcmp(argv[1], "--process") == 0) {
		allocation_probe = true;
		process_probe = true;
		path_offset = 2;
	} else if (argc == CHUNK_COUNT + 4 &&
		   strcmp(argv[1], "--process-dsp") == 0) {
		char *end = NULL;
		unsigned long parsed;

		errno = 0;
		parsed = strtoul(argv[2], &end, 10);
		if (errno || !end || *end != '\0' || parsed >= DSP_COUNT) {
			fprintf(stderr, "experiment-041: target DSP is invalid\n");
			return EXIT_FAILURE;
		}
		target_dsp = (unsigned int)parsed;
		allocation_probe = true;
		process_probe = true;
		isolation_trial = true;
		path_offset = 3;
	} else if (argc == CHUNK_COUNT + 4 &&
		   strcmp(argv[1], "--process-impulse-dsp") == 0) {
		char *end = NULL;
		unsigned long parsed;

		errno = 0;
		parsed = strtoul(argv[2], &end, 10);
		if (errno || !end || *end != '\0' || parsed >= DSP_COUNT) {
			fprintf(stderr, "experiment-042: target DSP is invalid\n");
			return EXIT_FAILURE;
		}
		target_dsp = (unsigned int)parsed;
		allocation_probe = true;
		process_probe = true;
		impulse_probe = true;
		path_offset = 3;
	} else if (argc == CHUNK_COUNT + 4 &&
		   strcmp(argv[1], "--process-stream-dsp") == 0) {
		char *end = NULL;
		unsigned long parsed;

		errno = 0;
		parsed = strtoul(argv[2], &end, 10);
		if (errno || !end || *end != '\0' || parsed >= DSP_COUNT) {
			fprintf(stderr, "experiment-044: target DSP is invalid\n");
			return EXIT_FAILURE;
		}
		target_dsp = (unsigned int)parsed;
		allocation_probe = true;
		process_probe = true;
		impulse_probe = true;
		stream_probe = true;
		process_ticks = PROCESS_TICKS_MAX;
		path_offset = 3;
	} else if (argc != CHUNK_COUNT + 1) {
		fprintf(stderr, "usage: vfio_realverb_sequence --cleanup | [--allocation|--process] exact-chunk-0 ... exact-chunk-15 [exact-memspec] | [--process-dsp|--process-impulse-dsp|--process-stream-dsp] {0..7} exact-chunk-0 ... exact-chunk-15 exact-memspec\n");
		return EXIT_FAILURE;
	}
	if (sysconf(_SC_PAGESIZE) != PAGE_SIZE_4K) {
		fprintf(stderr, "experiment-032: requires 4096-byte host pages\n");
		return EXIT_FAILURE;
	}

	container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (container < 0 || ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
	    ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1) {
		perror("experiment-032: initialize VFIO");
		goto cleanup;
	}
	group = open(GROUP_PATH, O_RDWR | O_CLOEXEC);
	if (group < 0 || ioctl(group, VFIO_GROUP_GET_STATUS, &group_status) < 0 ||
	    !(group_status.flags & VFIO_GROUP_FLAGS_VIABLE) ||
	    ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0) {
		perror("experiment-032: open viable group");
		goto cleanup;
	}
	container_set = true;
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0 ||
	    ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info) < 0 ||
	    !(iommu_info.iova_pgsizes & PAGE_SIZE_4K)) {
		perror("experiment-032: configure IOMMU");
		goto cleanup;
	}

	memory = mmap(NULL, memory_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED || mlock(memory, memory_size) < 0) {
		perror("experiment-032: allocate locked pages");
		goto cleanup;
	}
	memset(memory, 0, memory_size);
	if (!cleanup_probe && !load_exact_chunks(memory, &argv[path_offset]))
		goto cleanup;
	if (allocation_probe &&
	    !load_exact_memspec(memory, argv[path_offset + CHUNK_COUNT]))
		goto cleanup;
	for (resource_index = 0; !cleanup_probe && resource_index < RESOURCE_COUNT;
	     resource_index++)
		memset(memory + (RESPONSE_PAGE_BASE + resource_index) * PAGE_SIZE_4K,
		       0xa5, PAGE_SIZE_4K);
	memset(memory + READBACK_PAGE * PAGE_SIZE_4K, 0xa5, PAGE_SIZE_4K);
	if (process_probe) {
		unsigned int channel, tick;

		for (tick = 0; tick < process_ticks; tick++) {
			for (channel = 0; channel < PROCESS_CHANNELS; channel++) {
				unsigned int page = tick * PROCESS_CHANNELS + channel;
				uint32_t *input = (uint32_t *)(memory +
					(PROCESS_INPUT_PAGE_BASE + page) * PAGE_SIZE_4K);
				uint32_t *output = (uint32_t *)(memory +
					(PROCESS_OUTPUT_PAGE_BASE + page) * PAGE_SIZE_4K);

				input[0] = 0x00070042;
				input[1] = PROCESS_PROPERTY_7 + channel * 0x40;
				if (impulse_probe && tick == 0)
					input[2] = channel == 0 ? 0x3f000000U : 0xbf000000U;
				memset(output, 0xcc, 4 * sizeof(uint32_t));
				output[PROCESS_OUTPUT_DWORDS - 1] = 0xffffdead;
			}
		}
	}

	for (resource_index = 0; !cleanup_probe && resource_index < RESOURCE_COUNT;
	     resource_index++) {
		const struct resource_definition *definition = &resources[resource_index];
		uint32_t *response_descriptor = ring_entry(memory, target_dsp, 1,
			resource_index);
		uint64_t response_iova = TEST_IOVA +
			(RESPONSE_PAGE_BASE + resource_index) * (uint64_t)PAGE_SIZE_4K;

		response_descriptor[0] = 0x80000000U | BILL_RESPONSE_WORDS;
		response_descriptor[2] = (uint32_t)response_iova;
		response_descriptor[3] = (uint32_t)(response_iova >> 32);
		for (part = 0; part < definition->chunk_count; part++) {
			uint32_t *command_descriptor = ring_entry(memory, target_dsp, 0,
				definition->first_chunk + part);
			uint64_t chunk_iova = TEST_IOVA +
				(RESOURCE_PAGE_BASE + definition->first_chunk + part) *
				(uint64_t)PAGE_SIZE_4K;

			command_descriptor[0] = 0x80000000U |
				(uint32_t)(definition->chunk_bytes[part] / 4U);
			command_descriptor[2] = (uint32_t)chunk_iova;
			command_descriptor[3] = (uint32_t)(chunk_iova >> 32);
		}
	}
	if (cleanup_probe) {
		for (resource_index = 0; resource_index < RESOURCE_COUNT;
		     resource_index++) {
			uint32_t *command = ring_entry(memory, target_dsp, 0,
				resource_index);

			command[0] = 0x00030002;
			command[1] = resources[resource_index].id;
		}
	} else {
		uint32_t readback_index = CHUNK_COUNT;
		uint32_t *command;
		uint32_t *response = ring_entry(memory, target_dsp, 1, RESOURCE_COUNT);
		uint64_t response_iova = TEST_IOVA +
			READBACK_PAGE * (uint64_t)PAGE_SIZE_4K;

		if (allocation_probe) {
			unsigned int zero_index;

			for (zero_index = 0; zero_index < ZERO_COMMAND_COUNT; zero_index++)
				memcpy(ring_entry(memory, target_dsp, 0,
					       CHUNK_COUNT + zero_index),
				       zero_commands[zero_index], sizeof(zero_commands[zero_index]));
			{
				uint32_t *descriptor = ring_entry(memory, target_dsp, 0,
					CHUNK_COUNT + ZERO_COMMAND_COUNT);
				uint64_t memspec_iova = TEST_IOVA +
					MEMSPEC_PAGE * (uint64_t)PAGE_SIZE_4K;

				descriptor[0] = 0x80000000U | MEMSPEC_DWORDS;
				descriptor[2] = (uint32_t)memspec_iova;
				descriptor[3] = (uint32_t)(memspec_iova >> 32);
			}
			readback_index += ZERO_COMMAND_COUNT + 1;
		}
		if (process_probe) {
			unsigned int channel, tick;

			for (tick = 0; tick < process_ticks; tick++) {
				for (channel = 0; channel < PROCESS_CHANNELS; channel++) {
					unsigned int page = tick * PROCESS_CHANNELS + channel;
					uint32_t *input_descriptor = ring_entry(memory, target_dsp, 0,
						readback_index + tick * 3 + channel);
					uint32_t *output_descriptor = ring_entry(memory, target_dsp, 1,
						RESOURCE_COUNT + page);
					uint64_t input_iova = TEST_IOVA +
						(PROCESS_INPUT_PAGE_BASE + page) *
						(uint64_t)PAGE_SIZE_4K;
					uint64_t output_iova = TEST_IOVA +
						(PROCESS_OUTPUT_PAGE_BASE + page) *
						(uint64_t)PAGE_SIZE_4K;

					input_descriptor[0] = 0x80000000U | PROCESS_INPUT_DWORDS;
					input_descriptor[2] = (uint32_t)input_iova;
					input_descriptor[3] = (uint32_t)(input_iova >> 32);
					output_descriptor[0] = 0x80000000U | PROCESS_OUTPUT_DWORDS;
					output_descriptor[2] = (uint32_t)output_iova;
					output_descriptor[3] = (uint32_t)(output_iova >> 32);
				}
				command = ring_entry(memory, target_dsp, 0,
					readback_index + tick * 3 + PROCESS_CHANNELS);
				command[0] = PROCESS_COMMAND_BASE | PROCESS_COMMAND_DWORDS;
				command[1] = PROCESS_FLAGS;
				command[2] = tick + 1;
				command[3] = PROCESS_PLUGIN_ADDRESS;
			}
		} else {
			command = ring_entry(memory, target_dsp, 0, readback_index);
			command[0] = READBACK_COMMAND;
			command[1] = resources[0].allocation;
			command[2] = READBACK_DWORDS;
			command[3] = 0;
			response[0] = 0x80000000U | READBACK_RESPONSE_WORDS;
			response[2] = (uint32_t)response_iova;
			response[3] = (uint32_t)(response_iova >> 32);
		}
	}
	snapshot = malloc(memory_size);
	if (!snapshot) {
		perror("experiment-032: allocate snapshot");
		goto cleanup;
	}
	memcpy(snapshot, memory, memory_size);

	dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	dma_map.vaddr = (uintptr_t)memory;
	dma_map.iova = TEST_IOVA;
	dma_map.size = memory_size;
	if (ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
		perror("experiment-032: map IOVA pages");
		goto cleanup;
	}
	dma_mapped = true;
	device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, DEVICE_NAME);
	if (device < 0 || ioctl(device, VFIO_DEVICE_GET_INFO, &device_info) < 0 ||
	    !(device_info.flags & VFIO_DEVICE_FLAGS_RESET) ||
	    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &bar_info) < 0 ||
	    bar_info.size != BAR0_SIZE || !(bar_info.flags & VFIO_REGION_INFO_FLAG_WRITE) ||
	    !(bar_info.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
		perror("experiment-032: validate device");
		goto cleanup;
	}
	bar = mmap(NULL, BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		   device, bar_info.offset);
	if (bar == MAP_FAILED) {
		perror("experiment-032: map BAR0");
		goto cleanup;
	}
	if (!all_ready(bar) || read32(bar, DMA_CONTROL) != DMA_COLD_RESET ||
	    read32(bar, INTERRUPT_ENABLE) != 0) {
		fprintf(stderr, "experiment-032: baseline precondition changed\n");
		goto cleanup;
	}
	clear_rings(bar);
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

			initialize_ring(bar, base, iova);
		}
		write32(bar, DMA_CONTROL, (1U << (dsp + 2)) - 1);
	}

	if (cleanup_probe) {
		for (resource_index = 0;
		     resource_index < RESOURCE_COUNT && !interrupted;
		     resource_index++) {
			uint32_t expected_command_position = command_position + 1;
			unsigned int poll;

			write32(bar, dsp_banks[target_dsp] + 0x24, expected_command_position);
			write32(bar, dsp_banks[target_dsp] + 0x20, expected_command_position);
			interrupt_shadow |= 1U << (target_dsp * 4);
			write32(bar, INTERRUPT_ENABLE, interrupt_shadow);
			for (poll = 0; poll < RESOURCE_WAIT_MS && !interrupted; poll++) {
				if (read32(bar, dsp_banks[target_dsp] + 0x28) ==
				    expected_command_position)
					break;
				nanosleep(&delay, NULL);
			}
			unload_polls[resource_index] = poll;
			if (read32(bar, dsp_banks[target_dsp] + 0x28) !=
			    expected_command_position)
				break;
			unload_consumed_count++;
			command_position = expected_command_position;
		}
	}

	for (resource_index = 0;
	     !cleanup_probe &&
	     resource_index < RESOURCE_COUNT && !interrupted;
	     resource_index++) {
		const struct resource_definition *definition = &resources[resource_index];
		struct resource_result *resource_result = &results[resource_index];
		uint32_t *response_buffer = (uint32_t *)(memory +
			(RESPONSE_PAGE_BASE + resource_index) * PAGE_SIZE_4K);
		uint32_t expected_command_position = command_position +
			definition->chunk_count;
		uint32_t expected_response_position = response_position + 1;
		unsigned int poll;

		write32(bar, dsp_banks[target_dsp] + 0x40 + 0x24,
			expected_response_position);
		write32(bar, dsp_banks[target_dsp] + 0x40 + 0x20,
			expected_response_position);
		interrupt_shadow |= 2U << (target_dsp * 4);
		write32(bar, INTERRUPT_ENABLE, interrupt_shadow);
		write32(bar, dsp_banks[target_dsp] + 0x24, expected_command_position);
		write32(bar, dsp_banks[target_dsp] + 0x20, expected_command_position);
		interrupt_shadow |= 1U << (target_dsp * 4);
		write32(bar, INTERRUPT_ENABLE, interrupt_shadow);

		for (poll = 0; poll < RESOURCE_WAIT_MS && !interrupted; poll++) {
			if (read32(bar, dsp_banks[target_dsp] + 0x28) == expected_command_position &&
			    read32(bar, dsp_banks[target_dsp] + 0x40 + 0x28) ==
				expected_response_position &&
			    response_buffer[0] != 0xa5a5a5a5U)
				break;
			nanosleep(&delay, NULL);
		}
		__sync_synchronize();
		resource_result->polls_ms = poll;
		for (word = 0; word < BILL_RESPONSE_WORDS; word++)
			resource_result->response[word] = response_buffer[word];
		resource_result->command_consumed =
			read32(bar, dsp_banks[target_dsp] + 0x28) == expected_command_position;
		resource_result->response_consumed =
			read32(bar, dsp_banks[target_dsp] + 0x40 + 0x28) ==
			expected_response_position;
		resource_result->accepted = resource_result->command_consumed &&
			resource_result->response_consumed &&
			resource_result->response[0] == BILL_RESPONSE_HEADER &&
			resource_result->response[1] == 0 &&
			resource_result->response[2] == definition->id &&
			resource_result->response[3] == definition->command;
		if (!resource_result->accepted) {
			all_accepted = false;
			break;
		}
		accepted_count++;
		command_position = expected_command_position;
		response_position = expected_response_position;
	}

	if (allocation_probe && all_accepted && accepted_count == RESOURCE_COUNT &&
	    !interrupted) {
		unsigned int zero_index;

		for (zero_index = 0;
		     zero_index < ZERO_COMMAND_COUNT && !interrupted;
		     zero_index++) {
			uint32_t expected_command_position = command_position + 1;
			unsigned int poll;

			write32(bar, dsp_banks[target_dsp] + 0x24, expected_command_position);
			write32(bar, dsp_banks[target_dsp] + 0x20, expected_command_position);
			write32(bar, INTERRUPT_ENABLE, interrupt_shadow);
			for (poll = 0; poll < RESOURCE_WAIT_MS && !interrupted; poll++) {
				if (read32(bar, dsp_banks[target_dsp] + 0x28) ==
				    expected_command_position)
					break;
				nanosleep(&delay, NULL);
			}
			zero_polls[zero_index] = poll;
			if (read32(bar, dsp_banks[target_dsp] + 0x28) !=
			    expected_command_position) {
				allocation_commands_consumed = false;
				break;
			}
			zero_consumed_count++;
			command_position = expected_command_position;
		}
		if (allocation_commands_consumed &&
		    zero_consumed_count == ZERO_COMMAND_COUNT && !interrupted) {
			uint32_t expected_command_position = command_position + 1;

			write32(bar, dsp_banks[target_dsp] + 0x24, expected_command_position);
			write32(bar, dsp_banks[target_dsp] + 0x20, expected_command_position);
			write32(bar, INTERRUPT_ENABLE, interrupt_shadow);
			for (memspec_polls = 0;
			     memspec_polls < RESOURCE_WAIT_MS && !interrupted;
			     memspec_polls++) {
				if (read32(bar, dsp_banks[target_dsp] + 0x28) ==
				    expected_command_position)
					break;
				nanosleep(&delay, NULL);
			}
			memspec_consumed = read32(bar, dsp_banks[target_dsp] + 0x28) ==
				expected_command_position;
			if (memspec_consumed)
				command_position = expected_command_position;
		}
	}

	if (process_probe && all_accepted && accepted_count == RESOURCE_COUNT &&
	    allocation_commands_consumed &&
	    zero_consumed_count == ZERO_COMMAND_COUNT && memspec_consumed &&
	    !interrupted) {
		uint32_t expected_command_position = command_position +
			process_ticks * (PROCESS_CHANNELS + 1);
		uint32_t expected_response_position = response_position +
			process_ticks * PROCESS_CHANNELS;
		unsigned int channel, tick;

		process_requested = true;
		if (read32(bar, property_7_offsets[target_dsp]) != PROCESS_PROPERTY_7) {
			fprintf(stderr, "experiment-040: target DSP property 7 changed\n");
		} else {
			write32(bar, dsp_banks[target_dsp] + 0x40 + 0x24,
				expected_response_position);
			write32(bar, dsp_banks[target_dsp] + 0x40 + 0x20,
				expected_response_position);
			write32(bar, INTERRUPT_ENABLE, interrupt_shadow);
			write32(bar, dsp_banks[target_dsp] + 0x24,
				expected_command_position);
			write32(bar, dsp_banks[target_dsp] + 0x20,
				expected_command_position);
			write32(bar, INTERRUPT_ENABLE, interrupt_shadow);
			for (process_polls = 0;
			     process_polls < PROCESS_WAIT_MS && !interrupted;
			     process_polls++) {
				if (read32(bar, dsp_banks[target_dsp] + 0x28) ==
				    expected_command_position &&
				    read32(bar, dsp_banks[target_dsp] + 0x40 + 0x28) ==
				    expected_response_position)
					break;
				nanosleep(&delay, NULL);
			}
			__sync_synchronize();
			process_commands_consumed =
				read32(bar, dsp_banks[target_dsp] + 0x28) ==
				expected_command_position;
			process_responses_consumed =
				read32(bar, dsp_banks[target_dsp] + 0x40 + 0x28) ==
				expected_response_position;
			process_outputs_observed = true;
			process_response_headers_valid = true;
			process_input_roundtrip_valid = impulse_probe;
			for (tick = 0; tick < process_ticks; tick++) {
				for (channel = 0; channel < PROCESS_CHANNELS; channel++) {
					unsigned int page = tick * PROCESS_CHANNELS + channel;
					const uint32_t *input = (const uint32_t *)(memory +
						(PROCESS_INPUT_PAGE_BASE + page) * PAGE_SIZE_4K);
					const uint32_t *output = (const uint32_t *)(memory +
						(PROCESS_OUTPUT_PAGE_BASE + page) * PAGE_SIZE_4K);
					const uint32_t *before = (const uint32_t *)(snapshot +
						(PROCESS_OUTPUT_PAGE_BASE + page) * PAGE_SIZE_4K);

					for (word = 0; word < PROCESS_OUTPUT_DWORDS; word++)
						if (output[word] != before[word])
							process_changed_dwords[tick][channel]++;
					for (word = 4; word < PROCESS_OUTPUT_DWORDS; word++)
						if (output[word] != 0)
							process_nonzero_audio[tick][channel]++;
					memcpy(process_response_headers[tick][channel], output,
					       sizeof(process_response_headers[tick][channel]));
					process_response_tails[tick][channel] =
						output[PROCESS_OUTPUT_DWORDS - 1];
					process_output_hashes[tick][channel] = fnv1a32(output + 4,
						PROCESS_OUTPUT_DWORDS - 4);
					memcpy(process_output_preview[tick][channel], output + 4,
					       sizeof(process_output_preview[tick][channel]));
					if (process_changed_dwords[tick][channel] == 0)
						process_outputs_observed = false;
					if (process_response_headers[tick][channel][0] != 0x80020044U ||
					    process_response_headers[tick][channel][1] != tick + 1 ||
					    process_response_headers[tick][channel][2] != channel ||
					    process_response_headers[tick][channel][3] !=
						PROCESS_RESPONSE_MARKER)
						process_response_headers_valid = false;
					if (impulse_probe && tick == 0 &&
					    memcmp(output + 4, input + 2,
						   (PROCESS_OUTPUT_DWORDS - 4) * sizeof(uint32_t)) != 0)
						process_input_roundtrip_valid = false;
					if (tick > 0 && process_nonzero_audio[tick][channel] != 0)
						process_tail_observed = true;
				}
			}
			if (process_commands_consumed)
				command_position = expected_command_position;
			if (process_responses_consumed)
				response_position = expected_response_position;
		}
	}

	if (!process_probe && all_accepted && accepted_count == RESOURCE_COUNT &&
	    (!allocation_probe ||
	     (allocation_commands_consumed &&
	      zero_consumed_count == ZERO_COMMAND_COUNT && memspec_consumed)) &&
	    !interrupted) {
		uint32_t *response_buffer = (uint32_t *)(memory +
			READBACK_PAGE * PAGE_SIZE_4K);
		uint32_t expected_command_position = command_position + 1;
		uint32_t expected_response_position = response_position + 1;

		readback_requested = true;
		write32(bar, dsp_banks[target_dsp] + 0x40 + 0x24,
			expected_response_position);
		write32(bar, dsp_banks[target_dsp] + 0x40 + 0x20,
			expected_response_position);
		write32(bar, INTERRUPT_ENABLE, interrupt_shadow);
		write32(bar, dsp_banks[target_dsp] + 0x24, expected_command_position);
		write32(bar, dsp_banks[target_dsp] + 0x20, expected_command_position);
		write32(bar, INTERRUPT_ENABLE, interrupt_shadow);
		for (readback_polls = 0;
		     readback_polls < READBACK_WAIT_MS && !interrupted;
		     readback_polls++) {
			if (read32(bar, dsp_banks[target_dsp] + 0x28) == expected_command_position &&
			    read32(bar, dsp_banks[target_dsp] + 0x40 + 0x28) ==
				expected_response_position &&
			    response_buffer[0] != 0xa5a5a5a5U)
				break;
			nanosleep(&delay, NULL);
		}
		__sync_synchronize();
		for (word = 0; word < READBACK_RESPONSE_WORDS; word++)
			readback_response[word] = response_buffer[word];
		readback_command_consumed = read32(bar, dsp_banks[target_dsp] + 0x28) ==
			expected_command_position;
		readback_response_consumed = read32(bar,
			dsp_banks[target_dsp] + 0x40 + 0x28) == expected_response_position;
		readback_observed = response_buffer[0] != 0xa5a5a5a5U;
	}

	ready_after = all_ready(bar);
	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		uint32_t command_base = dsp_banks[dsp];
		uint32_t response_base = command_base + 0x40;

		if (dsp == target_dsp)
			continue;

		if (read32(bar, command_base + 0x20) != 0 ||
		    read32(bar, command_base + 0x24) != 0 ||
		    read32(bar, command_base + 0x28) != 0 ||
		    read32(bar, response_base + 0x20) != 0 ||
		    read32(bar, response_base + 0x24) != 0 ||
		    read32(bar, response_base + 0x28) != 0)
			non_target_indices_unchanged = false;
	}
	writes_bounded = memory_writes_bounded(memory, snapshot, process_probe,
		process_ticks);
	if (cleanup_probe) {
		if (!interrupted && unload_consumed_count == RESOURCE_COUNT &&
		    ready_after && non_target_indices_unchanged && writes_bounded)
			result = EXIT_SUCCESS;
		} else if (!interrupted && all_accepted &&
			   accepted_count == RESOURCE_COUNT &&
			   (!allocation_probe ||
			    (allocation_commands_consumed &&
			     zero_consumed_count == ZERO_COMMAND_COUNT && memspec_consumed)) &&
			   (!process_probe ||
			    (process_requested && process_commands_consumed &&
			     process_responses_consumed && process_outputs_observed &&
			     process_response_headers_valid &&
			     (!impulse_probe || process_input_roundtrip_valid))) &&
			   ready_after && non_target_indices_unchanged && writes_bounded) {
		result = EXIT_SUCCESS;
	}

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
	if (dma_mapped) {
		dma_unmap.iova = TEST_IOVA;
		dma_unmap.size = memory_size;
		if (ioctl(container, VFIO_IOMMU_UNMAP_DMA, &dma_unmap) == 0 &&
		    dma_unmap.size == memory_size)
			iommu_unmap_succeeded = true;
		else
			result = EXIT_FAILURE;
		dma_mapped = false;
	}

	printf("{\n");
	printf("  \"experiment\": \"%s\",\n",
	       cleanup_probe ? "033-realverb-resource-cleanup" :
	       stream_probe ? "044-realverb-eight-tick-impulse-stream" :
	       impulse_probe ? "042-realverb-impulse-buffer-job" :
	       isolation_trial ? "041-realverb-bounded-process-isolation-trial" :
	       process_probe ? "040-realverb-bounded-dsp0-process" :
	       allocation_probe ? "034-realverb-allocation-and-memspec" :
	       "032-complete-realverb-resource-pass");
	printf("  \"target_dsp\": %u,\n", target_dsp);
	printf("  \"resource_count\": %u,\n", RESOURCE_COUNT);
	printf("  \"accepted_count\": %u,\n", accepted_count);
	printf("  \"all_resources_accepted\": %s,\n",
	       all_accepted && accepted_count == RESOURCE_COUNT ? "true" : "false");
	printf("  \"cleanup_requested\": %s,\n",
	       cleanup_probe ? "true" : "false");
	printf("  \"unload_command\": \"0x00030002\",\n");
	printf("  \"unload_commands_consumed\": %u,\n",
	       unload_consumed_count);
	printf("  \"unload_polls_ms\": [");
	for (word = 0; word < (cleanup_probe ? RESOURCE_COUNT : 0); word++)
		printf("%u%s", unload_polls[word],
		       word + 1 == RESOURCE_COUNT ? "" : ", ");
	printf("],\n");
	printf("  \"resources\": [\n");
	for (resource_index = 0; resource_index < RESOURCE_COUNT; resource_index++) {
		const struct resource_definition *definition = &resources[resource_index];
		const struct resource_result *resource_result = &results[resource_index];

		printf("    {\"id\": \"0x%08x\", \"allocation\": \"0x%08x\", "
		       "\"command\": \"0x%08x\", \"bytes\": %zu, "
		       "\"descriptors\": %u, \"polls_ms\": %u, "
		       "\"command_consumed\": %s, \"response_consumed\": %s, "
		       "\"response\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", "
		       "\"0x%08x\"], \"accepted\": %s}%s\n",
		       definition->id, definition->allocation, definition->command,
		       definition->total_bytes, definition->chunk_count,
		       resource_result->polls_ms,
		       resource_result->command_consumed ? "true" : "false",
		       resource_result->response_consumed ? "true" : "false",
		       resource_result->response[0], resource_result->response[1],
		       resource_result->response[2], resource_result->response[3],
		       resource_result->accepted ? "true" : "false",
		       resource_index + 1 == RESOURCE_COUNT ? "" : ",");
	}
	printf("  ],\n");
	printf("  \"allocation_phase_requested\": %s,\n",
	       allocation_probe ? "true" : "false");
	printf("  \"zero_resource_count\": %u,\n", ZERO_COMMAND_COUNT);
	printf("  \"zero_resource_commands_consumed\": %u,\n",
	       zero_consumed_count);
	printf("  \"zero_resource_polls_ms\": [");
	for (word = 0; word < (allocation_probe ? ZERO_COMMAND_COUNT : 0); word++)
		printf("%u%s", zero_polls[word],
		       word + 1 == ZERO_COMMAND_COUNT ? "" : ", ");
	printf("],\n");
	printf("  \"memspec_command\": \"0x00150041\",\n");
	printf("  \"memspec_dwords\": %u,\n", MEMSPEC_DWORDS);
	printf("  \"memspec_polls_ms\": %u,\n", memspec_polls);
	printf("  \"memspec_consumed\": %s,\n",
	       memspec_consumed ? "true" : "false");
	printf("  \"process_requested\": %s,\n",
	       process_requested ? "true" : "false");
	printf("  \"process_command\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"],\n",
	       PROCESS_COMMAND_BASE | PROCESS_COMMAND_DWORDS, PROCESS_FLAGS, 1U,
	       PROCESS_PLUGIN_ADDRESS);
	printf("  \"process_property_7\": \"0x%08x\",\n", PROCESS_PROPERTY_7);
	printf("  \"process_ticks\": %u,\n", process_ticks);
	printf("  \"process_input_pattern\": \"%s\",\n",
	       stream_probe ? "opposed-half-scale-impulse-then-seven-zero-ticks" :
	       impulse_probe ? "stereo-opposed-half-scale-impulse" : "stereo-zero");
	printf("  \"process_polls_ms\": %u,\n", process_polls);
	printf("  \"process_commands_consumed\": %s,\n",
	       process_commands_consumed ? "true" : "false");
	printf("  \"process_responses_consumed\": %s,\n",
	       process_responses_consumed ? "true" : "false");
	printf("  \"process_outputs_observed\": %s,\n",
	       process_outputs_observed ? "true" : "false");
	printf("  \"process_response_headers_valid\": %s,\n",
	       process_response_headers_valid ? "true" : "false");
	printf("  \"process_input_roundtrip_valid\": %s,\n",
	       process_input_roundtrip_valid ? "true" : "false");
	printf("  \"post_impulse_tail_observed\": %s,\n",
	       process_tail_observed ? "true" : "false");
	printf("  \"process_outputs\": [\n");
	for (resource_index = 0; resource_index < process_ticks; resource_index++) {
		for (word = 0; word < PROCESS_CHANNELS; word++)
			printf("    {\"tick\": %u, \"channel\": %u, "
			       "\"changed_dwords\": %u, \"nonzero_audio_dwords\": %u, "
			       "\"header\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"], "
			       "\"audio_preview\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"], "
			       "\"audio_fnv1a32\": \"0x%08x\", \"tail\": \"0x%08x\"}%s\n",
			       resource_index, word,
			       process_changed_dwords[resource_index][word],
			       process_nonzero_audio[resource_index][word],
			       process_response_headers[resource_index][word][0],
			       process_response_headers[resource_index][word][1],
			       process_response_headers[resource_index][word][2],
			       process_response_headers[resource_index][word][3],
			       process_output_preview[resource_index][word][0],
			       process_output_preview[resource_index][word][1],
			       process_output_preview[resource_index][word][2],
			       process_output_preview[resource_index][word][3],
			       process_output_hashes[resource_index][word],
			       process_response_tails[resource_index][word],
			       resource_index + 1 == process_ticks &&
			       word + 1 == PROCESS_CHANNELS ? "" : ",");
	}
	printf("  ],\n");
	printf("  \"readback_requested_after_complete_pass\": %s,\n",
	       readback_requested ? "true" : "false");
	printf("  \"readback_address_dwords\": \"0x%08x\",\n",
	       resources[0].allocation);
	printf("  \"readback_dwords\": %u,\n", READBACK_DWORDS);
	printf("  \"readback_polls_ms\": %u,\n", readback_polls);
	printf("  \"readback_command_consumed\": %s,\n",
	       readback_command_consumed ? "true" : "false");
	printf("  \"readback_response_consumed\": %s,\n",
	       readback_response_consumed ? "true" : "false");
	printf("  \"readback_observed\": %s,\n",
	       readback_observed ? "true" : "false");
	printf("  \"readback_response\": [");
	for (word = 0; word < READBACK_RESPONSE_WORDS; word++)
		printf("\"0x%08x\"%s", readback_response[word],
		       word + 1 == READBACK_RESPONSE_WORDS ? "" : ", ");
	printf("],\n");
	printf("  \"non_target_ring_indices_unchanged\": %s,\n",
	       non_target_indices_unchanged ? "true" : "false");
	printf("  \"writes_confined_to_response_prefixes\": %s,\n",
	       writes_bounded ? "true" : "false");
	printf("  \"all_dsps_ready\": %s,\n", ready_after ? "true" : "false");
	printf("  \"explicit_restore_succeeded\": %s,\n",
	       restored ? "true" : "false");
	printf("  \"vfio_reset_recovered\": %s,\n",
	       reset_recovered ? "true" : "false");
	printf("  \"iommu_unmap_succeeded\": %s,\n",
	       iommu_unmap_succeeded ? "true" : "false");
	printf("  \"success\": %s\n", result == EXIT_SUCCESS ? "true" : "false");
	printf("}\n");

	if (bar != MAP_FAILED)
		munmap(bar, BAR0_SIZE);
	if (device >= 0)
		close(device);
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
