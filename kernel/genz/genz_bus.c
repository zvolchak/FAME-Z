#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "genz_baseline.h"
#include "genz_device.h"

MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);

static unsigned auto0 = 1;
module_param(auto0, uint, 0644);
MODULE_PARM_DESC(auto0, "auto-create bus instance genz0 (1)");

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

int genz_init_one(struct device *dev)
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

// static unsigned next_genz_bus = 0;

struct bus_type genz_bus;	// Only one directly in /sys/bus

struct device genz_dev_root;	// Limited to one for now

void genz_bus_exit(void)
{
	pr_info("%s()\n", __FUNCTION__);
	bus_unregister(&genz_bus);
	genz_classes_destroy();
}

int __init genz_bus_init(void)
{
	int ret = 0;

	pr_info("%s()\n", __FUNCTION__);

	if ((ret = genz_classes_init())) {
		pr_err("%s()->genz_classes_init() failed\n", __FUNCTION__);
		return ret;
	}
	genz_bus.name = "genz_bus";
	genz_bus.dev_name = "genz_BUS%u";	// "subsystem enumeration"
	genz_bus.dev_root = NULL;		// "Default parent device"
	genz_bus.match = genz_match;		// Can driver handle device?
	genz_bus.probe = genz_init_one;		// if match() call drv.probe
	genz_bus.num_vf = genz_num_vf;

	if ((ret = bus_register(&genz_bus))) {
		pr_err("%s()->bus_register() failed\n", __FUNCTION__);
		genz_classes_destroy();
		return ret;
	}

	if (!auto0)
		return 0;

	// FIXME: multiple steps to create enumerated buses correctly
	// 1. Insure I like the layout of sysfs after the explicit code below
	//    that forces creation of genz0.
	// 2. Move this explicit code into .match/.probe (ie genz_init_one)
	//    and call it directly.
	// 3. Trigger genz_init_one() from insmod of famez.ko via an explicit
	//    call.
	// 4. Have famez.ko generate a "hotplug uevent" that triggers it all.

	// LDD3:14 Device Model -> "Device Registration"; see also source for
	// "subsys_register()".  Need a separate object from bus to form an
	// anchor point; it's not a fully fleshed-out struct device but serves
	// the anchor purpose.  I'm not totally sure why this works, but it
	// does what I want.  Order matters.  Enumeration of extra buses
	// (cards) is left as an exercise for the reader.

	// .parent = NULL lands at the top of /sys/devices, which seems good
	genz_dev_root.bus = &genz_bus;
	device_initialize(&genz_dev_root);
	dev_set_name(&genz_dev_root, "genz0_dev_root");
	genz_dev_root.kobj.parent = NULL;
	if ((ret = device_add(&genz_dev_root))) {
		pr_err("%s()->device_add(genz_dev_root) failed\n", __FUNCTION__);
		genz_bus_exit();
	}
	return ret;
}

module_init(genz_bus_init);
module_exit(genz_bus_exit);
