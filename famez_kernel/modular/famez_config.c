/*
 * Copyright (C) 2018 Hewlett Packard Enterprise Development LP.
 * All rights reserved.
 *
 * This source code file is part of the FAME-Z project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <linux/utsname.h>

#include "famez.h"

//-------------------------------------------------------------------------
// Slot 0 is the globals data, so disallow its use.  The last slot
// (nSlots - 1) is the for the server.

struct famez_mailslot __iomem *calculate_mailslot(
	struct famez_config *config,
	unsigned slotnum)
{
	struct famez_mailslot __iomem *slot;

	if (slotnum < 1 || slotnum >= config->globals->nSlots) {
		pr_err(FZ ": mailslot %u is out of range\n", slotnum);
		return NULL;
	}
	slot = (void *)(
		(uint64_t)config->globals + slotnum * config->globals->slotsize);
	return slot;
}

//-------------------------------------------------------------------------

static void unmapBARs(struct pci_dev *pdev)
{
	struct famez_config *config = pci_get_drvdata(pdev);

	if (config->regs) pci_iounmap(pdev, config->regs);	// else whine
	config->regs = NULL;
	if (config->globals) pci_iounmap(pdev, config->globals);
	config->globals = NULL;
	pci_release_regions(pdev);
}

//-------------------------------------------------------------------------
// Map the regions and overlay data structures.  Since it's QEMU, ioremap
// (uncached) for BAR0/1 and ioremap_cached(BAR2) would be fine.  However,
// the proscribed calls do the start/end/length math so use them.

static int mapBARs(struct pci_dev *pdev)
{
	struct famez_config *config = pci_get_drvdata(pdev);
	int ret;

	// "cat /proc/iomem" seems to be very finicky about spaces and
	// punctuation even if there are other things in there with it.
	if ((ret = pci_request_regions(pdev, FAMEZ_NAME)) < 0) {
		pr_err(FZSP "pci_request_regions failed: %d\n", ret);
		return ret;
	}

	PR_V1(FZSP "Mapping BAR0 regs (%llu bytes)\n",
		pci_resource_len(pdev, 0));
	if (!(config->regs = pci_iomap(pdev, 0, 0)))
		goto err_unmap;

	PR_V1(FZSP "Mapping BAR2 globals/mailslots (%llu bytes)\n",
		pci_resource_len(pdev, 2));
	if (!(config->globals = pci_iomap(pdev, 2, 0)))
		goto err_unmap;
	
	return 0;

err_unmap:
	unmapBARs(pdev);
	return -ENOMEM;
}

//-------------------------------------------------------------------------

void famez_destroy_config(struct famez_config *config)
{
	struct pci_dev *pdev;

	if (!config) return;	// probably not worth whining
	if (!(pdev = config->pdev)) {
		pr_err(FZ "destroy_config() has NULL pdev\n");
		return;
	}

	unmapBARs(pdev);	// May have be done, doesn't hurt

	dev_set_drvdata(&pdev->dev, NULL);
	pci_set_drvdata(pdev, NULL);
	config->pdev = NULL;

	if (config->IRQ_private) kfree(config->IRQ_private);
	config->IRQ_private = NULL;
	// Probably other memory leakage if this ever executes.
	if (config->outgoing)
		kfree(config->outgoing);
	config->outgoing = NULL;

	kfree(config);
}

//-------------------------------------------------------------------------
// Set up more globals and mailbox references to realize dynamic padding.

struct famez_config *famez_create_config(struct pci_dev *pdev)
{
	struct famez_config *config = NULL;
	int ret;

	if (!(config = kzalloc(sizeof(*config), GFP_KERNEL))) {
		pr_err(FZSP "Cannot kzalloc(config)\n");
		return ERR_PTR(-ENOMEM);
	}
	// Lots of backpointers.
	pci_set_drvdata(pdev, config);		// Just pass around pdev.
	dev_set_drvdata(&pdev->dev, config);	// Never hurts to go deep.
	config->pdev = pdev;			// Reverse pointers never hurt.

	// Simple fields.
	init_waitqueue_head(&(config->incoming_slot_wqh));
	spin_lock_init(&(config->incoming_slot_lock));

	// Real work.
	if ((ret = mapBARs(pdev)))
		return ERR_PTR(ret);

	// Now that there's access to globals and registers...Docs for 
	// pci_iomap() say to use io[read|write]32.  Since this is QEMU,
	// direct memory references should work.  The offset passed in
	// globals is handcrafted in Python, make sure it's all kosher.
	// If these fail, go back and add tests to Python, not here.
	ret = -EINVAL;
	if (offsetof(struct famez_mailslot, msg) != config->globals->msg_offset) {
		pr_err(FZ "MSG_OFFSET global != C offset in here\n");
		goto err_kfree;
	}
	if (config->globals->slotsize <= config->globals->msg_offset) {
		pr_err(FZ "MSG_OFFSET global is > SLOTSIZE global\n");
		goto err_kfree;
	}
	config->max_msglen = config->globals->slotsize -
			     config->globals->msg_offset;
	config->my_id = config->regs->IVPosition;
	config->server_id = config->globals->nSlots - 1;  // that's the rule

	// All the needed parameters are set to finish this off.

	// My slot and message pointers.
	if (!(config->my_slot = calculate_mailslot(config, config->my_id)))
		goto err_kfree;
	memset(config->my_slot, 0, config->globals->slotsize);
	snprintf(config->my_slot->nodename,
		 sizeof(config->my_slot->nodename) - 1,
		 "%s.%02x", utsname()->nodename, config->pdev->devfn >> 3);

	PR_V1(FZSP "mailslot size=%llu, message offset=%llu, server=%d\n",
		config->globals->slotsize,
		config->globals->msg_offset,
		config->server_id);

	return config;

err_kfree:
	famez_destroy_config(config);
	return ERR_PTR(ret);
}
