// Stolen from res2hot famez_base stolen from MMS PoC "mimosa" mms_base.c

#include <linux/module.h>

#include "famez.h"

//-------------------------------------------------------------------------

MODULE_LICENSE("GPL");		// need other GPL routines
MODULE_VERSION(FAMEZ_VERSION);
MODULE_AUTHOR("Rocky Craig <rocky.craig@hpe.com>");
MODULE_DESCRIPTION("Simple driver to wiggle interrupts for FAME-Z project.");

// module parameters are global

int famez_verbose = 0;
module_param(famez_verbose, int, S_IRUGO);
MODULE_PARM_DESC(famez_verbose, "increase amount of printk info (0)");

static struct famez_configuration famez_single;	// as opposed to list_head

//-------------------------------------------------------------------------

STATIC int __init famez_init(void)
{
	int ret;
	
	if ((ret = famez_config(&famez_single)))
		pr_err(FZ "initialization failed\n");
	else
		pr_info(FZ "initialization complete\n");
	return ret;
}

NOINLINE void famez_exit(void)
{
	famez_unconfig(&famez_single);
	pr_info(FZ "unload complete\n");
}

module_init(famez_init);
module_exit(famez_exit);
