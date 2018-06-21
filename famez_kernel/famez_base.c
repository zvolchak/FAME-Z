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

//-------------------------------------------------------------------------

STATIC int famez_getconfig(struct famez_configuration *config)
{
	struct pci_dev *dev_ivshmem;

	memset(config, 0, sizeof(*config));

	pr_info(
	  "-------------------------------------------------------------------"
	);
	pr_info("famez: " FAMEZ_VERSION "; module loaded with\n");
	pr_info("       famez_verbose = %d\n", famez_verbose);

	// Find the first one with two BARs.  There should be only one.
	while ((dev_ivshmem = pci_get_device(
			IVSHMEM_VENDOR, IVSHMEM_DEVICE, NULL))) {
		struct resource *bar1, *bar2 = NULL;

		if (!(dev_ivshmem->resource[1])) {
			pr_info("Skipping an IVSHMEM\n");
			continue;
		}

		bar1 = &(dev_ivshmem->resource[1]);
		bar2 = &(dev_ivshmem->resource[2]);
		pr_info("     ivshmem           = 0x%llx - 0x%llx\n",
			bar1->start, bar1->end);
		pr_info("     ivshmem           = 0x%llx - 0x%llx\n",
			bar2->start, bar2->end);
		
		pci_dev_put(dev_ivshmem);

		// config->basePHY = bar2->start;
		// config->basePFN = config->basePHY >> PAGE_SHIFT;
		// config->nbytes = (bar2->end - bar2->start + 1) & SECTION_MASK;
		return 0;
	}
	return -ENODEV;
}

//-------------------------------------------------------------------------

static struct famez_configuration famez_single;	// as opposed to list_head

int famez_init(void)
{
	int ret = 0;

	if ((ret = famez_getconfig(&famez_single)))
		return ret;
	if (!famez_single.basePHY) {
		pr_info("r2h: no memory discovered or specified\n");
		goto done;
	}
	famez_check_reservations(&famez_single); // After I have a config

	if ((ret = famez_memory_map(&famez_single)))
		return ret;

done:	
	pr_info("r2h: initialization complete, refcount is %d\n",
		module_refcount(THIS_MODULE));
	return 0;
}

NOINLINE void famez_exit(void)
{
	famez_memory_unmap(&famez_single);
	PR_EXIT(FAMEZ_NAME " has been unloaded\n");
}

module_init(famez_init);
module_exit(famez_exit);
