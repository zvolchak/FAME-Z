// Link-level messages, mostly from the switch (IVSHMSG server).
// It may hijack and finish off the message.

#include "famez.h"

#define GENZ_LINK_CTL_PEER_ATTRIBUTE "Link CTL Peer-Attribute"

//-------------------------------------------------------------------------
// This is called in interrupt context and the incoming_slot_lock held.

irqreturn_t famez_link_request(struct famez_mailslot __iomem *incoming_slot,
			       struct famez_config *config)
{
	char outbuf[64];

	// These are all fixed values now, but someday...
	incoming_slot->peer_SID = FAMEZ_SID_DEFAULT;
	incoming_slot->peer_CID = incoming_slot->peer_id * 100;

	// Simple proof-of-life, must be an exact match.
	if (incoming_slot->buflen == 4 &&
	    STREQ_N(incoming_slot->buf, "ping", 4)) {
		incoming_slot->buflen = 0;	// buf received
		spin_unlock(&(config->incoming_slot_lock));
		famez_create_outgoing(
			FAMEZ_SID_CID_IS_PEER_ID,
			incoming_slot->peer_id,
			"pong", 4,
			config);
		return IRQ_HANDLED;
	}

	if (STREQ_N(incoming_slot->buf, GENZ_LINK_CTL_PEER_ATTRIBUTE,
		strlen(GENZ_LINK_CTL_PEER_ATTRIBUTE))) {
		incoming_slot->buflen = 0;	// buf received
		spin_unlock(&(config->incoming_slot_lock));
		sprintf(outbuf, "Link CTL ACK C-Class=Bridge,SID0=%d,CID0=%d",
			config->core->SID0, config->core->CID0);
		famez_create_outgoing(
			FAMEZ_SID_CID_IS_PEER_ID,
			incoming_slot->peer_id,
			outbuf, strlen(outbuf),
			config);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}
