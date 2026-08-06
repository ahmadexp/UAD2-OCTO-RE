// SPDX-License-Identifier: GPL-2.0-only
#include "uad2_compute.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream)
{
	fprintf(stream,
		"usage: uad2ctl [--card N] info|status|start|stop|reset DSP|run DSP PROGRAM.bundle\n");
}

static int parse_uint(const char *text, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 0);
	if (errno || !*text || *end || parsed > 0xffffffffUL)
		return -EINVAL;
	*value = (unsigned int)parsed;
	return 0;
}

static int show_info(struct uad2_compute *device)
{
	struct uad2_compute_info info;
	int error = uad2_compute_get_info(device, &info);

	if (error)
		return error;
	printf("abi_version=%u\n", info.abi_version);
	printf("pci=%04x:%04x subsystem=%04x:%04x\n", info.vendor_id,
	       info.device_id, info.subsystem_vendor_id,
	       info.subsystem_device_id);
	printf("fpga_revision=0x%08x extended_capabilities=0x%08x\n",
	       info.fpga_revision, info.extended_capabilities);
	printf("dsp_count=%u transport_started=%u capabilities=0x%016llx\n",
	       info.dsp_count, info.transport_started,
	       (unsigned long long)info.capabilities);
	return 0;
}

static int show_status(struct uad2_compute *device)
{
	unsigned int dsp;

	for (dsp = 0; dsp < UAD2_COMPUTE_DSP_COUNT; dsp++) {
		struct uad2_compute_dsp_status status;
		int error = uad2_compute_get_dsp_status(device, dsp, &status);

		if (error)
			return error;
		printf("dsp%u ready=%u raw=0x%08x dma=%u cmd=%u/%u rsp=%u/%u\n",
		       dsp, status.ready, status.raw_ready_word,
		       status.dma_enabled, status.command_read_index,
		       status.command_write_index, status.response_read_index,
		       status.response_write_index);
	}
	return 0;
}

static int run_impulse_job(struct uad2_compute *device, unsigned int dsp,
			   const char *bundle_path)
{
	struct uad2_compute_job_wait completion;
	uint64_t input_id = 0, output_id = 0, program_id = 0, job_id = 0;
	uint32_t *input, *output;
	unsigned char *bundle = NULL;
	FILE *file = NULL;
	int error = 0;

	bundle = malloc(UAD2_COMPUTE_PROGRAM_IMAGE_BYTES);
	if (!bundle)
		return -ENOMEM;
	file = fopen(bundle_path, "rb");
	if (!file) {
		error = -errno;
		goto cleanup;
	}
	if (fread(bundle, 1, UAD2_COMPUTE_PROGRAM_IMAGE_BYTES, file) !=
	    UAD2_COMPUTE_PROGRAM_IMAGE_BYTES || fgetc(file) != EOF) {
		error = -EINVAL;
		goto cleanup;
	}
	error = uad2_compute_start_transport(device);
	if (error)
		goto cleanup;
	error = uad2_compute_alloc_buffer(device, UAD2_COMPUTE_FRAME_BYTES,
		UAD2_BUFFER_INPUT, &input_id);
	if (error)
		goto stop;
	error = uad2_compute_alloc_buffer(device, UAD2_COMPUTE_FRAME_BYTES,
		UAD2_BUFFER_OUTPUT, &output_id);
	if (error)
		goto stop;
	input = uad2_compute_buffer_data(device, input_id, NULL);
	output = uad2_compute_buffer_data(device, output_id, NULL);
	if (!input || !output) {
		error = -EFAULT;
		goto stop;
	}
	memset(input, 0, UAD2_COMPUTE_FRAME_BYTES);
	input[0] = 0x3f000000;
	input[UAD2_COMPUTE_TICK_SAMPLES] = 0xbf000000;
	error = uad2_compute_load_program(device, dsp, bundle,
		UAD2_COMPUTE_PROGRAM_IMAGE_BYTES, &program_id);
	if (error)
		goto stop;
	error = uad2_compute_submit(device, dsp, program_id, input_id, output_id,
		&job_id);
	if (error)
		goto stop;
	error = uad2_compute_wait(device, job_id, 1000, &completion);
	if (error)
		goto stop;
	printf("program_id=%llu job_id=%llu request_id=%u marker=0x%08x\n",
	       (unsigned long long)program_id, (unsigned long long)job_id,
	       completion.request_id, completion.response_marker);
	printf("output_ch0_0=0x%08x output_ch1_0=0x%08x\n", output[0],
	       output[UAD2_COMPUTE_TICK_SAMPLES]);
	if (memcmp(output, input, UAD2_COMPUTE_FRAME_BYTES))
		error = -EPROTO;
stop:
	{
		int stop_error = uad2_compute_stop_transport(device);

		if (!error)
			error = stop_error;
	}
cleanup:
	if (file)
		fclose(file);
	free(bundle);
	return error;
}

int main(int argc, char **argv)
{
	struct uad2_compute *device = NULL;
	unsigned int card = 0, dsp = 0;
	const char *command;
	const char *bundle_path = NULL;
	int argument = 1, error;

	if (argc >= 3 && strcmp(argv[1], "--card") == 0) {
		if (parse_uint(argv[2], &card)) {
			usage(stderr);
			return EXIT_FAILURE;
		}
		argument = 3;
	}
	if (argument >= argc) {
		usage(stderr);
		return EXIT_FAILURE;
	}
	command = argv[argument++];
	if (strcmp(command, "reset") == 0 || strcmp(command, "run") == 0) {
		if (argument >= argc || parse_uint(argv[argument++], &dsp)) {
			usage(stderr);
			return EXIT_FAILURE;
		}
		if (strcmp(command, "run") == 0) {
			if (argument >= argc) {
				usage(stderr);
				return EXIT_FAILURE;
			}
			bundle_path = argv[argument++];
		}
	}
	if (argument != argc) {
		usage(stderr);
		return EXIT_FAILURE;
	}

	error = uad2_compute_open(card, &device);
	if (error)
		goto fail;
	if (strcmp(command, "info") == 0)
		error = show_info(device);
	else if (strcmp(command, "status") == 0)
		error = show_status(device);
	else if (strcmp(command, "start") == 0)
		error = uad2_compute_start_transport(device);
	else if (strcmp(command, "stop") == 0)
		error = uad2_compute_stop_transport(device);
	else if (strcmp(command, "reset") == 0) {
		struct uad2_compute_reset reset;

		error = uad2_compute_reset_dsp(device, dsp, &reset);
		if (!error)
			printf("dsp%u ready=%u dma_before=0x%08x dma_after=0x%08x\n",
			       dsp, reset.ready_after, reset.dma_control_before,
			       reset.dma_control_after);
	} else if (strcmp(command, "run") == 0) {
		error = run_impulse_job(device, dsp, bundle_path);
	} else {
		usage(stderr);
		error = -EINVAL;
	}
	uad2_compute_close(device);
	device = NULL;
	if (!error)
		return EXIT_SUCCESS;

fail:
	fprintf(stderr, "uad2ctl: %s\n", strerror(-error));
	uad2_compute_close(device);
	return EXIT_FAILURE;
}
