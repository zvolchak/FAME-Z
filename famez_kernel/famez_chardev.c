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
// A lookup table to take advantage of misc_register putting its argument
// into file->private at open().  Fill in the blanks for each config and go.
// This technique relies on the desired field being a pointer AND the first
// field, so that "container_of(..., anchor)" is a pointer to a pointer.
// I modified the article's solution to treat it as a container pointer and
// just grab whatever field I want, it doesn't even have to be the first one.
// If I put the "primary key" structure as the first field, then I wouldn't
// even need container_of as the address is synonymous with both.

struct miscdev2config {
	struct miscdevice miscdev;		// full structure, not a ptr
	struct famez_configuration *config;	// what I want to recover
};

static inline struct famez_configuration *extract_config(struct file *file)
{
	struct miscdevice *encapsulated_miscdev = file->private_data;
	struct miscdev2config *lookup = container_of(
		encapsulated_miscdev,	// the pointer to the member
		struct miscdev2config,	// the type of the container struct
		miscdev);		// the name of the member in the struct
	return lookup->config;
}

//-------------------------------------------------------------------------
// file->private is set to the miscdevice structure used to register.

static int famez_open(struct inode *inode, struct file *file)
{
	struct famez_configuration *config = extract_config(file);

	if ((uint64_t)atomic_read(&config->nr_users) > 5) {
		pr_err(FZ "you still haven't got container_of working right\n");
		return -ENOTTY;
	}

	PR_ENTER("config->max_msglen = %llu\n", config->max_msglen);

	atomic_inc(&config->nr_users);

	return 0;
}

//-------------------------------------------------------------------------
// Only at the last close

static int famez_release(struct inode *inode, struct file *file)
{
	struct famez_configuration *config = extract_config(file);

	if (!atomic_dec_and_test(&config->nr_users)) {
		pr_warn(FZ "final release() still has %d users\n",
			atomic_read(&config->nr_users));
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
	struct famez_configuration *config = extract_config(file);
	ssize_t returnlen = len;
	char *localbuf, *firstchar, *lastchar, *msgbody;
	int ret;
	uint16_t peer_id;

	PR_ENTER();
	if (len >= config->max_msglen - 1)	// Paranoia on term NUL
		return -E2BIG;
	if (!(localbuf = kzalloc(config->max_msglen, GFP_KERNEL)))
		return -ENOMEM;			// Bad coding here
	if (copy_from_user(localbuf, buf, len)) {
		ret = -EIO;
		goto alldone;
	}
	firstchar = localbuf;
	lastchar = firstchar + (len - 1);
	if (*(lastchar + 1) != '\0') {
		pr_err(FZ "Bad EOM conditions\n");
		ret = -EBADMSG;
		goto alldone;
	}

	// Strip leading and trailing whitespace and newlines.
	// firstchar + (len - 1) should equal lastchar when done.
	// Use many idiot checks.

	ret = strspn(firstchar, " \r\n\t");
	PR_V2(FZSP "stripping %d characters from front of \"%s\"\n",
		ret, firstchar);
	if (ret > len) {
		pr_err(FZ "strspn is weird\n");
		ret = -EDOM;
	}
	firstchar += ret;
	len -= ret;

	// Now the end.
	while (lastchar != firstchar && len && (
		*lastchar == ' ' ||
		*lastchar == '\n' ||
		*lastchar == '\r' ||
		*lastchar == '\t'
	)) {
		*lastchar-- = '\0';
		len--;
	}
	PR_V2(FZSP "\"%s\" has length %lu =? %lu\n",
		firstchar, len, strlen(firstchar));
	if (!len) {
		pr_warn(FZ "Original message was only whitespace\n");
		ret = returnlen;	// spoof success to caller
		goto alldone;
	}
	if (len < 0 || (firstchar + (len - 1) != lastchar)) {
		pr_err(FZ "send someone back to math: %lu\n", len);
		ret = -EIO;
		goto alldone;
	}

	// Split localbuf into two strings around the first colon
	if (!(msgbody = strchr(firstchar, ':'))) {
		pr_err(FZ "I see no colon in \"%s\"\n", firstchar);
		ret = -EBADMSG;
		goto alldone;
	}
	*msgbody = '\0';	// chomp ':', now two complete strings
	msgbody++;
	len = strlen(msgbody);
	if ((ret = kstrtou16(firstchar, 10, &peer_id)))
		goto alldone;	// -ERANGE, usually

	// Length or -ERRNO.  If length matched, then all is well, but
	// that len is always shorter than the original length.  Some
	// code (ie, "echo") will resubmit the partial if the count is
	// short.  So lie about it to the caller.
	ret = famez_sendmail(peer_id, msgbody, len, config);
	if (ret == len)
		ret = returnlen;

alldone:
	kfree(localbuf);	// FIXME: attach to config for life of file?
	return ret;
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
	struct miscdev2config *lookup = kzalloc(
		sizeof(struct miscdev2config), GFP_KERNEL);
	char *name;

	PR_ENTER("config->nr_users = %d\n", atomic_read(&config->nr_users));

	if (!lookup)
		return -ENOMEM;

	if (!(name = kzalloc(32, GFP_KERNEL))) {
		kfree(lookup);
		return -ENOMEM;
	}
	// Name should be reminiscent of lspci output
	sprintf(name, "%s%02x_bridge", FAMEZ_NAME, config->pdev->devfn >> 3);
	lookup->miscdev.name = name;
	lookup->miscdev.fops = &famez_fops;
	lookup->miscdev.minor = MISC_DYNAMIC_MINOR;
	lookup->miscdev.mode = 0666;

	lookup->config = config;	// Don't point that thing at me
	config->teardown_lookup = lookup;
	return misc_register(&lookup->miscdev);
}

//-------------------------------------------------------------------------
// Follow convention of PCI core: all (early) setup takes a pdev.

void famez_chardev_teardown(struct pci_dev *pdev)
{
	struct famez_configuration *config = pci_get_drvdata(pdev);
	struct miscdev2config *lookup = config->teardown_lookup;
	
	misc_deregister(&lookup->miscdev);
	kfree(lookup->miscdev.name);
	kfree(lookup);
	config->teardown_lookup = NULL;
}
