// Initial discovery and setup of IVSHMEM/IVSHMSG devices
// HP(E) lineage: res2hot from MMS PoC "mimosa" mms_base.c, flavored by zhpe.

#include <linux/export.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/utsname.h>

#include "famez.h"

MODULE_LICENSE("GPL");
MODULE_VERSION(FAMEZ_VERSION);
MODULE_AUTHOR("Rocky Craig <rocky.craig@hpe.com>");
MODULE_DESCRIPTION("Base subsystem for FAME-Z project.");

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
static DEFINE_SEMAPHORE(famez_active_sema);

//-------------------------------------------------------------------------
// Map the regions and overlay data structures.  Since it's QEMU, ioremap
// (uncached) for BAR0/1 and ioremap_cached(BAR2) would be fine.  However,
// the proscribed calls do the start/end/length math so use them.

static void unmapBARs(struct pci_dev *pdev)
{
	famez_configuration_t *config = pci_get_drvdata(pdev);

	if (config->regs) pci_iounmap(pdev, config->regs);	// else whine
	config->regs = NULL;
	if (config->globals) pci_iounmap(pdev, config->globals);
	config->globals = NULL;
	pci_release_regions(pdev);
}

static int mapBARs(struct pci_dev *pdev)
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


static void destroy_config(famez_configuration_t *config)
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
	// Probably memory leakage if this ever executes.
	if (config->writer_support) kfree(config->writer_support);
	config->writer_support = NULL;

	kfree(config);
}

static famez_configuration_t *create_config(struct pci_dev *pdev)
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
	init_waitqueue_head(&(config->legible_slot_wqh));
	FAMEZ_LOCK_INIT(&(config->legible_slot_lock));

	// Real work.
	if ((ret = mapBARs(pdev)))
		return ERR_PTR(ret);

	// Now that there's access to globals and registers...Docs for 
	// pci_iomap() say to use io[read|write]32.  Since this is QEMU,
	// direct memory references should work.  The offset passed in
	// globals is handcrafted in Python, make sure it's all kosher.
	// If these fail, go back and add tests to Python, not here.
	ret = -EINVAL;
	if (offsetof(famez_mailslot_t, msg) != config->globals->msg_offset) {
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
	ret = -ENOMEM;
	if (!(config->msix_entries = kzalloc(
		config->globals->nSlots * sizeof(struct msix_entry), GFP_KERNEL))) {
		pr_err(FZ "Can't create MSI-X entries table\n");
		goto err_kfree;
	}

	// My slot and message pointers.
	config->my_slot = (void *)(
		(uint64_t)config->globals + config->my_id * config->globals->slotsize);
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

	PR_V1("probe(%s)\n", CARDLOC(pdev));

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
		PR_V1("IVSHMEM @ %s is missing IVSHMSG features\n", CARDLOC(pdev));
		ret = -ENODEV;
		goto err_pci_disable_device;
	}
	pr_info(FZ "IVSHMEM @ %s has IVSHMSG features\n", CARDLOC(pdev));

	if (IS_ERR_VALUE((config = create_config(pdev)))) {
		ret = PTR_ERR(config);
		config = NULL;
		goto err_pci_disable_device;
	}

	if ((ret = famez_MSIX_setup(pdev)))
		goto err_pci_disable_device;

	// It's a keeper...unless it's already there.  Unlikely, but it's
	// not paranoia when in the kernel.
	ret = down_interruptible(&famez_active_sema);	// FIXME: deal with ret
	ret = 0;
	list_for_each_entry(cur, &famez_active_list, lister) {
		if (STREQ(CARDLOC(pdev), pci_resource_name(cur->pdev, 1))) {
			ret = -EALREADY;
			break;
		}
	}
	if (!ret) {
		list_add_tail(&config->lister, &famez_active_list);
		PR_V1("config added to active list\n")
	}
	up(&famez_active_sema);
	if (ret) {
		pr_err(FZSP "This device is already in active list\n");
		goto err_MSIX_teardown;
	}

	// Tell the server I'm here.
	snprintf(imalive, sizeof(imalive) - 1,
		"Client %d is ready", config->my_id);
	ret = famez_sendmail(config->server_id,
		imalive, strlen(imalive), config);
	if (ret > 0)
		ret = ret == strlen(imalive) ? 0 : -EIO;

	return ret;

err_MSIX_teardown:
	PR_V1("tearing down MSI-X %s\n", CARDLOC(pdev));
	famez_MSIX_teardown(pdev);

err_pci_disable_device:
	PR_V1("disabling device %s\n", CARDLOC(pdev));
	pci_disable_device(pdev);

// err_destroy_config:
	destroy_config(config);
	return ret;
}

void famez_remove(struct pci_dev *pdev)
{
	famez_configuration_t *cur, *next, *config = pci_get_drvdata(pdev);
	int ret;

	pr_info(FZ "famez_remove(%s): ", CARDLOC(pdev));
	if (!config) {
		pr_cont("still not my circus\n");
		return;
	}
	pr_cont("disabling/removing/freeing resources\n");

	famez_MSIX_teardown(pdev);

	pci_disable_device(pdev);

	if (atomic_read(&config->nr_users))
		pr_err(FZSP "# users is non-zero, very interesting\n");
	
	ret = down_interruptible(&famez_active_sema);	// FIXME: deal with ret
	list_for_each_entry_safe(cur, next, &famez_active_list, lister) {
		if (STREQ(CARDLOC(cur->pdev), CARDLOC(pdev)))
			list_del(&(cur->lister));
	}
	up(&famez_active_sema);

	destroy_config(config);
}

//-------------------------------------------------------------------------
// In the monolithic driver this was famez_bridge_setup()

int famez_misc_register(char *basename, const struct file_operations *fops)
{
	famez_configuration_t *config;
	struct pci_dev *pdev;
	miscdev2config_t *lookup;
	char *ownername, *devname;
	int ret;

	ownername = fops->owner->name;

	if ((ret = down_interruptible(&famez_active_sema)))
		return ret;

	list_for_each_entry(config, &famez_active_list, lister) {

		pdev = config->pdev;
		pr_info(FZ "binding %s to %s: ",
			ownername, pci_resource_name(pdev, 1));

		ret = -ENOMEM;
		if (!(lookup = kzalloc(sizeof(miscdev2config_t),
				       GFP_KERNEL)))
			goto up_and_out;
		if (!(devname = kzalloc(strlen(ownername) + 6,	// "_%02X
				     GFP_KERNEL))) {
			kfree(lookup);
			goto up_and_out;
		}

		// Device file name is meant to be reminiscent of lspci output
		sprintf(devname, "%s_%02x", ownername, pdev->devfn >> 3);
		lookup->miscdev.name = devname;
		lookup->miscdev.fops = fops;
		lookup->miscdev.minor = MISC_DYNAMIC_MINOR;
		lookup->miscdev.mode = 0666;
	
		lookup->config = config;	// Don't point that thing at me
		config->teardown_lookup = lookup;
		if ((ret = misc_register(&lookup->miscdev))) {
			kfree(devname);
			kfree(lookup);
			goto up_and_out;
		}
		pr_cont("success\n");
	}

up_and_out:
	if (ret)
		pr_cont("FAILURE\n");
	up(&famez_active_sema);
	return ret;
}
EXPORT_SYMBOL(famez_misc_register);

//-------------------------------------------------------------------------
// In the monolithic driver this was famez_bridge_teardown()

void famez_misc_deregister(const struct file_operations *fops)
{
	famez_configuration_t *config;
	struct pci_dev *pdev;
	// miscdev2config_t *lookup;
	char *ownername;
	int ret;

	ownername = fops->owner->name;

	if ((ret = down_interruptible(&famez_active_sema)))
		return;

	list_for_each_entry(config, &famez_active_list, lister) {

		pdev = config->pdev;
		pr_err(FZ "UNbinding %s from %s: FAILURE\n",
			ownername, pci_resource_name(pdev, 1));
	}
	up(&famez_active_sema);

}
EXPORT_SYMBOL(famez_misc_deregister);

//-------------------------------------------------------------------------

static struct pci_driver famez_pci_driver = {
	.name      = FAMEZ_NAME,
	.id_table  = famez_PCI_ID_table,
	.probe     = famez_probe,
	.remove    = famez_remove
};

int __init famez_init(void)
{
	int ret;

	PR_V1("-----------------------------------------------------------");
	PR_V1(FZ FAMEZ_VERSION "; parms:\n");
	PR_V1(FZSP "famez_verbose = %d\n", famez_verbose);

	if ((ret = pci_register_driver(&famez_pci_driver)))
		pr_err(FZ "pci_register_driver() = %d\n", ret);

	return ret;
}

module_init(famez_init);

//-------------------------------------------------------------------------
// Called from rmmod.

void famez_exit(void)
{
	pci_unregister_driver(&famez_pci_driver);
}

module_exit(famez_exit);
