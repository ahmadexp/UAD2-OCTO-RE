// SPDX-License-Identifier: GPL-2.0-only
/* Map and unmap one canary page through VFIO without publishing it to hardware. */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/vfio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GROUP_PATH "/dev/vfio/16"
#define TEST_IOVA UINT64_C(0x100000000)
#define CANARY_BYTE 0x5a

static void fail(const char *message)
{
	fprintf(stderr, "vfio DMA-map probe: %s: %s\n", message,
		strerror(errno));
	exit(EXIT_FAILURE);
}
int main(void)
{
	struct vfio_group_status status = { .argsz = sizeof(status) };
	struct vfio_iommu_type1_info info = { .argsz = sizeof(info) };
	struct vfio_iommu_type1_dma_map map = { .argsz = sizeof(map) };
	struct vfio_iommu_type1_dma_unmap unmap = { .argsz = sizeof(unmap) };
	long page_size = sysconf(_SC_PAGESIZE);
	unsigned char *page;
	int container, group;
	size_t i;

	if (page_size <= 0 || (TEST_IOVA % (uint64_t)page_size) != 0) {
		errno = EINVAL;
		fail("unsupported host page size");
	}

	container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (container < 0)
		fail("open /dev/vfio/vfio");
	if (ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
		errno = EPROTO;
		fail("VFIO API version mismatch");
	}
	if (ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1) {
		errno = ENOTSUP;
		fail("VFIO Type 1 IOMMU unavailable");
	}

	group = open(GROUP_PATH, O_RDWR | O_CLOEXEC);
	if (group < 0)
		fail("open isolated IOMMU group 16");
	if (ioctl(group, VFIO_GROUP_GET_STATUS, &status) < 0)
		fail("VFIO_GROUP_GET_STATUS");
	if (!(status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		errno = EBUSY;
		fail("IOMMU group is not viable");
	}
	if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0)
		fail("VFIO_GROUP_SET_CONTAINER");
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0)
		fail("VFIO_SET_IOMMU");
	if (ioctl(container, VFIO_IOMMU_GET_INFO, &info) < 0)
		fail("VFIO_IOMMU_GET_INFO");
	if (!(info.iova_pgsizes & (uint64_t)page_size)) {
		errno = EINVAL;
		fail("IOMMU does not support the host page size");
	}

	page = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED)
		fail("allocate canary page");
	memset(page, CANARY_BYTE, (size_t)page_size);
	if (mlock(page, (size_t)page_size) < 0)
		fail("lock canary page");

	map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	map.vaddr = (uintptr_t)page;
	map.iova = TEST_IOVA;
	map.size = (uint64_t)page_size;
	if (ioctl(container, VFIO_IOMMU_MAP_DMA, &map) < 0)
		fail("VFIO_IOMMU_MAP_DMA");

	usleep(250000);
	for (i = 0; i < (size_t)page_size; i++) {
		if (page[i] != CANARY_BYTE) {
			errno = EIO;
			fail("unpublished canary page changed");
		}
	}

	unmap.iova = TEST_IOVA;
	unmap.size = (uint64_t)page_size;
	if (ioctl(container, VFIO_IOMMU_UNMAP_DMA, &unmap) < 0)
		fail("VFIO_IOMMU_UNMAP_DMA");
	if (unmap.size != (uint64_t)page_size) {
		errno = EIO;
		fail("IOMMU reported a partial unmap");
	}

	printf("{\n");
	printf("  \"transport\": \"vfio-pci\",\n");
	printf("  \"iommu_type\": \"VFIO_TYPE1_IOMMU\",\n");
	printf("  \"iova_page_sizes\": \"0x%016" PRIx64 "\",\n",
	       (uint64_t)info.iova_pgsizes);
	printf("  \"mapped_iova\": \"0x%016" PRIx64 "\",\n", TEST_IOVA);
	printf("  \"mapped_bytes\": %ld,\n", page_size);
	printf("  \"published_to_device\": false,\n");
	printf("  \"canary_unchanged\": true,\n");
	printf("  \"unmapped_bytes\": %" PRIu64 "\n", (uint64_t)unmap.size);
	printf("}\n");

	munlock(page, (size_t)page_size);
	munmap(page, (size_t)page_size);
	ioctl(group, VFIO_GROUP_UNSET_CONTAINER);
	close(group);
	close(container);
	return EXIT_SUCCESS;
}
