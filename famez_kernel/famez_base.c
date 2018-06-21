// Stolen from res2hot famez_base stolen from MMS PoC "mimosa" mms_base.c

#include <linux/module.h>
#include <linux/pci.h>

#include "famez.h"

//-------------------------------------------------------------------------

MODULE_LICENSE("GPL");		// Not really, but I need other GPL routines
MODULE_VERSION(FAMEZ_VERSION);
MODULE_AUTHOR("Rocky Craig <rocky.craig@hpe.com>");
MODULE_DESCRIPTION("Simple driver to wiggle interrupts for FAME-Z project.");

// global
int famez_verbose = 0;
module_param(famez_verbose, int, S_IRUGO);
MODULE_PARM_DESC(famez_verbose, "increase amount of printk info (0)");

static struct famez_configuration famez_single;	// as opposed to list_head

//-------------------------------------------------------------------------

STATIC int famez_getconfig(struct famez_configuration *config)
{
	struct pci_dev *dev_famez;
	struct resource *bar1, *bar2 = NULL;

	memset(config, 0, sizeof(*config));

	pr_info(
	  "-------------------------------------------------------------------"
	);
	pr_info("famez: " FAMEZ_VERSION "; module loaded with\n");
	pr_info("       famez_verbose = %d\n", famez_verbose);

	// Find the first one with two BARs.  There should be only one.
	while ((dev_famez = pci_get_device(
			IVSHMEM_VENDOR, IVSHMEM_DEVICE, NULL))) {

		bar1 = &(dev_famez->resource[1]);
		bar2 = &(dev_famez->resource[2]);
		if (!bar1->start) {
			pr_info("Skipping an IVSHMEM %s\n", bar1->name);
			continue;
		}
		break;
	}
	if (!dev_famez)
		return -ENODEV;

	pci_dev_put(dev_famez);

	pr_info("     FAME-Z control  = 0x%llx - 0x%llx\n",
		bar1->start, bar1->end);
	pr_info("     FAME-Z mailbox  = 0x%llx - 0x%llx\n",
		bar2->start, bar2->end);

	config->pci_dev = dev_famez;
	config->res_registers = bar1;
	config->res_mailbox = bar2;

	// TODO: map the regions, set the interrupt handler.

	return 0;
}

//-------------------------------------------------------------------------

STATIC void famez_unconfig(struct famez_configuration *config)
{
}

//-------------------------------------------------------------------------

int famez_init(void)
{
	int ret = 0;

	if ((ret = famez_getconfig(&famez_single)))
		return ret;

	pr_info("famez: initialization complete, refcount is %d\n",
		module_refcount(THIS_MODULE));
	return 0;
}

NOINLINE void famez_exit(void)
{
	famez_unconfig(&famez_single);
	PR_EXIT(FAMEZ_NAME " has been unloaded\n");
}

module_init(famez_init);
module_exit(famez_exit);
