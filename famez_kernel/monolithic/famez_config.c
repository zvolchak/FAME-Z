// Initial discovery and setup of IVSHMEM/IVSHMSG devices
// HP(E) lineage: res2hot from MMS PoC "mimosa" mms_base.c, flavored by zhpe.

#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/utsname.h>

#include "famez.h"

MODULE_LICENSE("GPL");
MODULE_VERSION(FAMEZ_VERSION);
MODULE_AUTHOR("Rocky Craig <rocky.craig@hpe.com>");
MODULE_DESCRIPTION("Simple driver to wiggle interrupts for FAME-Z project.");

// Find the one macro that does the right thing.  Notice there is no "device"
// for QEMU in the PCI ID database, just the sub* things.

static struct pci_device_id famez_PCI_ID_table[] = {
    { PCI_DEVICE_SUB(	// vend, dev, subvend, subdev
    	PCI_VENDOR_ID_REDHAT_QUMRANET,
    	PCI_ANY_ID,
    	PCI_SUBVENDOR_ID_REDHAT_QUMRANET,
	PCI_SUBDEVICE_ID_QEMU)
    },
    { 0 },
};

MODULE_DEVICE_TABLE(pci, famez_PCI_ID_table);	// depmod, hotplug, modinfo

// module parameters are global

int famez_verbose = 0;
module_param(famez_verbose, int, S_IRUGO);
MODULE_PARM_DESC(famez_verbose, "increase amount of printk info (0)");

// Multiple bridge "devices" accepted by famez_probe().  It might be that PCI
// core does everything I need but I can't shake the feeling I want this for
// something else...right now it just tracks insmod/rmmod.

static LIST_HEAD(famez_active_list);
static DEFINE_SPINLOCK(famez_active_lock);

//-------------------------------------------------------------------------
// Map the regions and overlay data structures.  Since it's QEMU, ioremap
// (uncached) for BAR0/1 and ioremap_cached(BAR2) would be fine.  However,
// the proscribed calls do the start/end/length math so use them.

STATIC void unmapBARs(struct pci_dev *pdev)
{
	famez_configuration_t *config = pci_get_drvdata(pdev);

	if (config->regs) pci_iounmap(pdev, config->regs);	// else whine
	config->regs = NULL;
	if (config->globals) pci_iounmap(pdev, config->globals);
	config->globals = NULL;
	pci_release_regions(pdev);
}

STATIC int mapBARs(struct pci_dev *pdev)
{
	famez_configuration_t *config = pci_get_drvdata(pdev);
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
// Set up more globals and mailbox references to realize dynamic padding.


STATIC void destroy_config(famez_configuration_t *config)
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

	if (config->msix_entries) kfree(config->msix_entries);
	config->msix_entries = NULL;
	if (config->scratch_msg) kfree(config->scratch_msg);
	config->scratch_msg = NULL;

	kfree(config);
}

STATIC famez_configuration_t *create_config(struct pci_dev *pdev)
{
	famez_configuration_t *config = NULL;
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
	spin_lock_init(&(config->legible_slot_lock));
	init_waitqueue_head(&(config->legible_slot_wqh));

	// Real work.
	if ((ret = mapBARs(pdev)))
		return ERR_PTR(ret);

	// Now that there's access to globals and registers...
	// Docs for pci_iomap() say to use io[read|write]32.
	// Since this is QEMU, direct memory references should work.
	config->my_id = config->regs->IVPosition;
	config->server_id = config->globals->nSlots - 1;  // that's the rule
	config->max_msglen = config->globals->slotsize -
			     config->globals->msg_offset;

	// All the needed parameters are set to finish this off.
	ret = -ENOMEM;
	if (!(config->scratch_msg = kmalloc(config->max_msglen, GFP_KERNEL))) {
		pr_err(FZ "Can't create scratch buffer\n");
		goto err_kfree;
	}
	if (!(config->msix_entries = kzalloc(
		config->globals->nSlots * sizeof(struct msix_entry), GFP_KERNEL))) {
		pr_err(FZ "Can't create MSI-X entries table\n");
		goto err_kfree;
	}

	// My slot and message pointers.
	config->my_slot = (void *)(
		(uint64_t)config->globals + config->my_id * config->globals->slotsize);
	memset(config->my_slot, 0, config->globals->slotsize);
	config->my_slot->msg = (void *)(
		(uint64_t)config->my_slot + config->globals->msg_offset);
	snprintf(config->my_slot->nodename,
		 sizeof(config->my_slot->nodename) - 1,
		 "%s.%02x", utsname()->nodename, config->pdev->devfn >> 3);

	PR_V1(FZSP "mailslot size=%llu, message offset=%llu, server=%d\n",
		config->globals->slotsize,
		config->globals->msg_offset,
		config->server_id);

	return config;

err_kfree:
	destroy_config(config);
	return ERR_PTR(ret);
}

//-------------------------------------------------------------------------
// Called at insmod time and also at hotplug events (shouldn't be any).
// Only take IVSHMEM (filtered by PCI core) with a BAR 1 and 64 vectors.

int famez_probe(struct pci_dev *pdev, const struct pci_device_id *pdev_id)
{
	famez_configuration_t *config = NULL, *cur = NULL;
	int ret = -ENOTTY;
	char imalive[80];

	pr_info(FZ "probe(%s)\n", CARDLOC(pdev));

	// Has this device been configured already?

	if (pci_get_drvdata(pdev)) {	// Is this possible?
		pr_err(FZSP "This device is already configured\n");
		return -EALREADY;
	}

	// Enable it to discriminate values and create a configuration for
	// this instance.

	if ((ret = pci_enable_device(pdev)) < 0) {
		pr_err(FZSP "pci_enable_device failed: %d\n", ret);
		return ret;
	}
	if (pdev->revision != 1 ||
	    !pdev->msix_cap ||
	    !pci_resource_start(pdev, 1)) {
		pr_warn(FZSP "IVSHMEM @ %s is not my circus\n", CARDLOC(pdev));
		ret = -ENODEV;
		goto err_pci_disable_device;
	}
	pr_info(FZ "IVSHMSG @ %s is my monkey\n", CARDLOC(pdev));

	if (IS_ERR_VALUE((config = create_config(pdev)))) {
		ret = PTR_ERR(config);
		config = NULL;
		goto err_pci_disable_device;
	}

	if ((ret = famez_MSIX_setup(pdev)))
		goto err_pci_disable_device;

	// FIXME: rewrite this as a separate module that registers itself.
	if ((ret = famez_bridge_setup(pdev)))
		goto err_MSIX_teardown;

	// It's a keeper...unless it's already there.  Unlikely, but it's
	// not paranoia when in the kernel.
	spin_lock_bh(&famez_active_lock);
	ret = 0;
	list_for_each_entry(cur, &famez_active_list, lister) {
		if (STREQ(CARDLOC(pdev), pci_resource_name(cur->pdev, 1))) {
			ret = -EALREADY;
			break;
		}
	}
	if (!ret) {
		list_add_tail(&config->lister, &famez_active_list);
		PR_V1(FZSP "config added to active list\n")
	}
	spin_unlock_bh(&famez_active_lock);
	if (ret) {
		pr_err(FZSP "This device is already in active list\n");
		goto err_bridge_teardown;
	}

	// Tell the server I'm here.
	snprintf(imalive, sizeof(imalive) - 1,
		"Client %d is ready", config->my_id);
	ret = famez_sendstring(config->server_id, imalive, config);
	pr_info(FZSP "sendstring(\"%s\") to server %s\n", imalive,
		ret > 0 ? "succeeded" : "FAILED");

	return 0;

err_bridge_teardown:
	pr_warn(FZSP "tearing down bridge %s\n", CARDLOC(pdev));
	famez_bridge_teardown(pdev);

err_MSIX_teardown:
	pr_warn(FZSP "tearing down MSI-X %s\n", CARDLOC(pdev));
	famez_MSIX_teardown(pdev);

err_pci_disable_device:
	pr_warn(FZSP "disabling device %s\n", CARDLOC(pdev));
	pci_disable_device(pdev);

// err_destroy_config:
	destroy_config(config);
	return ret;
}

void famez_remove(struct pci_dev *pdev)
{
	famez_configuration_t *cur, *next, *config = pci_get_drvdata(pdev);

	pr_info(FZ "famez_remove(%s): ", CARDLOC(pdev));
	if (!config) {
		pr_cont("still not my circus\n");
		return;
	}
	pr_cont("disabling/removing/freeing resources\n");

	famez_bridge_teardown(pdev);

	famez_MSIX_teardown(pdev);

	pci_disable_device(pdev);

	if (atomic_read(&config->nr_users))
		pr_err(FZSP "# users is non-zero, very interesting\n");
	
	spin_lock_bh(&famez_active_lock);
	list_for_each_entry_safe(cur, next, &famez_active_list, lister) {
		if (STREQ(CARDLOC(cur->pdev), CARDLOC(pdev)))
			list_del(&(cur->lister));
	}
	spin_unlock_bh(&famez_active_lock);

	destroy_config(config);
}

static struct pci_driver famez_pci_driver = {
	.name      = FAMEZ_NAME,
	.id_table  = famez_PCI_ID_table,
	.probe     = famez_probe,
	.remove    = famez_remove
};

//-------------------------------------------------------------------------

int __init famez_init(void)
{
	int ret;

	pr_info("-----------------------------------------------------------");
	pr_info(FZ FAMEZ_VERSION "; parms:\n");
	pr_info(FZSP "famez_verbose = %d\n", famez_verbose);

	// Out with the old:
	// while ((dev = pci_get_device(IVSHMEM_VENDOR, IVSHMEM_DEVICE, dev)))

	if ((ret = pci_register_driver(&famez_pci_driver)) < 0) {
            pr_err(FZ "pci_register_driver() = %d\n", ret);
	    return ret;
	}

	// Everything else depends on probe finishing.
        pr_warn(FZ "pci_register_driver() successful\n");

	return 0;
}

module_init(famez_init);

//-------------------------------------------------------------------------
// Called from rmmod.

void famez_exit(void)
{
	pci_unregister_driver(&famez_pci_driver);
}

module_exit(famez_exit);

//-------------------------------------------------------------------------
// Assume a legal C string is passed in message.
// Return positive (bytecount) on success, negative on error, never 0.
// Has a spinlock-safe sleep.

int famez_sendstring(uint32_t peer_id, char *msg, famez_configuration_t *config)
{
	size_t msglen = strlen(msg);
	uint64_t hw_timeout = get_jiffies_64() + HZ/2;	// 500 ms
	union ringer ringer;

	PR_ENTER("sending \"%s\" to %d\n", msg, peer_id);

	if (peer_id < 1 || peer_id > config->server_id)
		return -EBADSLT;
	if (msglen >= config->max_msglen)
		return -E2BIG;
	if (!msglen)
		return -ENODATA; // FIXME: is there value to a "silent kick"?

	// Pseudo-HW ready: wait until my_slot has pushed a previous write
	// through, ie, the most recent responder clears my msglen.
	while (config->my_slot->msglen && get_jiffies_64() < hw_timeout)
		usleep_range(50000, 80000);
	if (config->my_slot->msglen)
		pr_warn(FZ "sendstring() is stomping on previous message\n");

	// Keep nodename and msg pointer; update msglen and msg contents.
	memset(config->my_slot->msg, 0, config->max_msglen);
	config->my_slot->msglen = msglen;
	memcpy(config->my_slot->msg, msg, msglen);
	ringer.vector = config->my_id;		// from this
	ringer.peer = peer_id;			// to this
	config->regs->Doorbell = ringer.push;
	return msglen;
}
