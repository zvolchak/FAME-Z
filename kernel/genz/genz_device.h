#ifndef GENZ_DEVICE_DOT_H
#define GENZ_DEVICE_DOT_H

#include <linux/cdev.h>

#include "genz_baseline.h"

// Composition pattern to realize all data needed to represent a device.
// "misc" class devices get it all clearly spelled out in struct miscdevice.
// and it's all populated by msic_register() in the core.  cdev is kept
// as a full structure; it can be be pulled from the filp->f_inode->i_cdev
// and used as anchor in to_xxxx lookups.

struct genz_char_device {
	unsigned CCE;			// MUST BE FIRST FIELD!
	const char *cclass;		// genz_component_class_str[CCE]
	void *file_private_data;	// Extracted at first fops->open()
	struct class *genz_class;	// Multi-purpose struct
	struct cdev cdev;		// full structure, has
					// kobject
					// owner
					// ops (fops)
					// list_head
					// dev_t (base maj/min)
					// count (of minors)
	struct bin_attribute CoreStructure;

	// Copied from miscdevice, in active use
	struct device *parent;		// set by caller, now to figure out WTF?
	struct device *this_device;	// created on the fly.

	// Copied from miscdevice, not used yet
	umode_t mode;
	const struct attribute_group **attr_groups;
	const char *name;		// used in device_create[_with_groups]
	const char *nodename;		// used in misc_class->devnode()
					// callback to name...

	// NOT copied from miscdevice
	// minor, because cdev has a dev_t
	// list_head, because cdev has one
};

static inline void *genz_char_drv_1stopen_private_data(struct file *file)
{
	struct genz_char_device *container = container_of(
		file->f_inode->i_cdev,		// member address
		struct genz_char_device,	// container type
		cdev);				// container member
	return container->file_private_data;
}

extern const char * const genz_component_class_str[];

// EXPORTed

extern struct genz_core_structure *genz_core_structure_create(uint64_t);
extern void genz_core_structure_destroy(struct genz_core_structure *);

extern struct genz_char_device *genz_register_bridge(
	unsigned, const struct file_operations *, void *, int);

extern void genz_unregister_char_device(struct genz_char_device *);
#endif
