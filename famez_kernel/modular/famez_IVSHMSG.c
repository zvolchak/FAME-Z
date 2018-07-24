// Configure and handle MSI-X interrupts from IVSHMEM device.  The sendstring
// method is also here, which keeps all "hardware IO" in one place.

#include <linux/delay.h>	// usleep_range, wait_event*
#include <linux/export.h>
#include <linux/jiffies.h>	// jiffies

#include "famez.h"

//-------------------------------------------------------------------------
// Return positive (bytecount) on success, negative on error, never 0.
// The synchronous rate seems to be determined mostly by the sleep 
// duration. I tried a 3x timeout whose success varied from 2 minutes to
// three hours before it popped. 4x was better, lasted until I did a
// compile, so...

#define PRIOR_RESP_WAIT (HZ/4)	// 5x
#define DELAY_MS	10	// or about 100 writes/second

static unsigned long longest = PRIOR_RESP_WAIT/2;

int famez_sendmail(uint32_t peer_id, char *msg, size_t msglen,
		   famez_configuration_t *config)
{
	unsigned long now = 0, hw_timeout = get_jiffies_64() + PRIOR_RESP_WAIT;
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
			mdelay(DELAY_MS); // udelay(25k) leads to compiler error
		else
		 	msleep(DELAY_MS);
	       now = get_jiffies_64();
	}
	if ((hw_timeout -= now) > longest) {
		// pr_warn(FZ "%s() biggest TO goes from %lu to %lu\n",
			// __FUNCTION__, longest, hw_timeout);
		longest = hw_timeout;
	}

	// FIXME: add stompcounter tracker, return -EXXXX. To start with, just
	// emit an error on first occurrence and see what falls out.
	if (config->my_slot->msglen) {
		PR_V1("%s() would stomp previous message to %llu\n",
			__FUNCTION__, config->my_slot->last_responder);
		return -ETIMEDOUT;
	}
	// Keep nodename and msg pointer; update msglen and msg contents.
	// msglen is the handshake out to the world that I'm busy.
	config->my_slot->msglen = msglen;
	config->my_slot->msg[msglen] = '\0';	// ASCII strings paranoia
	config->my_slot->last_responder = peer_id;
	memcpy(config->my_slot->msg, msg, msglen);

	// Choose the correct vector set from all sent to me via the peer.
	// Trigger the vector corresponding to me with the vector.
	ringer.peer = peer_id;
	ringer.vector = config->my_id;
	config->regs->Doorbell = ringer.Doorbell;
	return msglen;
}
EXPORT_SYMBOL(famez_sendmail);

//-------------------------------------------------------------------------
// Return a pointer to the data structure or ERRPTR, rather than an integer
// ret, so the caller doesn't need to understand the config structure to
// look it up.  Intermix locking with that in msix_all().

famez_mailslot_t *famez_await_legible_slot(struct file *file,
					   famez_configuration_t *config)
{
	int ret = 0;

	if (config->legible_slot)
		return config->legible_slot;
	if (file->f_flags & O_NONBLOCK)
		return ERR_PTR(-EAGAIN);
	PR_V2("%s() waiting...\n", __FUNCTION__);

	// wait_event_xxx checks the the condition BEFORE waiting but
	// does modify the run state.  Does that side effect matter?
	// FIXME: wait_event_interruptible_locked?
	if ((ret = wait_event_interruptible(config->legible_slot_wqh, 
					    config->legible_slot)))
		return ERR_PTR(ret);
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
