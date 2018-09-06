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
#include <linux/module.h>	// fops->owner

#include "famez.h"

//-------------------------------------------------------------------------
// In the monolithic driver this was famez_bridge_setup()

int famez_register(const char *Base_C_Class_str, const char *basename,
		   const struct file_operations *fops)
{
	struct famez_adapter *adapter;
	struct pci_dev *pdev;
	struct genz_char_device *lookup;
	char *ownername, *devname;
	int ret, nbindings;

	ownername = fops->owner->name;

	if ((ret = down_interruptible(&famez_active_sema)))
		return ret;

	nbindings = 0;
	list_for_each_entry(adapter, &famez_active_list, lister) {

		pdev = adapter->pdev;
		pr_info(FZ "binding %s to %s: ",
			ownername, pci_resource_name(pdev, 1));

		ret = -ENOMEM;
		if (!(lookup = kzalloc(sizeof(*lookup),
				       GFP_KERNEL)))
			goto up_and_out;
		if (!(devname = kzalloc(strlen(ownername) + 6,	// "_%02X
				     GFP_KERNEL))) {
			kfree(lookup);
			goto up_and_out;
		}

		// Device file name is meant to be reminiscent of lspci output
		sprintf(devname, "%s_%02x", ownername, pdev->devfn >> 3);
		lookup->miscdev.name = devname;
		lookup->miscdev.fops = fops;
		lookup->miscdev.minor = MISC_DYNAMIC_MINOR;
		lookup->miscdev.mode = 0666;
	
		strncpy(adapter->core->Base_C_Class_str, Base_C_Class_str,
			sizeof(adapter->core->Base_C_Class_str));
		lookup->adapter = adapter;	// Don't point that thing at me
		adapter->teardown_lookup = lookup;
		if ((ret = misc_register(&lookup->miscdev))) {
			kfree(devname);
			kfree(lookup);
			goto up_and_out;
		}
		pr_cont("success\n");
		nbindings++;
	}
	ret = nbindings;

up_and_out:
	if (ret < 0)
		pr_cont("FAILURE\n");
	up(&famez_active_sema);
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

	ownername = fops->owner->name;

	if ((ret = down_interruptible(&famez_active_sema)))
		return ret;

	ret = 0;
	list_for_each_entry(adapter, &famez_active_list, lister) {
		pr_info(FZ "UNbind %s from %s: ",
			ownername, pci_resource_name(adapter->pdev, 0));

		if ((lookup = adapter->teardown_lookup) &&
		    (lookup->miscdev.fops == fops)) {
				misc_deregister(&lookup->miscdev);
				kfree(lookup->miscdev.name);
				kfree(lookup);
				adapter->teardown_lookup = NULL;
				strcpy(adapter->core->Base_C_Class_str, FAMEZ_NAME);
				ret++;
				pr_cont("success\n");
		} else
			pr_cont("not actually bound\n");
	}
	up(&famez_active_sema);
	return ret;

}
EXPORT_SYMBOL(famez_deregister);

