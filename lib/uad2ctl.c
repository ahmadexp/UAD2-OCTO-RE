// SPDX-License-Identifier: GPL-2.0-only
#include "uad2_compute.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream)
{
	fprintf(stream,
		"usage: uad2ctl [--card N] info|status|start|stop|reset [DSP]\n");
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

int main(int argc, char **argv)
{
	struct uad2_compute *device = NULL;
	unsigned int card = 0, dsp = 0;
	const char *command;
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
	if (strcmp(command, "reset") == 0) {
		if (argument >= argc || parse_uint(argv[argument++], &dsp)) {
			usage(stderr);
			return EXIT_FAILURE;
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
	} else {
		usage(stderr);
		error = -EINVAL;
	}
	uad2_compute_close(device);
	if (!error)
		return EXIT_SUCCESS;

fail:
	fprintf(stderr, "uad2ctl: %s\n", strerror(-error));
	uad2_compute_close(device);
	return EXIT_FAILURE;
}
