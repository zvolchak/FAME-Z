// Initial discovery and setup of IVSHMEM/IVSHMSG device

#include <linux/module.h>
#include <linux/utsname.h>
#include <linux/pci.h>

#include "famez.h"

// They missed one...
#ifndef pci_resource_name
#define pci_resource_name(dev, bar) (char *)((dev)->resource[(bar)].name)
#endif

// Find the one macro that does the right thing.  Notice there is no "device"
// for QEMU in the PCI ID database, just the sub* things.

STATIC struct pci_device_id famez_PCI_ID_table[] = {
    { PCI_DEVICE_SUB(	// vend, dev, subvend, subdev
    	PCI_VENDOR_ID_REDHAT_QUMRANET,
    	PCI_ANY_ID,
    	PCI_SUBVENDOR_ID_REDHAT_QUMRANET,
	PCI_SUBDEVICE_ID_QEMU)
    },
    { 0 },
};

MODULE_DEVICE_TABLE(pci, famez_PCI_ID_table);	// depmod, hotplug, modinfo

//-------------------------------------------------------------------------
// Map the regions and overlay data structures.  Since it's QEMU, ioremap
// (uncached) for BAR0/1 and ioremap_cached(BAR2) would be fine.  However,
// do it with proscribed calls here to do the start/end/length math.

STATIC int mapBARs(struct pci_dev *pdev)
{
	struct famez_configuration *config = pci_get_drvdata(pdev);
	int ret;

	ret = -ENOSPC;
	pr_info(FZSP "Mapping BAR0 regs, size = %llu\n",
		pci_resource_len(pdev, 0));
	if (!(config->regs = pci_iomap(pdev, 0, 0))) {
		pr_err(FZ "can't map memory for registers\n");
		return ret;
	}
	pr_info(FZSP "Mapping BAR1 MSI-X\n");
	if (!(config->msix = pci_iomap(pdev, 1, 0))) {
		pr_err(FZ "can't map memory for MSI-X\n");
		return ret;
	}
	pr_info(FZSP "Mapping BAR2 globals/mailbox\n");
	if (!(config->globals = pci_iomap(pdev, 2, 0))) {
		pr_err(FZ "can't map memory for mailxbox\n");
		return ret;
	}

	// Docs for pci_iomap() say to use io[read|write]32.
	// Since this is QEMU, direct memory references should work.

	config->my_id = config->regs->IVPosition;
	config->server_id = config->globals->nSlots - 1;  // cuz I said so
	config->max_msglen = config->globals->slotsize -
			     config->globals->msg_offset;

	// My slot and invariant info.
	config->my_slot = (void *)(
		(uint64_t)config->globals + config->my_id * config->globals->slotsize);
	memset(config->my_slot, 0, config->globals->slotsize);
	config->my_slot->msg = (void *)(
		(uint64_t)config->my_slot + config->globals->msg_offset);
	snprintf(config->my_slot->nodename,
		 sizeof(config->my_slot->nodename) - 1,
		 utsname()->nodename);

	pr_info(FZSP "slot size = %llu, message offset = %llu, server = %d\n",
		config->globals->slotsize,
		config->globals->msg_offset,
		config->server_id);

	return 0;
}

//-------------------------------------------------------------------------

int famez_probe(struct pci_dev *pdev, const struct pci_device_id *pdev_id)
{
	struct famez_configuration *config = NULL;
	int ret;
	char *mygeo, buf80[80];

	mygeo = pci_resource_name(pdev, 1);
	pr_info(FZ "probe %s\n", mygeo);
	if ((ret = pci_enable_device(pdev)) < 0) {
		pr_err(FZSP "pci_enable_device failed: %d\n", ret);
		goto err_out;
	}

	config = (void *)pdev_id->driver_data;
	config->pci_dev = pdev;		

	ret = -ENODEV;
	if (pdev->revision != 1 ||
	    !pdev->msix_cap ||
	    !pci_resource_start(pdev, 1)) {
		pr_warn(FZSP "IVSHMEM @ %s is not my circus\n", mygeo);
		goto err_pci_disable_device;
	}
	pr_info(FZ "IVSHMSG @ %s is my monkey\n", mygeo);

	if ((ret = pci_request_regions(pdev, FAMEZ_NAME)) < 0) {
		pr_err(FZSP "pci_request_regions failed: %d\n", ret);
		goto err_pci_disable_device;
	}

	pci_set_drvdata(pdev, config);	// Now everyone has it
	mapBARs(pdev);
	
	if ((ret = famez_MSIX_setup(config, pdev)))
		goto err_pci_release_regions;

	// Tell the server I'm here.  Cover the NUL terminator in the length.
	sprintf(buf80, "Client %d is ready", config->my_id);
	famez_sendmsg(config->server_id, buf80, strlen(buf80) + 1, config);
	pr_info(FZSP "%s\n", buf80);

	return 0;

err_pci_release_regions:
	PR_V2(FZSP "releasing regions %s\n", mygeo);
	pci_release_regions(pdev);

err_pci_disable_device:
	PR_V2(FZSP "disabling device %s\n", mygeo);
	pci_disable_device(pdev);

err_out:
	if (config) config->pci_dev = NULL;
	return ret;
}

void famez_remove(struct pci_dev *pdev)
{
	struct famez_configuration *config = pci_get_drvdata(pdev);

	famez_MSIX_teardown(config);
	if (config->globals) pci_iounmap(pdev, config->globals);
	if (config->msix) pci_iounmap(pdev, config->msix);
	if (config->regs) pci_iounmap(pdev, config->regs);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static struct pci_driver famez_pci_driver = {
	.name      = FAMEZ_NAME,
	.id_table  = famez_PCI_ID_table,
	.probe     = famez_probe,
	.remove    = famez_remove
};

//-------------------------------------------------------------------------
// Find the first one with two BARs and MSI-X (slightly redundant).

int famez_config(struct famez_configuration *config)
{

	int ret;

	memset(config, 0, sizeof(*config));

	// Get config into probe().  FIXME how does this work for hotplug>
	famez_PCI_ID_table[0].driver_data = (kernel_ulong_t)config;

	pr_info("-----------------------------------------------------------");
	pr_info(FZ FAMEZ_VERSION "; parms:\n");
	pr_info(FZSP "famez_verbose = %d\n", famez_verbose);

	// Out with the old:
	// while ((dev = pci_get_device(IVSHMEM_VENDOR, IVSHMEM_DEVICE, dev)))

	if ((ret = pci_register_driver(&famez_pci_driver)) < 0) {
            pr_warn(FZ "pci_register_driver() = %d\n", ret);
	    return ret;
	}

	// Everything else depends on probe finishing.
        pr_warn(FZ "pci_register_driver() okay, waiting for probe()\n");

	return 0;

// undo:
	famez_unconfig(config);
	return ret;
}

//-------------------------------------------------------------------------
// Can be called from config early errors, check fields first.  PCI subsystem
// routines already do this.

void famez_unconfig(struct famez_configuration *config)
{
	pci_unregister_driver(&famez_pci_driver);
	memset(config, 0, sizeof(*config));
}

//-------------------------------------------------------------------------
// Return positive on success, negative on error, never 0.

int famez_sendmsg(uint32_t peer_id, char *msg, ssize_t msglen,
		  struct famez_configuration *config)
{
	union ringer ringer;

	if (msglen >= config->max_msglen)
		return -E2BIG;
	// Keep nodename and msg pointer; update msglen and msg contents.
	memset(config->my_slot->msg, 0, config->max_msglen);
	config->my_slot->msglen = msglen;
	memcpy(config->my_slot->msg, msg, msglen);
	ringer.vector = config->my_id;		// from this
	ringer.peer = peer_id;			// to this
	config->regs->Doorbell = ringer.push;
	return 0;
}

