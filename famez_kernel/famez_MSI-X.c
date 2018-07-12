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

//-------------------------------------------------------------------------

static irqreturn_t all_msix(int vector, void *data) {
	struct famez_configuration *config = data;
	int slotnum;
	uint16_t peer_id = 0;	// see pci.h for msix_entry
	struct famez_mailslot __iomem *peer_slot;

	// Match the IRQ vector to entry/vector pair which yields the sender.
	// Turns out i and msix_entries[i].entry are identical in famez.
	// FIXME: preload a lookup table if I ever care about speed.
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

	// All returns from here are IRQ_HANDLED

	if (!(peer_slot = famez_get_mailslot(config, peer_id))) {
		pr_err(FZ "Could not match peer %u\n", peer_id);
		return IRQ_HANDLED;
	}
	PR_V1(FZ "IRQ %d == peer %u -> \"%s\"\n",
		vector, peer_id, peer_slot->msg);

	// Easy way to see if this thing is alive.
	spin_lock(&config->buffer_slot_lock);
	if STREQ_N(peer_slot->msg, "ping", 4) {
		char pong[16];

		snprintf(pong, sizeof(pong) - 1, "pong (%2d)", config->my_id);
		famez_sendstring(peer_id, pong, config);
	} else {
		// FIXME: stompage occurs, should return NACK if necessary
		memcpy(&config->buffer_slot, peer_slot, config->globals->slotsize);
		// FIXME: better abstraction and encapsulation
		wake_up_all(&bridge_reader_wait);
	}
	spin_unlock(&config->buffer_slot_lock);

	// Clear the send mailslot so it can go: HW holdoff of pseudo-chip
	peer_slot->msglen = 0;
	return IRQ_HANDLED;
}

//-------------------------------------------------------------------------
// As there are only nSlots-2 actual clients (because mailslot 0 is globals
// and server @ nslots-1) I SHOULDN'T actually activate those two IRQs.

int famez_MSIX_setup(struct pci_dev *pdev)
{
	struct famez_configuration *config = pci_get_drvdata(pdev);
	int ret, i, nvectors = 0, last_irq_index;

	if ((nvectors = pci_msix_vec_count(pdev)) < 0) {
		pr_err(FZ "Error retrieving MSI-X vector count\n");
		return nvectors;
	}
	pr_info(FZSP "%2d MSI-X vectors available (%sabled)\n",
		nvectors, pdev->msix_enabled ? "en" : "dis");
	if (nvectors != 64) {	// Convention in FAME emulation_configure.sh
		pr_err(FZ "Expected 64 MSI-X vectors, not %d\n", nvectors);
		return -EINVAL;
	}

	// Remember, don't need a vector for slot 0
	if (config->globals->nSlots > nvectors) {
		pr_err(FZ "need %llu MSI-X vectors, only %d available\n",
			config->globals->nSlots, nvectors);
		return -ENOSPC;
	}

	nvectors = config->globals->nSlots;		// legibility
	if (!(config->msix_entries = kzalloc(
		nvectors * sizeof(struct msix_entry), GFP_KERNEL))) {
		pr_err(FZ "Can't create MSI-X entries table\n");
		return -ENOMEM;
	}
	// .vector was zeroed by kzalloc
	for (i = 0; i < nvectors; i++)
		config->msix_entries[i].entry  = i;

	// There used to be a direct call for "exact match".  Re-create it.
	if ((ret = pci_alloc_irq_vectors(
		pdev, nvectors, nvectors, PCI_IRQ_MSIX)) < 0) {
			pr_err(FZ "Can't allocate MSI-X vectors\n");
			goto err_kfree_msix_entries;
		}
	pr_info(FZSP "%2d MSI-X vectors used      (%sabled)\n",
		ret, pdev->msix_enabled ? "en" : "dis");
	if (ret < nvectors) {
		pr_err(FZ "%d vectors are not enough\n", ret);
		ret = -ENOSPC;		// Akin to pci_alloc_irq_vectors
		goto err_pci_free_irq_vectors;
	}

	// Attach each IRQ to the same handler.  pci_irq_vector() walks a
	// list and returns info on a match.  Success is merely a lookup,
	// not an allocation, so there's nothing to clean up from this step.
	// Reuse the table from the old pci_msix_xxx calls.  Note that
	// requested vectors are still option base 0.
	for (i = 0; i < nvectors; i++) {
		if ((ret = pci_irq_vector(pdev, i)) < 0) {
			pr_err("pci_irq_vector(%d) failed: %d\n", i, ret);
			goto err_pci_free_irq_vectors;
		}
		config->msix_entries[i].vector = ret;
	}

	// Now that they're all batched, assign them.  Each successful request
	// must be matched by a free_irq() someday.  No, the return value
	// is not stored anywhere.
	for (last_irq_index = 0;
	     last_irq_index < nvectors;
	     last_irq_index++) {
		if ((ret = request_irq(
			config->msix_entries[last_irq_index].vector,
			all_msix,
			0,
			"FAME-Z",
			config))) {
				pr_err(FZ "request_irq(%d) failed: %d\n",
					last_irq_index, ret);
				goto err_free_completed_irqs;
		}
		PR_V1(FZSP "%d = %d\n",
		      last_irq_index,
		      config->msix_entries[last_irq_index].vector);
	}
	return 0;

err_free_completed_irqs:
	for (i = 0; i < last_irq_index; i++)
		free_irq(config->msix_entries[i].vector, config);

err_pci_free_irq_vectors:
	pci_free_irq_vectors(pdev);

err_kfree_msix_entries:
	kfree(config->msix_entries);
	config->msix_entries = NULL;	// sentinel for teardown
	return ret;
}

//-------------------------------------------------------------------------
// There is no disable control on this "device", hope one doesn't fire...
// Can be called from setup() above.

void famez_MSIX_teardown(struct pci_dev *pdev)
{
	struct famez_configuration *config = pci_get_drvdata(pdev);
	int i;

	if (!config->msix_entries)	// Been there, done that
		return;

	for (i = 0; i < config->globals->nSlots; i++)
		free_irq(config->msix_entries[i].vector, config);
	pci_free_irq_vectors(pdev);
	kfree(config->msix_entries);
	config->msix_entries = NULL;
}
