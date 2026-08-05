// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experimental transport driver for the exact UAD-2 OCTO 1a00:0002/0005
 * profile. The ABI intentionally has no raw MMIO, physical-address, program
 * load, or arbitrary command operation.
 */

#include <linux/capability.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "uad2_compute.h"

#define UAD2_VENDOR_ID 0x1a00
#define UAD2_DEVICE_ID 0x0002
#define UAD2_SUBVENDOR_ID 0x1a00
#define UAD2_SUBDEVICE_OCTO 0x0005
#define UAD2_BAR0_SIZE 0x10000

#define UAD2_DSP_COUNT 8
#define UAD2_RING_COUNT 2
#define UAD2_RING_PAGES 4
#define UAD2_RING_PAGE_BYTES 4096
#define UAD2_RING_ENTRIES 1024

#define UAD2_DMA_CONTROL 0x2200
#define UAD2_INTERRUPT_ENABLE 0x2204
#define UAD2_INTERRUPT_ACK 0x2208
#define UAD2_FPGA_REVISION 0x2218
#define UAD2_NOTIFICATION_CONTROL 0x2220
#define UAD2_EXTENDED_CAPABILITIES 0x2234

#define UAD2_EXPECTED_FPGA_REVISION 0xa012dc0d
#define UAD2_EXPECTED_CAPABILITIES 0x00300811
#define UAD2_DMA_COLD_RESET 0x0001fe00
#define UAD2_DMA_RESET_GLOBAL 0x0001fe01
#define UAD2_DMA_GLOBAL_ONLY 0x00000001
#define UAD2_DMA_ALL_DSPS 0x000001ff

static const u32 uad2_dsp_banks[UAD2_DSP_COUNT] = {
	0x2000, 0x2080, 0x2100, 0x2180,
	0x6000, 0x6080, 0x6100, 0x6180,
};

static const u32 uad2_ready_offsets[UAD2_DSP_COUNT] = {
	0x01a4, 0x09a4, 0x11a4, 0x19a4,
	0x41a4, 0x49a4, 0x51a4, 0x59a4,
};

struct uad2_ring_page {
	void *cpu;
	dma_addr_t dma;
};

struct uad2_compute_device {
	struct pci_dev *pdev;
	void __iomem *bar;
	struct miscdevice misc;
	struct mutex lock;
	struct kref refcount;
	struct uad2_ring_page pages[UAD2_DSP_COUNT][UAD2_RING_COUNT]
				    [UAD2_RING_PAGES];
	bool started;
	bool removing;
	unsigned int index;
};

static atomic_t uad2_next_index = ATOMIC_INIT(0);

static u32 uad2_read(struct uad2_compute_device *device, u32 offset)
{
	return readl(device->bar + offset);
}

static void uad2_write(struct uad2_compute_device *device, u32 offset,
		       u32 value)
{
	writel(value, device->bar + offset);
}

static bool uad2_all_dsps_ready(struct uad2_compute_device *device)
{
	unsigned int dsp;

	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++)
		if (!(uad2_read(device, uad2_ready_offsets[dsp]) & 1))
			return false;
	return true;
}

static bool uad2_all_ring_words_zero(struct uad2_compute_device *device)
{
	unsigned int dsp, ring, word;

	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++)
		for (ring = 0; ring < UAD2_RING_COUNT; ring++)
			for (word = 0; word < 16; word++)
				if (uad2_read(device, uad2_dsp_banks[dsp] +
					      ring * 0x40 + word * 4))
					return false;
	return true;
}

static void uad2_clear_all_rings(struct uad2_compute_device *device)
{
	unsigned int dsp, ring, word;

	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++)
		for (ring = 0; ring < UAD2_RING_COUNT; ring++)
			for (word = 0; word < 10; word++)
				uad2_write(device, uad2_dsp_banks[dsp] +
					   ring * 0x40 + word * 4, 0);
}

static bool uad2_cold_precondition(struct uad2_compute_device *device)
{
	return uad2_read(device, UAD2_DMA_CONTROL) == UAD2_DMA_COLD_RESET &&
		uad2_read(device, UAD2_INTERRUPT_ENABLE) == 0 &&
		uad2_read(device, UAD2_FPGA_REVISION) ==
			UAD2_EXPECTED_FPGA_REVISION &&
		uad2_read(device, UAD2_EXTENDED_CAPABILITIES) ==
			UAD2_EXPECTED_CAPABILITIES &&
		uad2_all_ring_words_zero(device) &&
		uad2_all_dsps_ready(device);
}

static void uad2_initialize_ring(struct uad2_compute_device *device,
				 unsigned int dsp, unsigned int ring)
{
	u32 base = uad2_dsp_banks[dsp] + ring * 0x40;
	u32 index = uad2_read(device, base + 0x28);
	unsigned int page;

	if (index >= UAD2_RING_ENTRIES)
		index = 0;
	uad2_write(device, base + 0x24, index);
	uad2_write(device, base + 0x20, index);
	for (page = 0; page < UAD2_RING_PAGES; page++) {
		dma_addr_t address = device->pages[dsp][ring][page].dma;

		uad2_write(device, base + page * 8, lower_32_bits(address));
		uad2_write(device, base + page * 8 + 4,
			   upper_32_bits(address));
	}
}

static bool uad2_rings_published(struct uad2_compute_device *device)
{
	unsigned int dsp, ring, page;

	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++) {
		for (ring = 0; ring < UAD2_RING_COUNT; ring++) {
			u32 base = uad2_dsp_banks[dsp] + ring * 0x40;

			for (page = 0; page < UAD2_RING_PAGES; page++) {
				u64 observed = uad2_read(device, base + page * 8);

				observed |= (u64)uad2_read(device,
					base + page * 8 + 4) << 32;
				if (observed != device->pages[dsp][ring][page].dma)
					return false;
			}
		}
	}
	return true;
}

static int uad2_start_locked(struct uad2_compute_device *device)
{
	unsigned int dsp, ring, page;

	if (device->started)
		return 0;
	if (!uad2_cold_precondition(device))
		return -EBUSY;

	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++)
		for (ring = 0; ring < UAD2_RING_COUNT; ring++)
			for (page = 0; page < UAD2_RING_PAGES; page++)
				memset(device->pages[dsp][ring][page].cpu, 0,
				       UAD2_RING_PAGE_BYTES);
	dma_wmb();
	pci_set_master(device->pdev);
	uad2_write(device, UAD2_NOTIFICATION_CONTROL, 0);
	uad2_write(device, UAD2_INTERRUPT_ENABLE, 0);
	uad2_write(device, UAD2_INTERRUPT_ACK, 0);
	uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_RESET_GLOBAL);
	uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_GLOBAL_ONLY);
	uad2_write(device, UAD2_INTERRUPT_ACK, 0xffffffff);
	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++) {
		for (ring = 0; ring < UAD2_RING_COUNT; ring++)
			uad2_initialize_ring(device, dsp, ring);
		uad2_write(device, UAD2_DMA_CONTROL, (1U << (dsp + 2)) - 1);
	}
	uad2_read(device, UAD2_DMA_CONTROL);

	if (uad2_read(device, UAD2_DMA_CONTROL) != UAD2_DMA_ALL_DSPS ||
	    !uad2_rings_published(device) || !uad2_all_dsps_ready(device)) {
		uad2_write(device, UAD2_INTERRUPT_ENABLE, 0);
		uad2_write(device, UAD2_INTERRUPT_ACK, 0xffffffff);
		uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_RESET_GLOBAL);
		uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_GLOBAL_ONLY);
		uad2_clear_all_rings(device);
		uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_COLD_RESET);
		uad2_read(device, UAD2_DMA_CONTROL);
		pci_clear_master(device->pdev);
		return -EIO;
	}
	device->started = true;
	return 0;
}

static int uad2_stop_locked(struct uad2_compute_device *device)
{
	bool recovered;

	if (!device->started)
		return 0;
	uad2_write(device, UAD2_INTERRUPT_ENABLE, 0);
	uad2_write(device, UAD2_INTERRUPT_ACK, 0xffffffff);
	uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_RESET_GLOBAL);
	uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_GLOBAL_ONLY);
	uad2_clear_all_rings(device);
	uad2_write(device, UAD2_DMA_CONTROL, UAD2_DMA_COLD_RESET);
	uad2_read(device, UAD2_DMA_CONTROL);
	pci_clear_master(device->pdev);
	device->started = false;
	recovered = uad2_cold_precondition(device);
	return recovered ? 0 : -EIO;
}

static int uad2_reset_dsp_locked(struct uad2_compute_device *device,
				 struct uad2_compute_reset *reset)
{
	u32 enable_bit, reset_bit, disabled, before;
	bool ready_after_pulse;

	if (!device->started)
		return -EPIPE;
	if (reset->dsp_index >= UAD2_DSP_COUNT || reset->flags)
		return -EINVAL;
	before = uad2_read(device, UAD2_DMA_CONTROL);
	if (before != UAD2_DMA_ALL_DSPS)
		return -EBUSY;
	enable_bit = 1U << (reset->dsp_index + 1);
	reset_bit = 1U << (reset->dsp_index + 9);
	disabled = before & ~enable_bit;

	reset->dma_control_before = before;
	uad2_write(device, UAD2_DMA_CONTROL, disabled | reset_bit);
	uad2_write(device, UAD2_DMA_CONTROL, disabled);
	msleep(10);
	ready_after_pulse = !!(uad2_read(device,
		uad2_ready_offsets[reset->dsp_index]) & 1);
	uad2_write(device, UAD2_DMA_CONTROL, before);
	uad2_read(device, UAD2_DMA_CONTROL);
	reset->dma_control_after = uad2_read(device, UAD2_DMA_CONTROL);
	reset->ready_after = !!(uad2_read(device,
		uad2_ready_offsets[reset->dsp_index]) & 1);
	if (!ready_after_pulse || reset->dma_control_after != before ||
	    !reset->ready_after)
		return -EIO;
	return 0;
}

static long uad2_ioctl(struct file *file, unsigned int command,
		       unsigned long argument)
{
	struct uad2_compute_device *device = file->private_data;
	void __user *pointer = (void __user *)argument;
	long result = 0;

	if (_IOC_TYPE(command) != UAD2_COMPUTE_IOC_MAGIC)
		return -ENOTTY;
	mutex_lock(&device->lock);
	if (device->removing) {
		result = -ENODEV;
		goto unlock;
	}

	switch (command) {
	case UAD2_COMPUTE_IOC_GET_INFO: {
		struct uad2_compute_info info = {
			.size = sizeof(info),
			.abi_version = UAD2_COMPUTE_ABI_VERSION,
			.vendor_id = device->pdev->vendor,
			.device_id = device->pdev->device,
			.subsystem_vendor_id = device->pdev->subsystem_vendor,
			.subsystem_device_id = device->pdev->subsystem_device,
			.fpga_revision = uad2_read(device, UAD2_FPGA_REVISION),
			.extended_capabilities =
				uad2_read(device, UAD2_EXTENDED_CAPABILITIES),
			.dsp_count = UAD2_DSP_COUNT,
			.transport_started = device->started,
			.capabilities = UAD2_CAP_RING_TRANSPORT |
					UAD2_CAP_PER_DSP_RESET,
		};

		if (copy_to_user(pointer, &info, sizeof(info)))
			result = -EFAULT;
		break;
	}
	case UAD2_COMPUTE_IOC_GET_DSP_STATUS: {
		struct uad2_compute_dsp_status status;
		u32 command_base, response_base, dma_control;

		if (copy_from_user(&status, pointer, sizeof(status))) {
			result = -EFAULT;
			break;
		}
		if (status.size != sizeof(status) ||
		    status.dsp_index >= UAD2_DSP_COUNT) {
			result = -EINVAL;
			break;
		}
		command_base = uad2_dsp_banks[status.dsp_index];
		response_base = command_base + 0x40;
		dma_control = uad2_read(device, UAD2_DMA_CONTROL);
		status.raw_ready_word = uad2_read(device,
			uad2_ready_offsets[status.dsp_index]);
		status.ready = !!(status.raw_ready_word & 1);
		status.dma_enabled = !!(dma_control &
			(1U << (status.dsp_index + 1)));
		status.command_read_index = uad2_read(device,
			command_base + 0x28);
		status.command_write_index = uad2_read(device,
			command_base + 0x24);
		status.response_read_index = uad2_read(device,
			response_base + 0x28);
		status.response_write_index = uad2_read(device,
			response_base + 0x24);
		memset(status.reserved, 0, sizeof(status.reserved));
		if (copy_to_user(pointer, &status, sizeof(status)))
			result = -EFAULT;
		break;
	}
	case UAD2_COMPUTE_IOC_START_TRANSPORT:
		if (!capable(CAP_SYS_RAWIO))
			result = -EPERM;
		else
			result = uad2_start_locked(device);
		break;
	case UAD2_COMPUTE_IOC_STOP_TRANSPORT:
		if (!capable(CAP_SYS_RAWIO))
			result = -EPERM;
		else
			result = uad2_stop_locked(device);
		break;
	case UAD2_COMPUTE_IOC_RESET_DSP: {
		struct uad2_compute_reset reset;

		if (!capable(CAP_SYS_RAWIO)) {
			result = -EPERM;
			break;
		}
		if (copy_from_user(&reset, pointer, sizeof(reset))) {
			result = -EFAULT;
			break;
		}
		if (reset.size != sizeof(reset)) {
			result = -EINVAL;
			break;
		}
		memset(reset.reserved, 0, sizeof(reset.reserved));
		result = uad2_reset_dsp_locked(device, &reset);
		if (!result && copy_to_user(pointer, &reset, sizeof(reset)))
			result = -EFAULT;
		break;
	}
	default:
		result = -ENOTTY;
	}

unlock:
	mutex_unlock(&device->lock);
	return result;
}

static void uad2_release_device(struct kref *refcount)
{
	struct uad2_compute_device *device = container_of(refcount,
		struct uad2_compute_device, refcount);

	kfree(device->misc.name);
	kfree(device);
}

static int uad2_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct uad2_compute_device *device = container_of(misc,
		struct uad2_compute_device, misc);
	int result = 0;

	mutex_lock(&device->lock);
	if (device->removing || !kref_get_unless_zero(&device->refcount))
		result = -ENODEV;
	else
		file->private_data = device;
	mutex_unlock(&device->lock);
	if (result)
		return result;
	return nonseekable_open(inode, file);
}

static int uad2_release(struct inode *inode, struct file *file)
{
	struct uad2_compute_device *device = file->private_data;

	(void)inode;
	kref_put(&device->refcount, uad2_release_device);
	return 0;
}

static const struct file_operations uad2_fops = {
	.owner = THIS_MODULE,
	.open = uad2_open,
	.release = uad2_release,
	.unlocked_ioctl = uad2_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = uad2_ioctl,
#endif
	.llseek = noop_llseek,
};

static void uad2_free_pages(struct uad2_compute_device *device)
{
	unsigned int dsp, ring, page;

	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++)
		for (ring = 0; ring < UAD2_RING_COUNT; ring++)
			for (page = 0; page < UAD2_RING_PAGES; page++) {
				struct uad2_ring_page *ring_page =
					&device->pages[dsp][ring][page];

				if (!ring_page->cpu)
					continue;
				dma_free_coherent(&device->pdev->dev,
					UAD2_RING_PAGE_BYTES, ring_page->cpu,
					ring_page->dma);
				ring_page->cpu = NULL;
			}
}

static int uad2_allocate_pages(struct uad2_compute_device *device)
{
	unsigned int dsp, ring, page;

	for (dsp = 0; dsp < UAD2_DSP_COUNT; dsp++)
		for (ring = 0; ring < UAD2_RING_COUNT; ring++)
			for (page = 0; page < UAD2_RING_PAGES; page++) {
				struct uad2_ring_page *ring_page =
					&device->pages[dsp][ring][page];

				ring_page->cpu = dma_alloc_coherent(&device->pdev->dev,
					UAD2_RING_PAGE_BYTES, &ring_page->dma,
					GFP_KERNEL);
				if (!ring_page->cpu)
					return -ENOMEM;
				memset(ring_page->cpu, 0, UAD2_RING_PAGE_BYTES);
			}
	return 0;
}

static int uad2_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct uad2_compute_device *device;
	int result;

	(void)id;
	if (PAGE_SIZE != UAD2_RING_PAGE_BYTES)
		return dev_err_probe(&pdev->dev, -EOPNOTSUPP,
			"requires 4096-byte host pages\n");
	if (pci_resource_len(pdev, 0) != UAD2_BAR0_SIZE)
		return dev_err_probe(&pdev->dev, -EINVAL,
			"refusing BAR0 length %pa, expected 0x10000\n",
			&pdev->resource[0].end);

	device = kzalloc(sizeof(*device), GFP_KERNEL);
	if (!device)
		return -ENOMEM;
	device->pdev = pdev;
	device->index = atomic_fetch_inc(&uad2_next_index);
	mutex_init(&device->lock);
	kref_init(&device->refcount);
	pci_set_drvdata(pdev, device);

	result = pci_enable_device_mem(pdev);
	if (result)
		goto put_device;
	result = pci_request_region(pdev, 0, "uad2_compute");
	if (result)
		goto disable_device;
	result = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (result)
		goto release_region;
	device->bar = pci_iomap(pdev, 0, UAD2_BAR0_SIZE);
	if (!device->bar) {
		result = -ENOMEM;
		goto release_region;
	}
	if (!uad2_cold_precondition(device)) {
		result = dev_err_probe(&pdev->dev, -EBUSY,
			"cold-state precondition failed\n");
		goto unmap_bar;
	}
	result = uad2_allocate_pages(device);
	if (result)
		goto free_pages;

	device->misc.minor = MISC_DYNAMIC_MINOR;
	device->misc.name = kasprintf(GFP_KERNEL, "uad2_compute%u",
		device->index);
	device->misc.fops = &uad2_fops;
	device->misc.parent = &pdev->dev;
	device->misc.mode = 0600;
	if (!device->misc.name) {
		result = -ENOMEM;
		goto free_pages;
	}
	result = misc_register(&device->misc);
	if (result)
		goto free_pages;

	dev_info(&pdev->dev,
		 "registered /dev/%s with 64 coherent ring pages; transport stopped\n",
		 device->misc.name);
	return 0;

free_pages:
	uad2_free_pages(device);
unmap_bar:
	pci_iounmap(pdev, device->bar);
release_region:
	pci_release_region(pdev, 0);
disable_device:
	pci_disable_device(pdev);
put_device:
	pci_set_drvdata(pdev, NULL);
	kref_put(&device->refcount, uad2_release_device);
	return result;
}

static void uad2_remove(struct pci_dev *pdev)
{
	struct uad2_compute_device *device = pci_get_drvdata(pdev);
	int stop_result;

	misc_deregister(&device->misc);
	mutex_lock(&device->lock);
	device->removing = true;
	stop_result = uad2_stop_locked(device);
	if (stop_result)
		dev_err(&pdev->dev, "transport recovery failed during removal\n");
	uad2_free_pages(device);
	pci_iounmap(pdev, device->bar);
	pci_release_region(pdev, 0);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	mutex_unlock(&device->lock);
	kref_put(&device->refcount, uad2_release_device);
}

static const struct pci_device_id uad2_ids[] = {
	{
		PCI_DEVICE_SUB(UAD2_VENDOR_ID, UAD2_DEVICE_ID,
			       UAD2_SUBVENDOR_ID, UAD2_SUBDEVICE_OCTO)
	},
	{ }
};
MODULE_DEVICE_TABLE(pci, uad2_ids);

static struct pci_driver uad2_driver = {
	.name = "uad2_compute",
	.id_table = uad2_ids,
	.probe = uad2_probe,
	.remove = uad2_remove,
};
module_pci_driver(uad2_driver);

MODULE_AUTHOR("UAD-2 OCTO reverse-engineering project");
MODULE_DESCRIPTION("Experimental bounded UAD-2 OCTO ring transport");
MODULE_LICENSE("GPL");
