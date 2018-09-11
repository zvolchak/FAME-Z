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

/*
 * MINORBITS is 20, which is 1M components, which is cool, but it's 16k longs
 * in the bitmap, or 128k, which seems like uncool overkill.
 */

#define GENZ_MINORBITS	14			/* 16k components per class */
#define MAXMINORS	(1 << GENZ_MINORBITS)	/* 2k space per bitmap */

static DEFINE_MUTEX(bridge_mutex);
static DECLARE_BITMAP(bridge_minor_bitmap, MAXMINORS) = { 0 };
static LIST_HEAD(bridge_list);

static uint64_t bridge_major = 0;		/* Until first allocation */

/**
 * genz_core_structure_create - allocate and populate a Gen-Z Core Structure
 * @alloc: a bitfield directing which sub-structures to allocate.
 * 
 * Create a semantically-complete Core Structure (not binary field-precise).
 */

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

/**
 * genz_register_bridge - add a new bridge character device and driver
 * @devname: string containing the device name to be used in sysfs
 * @CCE: Component Class Encoding from the Gen-Z spec
 * @fops: driver set for the device
 * @file_private_data: to be attached as file->private_data in all fops
 *
 * Based on misc_register().  Returns 0 on success or -ESOMETHING.
 */

int genz_register_bridge(char *devname, unsigned CCE,
			 const struct file_operations *fops,
			 void *file_private_data,
			 int slot)
{
	int ret = 0;
	char *ownername = NULL;
	struct genz_char_device *wrapper = NULL;
	dev_t base_dev_t = 0;
	uint64_t minor;

	if (CCE < GENZ_CCE_DISCRETE_BRIDGE ||
	    CCE > GENZ_CCE_INTEGRATED_BRIDGE)
		return -EDOM;

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
	pr_info("%s dev_t = %llu:%llu\n", ownername, bridge_major, minor);

	if (!(wrapper = kzalloc(sizeof(*wrapper), GFP_KERNEL))) {
		ret = -ENOMEM;
		goto up_and_out;
	}

	// This sets .fops, .list, and .kobj == ktype_cdev_default.
	// Then add anything else.
	cdev_init(&wrapper->cdev, fops);
	wrapper->cdev.dev = MKDEV(bridge_major, minor);
	wrapper->cdev.count = 1;
	if ((ret = kobject_set_name(&wrapper->cdev.kobj, "%s", devname)))
		goto up_and_out;

	wrapper->genz_class = genz_class_getter(GENZ_CCE_DISCRETE_BRIDGE);
	wrapper->mode = 0666;
	wrapper->parent = genz_find_me_a_bus_device(slot);

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
		wrapper->parent,
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
	if (ret) {
		pr_cont("FAILURE\n");
		if (wrapper)
			kfree(wrapper);
	}
	mutex_unlock(&bridge_mutex);
	return ret;
}
EXPORT_SYMBOL(genz_register_bridge);
