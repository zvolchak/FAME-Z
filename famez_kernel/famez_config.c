// Initial discovery and setup of IVSHMEM/IVSHMSG device

#include <linux/pci.h>
#include <linux/utsname.h>

#include "famez.h"

//-------------------------------------------------------------------------
// Return positive on success, negative on error, never 0.

int famez_sendmail(uint32_t peer_id, char *msg, ssize_t msglen,
		   struct famez_configuration *config)
{
	struct ringer ringer;

	if (msglen > config->max_msglen)
		return -E2BIG;
	memset(config->my_mbox, 0, config->mbox->slotsize);
	snprintf(config->my_mbox->nodename,
		 sizeof(config->my_mbox->nodename) - 1,
		 utsname()->nodename);
	config->my_mbox->msglen = msglen;
	memcpy(config->my_mbox->msg, msg, msglen);
	ringer.vector = config->my_id;		// from this
	ringer.peer = peer_id;			// to this
	config->regs->Doorbell = ringer.push;
	return 0;
}

//-------------------------------------------------------------------------

int famez_config(struct famez_configuration *config)
{
	struct pci_dev *dev_famez = NULL;
	struct resource *bar0 = NULL, *bar1 = NULL, *bar2 = NULL;
	int ret = 0;
	char buf80[80];

	memset(config, 0, sizeof(*config));

	pr_info("-----------------------------------------------------------");
	pr_info(FZ FAMEZ_VERSION "; module loaded with\n");
	pr_info(FZSP "famez_verbose = %d\n", famez_verbose);

	// Find the first one with two BARs.
	while ((dev_famez = pci_get_device(
			IVSHMEM_VENDOR, IVSHMEM_DEVICE, dev_famez))) {

		bar1 = &(dev_famez->resource[1]);
		if (!bar1->start || dev_famez->revision != 1) {
			pr_info(FZSP "skipping an IVSHMEM @ %s\n", bar1->name);
			continue;
		}
		break;
	}
	if (!dev_famez)
		return -ENODEV;
	
	pci_dev_get(dev_famez);
	pr_info(FZ "keeping the IVSHMEM @ %s\n", bar1->name);

	bar0 = &(dev_famez->resource[0]);
	bar2 = &(dev_famez->resource[2]);
	PR_V1(FZSP "registers = 0x%llx - 0x%llx\n", bar0->start, bar0->end);
	PR_V1(FZSP "MSI-Z/PBA = 0x%llx - 0x%llx\n", bar1->start, bar1->end);
	PR_V1(FZSP "mailbox   = 0x%llx - 0x%llx\n", bar2->start, bar2->end);

	// Map the regions and overlay data structures.  Since it's QEMU,
	// ioremap (uncached) for BAR0/1 and ioremap_cached(BAR2) would be
	// fine.  However, do it with proscribed calls here.

	ret = -ENOMEM;
	if (!(config->regs = pci_iomap(dev_famez, 0, 0))) {
		pr_err(FZ "can't map memory for registers\n");
		goto putitback;
	}
	if (!(config->msix = pci_iomap(dev_famez, 1, 0))) {
		pr_err(FZ "can't map memory for MSI-X\n");
		goto undo_regs;
	}
	if (!(config->mbox = pci_iomap(dev_famez, 2, 0))) {
		pr_err(FZ "can't map memory for mailxbox\n");
		goto undo_msix;
	}

	// Docs for pci_iomap() say to use io[read|write]32.
	// Since this is QEMU, direct memory references should work.

	config->my_id = config->regs->IVPosition;
	config->server_id = config->mbox->nSlots - 1;
	config->max_msglen = config->mbox->slotsize - config->mbox->msg_offset;
	config->my_mbox = (void *)((uint64_t)config->mbox +
		config->my_id * config->mbox->slotsize);
	config->my_mbox->msg = (void *)((uint64_t)config->my_mbox +
		config->mbox->msg_offset);

	pr_info(FZSP "client ID = %d\n", config->my_id);
	pr_info(FZSP "slot size = %llu, server ID = %d\n",
		config->mbox->slotsize, config->server_id);

	// Tell the server I'm here
	sprintf(buf80, "Client %d is ready", config->my_id);
	if ((ret = famez_sendmail(config->server_id, buf80, strlen(buf80) + 1, config)) < 0)
		return ret;

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

