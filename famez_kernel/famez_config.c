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

	if (msglen >= config->max_msglen)
		return -E2BIG;
	// Keep nodename and msg pointer, update msglen and msg contents
	memset(config->my_slot->msg, 0, config->max_msglen);
	config->my_slot->msglen = msglen;
	memcpy(config->my_slot->msg, msg, msglen);
	ringer.vector = config->my_id;		// from this
	ringer.peer = peer_id;			// to this
	config->regs->Doorbell = ringer.push;
	return 0;
}

//-------------------------------------------------------------------------

int famez_config(struct famez_configuration *config)
{
	struct pci_dev *dev_famez = NULL;
	struct resource *bar1 = NULL;
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

	// Map the regions and overlay data structures.  Since it's QEMU,
	// ioremap (uncached) for BAR0/1 and ioremap_cached(BAR2) would be
	// fine.  However, do it with proscribed calls here so it will
	// do the start/end extraction and length math.

	ret = -EFAULT;
	if (!(config->regs = pci_iomap(dev_famez, 0, 0))) {
		pr_err(FZ "can't map memory for registers\n");
		goto putitback;
	}
	if (!(config->msix = pci_iomap(dev_famez, 1, 0))) {
		pr_err(FZ "can't map memory for MSI-X\n");
		goto undo_regs;
	}
	if (!(config->globals = pci_iomap(dev_famez, 2, 0))) {
		pr_err(FZ "can't map memory for mailxbox\n");
		goto undo_msix;
	}

	// Docs for pci_iomap() say to use io[read|write]32.
	// Since this is QEMU, direct memory references should work.

	config->my_id = config->regs->IVPosition;
	config->server_id = config->globals->nSlots - 1;
	config->max_msglen = config->globals->slotsize -
			     config->globals->msg_offset;

	// My slot and invariant info.
	config->my_slot = (void *)((uint64_t)config->globals +
		config->my_id * config->globals->slotsize);
	memset(config->my_slot, 0, config->globals->slotsize);
	snprintf(config->my_slot->nodename,
		 sizeof(config->my_slot->nodename) - 1,
		 utsname()->nodename);
	config->my_slot->msg = (void *)((uint64_t)config->my_slot +
		config->globals->msg_offset);
	pr_info(FZSP "slot size = %llu, server ID = %d\n",
		config->globals->slotsize, config->server_id);

	// Tell the server I'm here.  Cover the NUL terminator in the length.
	sprintf(buf80, "Client %d is ready", config->my_id);
	if ((ret = famez_sendmail(
		config->server_id, buf80, strlen(buf80) + 1, config)) < 0)
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
	iounmap(config->globals);
	iounmap(config->msix);
	iounmap(config->regs);
	pci_dev_put(config->pci_dev);
	memset(config, 0, sizeof(*config));
}

