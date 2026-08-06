/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef UAD2_COMPUTE_UAPI_H
#define UAD2_COMPUTE_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define UAD2_COMPUTE_ABI_VERSION 2U
#define UAD2_COMPUTE_DSP_COUNT 8U
#define UAD2_COMPUTE_TICK_SAMPLES 64U
#define UAD2_COMPUTE_CHANNELS 2U
#define UAD2_COMPUTE_FRAME_BYTES \
	(UAD2_COMPUTE_TICK_SAMPLES * UAD2_COMPUTE_CHANNELS * sizeof(__u32))
#define UAD2_COMPUTE_PROGRAM_IMAGE_BYTES (17U * 4096U)

#define UAD2_CAP_RING_TRANSPORT   (1ULL << 0)
#define UAD2_CAP_PER_DSP_RESET    (1ULL << 1)
#define UAD2_CAP_PROGRAM_LOAD     (1ULL << 2)
#define UAD2_CAP_DMA_BUFFERS      (1ULL << 3)
#define UAD2_CAP_JOB_COMPLETION   (1ULL << 4)
#define UAD2_CAP_PROGRAM_ISOLATION (1ULL << 5)

#define UAD2_BUFFER_INPUT  (1U << 0)
#define UAD2_BUFFER_OUTPUT (1U << 1)

struct uad2_compute_info {
	__u32 size;
	__u32 abi_version;
	__u16 vendor_id;
	__u16 device_id;
	__u16 subsystem_vendor_id;
	__u16 subsystem_device_id;
	__u32 fpga_revision;
	__u32 extended_capabilities;
	__u32 dsp_count;
	__u32 transport_started;
	__aligned_u64 capabilities;
	__aligned_u64 reserved[4];
};

struct uad2_compute_dsp_status {
	__u32 size;
	__u32 dsp_index;
	__u32 raw_ready_word;
	__u32 ready;
	__u32 dma_enabled;
	__u32 command_read_index;
	__u32 command_write_index;
	__u32 response_read_index;
	__u32 response_write_index;
	__u32 reserved[7];
};

struct uad2_compute_reset {
	__u32 size;
	__u32 dsp_index;
	__u32 flags;
	__u32 ready_after;
	__u32 dma_control_before;
	__u32 dma_control_after;
	__u32 reserved[4];
};

struct uad2_compute_buffer_alloc {
	__u32 size;
	__u32 flags;
	__u32 bytes;
	__u32 reserved0;
	__aligned_u64 buffer_id;
	__aligned_u64 mmap_offset;
	__aligned_u64 reserved[2];
};

struct uad2_compute_buffer_free {
	__u32 size;
	__u32 flags;
	__aligned_u64 buffer_id;
	__aligned_u64 reserved[2];
};

/*
 * The image is 17 page-sized slots: 16 authenticated RealVerb transport
 * chunks followed by the 65-dword private-resource memory specification.
 * Bytes outside each declared chunk must be zero. The device remains the
 * authority for resource authentication.
 */
struct uad2_compute_program_load {
	__u32 size;
	__u32 dsp_index;
	__u32 flags;
	__u32 reserved0;
	__aligned_u64 image_pointer;
	__aligned_u64 image_bytes;
	__aligned_u64 program_id;
	__aligned_u64 reserved[2];
};

struct uad2_compute_job_submit {
	__u32 size;
	__u32 dsp_index;
	__u32 flags;
	__u32 request_id;
	__aligned_u64 program_id;
	__aligned_u64 input_buffer_id;
	__aligned_u64 output_buffer_id;
	__aligned_u64 job_id;
	__s32 status;
	__u32 response_marker;
	__aligned_u64 reserved[2];
};

struct uad2_compute_job_wait {
	__u32 size;
	__s32 timeout_ms;
	__aligned_u64 job_id;
	__s32 status;
	__u32 completed;
	__u32 request_id;
	__u32 response_marker;
	__aligned_u64 reserved[2];
};

#define UAD2_COMPUTE_IOC_MAGIC 'U'
#define UAD2_COMPUTE_IOC_GET_INFO \
	_IOR(UAD2_COMPUTE_IOC_MAGIC, 0x00, struct uad2_compute_info)
#define UAD2_COMPUTE_IOC_GET_DSP_STATUS \
	_IOWR(UAD2_COMPUTE_IOC_MAGIC, 0x01, struct uad2_compute_dsp_status)
#define UAD2_COMPUTE_IOC_START_TRANSPORT \
	_IO(UAD2_COMPUTE_IOC_MAGIC, 0x02)
#define UAD2_COMPUTE_IOC_STOP_TRANSPORT \
	_IO(UAD2_COMPUTE_IOC_MAGIC, 0x03)
#define UAD2_COMPUTE_IOC_RESET_DSP \
	_IOWR(UAD2_COMPUTE_IOC_MAGIC, 0x04, struct uad2_compute_reset)
#define UAD2_COMPUTE_IOC_ALLOC_BUFFER \
	_IOWR(UAD2_COMPUTE_IOC_MAGIC, 0x05, struct uad2_compute_buffer_alloc)
#define UAD2_COMPUTE_IOC_FREE_BUFFER \
	_IOW(UAD2_COMPUTE_IOC_MAGIC, 0x06, struct uad2_compute_buffer_free)
#define UAD2_COMPUTE_IOC_LOAD_PROGRAM \
	_IOWR(UAD2_COMPUTE_IOC_MAGIC, 0x07, struct uad2_compute_program_load)
#define UAD2_COMPUTE_IOC_SUBMIT_JOB \
	_IOWR(UAD2_COMPUTE_IOC_MAGIC, 0x08, struct uad2_compute_job_submit)
#define UAD2_COMPUTE_IOC_WAIT_JOB \
	_IOWR(UAD2_COMPUTE_IOC_MAGIC, 0x09, struct uad2_compute_job_wait)

#endif
