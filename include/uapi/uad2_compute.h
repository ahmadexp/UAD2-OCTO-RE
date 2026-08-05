/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef UAD2_COMPUTE_UAPI_H
#define UAD2_COMPUTE_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define UAD2_COMPUTE_ABI_VERSION 1U
#define UAD2_COMPUTE_DSP_COUNT 8U

#define UAD2_CAP_RING_TRANSPORT   (1ULL << 0)
#define UAD2_CAP_PER_DSP_RESET    (1ULL << 1)
#define UAD2_CAP_PROGRAM_LOAD     (1ULL << 2)
#define UAD2_CAP_DMA_BUFFERS      (1ULL << 3)
#define UAD2_CAP_JOB_COMPLETION   (1ULL << 4)
#define UAD2_CAP_PROGRAM_ISOLATION (1ULL << 5)

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
	__u64 capabilities;
	__u64 reserved[4];
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

#endif
