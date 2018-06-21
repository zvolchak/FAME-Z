// Initial discovery and setup of IVSHMEM/IVSHMSG device

#include <linux/pci.h>

#include "famez.h"

//-------------------------------------------------------------------------

int famez_getconfig(struct famez_configuration *config)
{
	struct pci_dev *dev_famez = NULL;
	struct resource *bar1, *bar2 = NULL;

	memset(config, 0, sizeof(*config));

	pr_info("-----------------------------------------------------------");
	pr_info("famez: " FAMEZ_VERSION "; module loaded with\n");
	pr_info("       famez_verbose = %d\n", famez_verbose);

	// Find the first one with two BARs.  There should be only one.
	while ((dev_famez = pci_get_device(
			IVSHMEM_VENDOR, IVSHMEM_DEVICE, dev_famez))) {

		bar1 = &(dev_famez->resource[1]);
		bar2 = &(dev_famez->resource[2]);
		if (!bar1->start || dev_famez->revision != 1) {
			pr_info("Skipping an IVSHMEM %s\n", bar1->name);
			continue;
		}
		break;
	}
	if (!dev_famez)
		return -ENODEV;

	pci_dev_put(dev_famez);

	pr_info("       FAME-Z control  = 0x%llx - 0x%llx\n",
		bar1->start, bar1->end);
	pr_info("       FAME-Z mailbox  = 0x%llx - 0x%llx\n",
		bar2->start, bar2->end);

	config->pci_dev = dev_famez;
	config->res_registers = bar1;
	config->res_mailbox = bar2;

	// TODO: map the regions, set the interrupt handler.

	return 0;
}

//-------------------------------------------------------------------------

void famez_unconfig(struct famez_configuration *config)
{
}

