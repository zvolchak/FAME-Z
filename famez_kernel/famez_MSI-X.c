// Configure and handle MSI-X interrupts from IVSHMEM device.

#include <linux/interrupt.h>
#include <linux/pci.h>

#include "famez.h"

//-------------------------------------------------------------------------

static irqreturn_t all_msix(int vector, void *data) {
	struct famez_configuration *config = data;
	int i;
	uint64_t peer_id;
	struct famez_mailbox_slot *peer_slot;

	// Match the IRQ vector to entry/vector pair, which yields the sender.
	for (i = 0; i < config->globals->nSlots; i++) {
		if (vector == config->msix_entries[i].vector)
			break;
	}
	if (i >= config->globals->nSlots) {
		pr_err(FZ "IRQ handler could not match vector %d\n", vector);
		return IRQ_NONE;
	}
	// Turns out i and msix_entries[i].entry are identical, but anyhow...
	// Just read the message, don't copy it in this simple handler.

	peer_id = config->msix_entries[i].entry;
	peer_slot = (void *)(
		(uint64_t)config->globals + peer_id * config->globals->slotsize);
	peer_slot->msg = (void *)(
		(uint64_t)peer_slot + config->globals->msg_offset);

	pr_info(FZ "IRQ %d == peer ID %llu: \"%s\"\n",
		vector, peer_id, peer_slot->msg);

	// I know, bad form in a handler.  Is it tasklets I need?
	if STREQ(peer_slot->msg, "ping")
		famez_sendmsg(peer_id, "Pong", 5, config);

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
	pr_info(FZSP "%2d MSI-X vectors (%sabled)\n",
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
	if ((ret = pci_enable_msix_exact(dev, config->msix_entries, nvectors))) {
		pr_err(FZ "Can't enable MSI-X vectors\n");
		return ret;
	}
	pr_info(FZSP "%2d MSI-X vectors (%sabled)\n",
		nvectors, dev->msix_enabled ? "en" : "dis");

	// Attach each IRQ to the same handler.
	for (i = 0; i < nvectors; i++) {
		pr_info(FZSP "%d = %d\n", i, config->msix_entries[i].vector);
		if ((ret = request_irq(
			config->msix_entries[i].vector,
			all_msix,
			0,
			"FAME-Z",
			config))) {
				pr_err(FZ "request_irq(%d) failed: %d\n", i, ret);
				return ret;
		}
	}
	return 0;
}

//-------------------------------------------------------------------------

void famez_MSIX_teardown(struct famez_configuration *config)
{
	int i;

	// There is no disable control, hope one doesn't fire...
	for (i = 0; i < config->globals->nSlots; i++)
		free_irq(config->msix_entries[i].vector, config);
	pci_disable_msix(config->pci_dev);
	if (config->msix_entries) {
		kfree(config->msix_entries);
		config->msix_entries = NULL;
	}
}
