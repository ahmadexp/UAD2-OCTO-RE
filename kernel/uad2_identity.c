// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-shot, read-only identity probe for UAD-2 PCIe OCTO 1a00:0002/0005.
 *
 * This module deliberately contains no MMIO write, DMA, interrupt, reset, or
 * firmware path. It maps BAR0, reads six allowlisted 32-bit words, logs them,
 * and immediately releases the mapping. Returning -ENODEV prevents the probe
 * driver from remaining bound to the endpoint.
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>

#define UA_VENDOR_ID 0x1a00
#define UA_DEVICE_ID 0x0002
#define UA_SUBVENDOR_ID 0x1a00
#define UA_SUBDEVICE_OCTO 0x0005
#define UA_BAR0_SIZE 0x10000

struct uad2_identity_word {
	u32 offset;
	const char *name;
};

static const struct uad2_identity_word uad2_identity_words[] = {
	{ 0x0020, "identity_word_0" },
	{ 0x0024, "identity_word_1" },
	{ 0x0028, "identity_word_2" },
	{ 0x002c, "identity_word_3" },
	{ 0x2218, "fpga_revision" },
	{ 0x2234, "extended_capabilities" },
};

static int uad2_identity_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	void __iomem *bar;
	u16 command;
	unsigned int i;
	int ret;

	if (pci_resource_len(pdev, 0) != UA_BAR0_SIZE) {
		dev_err(&pdev->dev, "refusing BAR0 length %pa, expected 0x10000\n",
			&pdev->resource[0].end);
		return -EINVAL;
	}

	ret = pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (ret)
		return pcibios_err_to_errno(ret);
	if (command & PCI_COMMAND_MASTER) {
		dev_err(&pdev->dev, "refusing probe while bus mastering is enabled\n");
		return -EPERM;
	}

	ret = pci_enable_device_mem(pdev);
	if (ret)
		return ret;

	ret = pci_request_region(pdev, 0, "uad2_identity");
	if (ret)
		goto disable;

	bar = pci_iomap(pdev, 0, UA_BAR0_SIZE);
	if (!bar) {
		ret = -ENOMEM;
		goto release;
	}

	dev_info(&pdev->dev, "UAD2_IDENTITY_BEGIN\n");
	for (i = 0; i < ARRAY_SIZE(uad2_identity_words); i++) {
		u32 value = readl(bar + uad2_identity_words[i].offset);

		dev_info(&pdev->dev, "UAD2_IDENTITY 0x%04x %-24s 0x%08x\n",
			 uad2_identity_words[i].offset,
			 uad2_identity_words[i].name, value);
	}
	dev_info(&pdev->dev, "UAD2_IDENTITY_END\n");

	pci_iounmap(pdev, bar);
	ret = -ENODEV;

release:
	pci_release_region(pdev, 0);
disable:
	pci_disable_device(pdev);
	return ret;
}

static const struct pci_device_id uad2_identity_ids[] = {
	{
		PCI_DEVICE_SUB(UA_VENDOR_ID, UA_DEVICE_ID,
			       UA_SUBVENDOR_ID, UA_SUBDEVICE_OCTO)
	},
	{ }
};
MODULE_DEVICE_TABLE(pci, uad2_identity_ids);

static struct pci_driver uad2_identity_driver = {
	.name = "uad2_identity",
	.id_table = uad2_identity_ids,
	.probe = uad2_identity_probe,
};
module_pci_driver(uad2_identity_driver);

MODULE_AUTHOR("UAD-2 OCTO reverse-engineering project");
MODULE_DESCRIPTION("Read-only UAD-2 OCTO identity probe");
MODULE_LICENSE("GPL");
