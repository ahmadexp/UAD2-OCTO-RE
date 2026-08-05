#define _GNU_SOURCE
// SPDX-License-Identifier: GPL-2.0-only
#include "uad2_compute.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct uad2_compute {
	int fd;
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
	if (!device)
		return;
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
	(void)device;
	(void)bytes;
	(void)flags;
	(void)buffer_id;
	return -EOPNOTSUPP;
}

int uad2_compute_load_program(struct uad2_compute *device,
			      unsigned int dsp_index,
			      const void *image, size_t bytes,
			      uint64_t *program_id)
{
	(void)device;
	(void)dsp_index;
	(void)image;
	(void)bytes;
	(void)program_id;
	return -EOPNOTSUPP;
}

int uad2_compute_submit(struct uad2_compute *device, unsigned int dsp_index,
			uint64_t program_id, uint64_t input_buffer_id,
			uint64_t output_buffer_id, uint64_t *job_id)
{
	(void)device;
	(void)dsp_index;
	(void)program_id;
	(void)input_buffer_id;
	(void)output_buffer_id;
	(void)job_id;
	return -EOPNOTSUPP;
}

int uad2_compute_wait(struct uad2_compute *device, uint64_t job_id,
		      int timeout_ms)
{
	(void)device;
	(void)job_id;
	(void)timeout_ms;
	return -EOPNOTSUPP;
}
