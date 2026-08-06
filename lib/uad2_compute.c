#define _GNU_SOURCE
// SPDX-License-Identifier: GPL-2.0-only
#include "uad2_compute.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define UAD2_LIBRARY_MAX_BUFFERS 16

struct uad2_library_buffer {
	uint64_t id;
	void *mapping;
	size_t bytes;
	size_t mapped_bytes;
};

struct uad2_compute {
	int fd;
	struct uad2_library_buffer buffers[UAD2_LIBRARY_MAX_BUFFERS];
};

int uad2_compute_open(unsigned int card_index, struct uad2_compute **out)
{
	struct uad2_compute *device;
	char path[64];
	int length;

	if (!out)
		return -EINVAL;
	*out = NULL;
	length = snprintf(path, sizeof(path), "/dev/uad2_compute%u", card_index);
	if (length < 0 || (size_t)length >= sizeof(path))
		return -ENAMETOOLONG;

	device = calloc(1, sizeof(*device));
	if (!device)
		return -ENOMEM;
	device->fd = open(path, O_RDWR | O_CLOEXEC);
	if (device->fd < 0) {
		int error = -errno;

		free(device);
		return error;
	}
	*out = device;
	return 0;
}

void uad2_compute_close(struct uad2_compute *device)
{
	unsigned int index;

	if (!device)
		return;
	for (index = 0; index < UAD2_LIBRARY_MAX_BUFFERS; index++)
		if (device->buffers[index].mapping)
			munmap(device->buffers[index].mapping,
			       device->buffers[index].mapped_bytes);
	close(device->fd);
	free(device);
}

int uad2_compute_get_info(struct uad2_compute *device,
			  struct uad2_compute_info *info)
{
	if (!device || !info)
		return -EINVAL;
	*info = (struct uad2_compute_info) { .size = sizeof(*info) };
	return ioctl(device->fd, UAD2_COMPUTE_IOC_GET_INFO, info) < 0 ? -errno : 0;
}

int uad2_compute_get_dsp_status(struct uad2_compute *device,
				unsigned int dsp_index,
				struct uad2_compute_dsp_status *status)
{
	if (!device || !status || dsp_index >= UAD2_COMPUTE_DSP_COUNT)
		return -EINVAL;
	*status = (struct uad2_compute_dsp_status) {
		.size = sizeof(*status), .dsp_index = dsp_index,
	};
	return ioctl(device->fd, UAD2_COMPUTE_IOC_GET_DSP_STATUS, status) < 0 ?
		-errno : 0;
}

int uad2_compute_start_transport(struct uad2_compute *device)
{
	if (!device)
		return -EINVAL;
	return ioctl(device->fd, UAD2_COMPUTE_IOC_START_TRANSPORT) < 0 ? -errno : 0;
}

int uad2_compute_stop_transport(struct uad2_compute *device)
{
	if (!device)
		return -EINVAL;
	return ioctl(device->fd, UAD2_COMPUTE_IOC_STOP_TRANSPORT) < 0 ? -errno : 0;
}

int uad2_compute_reset_dsp(struct uad2_compute *device,
			   unsigned int dsp_index,
			   struct uad2_compute_reset *result)
{
	struct uad2_compute_reset reset = {
		.size = sizeof(reset), .dsp_index = dsp_index,
	};

	if (!device || dsp_index >= UAD2_COMPUTE_DSP_COUNT)
		return -EINVAL;
	if (ioctl(device->fd, UAD2_COMPUTE_IOC_RESET_DSP, &reset) < 0)
		return -errno;
	if (result)
		*result = reset;
	return 0;
}

int uad2_compute_alloc_buffer(struct uad2_compute *device, size_t bytes,
			      unsigned int flags, uint64_t *buffer_id)
{
	struct uad2_compute_buffer_alloc allocation = {
		.size = sizeof(allocation), .flags = flags, .bytes = bytes,
	};
	struct uad2_library_buffer *slot = NULL;
	long page_size;
	size_t mapped_bytes;
	unsigned int index;
	void *mapping;

	if (!device || !buffer_id || !bytes || bytes > UINT32_MAX)
		return -EINVAL;
	for (index = 0; index < UAD2_LIBRARY_MAX_BUFFERS; index++)
		if (!device->buffers[index].mapping) {
			slot = &device->buffers[index];
			break;
		}
	if (!slot)
		return -ENOSPC;
	if (ioctl(device->fd, UAD2_COMPUTE_IOC_ALLOC_BUFFER, &allocation) < 0)
		return -errno;
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) {
		struct uad2_compute_buffer_free release = {
			.size = sizeof(release), .buffer_id = allocation.buffer_id,
		};

		ioctl(device->fd, UAD2_COMPUTE_IOC_FREE_BUFFER, &release);
		return -EINVAL;
	}
	mapped_bytes = (bytes + (size_t)page_size - 1) &
		~((size_t)page_size - 1);
	mapping = mmap(NULL, mapped_bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
		device->fd, (off_t)allocation.mmap_offset);
	if (mapping == MAP_FAILED) {
		int error = -errno;
		struct uad2_compute_buffer_free release = {
			.size = sizeof(release), .buffer_id = allocation.buffer_id,
		};

		ioctl(device->fd, UAD2_COMPUTE_IOC_FREE_BUFFER, &release);
		return error;
	}
	slot->id = allocation.buffer_id;
	slot->mapping = mapping;
	slot->bytes = bytes;
	slot->mapped_bytes = mapped_bytes;
	*buffer_id = allocation.buffer_id;
	return 0;
}

void *uad2_compute_buffer_data(struct uad2_compute *device,
			       uint64_t buffer_id, size_t *bytes)
{
	unsigned int index;

	if (!device)
		return NULL;
	for (index = 0; index < UAD2_LIBRARY_MAX_BUFFERS; index++) {
		struct uad2_library_buffer *buffer = &device->buffers[index];

		if (buffer->mapping && buffer->id == buffer_id) {
			if (bytes)
				*bytes = buffer->bytes;
			return buffer->mapping;
		}
	}
	return NULL;
}

int uad2_compute_free_buffer(struct uad2_compute *device, uint64_t buffer_id)
{
	struct uad2_compute_buffer_free release = {
		.size = sizeof(release), .buffer_id = buffer_id,
	};
	unsigned int index;

	if (!device)
		return -EINVAL;
	for (index = 0; index < UAD2_LIBRARY_MAX_BUFFERS; index++) {
		struct uad2_library_buffer *buffer = &device->buffers[index];

		if (!buffer->mapping || buffer->id != buffer_id)
			continue;
		if (munmap(buffer->mapping, buffer->mapped_bytes) < 0)
			return -errno;
		memset(buffer, 0, sizeof(*buffer));
		return ioctl(device->fd, UAD2_COMPUTE_IOC_FREE_BUFFER, &release) < 0 ?
			-errno : 0;
	}
	return -ENOENT;
}

int uad2_compute_load_program(struct uad2_compute *device,
			      unsigned int dsp_index,
			      const void *image, size_t bytes,
			      uint64_t *program_id)
{
	struct uad2_compute_program_load load = {
		.size = sizeof(load), .dsp_index = dsp_index,
		.image_pointer = (uintptr_t)image, .image_bytes = bytes,
	};

	if (!device || !image || !program_id ||
	    dsp_index >= UAD2_COMPUTE_DSP_COUNT)
		return -EINVAL;
	if (ioctl(device->fd, UAD2_COMPUTE_IOC_LOAD_PROGRAM, &load) < 0)
		return -errno;
	*program_id = load.program_id;
	return 0;
}

int uad2_compute_submit(struct uad2_compute *device, unsigned int dsp_index,
			uint64_t program_id, uint64_t input_buffer_id,
			uint64_t output_buffer_id, uint64_t *job_id)
{
	struct uad2_compute_job_submit submit = {
		.size = sizeof(submit), .dsp_index = dsp_index,
		.program_id = program_id, .input_buffer_id = input_buffer_id,
		.output_buffer_id = output_buffer_id,
	};

	if (!device || !job_id || dsp_index >= UAD2_COMPUTE_DSP_COUNT)
		return -EINVAL;
	if (ioctl(device->fd, UAD2_COMPUTE_IOC_SUBMIT_JOB, &submit) < 0)
		return -errno;
	*job_id = submit.job_id;
	return submit.status;
}

int uad2_compute_wait(struct uad2_compute *device, uint64_t job_id,
		      int timeout_ms, struct uad2_compute_job_wait *completion)
{
	struct uad2_compute_job_wait wait = {
		.size = sizeof(wait), .timeout_ms = timeout_ms, .job_id = job_id,
	};

	if (!device || !completion || timeout_ms < 0)
		return -EINVAL;
	if (ioctl(device->fd, UAD2_COMPUTE_IOC_WAIT_JOB, &wait) < 0)
		return -errno;
	*completion = wait;
	return wait.status;
}
