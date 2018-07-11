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
// file->private is set to the miscdevice structure used to register.

static int famez_open(struct inode *inode, struct file *file)
{
#if 0
	struct famez_configuration *cur = NULL;
	struct miscdevice *miscdev = file->private_data;

	spin_lock(&famez_active_lock);
	list_for_each_entry(cur, &famez_active_list, lister) {
		pr_info(FZ   "cur dev  @ 0x%p\n", &cur->pdev->dev);
		pr_info(FZSP "misc dev @ 0x%p\n", miscdev->this_device);
	}
	spin_unlock(&famez_active_lock);
#endif
	return 0;
}

//-------------------------------------------------------------------------
// Only at the last close

static int famez_release(struct inode *inode, struct file *file)
{
	return 0;
}

//-------------------------------------------------------------------------

static ssize_t famez_read(struct file *file, char __user *buf, size_t len,
                          loff_t *ppos)
{
	int ret = -EIO;

	spin_lock(&famez_last_slot_lock);
	if (!famez_last_slot.msglen) {	// Wait for new data?
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		PR_V2(FZ "read() waiting...\n");
		spin_unlock(&famez_last_slot_lock);
		wait_event_interruptible(famez_reader_wait, 
					 famez_last_slot.msglen);
		PR_V2(FZSP "wait finished, %llu bytes to read\n",
					 famez_last_slot.msglen);
	}
	if (len < famez_last_slot.msglen) {
		ret = -EINVAL;
		goto err_done;
	}
	len = famez_last_slot.msglen;

	// copy_to_user can sleep.  Returns the number of bytes that could NOT
	// be copied or -ERRNO.
	if (!(ret = copy_to_user(buf, famez_last_slot.msg, len)))
		famez_last_slot.msglen = 0;
	PR_V2(FZSP "copy_to_user returns %d\n", ret);
	ret = ret ? : len;

	// fall through

err_done:
	spin_unlock(&famez_last_slot_lock);
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


// On syscall_open() file->private_data is set to this

static struct miscdevice famez_miscdev = {
	.name	= FAMEZ_NAME,
	.fops	= &famez_fops,
	.minor	= MISC_DYNAMIC_MINOR,
	.mode	= 0666,
};

int famez_chardev_setup(void)
{
	return misc_register(&famez_miscdev);
}

void famez_chardev_teardown(void)
{
	misc_deregister(&famez_miscdev);
}
