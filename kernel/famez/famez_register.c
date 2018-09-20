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

#include <linux/export.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/module.h>	// fops->owner

#include "famez.h"
#include "genz_device.h"

//-------------------------------------------------------------------------

int famez_register(unsigned CCE, const struct file_operations *fops)
{
	struct famez_adapter *adapter;
	char *ownername;
	int ret, nbindings;
	const char * cclass;

	// pr_info("bitmask is %lu bytes\n", sizeof(famez_bridge_minor_bitmap));

	if ((ret = down_interruptible(&famez_adapter_sema)))
		return ret;
	ownername = fops->owner->name;	
	nbindings = 0;
	list_for_each_entry(adapter, &famez_adapter_list, lister) {
		struct pci_dev *pdev;

		pdev = adapter->pdev;
		// Device file name is meant to be reminiscent of lspci output.
		pr_info(FZ "binding %s to %s: ",
			ownername, pci_resource_name(pdev, 1));

		cclass = genz_register_bridge(CCE, fops, adapter, adapter->slot);
		if (IS_ERR(cclass)) {
			ret = PTR_ERR(cclass);
			goto up_and_out;
		}

		// Now that all allocs have worked, change adapter.  Yes it's
		// slightly after the "live" activation, get over it.
		strncpy(adapter->core->Base_C_Class_str, cclass,
			sizeof(adapter->core->Base_C_Class_str) - 1);
		strncpy(adapter->my_slot->cclass, cclass,
			sizeof(adapter->my_slot->cclass) - 1);

		pr_cont("success\n");
		nbindings++;
	}
	ret = nbindings;

up_and_out:
	up(&famez_adapter_sema);
	return ret;
}
EXPORT_SYMBOL(famez_register);

//-------------------------------------------------------------------------
// In the monolithic driver this was famez_bridge_teardown().  Return the
// count of bindings broken or -ERRNO.

int famez_unregister(const struct file_operations *fops)
{
	return 0;

#if 0
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
			strncpy(adapter->my_slot->cclass, DEFAULT_CCLASS,
				sizeof(adapter->my_slot->cclass) - 1);
			strncpy(adapter->core->Base_C_Class_str, DEFAULT_CCLASS,
				sizeof(adapter->core->Base_C_Class_str) - 1);
			ret++;
			pr_cont("success\n");
		} else
			pr_cont("not actually bound\n");
	}
	up(&famez_adapter_sema);
	return ret;
#endif
}
EXPORT_SYMBOL(famez_unregister);
