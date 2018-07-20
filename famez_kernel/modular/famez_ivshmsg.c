// Configure and handle MSI-X interrupts from IVSHMEM device.  The sendstring
// method is also here, which keeps all "hardware IO" in one place.

#include <linux/delay.h>	// usleep_range, wait_event*
#include <linux/export.h>
#include <linux/jiffies.h>	// jiffies
#include <linux/interrupt.h>	// irq_enable, etc
#include <linux/pci.h>		// all kinds

#include "famez.h"

//-------------------------------------------------------------------------
// Return positive (bytecount) on success, negative on error, never 0.
// I don't really believe usleep_range is atomic-safe but I'm on mutices now.

int famez_sendmail(uint32_t peer_id, char *msg, size_t msglen,
		   famez_configuration_t *config)
{
	uint64_t hw_timeout = get_jiffies_64() + HZ/2;	// 500 ms
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
	while (config->my_slot->msglen && get_jiffies_64() < hw_timeout)
		 usleep_range(50000, 80000);


	// FIXME: add stompcounter field, when it hits 5 ret(-ENOBUFS).
	// To start with, just emit that error on first occurrence and
	// see what falls out.
	if (config->my_slot->msglen)
		pr_warn(FZ "%s() stomps previous message to %llu\n",
			__FUNCTION__, config->my_slot->last_responder);

	// Keep nodename and msg pointer; update msglen and msg contents.
	// memset(config->my_slot->msg, 0, config->max_msglen);	# overkill
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
	int ret;

	if ((ret = FAMEZ_LOCK(&config->legible_slot_lock)))
		return ERR_PTR(ret);
	while (!config->legible_slot) {		// Wait for new data?
		FAMEZ_UNLOCK(&config->legible_slot_lock);
		if (file->f_flags & O_NONBLOCK)
			return ERR_PTR(-EAGAIN);
		PR_V2(FZ "read() waiting...\n");
		if (wait_event_interruptible(config->legible_slot_wqh, 
					     config->legible_slot))
			return ERR_PTR(-ERESTARTSYS);
		if ((ret = FAMEZ_LOCK(&config->legible_slot_lock)))
			return ERR_PTR(ret);
	}
	return config->legible_slot;
}
EXPORT_SYMBOL(famez_await_legible_slot);

//-------------------------------------------------------------------------

void famez_release_legible_slot(famez_configuration_t *config)
{
	config->legible_slot->msglen = 0;	// In the slot of the remote sender
	config->legible_slot = NULL;		// Seen by local MSIX handler
	FAMEZ_UNLOCK(&config->legible_slot_lock);
}
EXPORT_SYMBOL(famez_release_legible_slot);
