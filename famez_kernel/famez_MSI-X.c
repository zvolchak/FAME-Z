// Configure and handle MSI-X interrupts from IVSHMEM device.

#include <linux/interrupt.h>
#include <linux/pci.h>

#include "famez.h"

//-------------------------------------------------------------------------

struct famez_mailslot __iomem *famez_get_mailslot(
	struct famez_configuration *config,
	unsigned slotnum)
{
	struct famez_mailslot __iomem *slot;

	// Slot 0 is the globals data, don't play in there.  The last slot
	// (nSlots - 1) is the for the server.  This code gets [1, nSlots-2]
	if (slotnum < 1 || slotnum >= config->globals->nSlots) {
		pr_err(FZ ": %u is out of range\n", slotnum);
		return NULL;
	}
	slot = (void *)(
		(uint64_t)config->globals + slotnum * config->globals->slotsize);
	slot->msg = (void *)((uint64_t)slot + config->globals->msg_offset);
	return slot;
}

static irqreturn_t all_msix(int vector, void *data) {
	struct famez_configuration *config = data;
	int slotnum;
	uint16_t peer_id = 0;	// see pci.h for msix_entry
	struct famez_mailslot __iomem *peer_slot;

	// Match the IRQ vector to entry/vector pair which yields the sender.
	// Turns out i and msix_entries[i].entry are identical in famez.
	for (slotnum = 1; slotnum < config->globals->nSlots; slotnum++) {
		if (vector == config->msix_entries[slotnum].vector) {
			peer_id = config->msix_entries[slotnum].entry;
			break;
		}
	}
	if (slotnum >= config->globals->nSlots) {
		pr_err(FZ "IRQ handler could not match vector %d\n", vector);
		return IRQ_NONE;
	}
	if (!(peer_slot = famez_get_mailslot(config, peer_id))) {
		pr_err(FZ "Could not match peer %u\n", peer_id);
		return IRQ_HANDLED;
	}
	pr_info(FZ "IRQ %d == peer %u -> \"%s\"\n",
		vector, peer_id, peer_slot->msg);

	// Easy way to see if this thing is alive.  5 == len('Pong') + NUL
	if STREQ(peer_slot->msg, "ping")
		famez_sendmsg(peer_id, "Pong", 5, config);

	// FIXME send this info off to somewhere useful :-)
	return IRQ_HANDLED;
}

//-------------------------------------------------------------------------

int famez_MSIX_setup(struct famez_configuration *config, struct pci_dev *dev)
{
	int i, ret, nvectors = 0;

	if ((nvectors = pci_msix_vec_count(dev)) < 0) {
		pr_err(FZ "Error retrieving MSI-X vector count\n");
		return nvectors;
	}
	pr_info(FZSP "%2d MSI-X vectors available (%sabled)\n",
		nvectors, dev->msix_enabled ? "en" : "dis");
	if (!nvectors) {
		pr_err(FZ "Zero MSI-X vectors\n");	// QEMU invocation
		return -EINVAL;
	}
	if (config->globals->nSlots > nvectors) {
		pr_err(FZ "not enough MSI-X vectors\n");
		return -EDOM;
	}

	nvectors = config->globals->nSlots;		// Concession to legibility
	if (!(config->msix_entries = kzalloc(
		nvectors * sizeof(struct msix_entry), GFP_KERNEL))) {
		pr_err(FZ "Can't create MSI-X entries table\n");
		return -ENOMEM;
	}
	// .vector was zeroed by kzalloc
	for (i = 0; i < nvectors; i++)
		config->msix_entries[i].entry  = i;

	if ((ret = pci_alloc_irq_vectors(
		dev, nvectors, nvectors, PCI_IRQ_MSIX)) < 0) {
			pr_err(FZ "Can't allocate MSI-X vectors\n");
			return ret;
		}
	pr_info(FZSP "%2d MSI-X vectors used      (%sabled)\n",
		ret, dev->msix_enabled ? "en" : "dis");
	if (ret < nvectors) {
		pr_err(FZ "%d vectors are not enough\n", ret);
		ret = -ENOSPC;		// Akin to pci_alloc_irq_vectors
		goto cleanup_on_aisle_43;
	}

	// Attach each IRQ to the same handler.  pci_irq_vector() walks a
	// list and returns info on a match.  The only failure is if a 
	// match isn't found, nothing is allocated.  Do it up front and
	// reuse the table from the old pci_msix_xxx calls.  Then there's
	// nothing to clean up.
	for (i = 0; i < nvectors; i++) {
		if ((ret = pci_irq_vector(dev, i)) < 0) {
			pr_err("pci_irq_vector(%d) failed: %d\n", i, ret);
			goto cleanup_on_aisle_43;
		}
		config->msix_entries[i].vector = ret;
	}
	// Now that they're all batched, assign them.
	for (i = 0; i < nvectors; i++) {
		if ((ret = request_irq(
			config->msix_entries[i].vector,
			all_msix,
			0,
			"FAME-Z",
			config))) {
				pr_err(FZ "request_irq(%d) failed: %d\n", i, ret);
				goto cleanup_on_aisle_43;
		}
		pr_info(FZSP "%d = %d\n", i, config->msix_entries[i].vector);
	}
	return 0;

cleanup_on_aisle_43:
	famez_MSIX_teardown(config);
	return ret;
}

//-------------------------------------------------------------------------
// There is no disable control on this "device", hope one doesn't fire...
// Can be called from setup() above.

void famez_MSIX_teardown(struct famez_configuration *config)
{
	int i;

	if (!config->msix_entries)	// Been there, done that
		return;
	
	for (i = 0; i < config->globals->nSlots; i++)
		free_irq(config->msix_entries[i].vector, config);
	pci_free_irq_vectors(config->pci_dev);
	kfree(config->msix_entries);
	config->msix_entries = NULL;
}
