#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "genz_baseline.h"
#include "genz_device.h"

//-------------------------------------------------------------------------
// Boolean

static int genz_match(struct device *dev, struct device_driver *drv)
{
	struct genz_device __unused *genz_dev = to_genz_dev(dev);

	pr_info("%s()\n", __FUNCTION__);
	return 1;
};

//-------------------------------------------------------------------------

static int genz_num_vf(struct device *dev)
{
	pr_info("%s()\n", __FUNCTION__);
	return 1;
}

//-----------------------------------------------------------------------
// FIXME: how is this called?

static int genz_dev_init(struct genz_device *dev)
{
	pr_info("%s()\n", __FUNCTION__);

	return 0;
}

static void genz_dev_uninit(struct genz_device *dev)
{
	pr_info("%s()\n", __FUNCTION__);
}

static const struct genz_device_ops devops = {
	.init		= genz_dev_init,
	.uninit		= genz_dev_uninit,
};

//-----------------------------------------------------------------------
// This is a global common setup for all genz_devs, followed by a "personal"
// customization callback.  See the dummy driver and alloc_netdev.

struct genz_device *alloc_genzdev(const char *namefmt,
				  void (*customize_cb)(struct genz_device *))
{
	struct genz_device *genz_dev;

	pr_info("%s()\n", __FUNCTION__);

	if (!(genz_dev = kzalloc(sizeof(struct genz_device), GFP_KERNEL))) {
		pr_err("%s() failed kzalloc\n", __FUNCTION__);
		return NULL;
	}
	// FIXME: does it need to be 32-byte aligned like alloc_netdev_mqs?
	strcpy(genz_dev->namefmt, namefmt);
	customize_cb(genz_dev);
	return genz_dev;
}
EXPORT_SYMBOL(alloc_genzdev);

//-----------------------------------------------------------------------

static struct genz_device *the_one = NULL;

int register_genzdev(struct genz_device *genz_dev) {
	pr_info("%s()\n", __FUNCTION__);
	if (the_one)
		return -EALREADY;
	the_one = genz_dev;
	return 0;
}
EXPORT_SYMBOL(register_genzdev);

//-----------------------------------------------------------------------

void unregister_genzdev(struct genz_device *genz_dev) {
	pr_info("%s()\n", __FUNCTION__);
	if (the_one && the_one == genz_dev)
		the_one = NULL;
}
EXPORT_SYMBOL(unregister_genzdev);

//-----------------------------------------------------------------------
// A callback for the global alloc_genzdev()

static void genz_device_customize(struct genz_device *genz_dev)
{
	pr_info("%s()\n", __FUNCTION__);
}

int genz_init_one(void)
{
	struct genz_device *genz_dev;
	int ret = 0;

	pr_info("%s()\n", __FUNCTION__);

	if (!(genz_dev = alloc_genzdev("genz%02d", genz_device_customize))) {
		pr_err("%s()->alloc_genzdev failed \n", __FUNCTION__);
		return -ENOMEM;		// It had ONE job...
	}

	if ((ret = register_genzdev(genz_dev)))
		pr_err("%s()->register_genzdev() failed\n", __FUNCTION__);

	return ret;
}

//-----------------------------------------------------------------------
// Module wrapping.

struct bus_type genz_bus = {
	.name	= "genz",
	.dev_name = "genz_dev_name",
	.match = genz_match,
	.num_vf = genz_num_vf,
};

// See use of "mci_pdev" in edac_mc_sysfs.c
struct device genz_dev_root;

int __init genz_bus_init(void)
{
	int ret = 0;

	pr_info("%s()\n", __FUNCTION__);
	if ((ret = bus_register(&genz_bus))) {
		pr_err("%s()->bus_register() failed\n", __FUNCTION__);
		return ret;
	}
	genz_bus.dev_root = NULL; 	// &genz_dev_root;

	if ((ret = genz_classes_init())) {
		pr_err("%s()->genz_classes_init() failed\n", __FUNCTION__);
		goto early_done;
	}

	if ((ret = genz_devices_init())) {
		pr_err("%s()->genz_devices_init() failed\n", __FUNCTION__);
		goto early_done;
	}

	// Why am I doing this?  From net/dummy.  Or is this the "genz0"
	// thing I'm forcing below?
	// if ((ret = genz_init_one())) {
		// pr_err("%s()->genz_init_one() failed\n", __FUNCTION__);
		// goto early_done;
	// }

	// LDD3:14 Device Model -> "Device Registration"; see also source for
	// "subsys_register()".  Need a separate object from bus to form an
	// anchor point; it's not a fully fleshed-out struct device but serves
	// the anchor purpose.  I'm not totally sure why this works, but it
	// does what I want.  Order matters.  Enumeration of extra buses
	// (cards) is left as an exercise for the reader.
	genz_dev_root.bus = &genz_bus;
	device_initialize(&genz_dev_root);
	dev_set_name(&genz_dev_root, "genz0");
	genz_dev_root.kobj.parent = NULL;
	if ((ret = device_add(&genz_dev_root))) {
		pr_err("%s()->device_add(genz_dev_root) failed\n", __FUNCTION__);
		goto early_done;
	}

early_done:
	pr_info("%s() %s\n", __FUNCTION__, !ret ? "passed" : "FAILED");
	if (ret)
		bus_unregister(&genz_bus);
	return ret;
}

void genz_bus_exit(void)
{
	pr_info("%s()\n", __FUNCTION__);
	genz_classes_destroy();
	bus_unregister(&genz_bus);
}

module_init(genz_bus_init);
module_exit(genz_bus_exit);
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
