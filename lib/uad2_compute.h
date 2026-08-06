/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UAD2_COMPUTE_H
#define UAD2_COMPUTE_H

#include <stddef.h>
#include <stdint.h>

#include "../include/uapi/uad2_compute.h"

struct uad2_compute;

int uad2_compute_open(unsigned int card_index, struct uad2_compute **out);
void uad2_compute_close(struct uad2_compute *device);

int uad2_compute_get_info(struct uad2_compute *device,
			  struct uad2_compute_info *info);
int uad2_compute_get_dsp_status(struct uad2_compute *device,
				unsigned int dsp_index,
				struct uad2_compute_dsp_status *status);
int uad2_compute_start_transport(struct uad2_compute *device);
int uad2_compute_stop_transport(struct uad2_compute *device);
int uad2_compute_reset_dsp(struct uad2_compute *device,
			   unsigned int dsp_index,
			   struct uad2_compute_reset *result);

int uad2_compute_alloc_buffer(struct uad2_compute *device, size_t bytes,
			      unsigned int flags, uint64_t *buffer_id);
void *uad2_compute_buffer_data(struct uad2_compute *device,
			       uint64_t buffer_id, size_t *bytes);
int uad2_compute_free_buffer(struct uad2_compute *device, uint64_t buffer_id);
int uad2_compute_load_program(struct uad2_compute *device,
			      unsigned int dsp_index,
			      const void *image, size_t bytes,
			      uint64_t *program_id);
int uad2_compute_submit(struct uad2_compute *device, unsigned int dsp_index,
			uint64_t program_id, uint64_t input_buffer_id,
			uint64_t output_buffer_id, uint64_t *job_id);
int uad2_compute_wait(struct uad2_compute *device, uint64_t job_id,
		      int timeout_ms, struct uad2_compute_job_wait *completion);

#endif
