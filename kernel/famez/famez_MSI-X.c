// Arch-specific ISR handler for x86_64: configure and handle MSI-X interrupts
// from IVSHMEM device.

#include <linux/interrupt.h>	// irq_enable, etc

#include "famez.h"

//-------------------------------------------------------------------------
// FIXME: can a spurious interrupt get me here "too fast" so that I'm
// overrunning the incoming slot during a tight loop client?

static irqreturn_t all_msix(int vector, void *data) {
	struct famez_config *config = data;
	struct msix_entry *msix_entries = config->IRQ_private;
	int slotnum, stomped = 0;
	uint16_t incoming_id = 0;	// see pci.h for msix_entry
	struct famez_mailslot __iomem *incoming_slot;

	spin_lock(&(config->incoming_slot_lock));

	// Match the IRQ vector to entry/vector pair which yields the sender.
	// Turns out i and msix_entries[i].entry are identical in famez.
	// FIXME: preload a lookup table if I ever care about speed.
	for (slotnum = 1; slotnum < config->globals->nEvents; slotnum++) {
		if (vector == msix_entries[slotnum].vector)
			break;
	}
	if (slotnum >= config->globals->nEvents) {
		spin_unlock(&(config->incoming_slot_lock));
		pr_err(FZ "IRQ handler could not match vector %d\n", vector);
		return IRQ_NONE;
	}
	incoming_id = msix_entries[slotnum].entry;

	// All returns from here are IRQ_HANDLED

	if (!(incoming_slot = calculate_mailslot(config, incoming_id))) {
		spin_unlock(&(config->incoming_slot_lock));
		pr_err(FZ "Could not match peer %u\n", incoming_id);
		return IRQ_HANDLED;
	}

	// This may do weird things with the spinlock held...
	PR_V2("IRQ %d == sender %u -> \"%s\"\n",
		vector, incoming_id, incoming_slot->buf);

	// Easy loopback test as proof of life.  Handle it all right here
	// right now, don't let driver layers even see it.
	if (incoming_slot->buflen == 4 &&
	    STREQ_N(incoming_slot->buf, "ping", 4)) {
		// Needs to be okay with interrupt context.  Signal completion.
		spin_unlock(&(config->incoming_slot_lock));
		incoming_slot->buflen = 0;	// buf received
		famez_create_outgoing(FAMEZ_SID_CID_IS_PEER_ID, incoming_id,
			"pong", 4, config);
		return IRQ_HANDLED;
	}
	if (config->incoming_slot)	// print outside the spinlock
		stomped = config->incoming_slot->peer_id;
	config->incoming_slot = incoming_slot;
	spin_unlock(&(config->incoming_slot_lock));

	wake_up(&(config->incoming_slot_wqh));
	if (stomped)
		pr_warn(FZ "%s() stomped incoming slot for reader %d\n",
			__FUNCTION__, config->my_id);
	return IRQ_HANDLED;
}

//-------------------------------------------------------------------------
// As there are only nClients actual clients (because mailslot 0 is globals
// and server @ nslots-1) I SHOULDN'T actually activate those two IRQs.

int famez_ISR_setup(struct pci_dev *pdev)
{
	struct famez_config *config = pci_get_drvdata(pdev);
	int ret, i, nvectors = 0, last_irq_index;
	struct msix_entry *msix_entries;	// pci.h, will be an array

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
	if (config->globals->nEvents > nvectors) {
		pr_err(FZ "need %llu MSI-X vectors, only %d available\n",
			config->globals->nEvents, nvectors);
		return -ENOSPC;
	}
	nvectors = config->globals->nEvents;		// legibility

	ret = -ENOMEM;
	if (!(msix_entries = kzalloc(
			nvectors * sizeof(struct msix_entry), GFP_KERNEL))) {
		pr_err(FZ "Can't allocate MSI-X entries table\n");
		goto err_kfree_msix_entries;
	}
	config->IRQ_private = msix_entries;

	// .vector was zeroed by kzalloc
	for (i = 0; i < nvectors; i++)
		msix_entries[i].entry  = i;

	// There used to be a direct call for "exact match".  Re-create it.
	if ((ret = pci_alloc_irq_vectors(
		pdev, nvectors, nvectors, PCI_IRQ_MSIX)) < 0) {
			pr_err(FZ "Can't allocate MSI-X IRQ vectors\n");
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
		msix_entries[i].vector = ret;
	}

	// Now that they're all batched, assign them.  Each successful request
	// must be matched by a free_irq() someday.  No, the return value
	// is not stored anywhere.
	for (last_irq_index = 0;
	     last_irq_index < nvectors;
	     last_irq_index++) {
		if ((ret = request_irq(
			msix_entries[last_irq_index].vector,
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
		      msix_entries[last_irq_index].vector);
	}
	return 0;

err_free_completed_irqs:
	for (i = 0; i < last_irq_index; i++)
		free_irq(msix_entries[i].vector, config);

err_pci_free_irq_vectors:
	pci_free_irq_vectors(pdev);

err_kfree_msix_entries:
	kfree(msix_entries);
	config->IRQ_private = NULL;	// sentinel for teardown
	return ret;
}

//-------------------------------------------------------------------------
// There is no disable control on this "device", hope one doesn't fire...
// Can be called from setup() above.

void famez_ISR_teardown(struct pci_dev *pdev)
{
	struct famez_config *config = pci_get_drvdata(pdev);
	struct msix_entry *msix_entries = config->IRQ_private;
	int i;

	if (!msix_entries)	// Been there, done that
		return;

	for (i = 0; i < config->globals->nClients + 2; i++)
		free_irq(msix_entries[i].vector, config);
	pci_free_irq_vectors(pdev);
	kfree(msix_entries);
	config->IRQ_private = NULL;
}
