// Configure and handle MSI-X interrupts from IVSHMEM device.

#include <linux/interrupt.h>
#include <linux/pci.h>

#include "famez.h"

//-------------------------------------------------------------------------

static irqreturn_t all_msix(int vector, void *data) {
	struct famez_configuration *config = data;
	int i, peer_id;

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
	peer_id = config->msix_entries[i].entry;
	pr_info(FZ "IRQ vector %d -> FAMEZ peer ID %d\n", vector, peer_id);

	// TODO: retrieve message from mailbox slot "peer_id"

	return IRQ_HANDLED;
}

int famez_setupMSIX(struct famez_configuration *config, struct pci_dev *dev)
{
	int i, ret, nvectors = 0;

	if ((nvectors = pci_msix_vec_count(dev)) < 0) {
		pr_err(FZ "Error retrieving MSI-X vector count\n");
		return nvectors;
	}
	pr_info(FZSP "1: %d MSI-X vectors (%sabled)\n",
		nvectors, dev->msix_enabled ? "en" : "dis");
	if (!nvectors) {
		pr_err(FZ "Zero MSI-X vectors\n");	// QEMU invocation
		return -EINVAL;
	}
	if (config->globals->nSlots > nvectors) {
		pr_err(FZ "not enough MSI-X vectors\n");
		return -EDOM;
	}
	if (!(config->msix_entries = kzalloc(
		nvectors * sizeof(struct msix_entry), GFP_KERNEL))) {
		pr_err(FZ "Can't create MSI-X entries table\n");
		return -ENOMEM;
	}
	// .vector was zeroed by kzalloc
	for (i = 0; i < config->globals->nSlots; i++)
		config->msix_entries[i].entry  = i;
	if ((ret = pci_enable_msix_exact(
		dev, config->msix_entries, config->globals->nSlots))) {
		pr_err(FZ "Can't enable MSI-X vectors\n");
		return ret;
	}
	pr_info(FZSP "2: %d MSI-X vectors (now %sabled)\n",
		nvectors, dev->msix_enabled ? "en" : "dis");
	// Attach each IRQ to the same handler.  FIXME pass in something
	// with a backpointer to dev.
	for (i = 0; i < config->globals->nSlots; i++) {
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

