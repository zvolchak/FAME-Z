// Initial discovery and setup of IVSHMEM/IVSHMSG device

#include <linux/interrupt.h>
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
// Map the regions and overlay data structures.  Since it's QEMU, ioremap
// (uncached) for BAR0/1 and ioremap_cached(BAR2) would be fine.  However,
// do it with proscribed calls here to do the start/end/length math.

static int mapthings(struct famez_configuration *config, struct pci_dev *dev)
{
	int ret = -EFAULT;

	if (!(config->regs = pci_iomap(dev, 0, 0))) {
		pr_err(FZ "can't map memory for registers\n");
		return ret;
	}
	if (!(config->msix = pci_iomap(dev, 1, 0))) {
		pr_err(FZ "can't map memory for MSI-X\n");
		return ret;
	}
	if (!(config->globals = pci_iomap(dev, 2, 0))) {
		pr_err(FZ "can't map memory for mailxbox\n");
		return ret;
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
	return 0;
}

//-------------------------------------------------------------------------
// https://elixir.bootlin.com/linux/latest/source/drivers/pci/msi.c#L32
// gets the vector count.  The table is global because the IRQ handler
// needs something with an integer corresponding to the MSI vector.
// Rather than make a new array of integers, just use the .entry.

static struct msix_entry *msix_entries = NULL;

irqreturn_t all_msix(int theint, void *thepointer) {
	return IRQ_RETVAL(1);
}

static int setupMSIX(struct famez_configuration *config, struct pci_dev *dev)
{
	int i, ret, nvectors = 0;

	if ((nvectors = pci_msix_vec_count(dev)) < 0) {
		pr_err(FZ "Error retrieving MSI-X vector count\n");
		return nvectors;
	}
	pr_info(FZSP "%d MSI-X vectors (%s)\n",
		nvectors, dev->msix_enabled ? "en" : "dis");
	if (!nvectors) {
		pr_err(FZ "Zero MSI-X vectors\n");	// QEMU invocation
		return -EINVAL;
	}
	if (config->globals->nSlots > nvectors) {
		pr_err(FZ "not enough MSI-X vectors\n");
		return -EDOM;
	}
	if (!(msix_entries = kzalloc(nvectors * sizeof(struct msix_entry), GFP_KERNEL))) {
		pr_err(FZ "Can't create MSI-X entries table\n");
		return -ENOMEM;
	}
	for (i = 0; i < nvectors; i++) 		// .vector zeroed by kzalloc
		msix_entries[i].entry  = i;
	if ((ret = pci_enable_msix_exact(dev, msix_entries, nvectors))) {
		pr_err(FZ "Can't enable MSI-X vectors\n");
		return ret;
	}

	// Attach each IRQ to the same handler.  FIXME pass in something
	// with a backpointer to dev.
	for (i = 0; i < nvectors; i++) {
		if ((ret = request_irq(
			msix_entries[i].vector,
			all_msix,
			0,
			"FAME-Z",
			&msix_entries[i]))) {
		pr_err(FZ "Can't request IRQ for entry %d\n", i);
		return ret;
		}
	}

	return 0;
}

//-------------------------------------------------------------------------
// Find the first one with two BARs and MSI-X (slightly redundant).

int famez_config(struct famez_configuration *config)
{
	struct pci_dev *dev = NULL;
	int ret = 0;
	char buf80[80];

	memset(config, 0, sizeof(*config));

	pr_info("-----------------------------------------------------------");
	pr_info(FZ FAMEZ_VERSION "; module loaded with\n");
	pr_info(FZSP "famez_verbose = %d\n", famez_verbose);

	while ((dev = pci_get_device(IVSHMEM_VENDOR, IVSHMEM_DEVICE, dev))) {
		struct resource *bar1;

		bar1 = &(dev->resource[1]);
		if (!bar1->start || dev->revision != 1 || !dev->msix_cap) {
			pr_info(FZSP "skipping an IVSHMEM @ %s\n", bar1->name);
			continue;
		}
		pr_info(FZ "trying the IVSHMEM @ %s\n", bar1->name);
		break;
	}
	if (!dev)
		return -ENODEV;

	if ((ret = mapthings(config, dev)))
		goto unmapthings;
	pr_info(FZSP "slot size = %llu, server ID = %d\n",
		config->globals->slotsize, config->server_id);

	if ((ret = setupMSIX(config, dev)))
		goto unIRQ;

	// Tell the server I'm here.  Cover the NUL terminator in the length.
	sprintf(buf80, "Client %d is ready", config->my_id);
	if ((ret = famez_sendmail(
		config->server_id, buf80, strlen(buf80) + 1, config)) < 0)
			return ret;

	pci_dev_get(dev);	// Keep it
	config->pci_dev = dev;
	return 0;

unIRQ:
unmapthings:
	famez_unconfig(config);
	return ret;
}

//-------------------------------------------------------------------------
// Can be called from config early errors, check fields first.  PCI subsystem
// routines already do this.

void famez_unconfig(struct famez_configuration *config)
{
	pci_disable_msix(config->pci_dev);
	kfree(msix_entries);
	if (config->globals) iounmap(config->globals);
	if (config->msix) iounmap(config->msix);
	if (config->regs) iounmap(config->regs);
	pci_dev_put(config->pci_dev);
	memset(config, 0, sizeof(*config));
}

