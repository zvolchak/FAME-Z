/*
 * Copyright (C) 2018 Hewlett Packard Enterprise Development LP.
 * All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "famez.h"

DECLARE_WAIT_QUEUE_HEAD(famez_reader_wait);

//-------------------------------------------------------------------------
// https://stackoverflow.com/questions/39464028/device-specific-data-structure-with-platform-driver-and-character-device-interfa
// A lookup table to take advantage of misc_register putting
// its argument into file->private at open().  Fill in the
// blanks for each config and go.

typedef struct {
	// What I'm really looking for must be a pointer AND the first
	// field, so that "container_of(..., anchor)" as a void pointer is
	// essentially a union with this field.
	struct famez_configuration *config;	// what I want to recover
	struct miscdevice miscdev;		// full structure
} miscdev2config_t;

static inline struct famez_configuration *extract_config(void *private_data)
{
	void *tmp = container_of(private_data, miscdev2config_t, miscdev);
	return tmp;
}

//-------------------------------------------------------------------------
// file->private is set to the miscdevice structure used to register.

static int famez_open(struct inode *inode, struct file *file)
{
	struct famez_configuration *config = extract_config(file->private_data);

	atomic_inc(&config->nr_users);

	return 0;
}

//-------------------------------------------------------------------------
// Only at the last close

static int famez_release(struct inode *inode, struct file *file)
{
	struct famez_configuration *config = extract_config(file->private_data);

	if (!atomic_dec_and_test(&config->nr_users)) {
		pr_warn(FZ "final release still has users\n");
		atomic_set(&config->nr_users, 0);
	}
	return 0;
}

//-------------------------------------------------------------------------

static ssize_t famez_read(struct file *file, char __user *buf, size_t len,
                          loff_t *ppos)
{
	int ret = -EIO;

	if (!famez_last_slot.msglen) {	// Wait for new data?
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		PR_V2(FZ "read() waiting...\n");
		wait_event_interruptible(famez_reader_wait, 
					 famez_last_slot.msglen);
		PR_V2(FZSP "wait finished, %llu bytes to read\n",
					 famez_last_slot.msglen);
	}
	if (len < famez_last_slot.msglen)
		return -E2BIG;
	len = famez_last_slot.msglen;

	// copy_to_user can sleep.  Returns the number of bytes that could NOT
	// be copied or -ERRNO.
	if (!(ret = copy_to_user(buf, famez_last_slot.msg, len))) {
		spin_lock(&famez_last_slot_lock);
		famez_last_slot.msglen = 0;
		spin_unlock(&famez_last_slot_lock);
	}
	PR_V2(FZSP "copy_to_user returns %d\n", ret);
	ret = ret ? -EIO : len;
	return ret;
}

//-------------------------------------------------------------------------

static ssize_t famez_write(struct file *file, const char __user *buf,
			   size_t len, loff_t *ppos)
{
	return -ENOSYS;
}

//-------------------------------------------------------------------------
// Returning 0 will cause the caller (epoll/poll/select) to sleep.

static uint famez_poll(struct file *file, struct poll_table_struct *wait)
{
	uint ret = 0;

	poll_wait(file, &famez_reader_wait, wait);
		ret |= POLLIN | POLLRDNORM;
	return ret;
}

static const struct file_operations famez_fops = {
	.owner	=	THIS_MODULE,
	.open	=	famez_open,
	.release =      famez_release,
	.read	=       famez_read,
	.write	=       famez_write,
	.poll	=       famez_poll,
};

//-------------------------------------------------------------------------
// Follow convention of PCI core: all (early) setup takes a pdev.
// The argument of misc_register ends up in file->private_data.

int famez_chardev_setup(struct pci_dev *pdev)
{
	struct famez_configuration *config = pci_get_drvdata(pdev);
	miscdev2config_t *lookup = kzalloc(sizeof(*lookup), GFP_KERNEL);
	char *name;

	if (!lookup)
		return -ENOMEM;

	if (!(name = kzalloc(32, GFP_KERNEL))) {
		kfree(lookup);
		return -ENOMEM;
	}
	sprintf(name, "%s.%0x", FAMEZ_NAME, config->pdev->devfn);
	lookup->miscdev.name = name;
	lookup->miscdev.fops = &famez_fops;
	lookup->miscdev.minor = MISC_DYNAMIC_MINOR;
	lookup->miscdev.mode = 0666;

	lookup->config = config;
	lookup->config->teardown_miscdev = &lookup->miscdev;
	return misc_register(&lookup->miscdev);
}

//-------------------------------------------------------------------------
// Follow convention of PCI core: all (early) setup takes a pdev.

void famez_chardev_teardown(struct pci_dev *pdev)
{
	struct famez_configuration *config = pci_get_drvdata(pdev);
	// Remember, this is essentially a union of two pointers
	miscdev2config_t *lookup;
	
	lookup = (void *)extract_config(config->teardown_miscdev);

	misc_deregister(config->teardown_miscdev);
	kfree(lookup->miscdev.name);
	kfree(lookup);
	config->teardown_miscdev = NULL;
}
