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
	struct famez_configuration *config = pci_get_drvdata(pdev);

	if (config->globals) pci_iounmap(pdev, config->globals);
	config->globals = NULL;
	if (config->UNUSED) pci_iounmap(pdev, config->UNUSED);
	config->UNUSED = NULL;
	if (config->regs) pci_iounmap(pdev, config->regs);
	config->regs = NULL;
}

STATIC int mapBARs(struct pci_dev *pdev)
{
	struct famez_configuration *config = pci_get_drvdata(pdev);

	pr_info(FZSP "Mapping BAR0 regs (%llu bytes)\n",
		pci_resource_len(pdev, 0));
	if (!(config->regs = pci_iomap(pdev, 0, 0)))
		goto err_unmap;

#if 0
	pr_info(FZSP "Mapping BAR1 MSI-X (%llu bytes)\n",
		pci_resource_len(pdev, 1));
	if (!(config->UNUSED = pci_iomap(pdev, 1, 0)))
		goto err_unmap;
#endif

	pr_info(FZSP "Mapping BAR2 globals/mailslots (%llu bytes)\n",
		pci_resource_len(pdev, 2));
	if (!(config->globals = pci_iomap(pdev, 2, 0)))
		goto err_unmap;

	// Set up more globals and mailbox references to work around padding.
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
		 "%s.%02x", utsname()->nodename, config->pdev->devfn >> 3);

	pr_info(FZSP "mailslot size=%llu, message offset=%llu, server=%d\n",
		config->globals->slotsize,
		config->globals->msg_offset,
		config->server_id);

	return 0;

err_unmap:
	pr_err(FZSP "mapping failed\n");
	unmapBARs(pdev);
	return -ENOMEM;
}

//-------------------------------------------------------------------------
// Called at insmod time and also at hotplug events (shouldn't be any).
// Only take IVSHMEM (filtered by PCI core) with a BAR 1 and 64 vectors.

int famez_probe(struct pci_dev *pdev, const struct pci_device_id *pdev_id)
{
	struct famez_configuration *config = NULL, *cur = NULL;
	int ret;
	char imalive[80], regionname[32];

	pr_info(FZ "probe %s\n", CARDLOC(pdev));


	// Has this device been configured already?

	ret = -EALREADY;
	if (pci_get_drvdata(pdev)) {	// Is this possible?
		pr_err(FZSP "This device is already configured (1)\n");
		goto err_out;
	}

	// enable it and discriminate

	ret = -ENODEV;
	if ((ret = pci_enable_device(pdev)) < 0) {
		pr_err(FZSP "pci_enable_device failed: %d\n", ret);
		goto err_out;
	}
	if (pdev->revision != 1 ||
	    !pdev->msix_cap ||
	    !pci_resource_start(pdev, 1)) {
		pr_warn(FZSP "IVSHMEM @ %s is not my circus\n", CARDLOC(pdev));
		goto err_pci_disable_device;
	}
	pr_info(FZ "IVSHMSG @ %s is my monkey\n", CARDLOC(pdev));

	snprintf(regionname, sizeof(regionname) - 1,
		 "%s.%02x", FAMEZ_NAME, pdev->devfn >> 3);
	PR_V2(FZSP "Checkpoint 1\n");
	if ((ret = pci_request_regions(pdev, regionname)) < 0) {
		pr_err(FZSP "pci_request_regions failed: %d\n", ret);
		goto err_pci_disable_device;
	}

	// Make space and add it.  Either could sleep, as can many things after this
	// (esp kzalloc)
	
	PR_V2(FZSP "Checkpoint 2\n");
	if (!(config = kzalloc(sizeof(*config), GFP_KERNEL))) {
		pr_err(FZSP "Cannot kzalloc(config)\n");
		ret = -ENOMEM;
		goto err_out;
	}
	PR_V2(FZSP "Checkpoint 3\n");
	spin_lock_bh(&famez_active_lock);
	list_for_each_entry(cur, &famez_active_list, lister) {
		if (STREQ(CARDLOC(pdev), pci_resource_name(cur->pdev, 1))) {
			pr_err(FZSP "This device is already configured (2)\n");
			spin_unlock_bh(&famez_active_lock);
			goto err_out;
		}
	}
	list_add_tail(&config->lister, &famez_active_list);
	spin_unlock_bh(&famez_active_lock);
	PR_V1(FZSP "config added to active list\n")

	// Remainder of config

	pci_set_drvdata(pdev, config);		// Now everyone has it.
	dev_set_drvdata(&pdev->dev, config);	// Never hurts to go deep.
	config->pdev = pdev;			// Reverse pointers never hurt.

	PR_V2(FZSP "Checkpoint 4\n");
	if ((ret = mapBARs(pdev)))
		goto err_pci_release_regions;
	
	PR_V2(FZSP "Checkpoint 5\n");
	if ((ret = famez_MSIX_setup(pdev)))
		goto err_unmapBARs;

	PR_V2(FZSP "Checkpoint 6\n");
	if ((ret = famez_chardev_setup(pdev)))
		goto err_MSIX_teardown;

	// Tell the server I'm here.  Cover the NUL terminator in the length.
	snprintf(imalive, sizeof(imalive) - 1, "Client %d is ready", config->my_id);
	famez_sendmail(config->server_id, imalive, strlen(imalive) + 1, config);
	pr_info(FZSP "%s\n", imalive);

	return 0;

err_MSIX_teardown:
	PR_V2(FZSP "tearing down MSI-X %s\n", CARDLOC(pdev));
	famez_MSIX_teardown(pdev);

err_unmapBARs:
	PR_V2(FZSP "unmapping BAR(S) %s\n", CARDLOC(pdev));
	unmapBARs(pdev);

err_pci_release_regions:
	PR_V2(FZSP "releasing regions %s\n", CARDLOC(pdev));
	pci_release_regions(pdev);

err_pci_disable_device:
	PR_V2(FZSP "disabling device %s\n", CARDLOC(pdev));
	pci_disable_device(pdev);

err_out:
	if (config) {	// It's almost certainly in the list
		spin_lock_bh(&famez_active_lock);
#if 0
		list_for_each_entry(cur, &famez_active_list, lister) {
			if (STREQ(CARDLOC(pdev), pci_resource_name(cur->pdev, 1))) {
				pr_err(FZSP "This device is already configured (2)\n");
				spin_unlock_bh(&famez_active_lock);
				goto err_out;
			}
		}
		list_add_tail(&config->lister, &famez_active_list);
#endif
		spin_unlock_bh(&famez_active_lock);
		kfree(config);
	}
	pci_set_drvdata(pdev, NULL);
	dev_set_drvdata(&pdev->dev, NULL);
	return ret;
}

void famez_remove(struct pci_dev *pdev)
{
	struct famez_configuration *cur, *next, *config = pci_get_drvdata(pdev);

	pr_info(FZ "famez_remove(%s)", CARDLOC(pdev));
	if (!config) {
		pr_info(FZSP "still not my circus\n");
		return;
	}
	pr_info(FZSP "disabling/removing/freeing resources\n");

	famez_chardev_teardown(pdev);		// switch with MSIX teardown?

	spin_lock_bh(&famez_active_lock);		// some things sleep

	famez_MSIX_teardown(pdev);		// stop pong

	unmapBARs(pdev);

	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	
	list_for_each_entry_safe(cur, next, &famez_active_list, lister) {
		if (STREQ(CARDLOC(cur->pdev), CARDLOC(pdev)))
			list_del(&(cur->lister));
	}

	if (atomic_read(&config->nr_users))
		pr_err(FZSP "# users is non-zero, very interesting\n");

	spin_unlock_bh(&famez_active_lock);
	kfree(config);
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
// Return positive on success, negative on error, never 0.

int famez_sendmail(uint32_t peer_id, char *msg, ssize_t msglen,
		   struct famez_configuration *config)
{
	union ringer ringer;
	// uint64_t done = get_jiffies_64() + HZ;

	if (msglen >= config->max_msglen)
		return -E2BIG;

	// Pseudo-HW ready: wait until previous responder has cleared msglen.
	// while (config->my_slot->msglen && get_jiffies_64() < done)
		// msleep(50);
	if (config->my_slot->msglen)
		pr_warn(FZ "sendmail() is stomping on previous message\n");

	// Keep nodename and msg pointer; update msglen and msg contents.
	memset(config->my_slot->msg, 0, config->max_msglen);
	config->my_slot->msglen = msglen;
	memcpy(config->my_slot->msg, msg, msglen);
	ringer.vector = config->my_id;		// from this
	ringer.peer = peer_id;			// to this
	config->regs->Doorbell = ringer.push;
	return 0;
}
