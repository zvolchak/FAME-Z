// Initial discovery and setup of IVSHMEM/IVSHMSG device

#include <linux/pci.h>

#include "famez.h"

//-------------------------------------------------------------------------

int famez_config(struct famez_configuration *config)
{
	struct pci_dev *dev_famez = NULL;
	struct resource *bar0 = NULL, *bar1 = NULL, *bar2 = NULL;
	int ret = 0;
	struct ringer ringer;

	memset(config, 0, sizeof(*config));

	pr_info("-----------------------------------------------------------");
	pr_info("famez: " FAMEZ_VERSION "; module loaded with\n");
	pr_info("     famez_verbose = %d\n", famez_verbose);

	// Find the first one with two BARs.
	while ((dev_famez = pci_get_device(
			IVSHMEM_VENDOR, IVSHMEM_DEVICE, dev_famez))) {

		bar1 = &(dev_famez->resource[1]);
		if (!bar1->start || dev_famez->revision != 1) {
			pr_info("famez: skipping an IVSHMEM @ %s\n",
				bar1->name);
			continue;
		}
		break;
	}
	if (!dev_famez)
		return -ENODEV;
	
	pr_info("famez: keeping the IVSHMEM @ %s\n", bar1->name);
	pci_dev_get(dev_famez);

	bar0 = &(dev_famez->resource[0]);
	bar2 = &(dev_famez->resource[2]);
	pr_info("       registers = 0x%llx - 0x%llx\n", bar0->start, bar0->end);
	pr_info("       MSI-Z/PBA = 0x%llx - 0x%llx\n", bar1->start, bar1->end);
	pr_info("       mailbox   = 0x%llx - 0x%llx\n", bar2->start, bar2->end);

	// Map the regions and overlay data structures

	ret = -ENOMEM;
	if (!(config->regs = pci_iomap(dev_famez, 0, 0)))
		goto putitback;
	if (!(config->msix = pci_iomap(dev_famez, 1, 0)))
		goto undo_regs;
	if (!(config->mbox = pci_iomap(dev_famez, 2, 0)))
		goto undo_msix;

	// Docs for pci_iomap() say to use pci_ioread/write, but since
	// this is QEMU, a direct memory reference should work.

	pr_info("     client ID = %d\n", config->regs->IVPosition);

	ringer.peer = FAMEZ_PEER_SERVER;
	ringer.vector = 5;
	config->regs->Doorbell = ringer.push;

	// TODO: set the interrupt handler.

	return 0;


// undo_mbox:
	// iounmap(config->mbox);

undo_msix:
	iounmap(config->msix);

undo_regs:
	iounmap(config->regs);

putitback:
	pci_dev_put(dev_famez);
	return ret;
}

//-------------------------------------------------------------------------

void famez_unconfig(struct famez_configuration *config)
{
	iounmap(config->mbox);
	iounmap(config->msix);
	iounmap(config->regs);
	pci_dev_put(config->pci_dev);
	memset(config, 0, sizeof(*config));
}

