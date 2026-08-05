#define _GNU_SOURCE
// SPDX-License-Identifier: GPL-2.0-only
/* Experiments 013/017: full empty startup and isolated per-DSP reset pulses. */

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
#define RINGS_PER_DSP 2
#define PAGES_PER_RING 4
#define PAGE_COUNT (DSP_COUNT * RINGS_PER_DSP * PAGES_PER_RING)
#define DMA_CONTROL 0x2200
#define INTERRUPT_ENABLE 0x2204
#define INTERRUPT_ACK 0x2208
#define NOTIFICATION_CONTROL 0x2220
#define FPGA_REVISION 0x2218
#define EXTENDED_CAPABILITIES 0x2234
#define DMA_COLD_RESET 0x0001fe00
#define DMA_RESET_GLOBAL 0x0001fe01
#define DMA_GLOBAL_ONLY 0x00000001
#define DMA_ALL_DSPS 0x000001ff
#define EXPECTED_FPGA_REVISION 0xa012dc0d
#define EXPECTED_CAPABILITIES 0x00300811

static const uint32_t dsp_banks[DSP_COUNT] = {
	0x2000, 0x2080, 0x2100, 0x2180,
	0x6000, 0x6080, 0x6100, 0x6180,
};

static const uint32_t boot_offsets[DSP_COUNT] = {
	0x01a4, 0x09a4, 0x11a4, 0x19a4,
	0x41a4, 0x49a4, 0x51a4, 0x59a4,
};

static volatile sig_atomic_t interrupted;
static unsigned int mmio_write_count;

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
	volatile uint32_t *word = (volatile uint32_t *)((char *)bar + offset);

	*word = value;
	__sync_synchronize();
	mmio_write_count++;
}

static uint32_t bounded_index(const void *bar, uint32_t base, uint32_t *raw)
{
	*raw = mmio_read32(bar, base + 0x28);
	return *raw < 1024 ? *raw : 0;
}

static void initialize_ring(void *bar, uint32_t base, uint64_t first_iova,
			    uint32_t index)
{
	unsigned int page;

	mmio_write32(bar, base + 0x24, index);
	mmio_write32(bar, base + 0x20, index);
	for (page = 0; page < PAGES_PER_RING; page++) {
		uint64_t iova = first_iova + page * (uint64_t)PAGE_SIZE_4K;

		mmio_write32(bar, base + page * 8, (uint32_t)iova);
		mmio_write32(bar, base + page * 8 + 4, (uint32_t)(iova >> 32));
	}
}

static bool all_dsps_ready(const void *bar)
{
	unsigned int dsp;

	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		if (!(mmio_read32(bar, boot_offsets[dsp]) & 1))
			return false;
	}
	return true;
}

static bool all_ring_words_zero(const void *bar)
{
	unsigned int dsp, ring, field;

	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		for (ring = 0; ring < RINGS_PER_DSP; ring++) {
			uint32_t base = dsp_banks[dsp] + ring * 0x40;

			for (field = 0; field < 16; field++) {
				if (mmio_read32(bar, base + field * 4) != 0)
					return false;
			}
		}
	}
	return true;
}

static bool all_rings_match(const void *bar, const uint32_t indexes[DSP_COUNT][2])
{
	unsigned int dsp, ring, page;

	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		for (ring = 0; ring < RINGS_PER_DSP; ring++) {
			uint32_t base = dsp_banks[dsp] + ring * 0x40;
			uint64_t first = TEST_IOVA +
				(dsp * 8 + ring * 4) * (uint64_t)PAGE_SIZE_4K;

			if (mmio_read32(bar, base + 0x20) != indexes[dsp][ring] ||
			    mmio_read32(bar, base + 0x24) != indexes[dsp][ring])
				return false;
			for (page = 0; page < PAGES_PER_RING; page++) {
				uint64_t observed = mmio_read32(bar, base + page * 8);
				observed |= (uint64_t)mmio_read32(bar,
					base + page * 8 + 4) << 32;
				if (observed != first + page * (uint64_t)PAGE_SIZE_4K)
					return false;
			}
		}
	}
	return true;
}

static void clear_all_rings(void *bar)
{
	unsigned int dsp, ring, field;

	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		for (ring = 0; ring < RINGS_PER_DSP; ring++) {
			uint32_t base = dsp_banks[dsp] + ring * 0x40;

			for (field = 0; field < 10; field++)
				mmio_write32(bar, base + field * 4, 0);
		}
	}
}

static void fill_canaries(unsigned char *memory)
{
	unsigned int page;

	for (page = 0; page < PAGE_COUNT; page++)
		memset(memory + page * PAGE_SIZE_4K, 0x40 + page, PAGE_SIZE_4K);
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
	struct timespec reset_wait = { .tv_sec = 0, .tv_nsec = 10000000 };
	uint32_t raw_indexes[DSP_COUNT][2] = {{0}};
	uint32_t indexes[DSP_COUNT][2] = {{0}};
	unsigned char *memory = MAP_FAILED;
	void *bar = MAP_FAILED;
	int container = -1, group = -1, device = -1;
	bool container_set = false, dma_mapped = false, startup_begun = false;
	bool publication_ok = false, pages_ok = false, ready_ok = false;
	bool interrupt_ok = false, dma_ok = false, reset_called = false;
	bool reset_recovered = false, restored = false;
	bool exercise_resets = false, per_dsp_resets_ok = true;
	uint32_t reset_pass_mask = 0;
	unsigned int dsp, ring;
	int result = EXIT_FAILURE;

	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	if (argc == 2 && strcmp(argv[1], "--exercise-dsp-resets") == 0)
		exercise_resets = true;
	else if (argc != 1) {
		fprintf(stderr,
			"usage: vfio_full_octo_start [--exercise-dsp-resets]\n");
		goto cleanup;
	}
	if (sysconf(_SC_PAGESIZE) != PAGE_SIZE_4K) {
		fprintf(stderr, "experiment-013: requires 4096-byte host pages\n");
		goto cleanup;
	}

	container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (container < 0 || ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
	    ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1) {
		perror("experiment-013: initialize VFIO container");
		goto cleanup;
	}
	group = open(GROUP_PATH, O_RDWR | O_CLOEXEC);
	if (group < 0 || ioctl(group, VFIO_GROUP_GET_STATUS, &group_status) < 0 ||
	    !(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		perror("experiment-013: open viable group 16");
		goto cleanup;
	}
	if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0) {
		perror("experiment-013: set container");
		goto cleanup;
	}
	container_set = true;
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0 ||
	    ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info) < 0 ||
	    !(iommu_info.iova_pgsizes & PAGE_SIZE_4K)) {
		perror("experiment-013: configure Type 1 IOMMU");
		goto cleanup;
	}

	memory = mmap(NULL, memory_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED || mlock(memory, memory_size) < 0) {
		perror("experiment-013: allocate locked ring pages");
		goto cleanup;
	}
	fill_canaries(memory);
	dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	dma_map.vaddr = (uintptr_t)memory;
	dma_map.iova = TEST_IOVA;
	dma_map.size = memory_size;
	if (ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
		perror("experiment-013: map 64 IOVA pages");
		goto cleanup;
	}
	dma_mapped = true;

	device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, DEVICE_NAME);
	if (device < 0 || ioctl(device, VFIO_DEVICE_GET_INFO, &device_info) < 0 ||
	    !(device_info.flags & VFIO_DEVICE_FLAGS_PCI) ||
	    !(device_info.flags & VFIO_DEVICE_FLAGS_RESET) ||
	    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &bar_info) < 0 ||
	    bar_info.size != BAR0_SIZE || !(bar_info.flags & VFIO_REGION_INFO_FLAG_READ) ||
	    !(bar_info.flags & VFIO_REGION_INFO_FLAG_WRITE) ||
	    !(bar_info.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
		perror("experiment-013: validate VFIO device and BAR0");
		goto cleanup;
	}
	bar = mmap(NULL, bar_info.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   device, bar_info.offset);
	if (bar == MAP_FAILED) {
		perror("experiment-013: map BAR0");
		goto cleanup;
	}

	if (!all_ring_words_zero(bar) || !all_dsps_ready(bar) ||
	    mmio_read32(bar, DMA_CONTROL) != DMA_COLD_RESET ||
	    mmio_read32(bar, INTERRUPT_ENABLE) != 0 ||
	    mmio_read32(bar, FPGA_REVISION) != EXPECTED_FPGA_REVISION ||
	    mmio_read32(bar, EXTENDED_CAPABILITIES) != EXPECTED_CAPABILITIES) {
		fprintf(stderr, "experiment-013: baseline precondition changed\n");
		goto cleanup;
	}

	startup_begun = true;
	mmio_write32(bar, NOTIFICATION_CONTROL, 0);
	mmio_write32(bar, INTERRUPT_ENABLE, 0);
	mmio_write32(bar, INTERRUPT_ACK, 0);
	mmio_write32(bar, DMA_CONTROL, DMA_RESET_GLOBAL);
	mmio_write32(bar, DMA_CONTROL, DMA_GLOBAL_ONLY);
	mmio_write32(bar, INTERRUPT_ACK, 0xffffffff);
	for (dsp = 0; dsp < DSP_COUNT; dsp++) {
		for (ring = 0; ring < RINGS_PER_DSP; ring++) {
			uint32_t base = dsp_banks[dsp] + ring * 0x40;
			uint64_t first = TEST_IOVA +
				(dsp * 8 + ring * 4) * (uint64_t)PAGE_SIZE_4K;

			indexes[dsp][ring] = bounded_index(bar, base,
				&raw_indexes[dsp][ring]);
			initialize_ring(bar, base, first, indexes[dsp][ring]);
		}
		mmio_write32(bar, DMA_CONTROL, (1U << (dsp + 2)) - 1);
	}

	publication_ok = all_rings_match(bar, indexes);
	if (exercise_resets && publication_ok) {
		for (dsp = 0; dsp < DSP_COUNT && !interrupted; dsp++) {
			uint32_t enable_bit = 1U << (dsp + 1);
			uint32_t reset_bit = 1U << (dsp + 9);
			uint32_t disabled = DMA_ALL_DSPS & ~enable_bit;

			mmio_write32(bar, DMA_CONTROL, disabled | reset_bit);
			mmio_write32(bar, DMA_CONTROL, disabled);
			nanosleep(&reset_wait, NULL);
			if (mmio_read32(bar, DMA_CONTROL) != disabled ||
			    !all_dsps_ready(bar)) {
				per_dsp_resets_ok = false;
				break;
			}
			mmio_write32(bar, DMA_CONTROL, disabled | enable_bit);
			if (mmio_read32(bar, DMA_CONTROL) != DMA_ALL_DSPS ||
			    !all_dsps_ready(bar)) {
				per_dsp_resets_ok = false;
				break;
			}
			reset_pass_mask |= enable_bit;
		}
	}
	nanosleep(&wait_time, NULL);
	pages_ok = pages_unchanged(memory);
	ready_ok = all_dsps_ready(bar);
	interrupt_ok = mmio_read32(bar, INTERRUPT_ENABLE) == 0;
	dma_ok = mmio_read32(bar, DMA_CONTROL) == DMA_ALL_DSPS;
	if (!interrupted && publication_ok && pages_ok && ready_ok &&
	    interrupt_ok && dma_ok && (!exercise_resets || per_dsp_resets_ok))
		result = EXIT_SUCCESS;

cleanup:
	if (startup_begun && bar != MAP_FAILED) {
		mmio_write32(bar, INTERRUPT_ENABLE, 0);
		mmio_write32(bar, INTERRUPT_ACK, 0xffffffff);
		mmio_write32(bar, DMA_CONTROL, DMA_RESET_GLOBAL);
		mmio_write32(bar, DMA_CONTROL, DMA_GLOBAL_ONLY);
		clear_all_rings(bar);
		mmio_write32(bar, DMA_CONTROL, DMA_COLD_RESET);
		restored = all_ring_words_zero(bar) &&
			mmio_read32(bar, DMA_CONTROL) == DMA_COLD_RESET;
		if (!restored)
			result = EXIT_FAILURE;
	}
	if (device >= 0 && bar != MAP_FAILED) {
		reset_called = true;
		if (ioctl(device, VFIO_DEVICE_RESET) == 0)
			reset_recovered = all_ring_words_zero(bar) &&
				mmio_read32(bar, DMA_CONTROL) == DMA_COLD_RESET &&
				all_dsps_ready(bar);
		if (!reset_recovered)
			result = EXIT_FAILURE;
	}

	printf("{\n");
	printf("  \"experiment\": \"%s\",\n",
	       exercise_resets ? "017-per-dsp-reset-isolation" :
	       "013-full-octo-empty-start");
	printf("  \"mapped_pages\": %u,\n", PAGE_COUNT);
	printf("  \"dsp_engines_enabled\": %u,\n", DSP_COUNT);
	printf("  \"command_entries_submitted\": 0,\n");
	printf("  \"audio_extension_programmed\": false,\n");
	printf("  \"published_all_sixteen_rings\": %s,\n", publication_ok ? "true" : "false");
	printf("  \"pages_unchanged\": %s,\n", pages_ok ? "true" : "false");
	printf("  \"all_dsps_ready\": %s,\n", ready_ok ? "true" : "false");
	printf("  \"interrupt_enable_remained_zero\": %s,\n", interrupt_ok ? "true" : "false");
	printf("  \"dma_control_reached\": \"0x%08x\",\n", dma_ok ? DMA_ALL_DSPS : 0);
	printf("  \"per_dsp_resets_requested\": %s,\n",
	       exercise_resets ? "true" : "false");
	printf("  \"per_dsp_reset_pass_mask\": \"0x%08x\",\n",
	       reset_pass_mask);
	printf("  \"per_dsp_resets_isolated_and_reenabled\": %s,\n",
	       exercise_resets && per_dsp_resets_ok && reset_pass_mask == 0x1fe ?
	       "true" : "false");
	printf("  \"mmio_writes_including_cleanup\": %u,\n", mmio_write_count);
	printf("  \"explicit_restore_succeeded\": %s,\n", restored ? "true" : "false");
	printf("  \"vfio_reset_called\": %s,\n", reset_called ? "true" : "false");
	printf("  \"vfio_reset_recovered\": %s\n", reset_recovered ? "true" : "false");
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
	if (container_set)
		ioctl(group, VFIO_GROUP_UNSET_CONTAINER);
	if (group >= 0)
		close(group);
	if (container >= 0)
		close(container);
	return result;
}
