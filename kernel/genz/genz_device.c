#include <linux/export.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include "genz_baseline.h"
#include "genz_routing_fabric.h"

#define UNUSED __attribute__ ((unused))

//-------------------------------------------------------------------------
// alloc is a bitfield directing which sub-structures to allocate.

struct genz_core_structure *genz_core_structure_create(uint64_t alloc)
{
	struct genz_core_structure *core;

	if (!(core = kzalloc(sizeof *core, GFP_KERNEL)))
		return ERR_PTR(-ENOMEM);

	if ((alloc & GENZ_CORE_STRUCTURE_ALLOC_COMP_DEST_TABLE) &&
	    !(core->comp_dest_table =
	      kzalloc(sizeof(*core->comp_dest_table), GFP_KERNEL))) {
		genz_core_structure_destroy(core);
		return ERR_PTR(-ENOMEM);
	}
	return core;
}
EXPORT_SYMBOL(genz_core_structure_create);

//-------------------------------------------------------------------------
void genz_core_structure_destroy(struct genz_core_structure *core)
{
	if (core->comp_dest_table) {
		kfree(core->comp_dest_table);
		core->comp_dest_table = NULL;
	}
	kfree(core);
}
EXPORT_SYMBOL(genz_core_structure_destroy);

//-------------------------------------------------------------------------
// CCE = Component Class Encoding, appendix C

int genz_register_bridge(unsigned CCE, const struct file_operations *fops)
{
	return -ENOSYS;
}
EXPORT_SYMBOL(genz_register_bridge);

//-------------------------------------------------------------------------
#ifdef DEVICE_REGISTER_PARENT

static void release_famez_parent(struct device *dev)
{
	pr_info("%s()\n", __FUNCTION__);
}

static struct device UNUSED famez_parent = {
	.init_name	= "FAME-Z_adapter",
	.bus		= &genz_bus,
	.release	= release_famez_parent,
};
	if ((ret = device_register(&famez_parent))) {
		pr_err("Registering parent device failed\n");
		bus_unregister(&genz_bus);
		return ret;
	}
#endif

