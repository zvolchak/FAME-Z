// Stolen from res2hot famez_base stolen from MMS PoC "mimosa" mms_base.c

#include <linux/module.h>

#include "famez.h"

//-------------------------------------------------------------------------

MODULE_LICENSE("GPL");		// need other GPL routines
MODULE_VERSION(FAMEZ_VERSION);
MODULE_AUTHOR("Rocky Craig <rocky.craig@hpe.com>");
MODULE_DESCRIPTION("Simple driver to wiggle interrupts for FAME-Z project.");

// module parameters are global
int famez_max_clients = 63;		// must be one less than a power of 2
module_param(famez_max_clients, int, S_IRUGO);
MODULE_PARM_DESC(famez_max_clients, "max number of clients (63)");

int famez_verbose = 0;
module_param(famez_verbose, int, S_IRUGO);
MODULE_PARM_DESC(famez_verbose, "increase amount of printk info (0)");

static struct famez_configuration famez_single;	// as opposed to list_head

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
