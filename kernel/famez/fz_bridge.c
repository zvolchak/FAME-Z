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

#include <linux/ctype.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include <asm-generic/bug.h>	// yes after the others

#include "famez.h"
#include "fz_bridge.h"

MODULE_LICENSE("GPL");
MODULE_VERSION(FAMEZ_VERSION);
MODULE_AUTHOR("Rocky Craig <rocky.craig@hpe.com>");
MODULE_DESCRIPTION("Base subsystem for FAME-Z project.");

// module parameters are global

int verbose = 0;
module_param(verbose, uint, 0644);
MODULE_PARM_DESC(verbose, "increase amount of printk info (0)");

DECLARE_WAIT_QUEUE_HEAD(bridge_reader_wait);

//-------------------------------------------------------------------------
// file->private is set to the miscdevice structure used in misc_register.

static int bridge_open(struct inode *inode, struct file *file)
{
	struct famez_config *config = extract_config(file);
	int n, ret;

	// FIXME: got to come up with more 'local module' support for this.
	// Just keep it single user for now.
	ret = 0;
	if ((n = atomic_add_return(1, &config->nr_users) == 1)) {
		struct bridge_buffers *buffers;

		if (!(buffers = kzalloc(sizeof(*buffers), GFP_KERNEL))) {
			ret = -ENOMEM;
			goto alldone;
		}
		if (!(buffers->wbuf = kzalloc(config->max_buflen, GFP_KERNEL))) {
			kfree(buffers);
			ret = -ENOMEM;
			goto alldone;
		}
		mutex_init(&(buffers->wbuf_mutex));
		config->outgoing = buffers;
	} else {
		pr_warn(FZBRSP "Sorry, just exclusive open() for now\n");
		ret = -EBUSY;
		goto alldone;
	}

	PR_V1("open: %d users\n", atomic_read(&config->nr_users));

alldone:
	if (ret) 
		atomic_dec(&config->nr_users);
	return ret;
}

//-------------------------------------------------------------------------
// At any close of a process fd

static int bridge_flush(struct file *file, fl_owner_t id)
{
	struct famez_config *config = extract_config(file);
	int nr_users, f_count;

	spin_lock(&file->f_lock);
	nr_users = atomic_read(&config->nr_users);
	f_count = atomic_long_read(&file->f_count);
	spin_unlock(&file->f_lock);
	if (f_count == 1) {
		atomic_dec(&config->nr_users);
		nr_users--;
	}

	PR_V1("flush: after (optional) dec: %d users, file count = %d\n",
		nr_users, f_count);
	
	return 0;
}

//-------------------------------------------------------------------------
// Only at the final close of the last process fd

static int bridge_release(struct inode *inode, struct file *file)
{
	struct famez_config *config = extract_config(file);
	struct bridge_buffers *buffers = config->outgoing;
	int nr_users, f_count;

	spin_lock(&file->f_lock);
	nr_users = atomic_read(&config->nr_users);
	f_count = atomic_long_read(&file->f_count);
	spin_unlock(&file->f_lock);
	PR_V1("release: %d users, file count = %d\n", nr_users, f_count);
	BUG_ON(nr_users);
	kfree(buffers->wbuf);
	kfree(buffers);
	config->outgoing = NULL;
	return 0;
}

//-------------------------------------------------------------------------
// Prepend the sender id as a field separated by a colon, realized by two
// calls to copy_to_user and avoiding a temporary buffer here. copy_to_user
// can sleep and returns the number of bytes that could NOT be copied or
// -ERRNO.  Require both copies to work all the way.  

static ssize_t bridge_read(struct file *file, char __user *buf,
			   size_t buflen, loff_t *ppos)
{
	struct famez_config *config = extract_config(file);
	struct famez_mailslot *sender;
	int ret, n;
	// SID is 28 bits or 10 decimal digits; CID is 16 bits or 5 digits
	// so make the buffer big enough.
	char sidcidstr[32];

	// A successful return needs cleanup via famez_release_incoming().
	sender = famez_await_incoming(config, file->f_flags & O_NONBLOCK);
	if (IS_ERR(sender))
		return PTR_ERR(sender);
	PR_V2(FZSP "wait finished, %llu bytes to read\n", sender->buflen);

	// Two parts to the response: first is the sender "CID,SID:".
	// Omit  the [] brackets commonly seen in the spec, ala [CID,SID].
	n = snprintf(sidcidstr, sizeof(sidcidstr) - 1,
		"%llu,%llu:", sender->peer_CID, sender->peer_SID);

	if (n >= sizeof(sidcidstr) || buflen < sender->buflen + n - 1) {
		ret = -E2BIG;
		goto read_complete;
	}
	if ((ret = copy_to_user(buf, sidcidstr, n))) {
		if (ret > 0) ret= -EFAULT;	// partial transfer
		goto read_complete;
	}

	// The message body follows the colon of the previous snippet.
	ret = copy_to_user(buf + (uint64_t)n, sender->buf, sender->buflen);
	ret = !ret ? sender->buflen + n :
		(ret > 0 ? -EFAULT : ret);
	// Now it's either the length of the full responose or -ESOMETHING
	if (ret > 0)
		*ppos = 0;

read_complete:	// Whether I used it or not, let everything go
	famez_release_incoming(config);
	return ret;
}

//-------------------------------------------------------------------------
// Use many idiot checks.  Performance is not the issue here.  The data
// might be binary (including unprintables and NULs), not just a C string.

static ssize_t bridge_write(struct file *file, const char __user *buf,
			    size_t buflen, loff_t *ppos)
{
	struct famez_config *config = extract_config(file);
	struct bridge_buffers *buffers = config->outgoing;
	ssize_t successlen = buflen;
	char *bufbody;
	int ret, restarts, SID, CID;

	if (buflen >= config->max_buflen - 1) {		// Paranoia on term NUL
		PR_V1("buflen of %lu is too big\n", buflen);
		return -E2BIG;
	}
	mutex_lock(&buffers->wbuf_mutex);	// Multiuse of *file
	if ((ret = copy_from_user(buffers->wbuf, buf, buflen))) {
		if (ret > 0)
			ret = -EFAULT;
		goto unlock_return;
	}
	// Even if it's not a string, this puts a bound on the strchr(':')
	buffers->wbuf[buflen] = '\0';		

	// Split body into two pieces around the first colon: a proper string
	// and whatever the real payload is (string or binary).
	if (!(bufbody = strchr(buffers->wbuf, ':'))) {
		pr_err(FZBR "no colon in \"%s\"\n", buffers->wbuf);
		ret = -EBADMSG;
		goto unlock_return;
	}
	*bufbody = '\0';	// chomp ':', now two NUL-terminated sections
	bufbody++;
	buflen -= (uint64_t)bufbody - (uint64_t)buffers->wbuf;

	// SID and CID from varying input, including "expert use" of a peer id.

	SID = FAMEZ_SID_CID_IS_PEER_ID;
	if (STREQ(buffers->wbuf, "server") || STREQ(buffers->wbuf, "switch") ||
	    STREQ(buffers->wbuf, "link") || STREQ(buffers->wbuf, "interface"))
		CID = config->globals->server_id;
	else {
		char *comma = strchr(buffers->wbuf, ','); // Want CID,SID

		if (comma) {
			*comma = '\0';
			if ((ret = kstrtoint(buffers->wbuf, 0, &SID)))
				goto unlock_return;
			if ((ret = kstrtoint(comma + 1, 0, &CID)))
				goto unlock_return;
		} else {	// Direct use of an IVSHMSG peer id
			if ((ret = kstrtoint(buffers->wbuf, 0, &CID)))
				goto unlock_return;
		}
	}

	// Length or -ERRNO.  If length matched, then all is well, but
	// this final len is always shorter than the original length.  Some
	// code (ie, "echo") will resubmit the partial if the count is
	// short.  So lie about it to the caller.

	restarts = 0;
restart:
	ret = famez_create_outgoing(CID, SID, bufbody, buflen, config);
	if (ret == -ERESTARTSYS) {	// spurious timeout
		if (restarts++ < 2)
			goto restart;
		ret = -ETIMEDOUT;
	} else if (ret == buflen)
		ret = successlen;
	else if (ret >= 0)
		ret = -EIO;	// partial transfer paranoia

unlock_return:
	mutex_unlock(&buffers->wbuf_mutex);
	return ret;
}

//-------------------------------------------------------------------------
// Returning 0 will cause the caller (epoll/poll/select) to sleep.

static uint bridge_poll(struct file *file, struct poll_table_struct *wait)
{
	struct famez_config *config = extract_config(file);
	uint ret = 0;

	poll_wait(file, &bridge_reader_wait, wait);
		ret |= POLLIN | POLLRDNORM;
	// FIXME encapsulate this better, it's really the purview of sendstring
	if (!config->my_slot->buflen)
		ret |= POLLOUT | POLLWRNORM;
	return ret;
}

static const struct file_operations bridge_fops = {
	.owner	=	THIS_MODULE,
	.open	=	bridge_open,
	.flush  =	bridge_flush,
	.release =      bridge_release,
	.read	=       bridge_read,
	.write	=       bridge_write,
	.poll	=       bridge_poll,
};

//-------------------------------------------------------------------------
// Called from insmod.  Bind the driver set to all available FAME-Z devices.

static int _nbindings = 0;

int __init fzbridge_init(void)
{
	int ret;

	pr_info("-------------------------------------------------------");
	pr_info(FZBR FZBRIDGE_VERSION "; parms:\n");
	pr_info(FZSP "verbose = %d\n", verbose);

	_nbindings = 0;
	if ((ret = famez_misc_register("bridge", &bridge_fops)) < 0)
		return ret;
	_nbindings = ret;
	pr_info(FZBR "%d bindings made\n", _nbindings);
	return _nbindings ? 0 : -ENODEV;
}

module_init(fzbridge_init);

//-------------------------------------------------------------------------
// Called from rmmod.  Unbind this driver set from any registered bindings.

void fzbridge_exit(void)
{
	int ret = famez_misc_deregister(&bridge_fops);
	if (ret >= 0)
		pr_info(FZBR "%d/%d bindings released\n", ret, _nbindings);
	else
		pr_err(FZBR "module exit errno %d\n", -ret);
}

module_exit(fzbridge_exit);
