#include <linux/export.h>
#include <linux/slab.h>

#include "genz_baseline.h"
#include "genz_routing_fabric.h"

#define __unused __attribute__ ((unused))

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

void genz_core_structure_destroy(struct genz_core_structure *core)
{
	if (core->comp_dest_table) {
		kfree(core->comp_dest_table);
		core->comp_dest_table = NULL;
	}
	kfree(core);
}
EXPORT_SYMBOL(genz_core_structure_destroy);
