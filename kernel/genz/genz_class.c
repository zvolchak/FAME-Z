#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "genz_baseline.h"

//-------------------------------------------------------------------------
// Gen-Z 1.0, Appendix C Component Class Encodings are the array index.
// Some names are tweaked to facilitate alphabetical ordering.

static struct class genz_classes[] = {
	{ .owner = NULL,	.name = "RESERVED" },			// 0x0
	{ .owner = THIS_MODULE, .name = "genz_memory_p2p" },
	{ .owner = THIS_MODULE, .name = "genz_memory_explicit" },
	{ .owner = THIS_MODULE, .name = "genz_switch_integrated" },
	{ .owner = THIS_MODULE, .name = "genz_switch_enclosure" },
	{ .owner = THIS_MODULE, .name = "genz_switch_fabric" },		// 0x5
	{ .owner = THIS_MODULE, .name = "genz_processor" },
	{ .owner = THIS_MODULE, .name = "genz_processor_nb" },
	{ .owner = THIS_MODULE, .name = "genz_accelerator_nb_nc" },
	{ .owner = THIS_MODULE, .name = "genz_accelerator_nb" },
	{ .owner = THIS_MODULE, .name = "genz_accelerator_nc" },	// 0xA
	{ .owner = THIS_MODULE, .name = "genz_accelerator" },
	{ .owner = THIS_MODULE, .name = "genz_io_nb_nc" },
	{ .owner = THIS_MODULE, .name = "genz_io_nb" },
	{ .owner = THIS_MODULE, .name = "genz_io_nc" },
	{ .owner = THIS_MODULE, .name = "genz_io" },			// 0xF
	{ .owner = THIS_MODULE, .name = "genz_block" },			// 0x10
	{ .owner = THIS_MODULE, .name = "genz_block_nb" },
	{ .owner = THIS_MODULE, .name = "genz_tr" },
	{ .owner = THIS_MODULE, .name = "genz_multiclass" },
	{ .owner = THIS_MODULE, .name = "genz_bridge_discrete" },
	{ .owner = THIS_MODULE, .name = "genz_bridge_integrated" },	// 0x15
	{}
};

//-------------------------------------------------------------------------
// It's actually all pointers so the math is good.

static unsigned maxindex = (sizeof(genz_classes)/sizeof(genz_classes[0])) - 2;

struct class *genz_class_getter(unsigned index)
{
	return (index && index <= maxindex) ? &genz_classes[index] : NULL;
}
EXPORT_SYMBOL(genz_class_getter);

//-------------------------------------------------------------------------
// 0 or error

int genz_classes_init()
{
	int i, ret = 0;

	pr_info("%s() max class index = 0x%x\n", __FUNCTION__, maxindex);

	// class_register() defaults to a kobj of "sysfs_dev_char_kobj".  It's
	// possible set kobj to something else first.  Or use create_class()
	// which does kzalloc behind the scenes along with class_register.
	// Thus things that piggyback off cls->kobj go under dev, see 
	// devices_init() in bootlin.

	for (i = 1; genz_classes[i].name; i++) {
		if ((ret = class_register(&genz_classes[i]))) {
			pr_err("class_register(%s) failed\n",
				genz_classes[i].name);
			while (--i > 0)
				class_unregister(&genz_classes[i]);
			return ret;
		}
	}
	return 0;
};

//-------------------------------------------------------------------------

void genz_classes_destroy()
{
	int i;

	pr_info("%s()\n", __FUNCTION__);
	for (i = 1; genz_classes[i].name; i++)
		class_unregister(&genz_classes[i]);
}
