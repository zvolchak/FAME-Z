// Configure and handle MSI-X interrupts from IVSHMEM device.  The sendstring
// method is also here, which keeps all "hardware IO" in one place.

#include <linux/delay.h>	// usleep_range, wait_event*
#include <linux/export.h>
#include <linux/jiffies.h>	// jiffies

#include "famez.h"

//-------------------------------------------------------------------------
// Return positive (bytecount) on success, negative on error, never 0.
// I don't really believe usleep_range is atomic-safe but I'm on mutices now.

int famez_sendmail(uint32_t peer_id, char *msg, size_t msglen,
		   famez_configuration_t *config)
{
	unsigned long now = 0, hw_timeout = get_jiffies_64() + HZ/5;	// 200 ms
	ivshmsg_ringer_t ringer;

	// Might NOT be printable C string.
	PR_V1("sendmail(%lu bytes) to %d\n", msglen, peer_id);

	if (peer_id < 1 || peer_id > config->server_id)
		return -EBADSLT;
	if (msglen >= config->max_msglen)
		return -E2BIG;
	if (!msglen)
		return -ENODATA; // FIXME: is there value to a "silent kick"?

	// Pseudo-"HW ready": wait until my_slot has pushed a previous write
	// through. In truth it's the previous responder clearing my msglen.
	// The macro makes many references to its parameters, so...
	while (config->my_slot->msglen && time_before(now, hw_timeout)) {
		if (in_interrupt())
			mdelay(25);	// udelay(25k) leads to compiler error
		else
		 	usleep_range(30000, 60000);
	       now = get_jiffies_64();
	}

	// FIXME: add stompcounter tracker, return -EXXXX. To start with, just
	// emit an error on first occurrence and see what falls out.
	if (config->my_slot->msglen) {
		if (config->my_slot->last_responder == peer_id) {
			pr_err("Peer %u has left the building\n", peer_id);
			config->my_slot->msglen = 0;
			config->my_slot->last_responder = 0;
			return -EHOSTDOWN;
		}
		PR_V1("%s() would stomp previous message to %llu\n",
			__FUNCTION__, config->my_slot->last_responder);
		return -ENOBUFS;
	}
	// Keep nodename and msg pointer; update msglen and msg contents.
	config->my_slot->msglen = msglen;
	config->my_slot->msg[msglen] = '\0';	// ASCII strings paranoia
	config->my_slot->last_responder = peer_id;

	memcpy(config->my_slot->msg, msg, msglen);
	ringer.vector = config->my_id;		// from this
	ringer.peer = peer_id;			// to this
	config->regs->Doorbell = ringer.Doorbell;
	return msglen;
}
EXPORT_SYMBOL(famez_sendmail);

//-------------------------------------------------------------------------
// Failure returns < 0 without lock held; success returns >=0 number of bytes
// ready with lock held.  Intermix locking with that in msix_all().

famez_mailslot_t *famez_await_legible_slot(struct file *file,
					   famez_configuration_t *config)
{
	spin_lock(&config->legible_slot_lock);
	while (!config->legible_slot) {		// Wait for new data?
		spin_unlock(&config->legible_slot_lock);
		if (file->f_flags & O_NONBLOCK)
			return ERR_PTR(-EAGAIN);
		PR_V2("read() waiting...\n");
		if (wait_event_interruptible(config->legible_slot_wqh, 
					     config->legible_slot))
			return ERR_PTR(-ERESTARTSYS);
		spin_lock(&config->legible_slot_lock);
	}
	spin_unlock(&config->legible_slot_lock);
	return config->legible_slot;
}
EXPORT_SYMBOL(famez_await_legible_slot);

//-------------------------------------------------------------------------

void famez_release_legible_slot(famez_configuration_t *config)
{
	spin_lock(&config->legible_slot_lock);
	config->legible_slot->msglen = 0;	// In the slot of the remote sender
	config->legible_slot = NULL;		// Seen by local MSIX handler
	spin_unlock(&config->legible_slot_lock);
}
EXPORT_SYMBOL(famez_release_legible_slot);
