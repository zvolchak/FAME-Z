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
#include <linux/kdev_t.h>
#include <linux/module.h>	// fops->owner

#include "famez.h"

//-------------------------------------------------------------------------
// MINORBITS is 20, which is 1M devices, which is cool, but it's 16k longs
// in the bitmap, or 128k, which seems like uncool overkill.

#define GENZ_MINORBITS	14			// 16k devices per class
#define MAXMINORS	(1 << GENZ_MINORBITS)	// 2k space per bitmap

static DECLARE_BITMAP(famez_bridge_minor_bitmap, MAXMINORS) = { 0 };

static uint64_t famez_bridge_major = 0;		// Until first allocation

int famez_register(const char *Base_C_Class_str, const char *basename,
		   const struct file_operations *fops)
{
	struct famez_adapter *adapter;
	struct pci_dev *pdev;
	struct genz_char_device *lookup;
	char *ownername, *devname = NULL;	// use create_device() vararags
	int ret, nbindings;
	dev_t base_dev_t;
	uint64_t minor;

	// pr_info("bitmask is %lu bytes\n", sizeof(famez_bridge_minor_bitmap));

	if ((ret = down_interruptible(&famez_adapter_sema)))
		return ret;
	
	ownername = fops->owner->name;

	// Following the style of misc_register()
	minor = find_first_zero_bit(famez_bridge_minor_bitmap, GENZ_MINORBITS);
	if (minor >= GENZ_MINORBITS) {
		pr_err("Exhausted all minor numbers for major %llu (%s)\n",
			famez_bridge_major, ownername);
		return -EDOM;
	}
	if (famez_bridge_major) {
		base_dev_t = MKDEV(famez_bridge_major, minor);
		ret = register_chrdev_region(base_dev_t, 1, ownername);
	} else {
		if (!(ret = alloc_chrdev_region(&base_dev_t, minor, 1, ownername)))
			famez_bridge_major = MAJOR(base_dev_t);
	}
	if (ret) {
		pr_err("Can't allocate chrdev_region: %d\n", ret);
		return ret;
	}
	set_bit(minor, famez_bridge_minor_bitmap);
	pr_info("%s is major minor %llu %llu\n",
		ownername, famez_bridge_major, minor);

	nbindings = 0;
	list_for_each_entry(adapter, &famez_adapter_list, lister) {

		pdev = adapter->pdev;
		pr_info(FZ "binding %s to %s: ",
			ownername, pci_resource_name(pdev, 1));

		ret = -ENOMEM;
		if (!(lookup = kzalloc(sizeof(*lookup), GFP_KERNEL)))
			goto up_and_out;
		if (devname)
			kfree(devname);
		if (!(devname = kzalloc(strlen(ownername) + 6,	// "_%02X
				     GFP_KERNEL)))
			goto up_and_out;

		// Device file name is meant to be reminiscent of lspci output.
		sprintf(devname, "%s_%02x", ownername, pdev->devfn >> 3);

		// This sets .fops, .list, and .kobj == ktype_cdev_default.
		// Then add anything else.
		cdev_init(&lookup->cdev, fops);
		lookup->cdev.dev = MKDEV(famez_bridge_major, minor);
		lookup->cdev.count = 1;
		kobject_set_name(&lookup->cdev.kobj, "%s", devname);

		lookup->genz_class = genz_class_getter(GENZ_CCE_DISCRETE_BRIDGE);
		lookup->mode = 0666;

		// Finish init for teardown
		adapter->teardown_lookup = lookup;

		if ((ret = cdev_add(&lookup->cdev,
				    lookup->cdev.dev,
				    lookup->cdev.count))) {
			goto up_and_out;
		}
		
		// Final work: there's also plain "device_create()".  Driver
		// bcomes "live" on success so insure data is ready.
		lookup->file_private_data = adapter;
		lookup->this_device = device_create_with_groups(
			lookup->genz_class,
			lookup->parent,		// NULL for now
			lookup->cdev.dev,
			lookup,			// Not sure where this goes
			lookup->attr_groups,
			"%s",
			devname);
		if (IS_ERR(lookup->this_device)) {
			ret = PTR_ERR(lookup->this_device);
			goto up_and_out;
		}

		// Now that all allocs have worked, change adapter.  Yes it's
		// slightly after the "live" activation but REALLY?
		strncpy(adapter->core->Base_C_Class_str, Base_C_Class_str,
			sizeof(adapter->core->Base_C_Class_str));

		pr_cont("success\n");
		nbindings++;
	}
	ret = nbindings;

up_and_out:
	if (devname)
		kfree(devname);
	if (ret < 0) {
		pr_cont("FAILURE\n");
		if (lookup)
			kfree(lookup);
		adapter->teardown_lookup = NULL;
	}
	up(&famez_adapter_sema);
	return ret;
}
EXPORT_SYMBOL(famez_register);

//-------------------------------------------------------------------------
// In the monolithic driver this was famez_bridge_teardown().  Return the
// count of bindings broken or -ERRNO.

int famez_deregister(const struct file_operations *fops)
{
	struct famez_adapter *adapter;
	struct genz_char_device *lookup;
	char *ownername;
	int ret;
	uint64_t minor;

	ownername = fops->owner->name;

	if ((ret = down_interruptible(&famez_adapter_sema)))
		return ret;

	// If I get them all it releases the major number automatically.
	for (minor = 0; minor < MAXMINORS; minor++) {
		dev_t base_dev_t;

		if (test_bit(minor, famez_bridge_minor_bitmap)) {
			base_dev_t = MKDEV(famez_bridge_major, minor);
			unregister_chrdev_region(base_dev_t, 1);
			// cdev_del(...
			// free(devname in kobject_set_name?)
		}
	}
	memset(famez_bridge_minor_bitmap, 0, sizeof(famez_bridge_minor_bitmap));

	ret = 0;
	list_for_each_entry(adapter, &famez_adapter_list, lister) {
		pr_info(FZ "UNbind %s from %s: ",
			ownername, pci_resource_name(adapter->pdev, 0));

		if ((lookup = adapter->teardown_lookup) &&
		    (lookup->cdev.ops == fops)) {
			device_destroy(lookup->genz_class, lookup->cdev.dev);
			kfree(lookup);
			adapter->teardown_lookup = NULL;
			strcpy(adapter->core->Base_C_Class_str, FAMEZ_NAME);
			ret++;
			pr_cont("success\n");
		} else
			pr_cont("not actually bound\n");
	}
	up(&famez_adapter_sema);
	return ret;

}
EXPORT_SYMBOL(famez_deregister);
