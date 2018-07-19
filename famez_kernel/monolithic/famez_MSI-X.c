// Configure and handle MSI-X interrupts from IVSHMEM device.  The sendstring
// method is also here, which keeps all "hardware IO" in one place.

#include <linux/delay.h>	// usleep_range, wait_event*
#include <linux/jiffies.h>	// jiffies
#include <linux/interrupt.h>	// irq_enable, etc
#include <linux/pci.h>		// all kinds

#include "famez.h"

//-------------------------------------------------------------------------
// Return positive (bytecount) on success, negative on error, never 0.
// I don't really believe usleep_range is atomic-safe but I'm on mutices now.

int famez_sendmail(uint32_t peer_id, char *msg, size_t msglen,
		   famez_configuration_t *config)
{
	uint64_t hw_timeout = get_jiffies_64() + HZ/2;	// 500 ms
	ivshmsg_ringer_t ringer;

	// Might NOT be printable C string.
	PR_V1("sendmail(%lu bytes) to %d\n", msglen, peer_id);

	if (peer_id < 1 || peer_id > config->server_id)
		return -EBADSLT;
	if (msglen >= config->max_msglen)
		return -E2BIG;
	if (!msglen)
		return -ENODATA; // FIXME: is there value to a "silent kick"?

	// Pseudo-"HW ready": wait until my_slot has pushed a previous write
	// through. In truth it's the previous responder clearing my msglen.
	while (config->my_slot->msglen && get_jiffies_64() < hw_timeout)
		 usleep_range(50000, 80000);
	if (config->my_slot->msglen)
		pr_warn(FZ "%s() stomps previous message to %llu\n",
			__FUNCTION__, config->my_slot->last_responder);

	// Keep nodename and msg pointer; update msglen and msg contents.
	// memset(config->my_slot->msg, 0, config->max_msglen);	# overkill
	config->my_slot->msglen = msglen;
	config->my_slot->msg[msglen] = '\0';	// ASCII strings paranoia
	config->my_slot->last_responder = peer_id;

	memcpy(config->my_slot->msg, msg, msglen);
	ringer.vector = config->my_id;		// from this
	ringer.peer = peer_id;			// to this
	config->regs->Doorbell = ringer.Doorbell;
	return msglen;
}
//-------------------------------------------------------------------------

static famez_mailslot_t __iomem *calculate_mailslot(
	famez_configuration_t *config,
	unsigned slotnum)
{
	famez_mailslot_t __iomem *slot;

	// Slot 0 is the globals data, don't play in there.  The last slot
	// (nSlots - 1) is the for the server.  This code gets [1, nSlots-2].
	// This check should never occur :-)
	if (slotnum < 1 || slotnum >= config->globals->nSlots) {
		pr_err(FZ ": mailslot %u is out of range\n", slotnum);
		return NULL;
	}
	slot = (void *)(
		(uint64_t)config->globals + slotnum * config->globals->slotsize);
	return slot;
}

//-------------------------------------------------------------------------

static irqreturn_t all_msix(int vector, void *data) {
	famez_configuration_t *config = data;
	int slotnum;
	uint16_t sender_id = 0;	// see pci.h for msix_entry
	famez_mailslot_t __iomem *sender_slot;

	// Match the IRQ vector to entry/vector pair which yields the sender.
	// Turns out i and msix_entries[i].entry are identical in famez.
	// FIXME: preload a lookup table if I ever care about speed.
	for (slotnum = 1; slotnum < config->globals->nSlots; slotnum++) {
		if (vector == config->msix_entries[slotnum].vector)
			break;
	}
	if (slotnum >= config->globals->nSlots) {
		pr_err(FZ "IRQ handler could not match vector %d\n", vector);
		return IRQ_NONE;
	}
	sender_id = config->msix_entries[slotnum].entry;

	// All returns from here are IRQ_HANDLED

	if (!(sender_slot = calculate_mailslot(config, sender_id))) {
		pr_err(FZ "Could not match peer %u\n", sender_id);
		return IRQ_HANDLED;
	}
	PR_V2("IRQ %d == sender %u -> \"%s\"\n",
		vector, sender_id, sender_slot->msg);

	sender_slot->peer_id = sender_id;	// FIXME WHY IS THIS NEEDED?

	// Easy loopback test as proof of life.  Handle it all right here
	// right now, don't let driver layers even see it.
	if (sender_slot->msglen == 4 && STREQ_N(sender_slot->msg, "ping", 4))
		famez_sendmail(sender_id, "pong", 4, config);
	else {
		int bad_lock = FAMEZ_LOCK(&(config->legible_slot_lock));
		if (config->legible_slot)
			pr_warn(FZ "stomping legible slot\n");
		config->legible_slot = sender_slot;
		// On wakeup, it's gonna grab the lock first so...
		if (!bad_lock)
			FAMEZ_UNLOCK(&(config->legible_slot_lock));
		wake_up(&(config->legible_slot_wqh));
	}
	return IRQ_HANDLED;
}

//-------------------------------------------------------------------------
// As there are only nSlots-2 actual clients (because mailslot 0 is globals
// and server @ nslots-1) I SHOULDN'T actually activate those two IRQs.

int famez_MSIX_setup(struct pci_dev *pdev)
{
	famez_configuration_t *config = pci_get_drvdata(pdev);
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
	famez_configuration_t *config = pci_get_drvdata(pdev);
	int i;

	if (!config->msix_entries)	// Been there, done that
		return;

	for (i = 0; i < config->globals->nSlots; i++)
		free_irq(config->msix_entries[i].vector, config);
	pci_free_irq_vectors(pdev);
	kfree(config->msix_entries);
	config->msix_entries = NULL;
}
