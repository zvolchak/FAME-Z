// Initial discovery and setup of IVSHMEM/IVSHMSG devices
// HP(E) lineage: res2hot from MMS PoC "mimosa" mms_base.c, flavored by zhpe.

#include <linux/module.h>

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
module_param(famez_verbose, uint, 0644);
MODULE_PARM_DESC(famez_verbose, "increase amount of printk info (0)");

// Multiple bridge "devices" accepted by famez_probe().  It might be that PCI
// core does everything I need but I can't shake the feeling I want this for
// something else...right now it just tracks insmod/rmmod.

LIST_HEAD(famez_active_list);
DEFINE_SEMAPHORE(famez_active_sema);

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

	if (IS_ERR_VALUE((config = famez_create_config(pdev)))) {
		ret = PTR_ERR(config);
		config = NULL;
		goto err_pci_disable_device;
	}

	if ((ret = famez_ISR_setup(pdev)))
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
	famez_ISR_teardown(pdev);

err_pci_disable_device:
	PR_V1("disabling device %s\n", CARDLOC(pdev));
	pci_disable_device(pdev);

// err_destroy_config:
	famez_destroy_config(config);
	return ret;
}

//-------------------------------------------------------------------------

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

	famez_ISR_teardown(pdev);

	pci_disable_device(pdev);

	if (atomic_read(&config->nr_users))
		pr_err(FZSP "# users is non-zero, very interesting\n");
	
	ret = down_interruptible(&famez_active_sema);	// FIXME: deal with ret
	list_for_each_entry_safe(cur, next, &famez_active_list, lister) {
		if (STREQ(CARDLOC(cur->pdev), CARDLOC(pdev)))
			list_del(&(cur->lister));
	}
	up(&famez_active_sema);

	famez_destroy_config(config);
}

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

	pr_info("-------------------------------------------------------");
	pr_info(FZ FAMEZ_VERSION "; parms:\n");
	pr_info(FZSP "famez_verbose = %d\n", famez_verbose);

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
