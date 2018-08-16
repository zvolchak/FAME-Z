#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#define DRV_NAME	"Gen-Z"
#define DRV_VERSION	"0.1"

#define GZNAMFMTSIZ	64

#define __unused __attribute__ ((unused))

struct genz_private {			// Just following netdev
	int junk;
};

struct genz_device {
	char namefmt[GZNAMFMTSIZ];
	struct list_head lister;
	uint64_t flags;
	struct device dev;		// for to_genz_dev
	struct genz_priv *priv;
};
#define to_genz_dev(nnn) container_of(nnn, struct genz_device, dev)

struct genz_device_ops {
	int (*init)(struct genz_device *genz_dev);
	void (*uninit)(struct genz_device *genz_dev);
};

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

//-------------------------------------------------------------------------

static void release_famez_parent(struct device *dev)
{
	pr_info("%s()\n", __FUNCTION__);
}

//-----------------------------------------------------------------------
// FIXME: how is this called?

static int genz_dev_init(struct genz_device *dev)
{
	struct genz_private __unused *priv = kzalloc(sizeof(*priv), GFP_KERNEL);

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

struct genz_device *alloc_genzdev(int sizeof_priv, const char *namefmt,
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

	if (!(genz_dev = alloc_genzdev(sizeof(struct genz_private),
				       "genz%02d", genz_device_customize))) {
		pr_err("%s()->alloc_genzdev failed \n", __FUNCTION__);
		return -ENOMEM;		// It had ONE job...
	}

	if ((ret = register_genzdev(genz_dev)))
		pr_err("%s()->register_genzdev() failed\n", __FUNCTION__);
		// FIXME memory leaks

	return ret;
}

//-----------------------------------------------------------------------
// Module wrapping.

static struct bus_type genz_bus = {
	.name	= "genz",
	.match = genz_match,
	.num_vf = genz_num_vf,
};

static struct device famez_parent = {
	.init_name	= "FAME-Z_adapter",
	.bus		= &genz_bus,
	.release	= release_famez_parent,
};

int __init jfdi(void)
{
	int ret = 0;

	pr_info("%s()\n", __FUNCTION__);
	if ((ret = bus_register(&genz_bus))) {
		pr_err("Registering Gen-Z bus failed\n");
		return ret;
	}
	if ((ret = device_register(&famez_parent))) {
		pr_err("Registering parent device failed\n");
		bus_unregister(&genz_bus);
		return ret;
	}
	if ((ret = genz_init_one())) {
		pr_err("%s()->genz_init_one() failed\n", __FUNCTION__);
		device_unregister(&famez_parent);
		bus_unregister(&genz_bus);
		return ret;
	}

	pr_info("%s() passed\n", __FUNCTION__);
	return 0;
}

void genz_exit(void)
{
	pr_info("%s()\n", __FUNCTION__);
	device_unregister(&famez_parent);
	bus_unregister(&genz_bus);
}

module_init(jfdi);
module_exit(genz_exit);
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);