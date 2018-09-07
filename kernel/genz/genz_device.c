/*
 * Copyright (C) 2018 Hewlett Packard Enterprise Development LP.
 * All rights reserved.
 *
 * This source code file is part of the FAME-Z project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <linux/bitops.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "genz_baseline.h"
#include "genz_device.h"
#include "genz_routing_fabric.h"

#define UNUSED __attribute__ ((unused))

//-------------------------------------------------------------------------
// MINORBITS is 20, which is 1M devices, which is cool, but it's 16k longs
// in the bitmap, or 128k, which seems like uncool overkill.

#define GENZ_MINORBITS	14			// 16k devices per class
#define MAXMINORS	(1 << GENZ_MINORBITS)	// 2k space per bitmap

static DECLARE_BITMAP(bridge_minor_bitmap, MAXMINORS) = { 0 };
static struct mutex bridge_mutex;

static uint64_t bridge_major = 0;		// Until first allocation

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
// Following the style of misc_register().
// CCE = Component Class Encoding, Gen-Z spec 1.0 Appendix C
// Return 0 or -ESOMETHING

int genz_register_bridge(char *devname, unsigned CCE,
			 const struct file_operations *fops,
			 void *file_private_data)
{
	int ret = 0;
	char *ownername = NULL;
	struct genz_char_device *wrapper = NULL;
	dev_t base_dev_t = 0;
	uint64_t minor;

	mutex_lock(&bridge_mutex);

	ownername = fops->owner->name;

	minor = find_first_zero_bit(bridge_minor_bitmap, GENZ_MINORBITS);
	if (minor >= GENZ_MINORBITS) {
		pr_err("Exhausted all minor numbers for major %llu (%s)\n",
			bridge_major, ownername);
		ret = -EDOM;
		goto up_and_out;
	}
	if (bridge_major) {
		base_dev_t = MKDEV(bridge_major, minor);
		ret = register_chrdev_region(base_dev_t, 1, ownername);
	} else {
		if (!(ret = alloc_chrdev_region(&base_dev_t, minor, 1, ownername)))
			bridge_major = MAJOR(base_dev_t);
	}
	if (ret) {
		pr_err("Can't allocate chrdev_region: %d\n", ret);
		goto up_and_out;
	}
	set_bit(minor, bridge_minor_bitmap);
	pr_info("%s is major minor %llu %llu\n",
		ownername, bridge_major, minor);

	ret = -ENOMEM;
	if (!(wrapper = kzalloc(sizeof(*wrapper), GFP_KERNEL)))
		goto up_and_out;
	if (devname)
		kfree(devname);
	if (!(devname = kzalloc(strlen(ownername) + 6,	// "_%02X
			     GFP_KERNEL)))
		goto up_and_out;


	// This sets .fops, .list, and .kobj == ktype_cdev_default.
	// Then add anything else.
	cdev_init(&wrapper->cdev, fops);
	wrapper->cdev.dev = MKDEV(bridge_major, minor);
	wrapper->cdev.count = 1;
	kobject_set_name(&wrapper->cdev.kobj, "%s", devname);

	wrapper->genz_class = genz_class_getter(GENZ_CCE_DISCRETE_BRIDGE);
	wrapper->mode = 0666;

	if ((ret = cdev_add(&wrapper->cdev,
			    wrapper->cdev.dev,
			    wrapper->cdev.count))) {
		goto up_and_out;
	}
		
	// Final work: there's also plain "device_create()".  Driver
	// bcomes "live" on success so insure data is ready.
	wrapper->file_private_data = file_private_data;
	wrapper->this_device = device_create_with_groups(
		wrapper->genz_class,
		wrapper->parent,	// NULL for now
		wrapper->cdev.dev,
		wrapper,		// drvdata: not sure where this goes
		wrapper->attr_groups,
		"%s",
		devname);
	if (IS_ERR(wrapper->this_device)) {
		ret = PTR_ERR(wrapper->this_device);
		goto up_and_out;
	}

up_and_out:
	if (devname)
		kfree(devname);
	if (ret) {
		pr_cont("FAILURE\n");
		if (wrapper)
			kfree(wrapper);
	}
	mutex_unlock(&bridge_mutex);
	return ret;
}
EXPORT_SYMBOL(genz_register_bridge);

//-------------------------------------------------------------------------
// Because there are no mutex initializers

int genz_devices_init() {
	pr_info("%s()\n", __FUNCTION__);
	mutex_init(&bridge_mutex);
	return 0;
}

void genz_devices_destroy() {
	pr_info("%s()\n", __FUNCTION__);
}

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

