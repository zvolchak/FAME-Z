// Implement the mailbox/mailslot protocol of IVSHMSG.

#include <linux/delay.h>	// usleep_range, wait_event*
#include <linux/export.h>
#include <linux/jiffies.h>	// jiffies

#include "famez.h"

//-------------------------------------------------------------------------
// Return positive (bytecount) on success, negative on error, never 0.
// The synchronous rate seems to be determined mostly by the sleep 
// duration. I tried a 3x timeout whose success varied from 2 minutes to
// three hours before it popped. 4x was better, lasted until I did a
// compile, so...use a slightly adaptive timeout to reach the LOOP_MAX.

#define PRIOR_RESP_WAIT		(5 * HZ)	// 5x
#define DELAY_MS_LOOP_MAX	10		// or about 100 writes/second

static unsigned long longest = PRIOR_RESP_WAIT/2;

int famez_sendmail(uint32_t peer_id, char *msg, size_t msglen,
		   struct famez_config *config)
{
	// The IVSHMEM "vector" will map to an MSI-X "entry" value.  "vector"
	// is the lower 16 bits and the combo must be assigned atomically.
	union __attribute__ ((packed)) {
		struct { uint16_t vector, peer; };
		uint32_t Doorbell;
	} ringer;
	unsigned long now = 0, this_delay,
		 hw_timeout = get_jiffies_64() + PRIOR_RESP_WAIT;

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
	this_delay = 1;
	while (config->my_slot->msglen && time_before(now, hw_timeout)) {
		if (in_interrupt())
			mdelay(this_delay); // (25k) leads to compiler error
		else
		 	msleep(this_delay);
		if (this_delay < DELAY_MS_LOOP_MAX)
			this_delay += 2;
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
		pr_err("%s() would stomp previous message to %llu\n",
			__FUNCTION__, config->my_slot->last_responder);
		return -ERESTARTSYS;
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

struct famez_mailslot *famez_await_incoming(struct famez_config *config,
					    int nonblocking)
{
	int ret = 0;

	if (config->incoming_slot)
		return config->incoming_slot;
	if (nonblocking)
		return ERR_PTR(-EAGAIN);
	PR_V2("%s() waiting...\n", __FUNCTION__);

	// wait_event_xxx checks the the condition BEFORE waiting but
	// does modify the run state.  Does that side effect matter?
	// FIXME: wait_event_interruptible_locked?
	if ((ret = wait_event_interruptible(config->incoming_slot_wqh, 
					    config->incoming_slot)))
		return ERR_PTR(ret);
	return config->incoming_slot;
}
EXPORT_SYMBOL(famez_await_incoming);

//-------------------------------------------------------------------------

void famez_release_incoming(struct famez_config *config)
{
	spin_lock(&config->incoming_slot_lock);
	config->incoming_slot->msglen = 0;	// The slot of the sender.
	config->incoming_slot = NULL;		// The local MSI-X handler.
	spin_unlock(&config->incoming_slot_lock);
}
EXPORT_SYMBOL(famez_release_incoming);
